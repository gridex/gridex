#include "Presentation/Views/QueryEditor/QueryEditorView.h"

#include <vector>

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QMenu>
#include <QMessageBox>
#include <QStandardPaths>
#include <QTextStream>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFont>
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QPaintEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QTabBar>
#include <QTableView>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextLayout>
#include <QTimer>
#include <QtConcurrent/QtConcurrent>
#include <QVBoxLayout>

#include "Presentation/Theme/ThemeManager.h"
#include "Presentation/Views/Chrome/GxIcons.h"

#include "Core/Errors/GridexError.h"
#include "Core/Protocols/Database/IDatabaseAdapter.h"
#include "Presentation/Views/DataGrid/QueryResultModel.h"
#include "Presentation/Views/QueryEditor/Autocomplete/AutocompleteProvider.h"
#include "Presentation/Views/QueryEditor/Autocomplete/CompletionModels.h"
#include "Presentation/Views/QueryEditor/Autocomplete/CompletionPopup.h"
#include "Presentation/Views/QueryEditor/Autocomplete/SqlContextParser.h"
#include "Presentation/Views/QueryEditor/SqlHighlighter.h"

namespace gridex {

namespace {

class GxEditor;

// Line-number gutter painted to the left of the editor viewport.
// Width is driven by current line count; current-line gets the accent
// color matching .gx-ln.is-cursor in the design.
class LineGutter : public QWidget {
public:
    explicit LineGutter(GxEditor* editor);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    GxEditor* editor_;
};

// QPlainTextEdit subclass that:
//  - suppresses the built-in placeholder during IME preedit (Vietnamese
//    Telex/VNI would otherwise overlap the composing character),
//  - reserves left margin for the gutter and paints the current-line
//    highlight band matching .gx-cline.is-cursor.
class GxEditor : public QPlainTextEdit {
public:
    using QPlainTextEdit::QPlainTextEdit;

    // LineGutter paints using firstVisibleBlock / blockBoundingGeometry /
    // contentOffset / blockBoundingRect, all of which are protected on
    // QPlainTextEdit. Friend access keeps the gutter implementation simple.
    friend class LineGutter;

    int gutterWidth() const {
        int digits = 1;
        int max = qMax(1, blockCount());
        while (max >= 10) { max /= 10; ++digits; }
        const int ch = fontMetrics().horizontalAdvance(QLatin1Char('9'));
        return 12 + ch * digits + 8;
    }

    void setGutter(LineGutter* g) {
        gutter_ = g;
        updateMargins();
        connect(this, &QPlainTextEdit::blockCountChanged, this, [this](int){ updateMargins(); });
        connect(this, &QPlainTextEdit::updateRequest, this,
                [this](const QRect& rect, int dy) {
                    if (!gutter_) return;
                    if (dy) gutter_->scroll(0, dy);
                    else gutter_->update(0, rect.y(), gutter_->width(), rect.height());
                });
        connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this]{
            if (gutter_) gutter_->update();
            viewport()->update();
        });
    }

    void updateMargins() {
        setViewportMargins(gutter_ ? gutterWidth() : 0, 0, 0, 0);
        if (gutter_) {
            const QRect cr = contentsRect();
            gutter_->setGeometry(QRect(cr.left(), cr.top(), gutterWidth(), cr.height()));
        }
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        // Caret-line band — paint behind text. Uses bg-1 against the bg-0
        // editor background so the band is subtly visible in both themes.
        {
            QPainter p(viewport());
            const QRect cur = cursorRect();
            QRect band(0, cur.top(), viewport()->width(), cur.height());
            const QColor bg1 = ThemeManager::instance().isDark()
                ? QColor(0x11, 0x15, 0x1a)
                : QColor(0xf4, 0xf5, 0xf7);
            p.fillRect(band, bg1);
        }
        const bool suppress = document()->isEmpty() && hasPreeditText();
        QString savedPlaceholder;
        if (suppress) {
            savedPlaceholder = placeholderText();
            QPlainTextEdit::setPlaceholderText(QString{});
        }
        QPlainTextEdit::paintEvent(event);
        if (suppress) {
            QPlainTextEdit::setPlaceholderText(savedPlaceholder);
        }
    }

    void resizeEvent(QResizeEvent* event) override {
        QPlainTextEdit::resizeEvent(event);
        if (gutter_) {
            const QRect cr = contentsRect();
            gutter_->setGeometry(QRect(cr.left(), cr.top(), gutterWidth(), cr.height()));
        }
    }

    void inputMethodEvent(QInputMethodEvent* event) override {
        QPlainTextEdit::inputMethodEvent(event);
        viewport()->update();
    }

private:
    bool hasPreeditText() const {
        auto* layout = textCursor().block().layout();
        return layout && !layout->preeditAreaText().isEmpty();
    }

    LineGutter* gutter_ = nullptr;
};

using IMEAwareTextEdit = GxEditor;  // kept for back-compat in this TU

LineGutter::LineGutter(GxEditor* editor) : QWidget(editor), editor_(editor) {
    setAttribute(Qt::WA_StyledBackground, false);
}

QSize LineGutter::sizeHint() const {
    return QSize(editor_ ? editor_->gutterWidth() : 32, 0);
}

void LineGutter::paintEvent(QPaintEvent* event) {
    if (!editor_) return;
    QPainter p(this);

    // Resolve token palette at paint time so a Light/Dark switch redraws
    // the gutter without needing a widget rebuild.
    const bool dark = ThemeManager::instance().isDark();
    const QColor bg0    = dark ? QColor(0x09, 0x0e, 0x12) : QColor(0xff, 0xff, 0xff);
    const QColor border = dark ? QColor(0x2e, 0x33, 0x39) : QColor(0xc5, 0xca, 0xd1);
    const QColor bg1    = dark ? QColor(0x11, 0x15, 0x1a) : QColor(0xf4, 0xf5, 0xf7);
    const QColor accent = dark ? QColor(0x00, 0xb8, 0xe1) : QColor(0x00, 0x98, 0xbd);
    const QColor faint  = dark ? QColor(0x55, 0x58, 0x5c) : QColor(0x93, 0x98, 0xa0);

    p.fillRect(event->rect(), bg0);
    p.setPen(border);
    p.drawLine(width() - 1, 0, width() - 1, height());

    QTextBlock block = editor_->firstVisibleBlock();
    int blockNumber  = block.blockNumber();
    int top    = qRound(editor_->blockBoundingGeometry(block).translated(editor_->contentOffset()).top());
    int bottom = top + qRound(editor_->blockBoundingRect(block).height());
    const int curLine = editor_->textCursor().blockNumber();

    QFont f = editor_->font();
    p.setFont(f);

    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            const bool isCur = (blockNumber == curLine);
            if (isCur) {
                p.fillRect(0, top, width() - 1, qRound(editor_->blockBoundingRect(block).height()),
                           bg1);
                p.setPen(accent);
            } else {
                p.setPen(faint);
            }
            const QString number = QString::number(blockNumber + 1);
            p.drawText(0, top, width() - 8, qRound(editor_->blockBoundingRect(block).height()),
                       Qt::AlignRight | Qt::AlignVCenter, number);
        }
        block = block.next();
        top = bottom;
        bottom = top + qRound(editor_->blockBoundingRect(block).height());
        ++blockNumber;
    }
}

// All editor/result chrome lives in resources/style-gx{,-light}.qss.
// See the "Query editor" section in each sheet.

}  // namespace

namespace {
// Process-wide list of toolbar extension factories. Plain function-local
// static so it's leak-free and avoids global-ctor ordering pitfalls.
std::vector<QueryEditorView::ExtensionFactory>& extensionFactories() {
    static std::vector<QueryEditorView::ExtensionFactory> v;
    return v;
}
}

void QueryEditorView::registerExtension(ExtensionFactory f) {
    if (f) extensionFactories().push_back(std::move(f));
}

QueryEditorView::QueryEditorView(QWidget* parent)
    : QWidget(parent),
      provider_(std::make_unique<AutocompleteProvider>()),
      parser_(std::make_unique<SqlContextParser>()) {
    buildUi();
    // Let extensions decorate the toolbar after it's fully constructed.
    for (const auto& f : extensionFactories()) f(this);
}

QueryEditorView::~QueryEditorView() = default;

void QueryEditorView::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ---- Toolbar: [Run ▶] [status ...] [Save] ----
    auto* top = new QWidget(this);
    top->setObjectName(QStringLiteral("QueryEditorToolbar"));
    top->setAttribute(Qt::WA_StyledBackground, true);
    top->setFixedHeight(36);
    auto* topH = new QHBoxLayout(top);
    topH->setContentsMargins(10, 0, 10, 0);
    topH->setSpacing(8);
    toolbarLay_ = topH;

    runBtn_ = new QPushButton(tr("Run statement"), top);
    runBtn_->setObjectName(QStringLiteral("QueryEditorRun"));
    runBtn_->setIcon(GxIcons::glyph(QStringLiteral("play"), QStringLiteral("#090e12")));
    runBtn_->setIconSize(QSize(11, 11));
    runBtn_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return));
    runBtn_->setCursor(Qt::PointingHandCursor);
    runBtn_->setToolTip(tr("Execute query (Ctrl+Enter)"));
    connect(runBtn_, &QPushButton::clicked, this, &QueryEditorView::onRunClicked);
    topH->addWidget(runBtn_);

    statusLbl_ = new QLabel(QString{}, top);
    statusLbl_->setObjectName(QStringLiteral("QueryEditorStatus"));
    topH->addWidget(statusLbl_, 1);

    auto* saveBtn = new QPushButton(tr("Save"), top);
    saveBtn->setObjectName(QStringLiteral("QueryEditorSave"));
    saveBtn->setIcon(GxIcons::glyph(QStringLiteral("save")));
    saveBtn->setIconSize(QSize(11, 11));
    saveBtn->setCursor(Qt::PointingHandCursor);
    saveBtn->setToolTip(tr("Save this query to Saved Queries"));
    connect(saveBtn, &QPushButton::clicked, this, [this] {
        const QString sql = editor_ ? editor_->toPlainText().trimmed() : QString{};
        if (!sql.isEmpty()) emit saveQueryRequested(sql);
    });
    topH->addWidget(saveBtn);

    root->addWidget(top);

    // ---- Splitter: editor (top) | results panel (bottom) ----
    splitter_ = new QSplitter(Qt::Vertical, this);
    splitter_->setHandleWidth(4);
    splitter_->setChildrenCollapsible(false);

    editor_ = new GxEditor(splitter_);
    editor_->setObjectName(QStringLiteral("gxSqlEditor"));
    editor_->setPlaceholderText(tr("SELECT * FROM ..."));
    editor_->setFrameShape(QFrame::NoFrame);
    editor_->setTabStopDistance(32);
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    // Prefer JetBrains Mono / Fira Code if present.
    for (const auto& family : { QStringLiteral("JetBrains Mono"),
                                QStringLiteral("Fira Code"),
                                QStringLiteral("DejaVu Sans Mono") }) {
        if (QFontDatabase::families().contains(family)) {
            mono.setFamily(family);
            break;
        }
    }
    mono.setPointSize(11);
    editor_->setFont(mono);
    editor_->installEventFilter(this);
    auto* gutter = new LineGutter(static_cast<GxEditor*>(editor_));
    static_cast<GxEditor*>(editor_)->setGutter(gutter);
    hl_ = new SqlHighlighter(editor_->document());
    splitter_->addWidget(editor_);

    // ---- Autocomplete wiring ----
    popup_ = new CompletionPopup(this);
    popup_->hide();
    connect(popup_, &CompletionPopup::accepted, this, &QueryEditorView::acceptCompletion);
    connect(popup_, &CompletionPopup::dismissed, this, &QueryEditorView::hidePopup);

    // Debounce textChanged so we don't reparse on every keystroke for large
    // SQL files. 60ms feels instant while skipping rapid-fire edits.
    debounce_ = new QTimer(this);
    debounce_->setSingleShot(true);
    debounce_->setInterval(60);
    connect(debounce_, &QTimer::timeout, this, &QueryEditorView::maybeShowCompletions);
    connect(editor_, &QPlainTextEdit::textChanged, this, [this]() {
        if (debounce_) debounce_->start();
    });

    // ---- Results panel: 5-tab header + content stack -----------------
    auto* results = new QWidget(splitter_);
    auto* resultsV = new QVBoxLayout(results);
    resultsV->setContentsMargins(0, 0, 0, 0);
    resultsV->setSpacing(0);

    auto* resHdr = new QWidget(results);
    resHdr->setObjectName(QStringLiteral("QueryEditorResultsTabs"));
    resHdr->setAttribute(Qt::WA_StyledBackground, true);
    resHdr->setFixedHeight(28);
    auto* resHdrH = new QHBoxLayout(resHdr);
    resHdrH->setContentsMargins(4, 0, 8, 0);
    resHdrH->setSpacing(0);

    auto* resultsStack = new QStackedWidget(results);

    // Build tab buttons + placeholder/active pages.
    struct TabSpec { const char* id; const char* label; bool functional; };
    const std::vector<TabSpec> specs = {
        { "msg",     QT_TR_NOOP("Messages"),     false },
        { "grid",    QT_TR_NOOP("Results"),      true  },
        { "explain", QT_TR_NOOP("Explain"),      false },
        { "plan",    QT_TR_NOOP("Plan tree"),    false },
        { "stats",   QT_TR_NOOP("Statistics"),   false },
    };

    std::vector<QPushButton*> tabBtns;
    tabBtns.reserve(specs.size());

    int gridPageIndex = -1;

    for (std::size_t i = 0; i < specs.size(); ++i) {
        const auto& s = specs[i];
        auto* btn = new QPushButton(tr(s.label), resHdr);
        btn->setProperty("gxResTab", true);
        btn->setProperty("gxActive", false);
        btn->setFlat(true);
        btn->setCursor(Qt::PointingHandCursor);
        resHdrH->addWidget(btn);
        tabBtns.push_back(btn);

        QWidget* page = nullptr;
        if (s.functional && QString::fromLatin1(s.id) == QLatin1String("grid")) {
            // Real result grid.
            resultView_ = new QTableView(resultsStack);
            resultView_->setObjectName(QStringLiteral("QueryEditorResult"));
            resultView_->setFrameShape(QFrame::NoFrame);
            resultView_->setAlternatingRowColors(true);
            resultView_->setShowGrid(false);
            resultView_->setSelectionBehavior(QAbstractItemView::SelectItems);
            resultView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
            resultView_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
            resultView_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
            resultView_->horizontalHeader()->setObjectName(QStringLiteral("QueryEditorResultHeader"));
            resultView_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
            resultView_->horizontalHeader()->setStretchLastSection(true);
            resultView_->verticalHeader()->setDefaultSectionSize(22);
            resultView_->verticalHeader()->setVisible(false);

            resultModel_ = new QueryResultModel(this);
            resultView_->setModel(resultModel_);
            page = resultView_;
            gridPageIndex = static_cast<int>(i);
        } else {
            auto* ph = new QLabel(tr("%1 — no data yet. Run a query to populate.")
                                       .arg(tr(s.label)), resultsStack);
            ph->setObjectName(QStringLiteral("QueryEditorResultsPlaceholder"));
            ph->setAlignment(Qt::AlignCenter);
            ph->setAutoFillBackground(true);
            page = ph;
        }
        resultsStack->addWidget(page);
    }

    resHdrH->addStretch(1);
    resultsV->addWidget(resHdr);
    resultsV->addWidget(resultsStack, 1);

    auto activateTab = [tabBtns, resultsStack](int idx) {
        for (std::size_t i = 0; i < tabBtns.size(); ++i) {
            const bool a = (static_cast<int>(i) == idx);
            tabBtns[i]->setProperty("gxActive", a);
            tabBtns[i]->style()->unpolish(tabBtns[i]);
            tabBtns[i]->style()->polish(tabBtns[i]);
            tabBtns[i]->update();
        }
        resultsStack->setCurrentIndex(idx);
    };
    for (std::size_t i = 0; i < tabBtns.size(); ++i) {
        const int idx = static_cast<int>(i);
        QObject::connect(tabBtns[i], &QPushButton::clicked, this,
                         [activateTab, idx] { activateTab(idx); });
    }
    activateTab(gridPageIndex >= 0 ? gridPageIndex : 0);

    splitter_->addWidget(results);

    // ---- Export button overlay at bottom-right of result table ----
    exportResultMenu_ = new QMenu(resultView_);
    auto* actCsv  = exportResultMenu_->addAction(tr("Export as CSV..."));
    auto* actSql  = exportResultMenu_->addAction(tr("Export as SQL INSERT..."));
    auto* actJson = exportResultMenu_->addAction(tr("Export as JSON..."));
    connect(actCsv,  &QAction::triggered, this, &QueryEditorView::exportResultAsCsv);
    connect(actSql,  &QAction::triggered, this, &QueryEditorView::exportResultAsSql);
    connect(actJson, &QAction::triggered, this, &QueryEditorView::exportResultAsJson);

    exportResultBtn_ = new QPushButton(tr("Export"), resultView_);
    exportResultBtn_->setObjectName(QStringLiteral("QueryEditorExport"));
    exportResultBtn_->setIcon(GxIcons::glyph(QStringLiteral("export")));
    exportResultBtn_->setIconSize(QSize(11, 11));
    exportResultBtn_->setFixedSize(96, 26);
    exportResultBtn_->setToolTip(tr("Export query result"));
    exportResultBtn_->setCursor(Qt::PointingHandCursor);
    exportResultBtn_->setMenu(exportResultMenu_);
    exportResultBtn_->hide();  // shown when result has rows
    // Reposition on result view resize. Install filter on resultView_ so we
    // catch QEvent::Resize regardless of who triggered it (splitter, window).
    resultView_->installEventFilter(this);
    // Show/hide button when model data changes.
    connect(resultModel_, &QAbstractTableModel::modelReset, this, [this]() {
        const bool hasData = resultModel_->rowCount() > 0;
        exportResultBtn_->setVisible(hasData);
        if (hasData) {
            // Position: bottom-right with 14px margin to clear scrollbars.
            const int x = resultView_->width()  - exportResultBtn_->width()  - 14;
            const int y = resultView_->height() - exportResultBtn_->height() - 14;
            exportResultBtn_->move(x, y);
            exportResultBtn_->raise();
        }
    });

    splitter_->setStretchFactor(0, 1);
    splitter_->setStretchFactor(1, 2);
    splitter_->setSizes({200, 400});

    root->addWidget(splitter_, 1);
}

void QueryEditorView::setSql(const QString& sql) {
    if (editor_) editor_->setPlainText(sql);
}

QString QueryEditorView::currentSql() const {
    return editor_ ? editor_->toPlainText() : QString();
}

void QueryEditorView::addToolbarWidget(QWidget* w) {
    if (!toolbarLay_ || !w) return;
    // Insert before the trailing Save button so toolbar order stays
    // [Run] [status (stretch)] [ext widgets...] [Save].
    const int trailing = 1;
    int at = toolbarLay_->count() - trailing;
    if (at < 0) at = toolbarLay_->count();
    toolbarLay_->insertWidget(at, w);
}

void QueryEditorView::setAdapter(IDatabaseAdapter* adapter) {
    adapter_ = adapter;
    runBtn_->setEnabled(adapter_ != nullptr);
    if (!adapter_) {
        resultModel_->clear();
        statusLbl_->setText(QString{});
        if (provider_) provider_->updateSchema({});
        return;
    }
    reloadSchema();
}

void QueryEditorView::reloadSchema() {
    if (!adapter_ || !provider_) return;

    // Fetch listTables + describeTable on a worker thread so the UI doesn't
    // stall on large schemas. Result is applied back on the GUI thread.
    IDatabaseAdapter* adapter = adapter_;
    QPointer<QueryEditorView> self(this);
    (void)QtConcurrent::run([self, adapter]() {
        std::vector<TableDescription> tables;
        try {
            const auto infos = adapter->listTables(std::nullopt);
            for (const auto& info : infos) {
                if (info.type != TableKind::Table) continue;
                try {
                    tables.push_back(adapter->describeTable(info.name, std::nullopt));
                } catch (...) { /* skip broken tables */ }
            }
        } catch (...) {
            // Best-effort. Empty schema is valid (just no suggestions).
        }
        QMetaObject::invokeMethod(qApp, [self, tables]() {
            if (self && self->provider_) self->provider_->updateSchema(tables);
        }, Qt::QueuedConnection);
    });
}

void QueryEditorView::hidePopup() {
    if (debounce_) debounce_->stop();  // cancel pending re-show
    if (popup_) popup_->hide();
}

void QueryEditorView::triggerCompletionNow() {
    // Force a fresh context computation regardless of prefix length.
    if (!provider_) return;
    const auto text = editor_->toPlainText();
    const int offset = editor_->textCursor().position();
    auto ctx = parser_->parse(text, offset);
    auto items = provider_->suggestions(ctx);
    if (items.empty()) { hidePopup(); return; }
    popup_->setItems(items);

    // Position popup just below the cursor.
    const auto rect = editor_->cursorRect();
    const QPoint gpos = editor_->viewport()->mapToGlobal(
        QPoint(rect.left(), rect.bottom() + 2));
    popup_->move(gpos);
    popup_->show();
    // X11/Wayland may pull focus to the newly-shown tool window despite
    // WA_ShowWithoutActivating — force focus back so keystrokes keep going
    // to the editor for inline filtering of the suggestion list.
    editor_->setFocus(Qt::OtherFocusReason);
}

void QueryEditorView::maybeShowCompletions() {
    if (!provider_) return;

    // Respect 2-char auto-trigger rule: only auto-popup when prefix has >=2
    // chars. Ctrl+Space still forces via triggerCompletionNow().
    const auto text = editor_->toPlainText();
    const int offset = editor_->textCursor().position();
    const auto ctx = parser_->parse(text, offset);
    if (ctx.prefix.size() < 2 && ctx.trigger.kind != CompletionTriggerKind::None) {
        hidePopup();
        return;
    }
    auto items = provider_->suggestions(ctx);
    if (items.empty()) { hidePopup(); return; }
    popup_->setItems(items);

    const auto rect = editor_->cursorRect();
    const QPoint gpos = editor_->viewport()->mapToGlobal(
        QPoint(rect.left(), rect.bottom() + 2));
    popup_->move(gpos);
    if (!popup_->isVisible()) {
        popup_->show();
        editor_->setFocus(Qt::OtherFocusReason);
    }
}

void QueryEditorView::acceptCompletion(const CompletionItem& item) {
    if (!editor_) return;
    // Replace current word (the in-progress prefix) with item.insertText.
    auto cursor = editor_->textCursor();
    // Walk back while previous char is [A-Za-z0-9_.]
    const QString doc = editor_->toPlainText();
    int pos = cursor.position();
    int start = pos;
    while (start > 0) {
        const QChar ch = doc.at(start - 1);
        if (ch.isLetterOrNumber() || ch == '_' || ch == '.') --start; else break;
    }
    cursor.setPosition(start, QTextCursor::MoveAnchor);
    cursor.setPosition(pos,   QTextCursor::KeepAnchor);
    cursor.insertText(item.insertText);

    if (provider_) provider_->trackUsed(item.text);
    hidePopup();
}

void QueryEditorView::onRunClicked() {
    if (!adapter_) return;
    const auto sql = editor_->toPlainText().trimmed().toStdString();
    if (sql.empty()) return;

    statusLbl_->setText(tr("Running…"));

    QElapsedTimer timer;
    timer.start();
    try {
        auto result = adapter_->executeRaw(sql);
        const int n   = static_cast<int>(result.rows.size());
        const int cols = static_cast<int>(result.columns.size());
        const int ms  = static_cast<int>(timer.elapsed());
        resultModel_->setResult(std::move(result));
        resultView_->resizeColumnsToContents();

        // Stretch last column to fill viewport when total column width < view width.
        if (auto* h = resultView_->horizontalHeader()) {
            const int cc = resultModel_->columnCount();
            if (cc > 0) {
                int used = 0;
                for (int i = 0; i < cc - 1; ++i) used += h->sectionSize(i);
                const int avail = resultView_->viewport()->width() - used;
                if (avail > h->sectionSize(cc - 1))
                    h->resizeSection(cc - 1, avail);
            }
        }

        statusLbl_->setProperty("gxText", QStringLiteral("success"));
        statusLbl_->style()->unpolish(statusLbl_);
        statusLbl_->style()->polish(statusLbl_);
        statusLbl_->setText(tr("%1 rows × %2 cols · %3 ms").arg(n).arg(cols).arg(ms));
        emit queryExecuted(QString::fromStdString(sql), n, ms);
    } catch (const GridexError& e) {
        resultModel_->clear();
        statusLbl_->setProperty("gxText", QStringLiteral("danger"));
        statusLbl_->style()->unpolish(statusLbl_);
        statusLbl_->style()->polish(statusLbl_);
        statusLbl_->setText(QString::fromUtf8(e.what()));
    }
}

bool QueryEditorView::eventFilter(QObject* obj, QEvent* event) {
    // Reposition export overlay when the result view resizes.
    if (obj == resultView_ && event->type() == QEvent::Resize
        && exportResultBtn_ && exportResultBtn_->isVisible()) {
        const int x = resultView_->width()  - exportResultBtn_->width()  - 14;
        const int y = resultView_->height() - exportResultBtn_->height() - 14;
        exportResultBtn_->move(x, y);
    }

    if (obj == editor_ && event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);

        // Ctrl+Enter -> Run (takes priority over popup).
        if ((ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter)
            && (ke->modifiers() & Qt::ControlModifier)) {
            onRunClicked();
            return true;
        }

        // Ctrl+Space -> force-open completion popup.
        if (ke->key() == Qt::Key_Space && (ke->modifiers() & Qt::ControlModifier)) {
            triggerCompletionNow();
            return true;
        }

        // While popup is visible, intercept navigation/accept/dismiss keys.
        if (popup_ && popup_->isVisible()) {
            switch (ke->key()) {
                case Qt::Key_Up:     popup_->moveSelection(-1); return true;
                case Qt::Key_Down:   popup_->moveSelection(+1); return true;
                case Qt::Key_Escape: hidePopup(); return true;
                case Qt::Key_Return:
                case Qt::Key_Enter:
                case Qt::Key_Tab:
                    if (auto* it = popup_->selectedItem()) {
                        acceptCompletion(*it);
                    }
                    return true;
                default:
                    break;
            }
        }
    }
    return QWidget::eventFilter(obj, event);
}

// --------------------------------------------------------------------
// Result export
// --------------------------------------------------------------------

namespace {

// CSV cell: quote if value contains separator, quote char, CR/LF.
// Quote char inside cell is doubled. Null -> empty cell (standard CSV).
QString csvCell(const RowValue& v) {
    if (v.isNull()) return QString{};
    const QString s = QString::fromStdString(v.displayString());
    if (s.contains(',') || s.contains('"') || s.contains('\n') || s.contains('\r')) {
        QString escaped = s;
        escaped.replace('"', QStringLiteral("\"\""));
        return '"' + escaped + '"';
    }
    return s;
}

// SQL literal: NULL, numbers unquoted, strings with single-quote escape.
// Conservative: treat everything except NULL as string-literal. DB engines
// will coerce when inserting.
QString sqlLiteral(const RowValue& v) {
    if (v.isNull()) return QStringLiteral("NULL");
    const QString s = QString::fromStdString(v.displayString());
    // Numbers/booleans pass through unquoted.
    bool ok = false;
    s.toDouble(&ok);
    if (ok) return s;
    if (s == QLatin1String("true") || s == QLatin1String("false")) return s;
    QString escaped = s;
    escaped.replace('\'', QStringLiteral("''"));
    return '\'' + escaped + '\'';
}

// Default filename from the first column's table name if present, else
// "query-result".
QString defaultExportStem(const QueryResult& r) {
    for (const auto& c : r.columns) {
        if (c.tableName && !c.tableName->empty()) {
            return QString::fromStdString(*c.tableName);
        }
    }
    return QStringLiteral("query-result");
}

}  // namespace

void QueryEditorView::exportResultAsCsv() {
    const auto& r = resultModel_->result();
    if (r.rows.empty()) return;

    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString defaultPath = dir + "/" + defaultExportStem(r) + ".csv";
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export Result as CSV"), defaultPath, tr("CSV (*.csv)"));
    if (path.isEmpty()) return;

    const QString outPath = path.endsWith(".csv", Qt::CaseInsensitive) ? path : path + ".csv";
    QFile f(outPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Export CSV"),
                             tr("Cannot open file for writing:\n%1").arg(outPath));
        return;
    }
    QTextStream out(&f);

    // Header
    QStringList header;
    for (const auto& c : r.columns) header << QString::fromStdString(c.name);
    out << header.join(',') << '\n';

    // Rows
    for (const auto& row : r.rows) {
        QStringList cells;
        cells.reserve(static_cast<int>(row.size()));
        for (const auto& v : row) cells << csvCell(v);
        out << cells.join(',') << '\n';
    }
}

void QueryEditorView::exportResultAsSql() {
    const auto& r = resultModel_->result();
    if (r.rows.empty()) return;

    const QString tableName = defaultExportStem(r);
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString defaultPath = dir + "/" + tableName + ".sql";
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export Result as SQL"), defaultPath, tr("SQL (*.sql)"));
    if (path.isEmpty()) return;

    const QString outPath = path.endsWith(".sql", Qt::CaseInsensitive) ? path : path + ".sql";
    QFile f(outPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Export SQL"),
                             tr("Cannot open file for writing:\n%1").arg(outPath));
        return;
    }
    QTextStream out(&f);

    // Column list: "col1", "col2", ...
    QStringList colList;
    for (const auto& c : r.columns) colList << '"' + QString::fromStdString(c.name) + '"';
    const QString colsJoined = colList.join(", ");
    const QString header = QString("INSERT INTO \"%1\" (%2) VALUES").arg(tableName, colsJoined);

    for (const auto& row : r.rows) {
        QStringList vals;
        vals.reserve(static_cast<int>(row.size()));
        for (const auto& v : row) vals << sqlLiteral(v);
        out << header << " (" << vals.join(", ") << ");\n";
    }
}

void QueryEditorView::exportResultAsJson() {
    const auto& r = resultModel_->result();
    if (r.rows.empty()) return;

    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString defaultPath = dir + "/" + defaultExportStem(r) + ".json";
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export Result as JSON"), defaultPath, tr("JSON (*.json)"));
    if (path.isEmpty()) return;

    const QString outPath = path.endsWith(".json", Qt::CaseInsensitive) ? path : path + ".json";
    QFile f(outPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Export JSON"),
                             tr("Cannot open file for writing:\n%1").arg(outPath));
        return;
    }

    QJsonArray rowsArr;
    for (const auto& row : r.rows) {
        QJsonObject obj;
        for (std::size_t c = 0; c < r.columns.size() && c < row.size(); ++c) {
            const QString key = QString::fromStdString(r.columns[c].name);
            const auto& v = row[c];
            if (v.isNull()) obj.insert(key, QJsonValue::Null);
            else obj.insert(key, QString::fromStdString(v.displayString()));
        }
        rowsArr.append(obj);
    }
    QJsonDocument doc(rowsArr);
    f.write(doc.toJson(QJsonDocument::Indented));
}

}
