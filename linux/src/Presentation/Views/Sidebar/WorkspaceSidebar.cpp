#include "Presentation/Views/Sidebar/WorkspaceSidebar.h"

#include <chrono>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QStackedWidget>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStyledItemDelegate>
#include <QTextStream>
#include <QToolButton>
#include <QTreeView>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include "Core/Errors/GridexError.h"
#include "Core/Utils/SqlStatementSplitter.h"
#include "Presentation/Views/Chrome/GxIcons.h"
#include "Presentation/Views/ImportSQL/SqlImportWizard.h"
#include "Core/Enums/DatabaseType.h"
#include "Core/Enums/SQLDialect.h"
#include "Core/Models/Schema/SchemaSnapshot.h"
#include "Data/Keychain/SecretStore.h"
#include "Data/Persistence/AppDatabase.h"
#include "Data/Persistence/SavedQueryRepository.h"
#include "Presentation/ViewModels/WorkspaceState.h"
#include "Presentation/Views/Backup/BackupProgressDialog.h"
#include "Presentation/Views/TableList/TableGridView.h"
#include "Presentation/Theme/ThemeManager.h"
#include "Services/Export/DatabaseDumpRunner.h"
#include "Services/Export/ExportService.h"

namespace gridex {

namespace {

constexpr int kRoleKind        = Qt::UserRole + 1;
constexpr int kRoleSchemaName  = Qt::UserRole + 2;
constexpr int kRoleTableName   = Qt::UserRole + 3;
constexpr int kRoleLoaded      = Qt::UserRole + 4;
constexpr int kRoleRoutineName = Qt::UserRole + 5;
constexpr int kRoleRowCount    = Qt::UserRole + 6;   // optional int — trail badge
constexpr int kRoleFolderLabel = Qt::UserRole + 7;   // bool — caps section header

enum NodeKind {
    NodeSchema      = 1,
    NodeTable       = 2,
    NodePlaceholder = 3,
    NodeFunctionsGroup  = 4,
    NodeProceduresGroup = 5,
    NodeFunction    = 6,
    NodeProcedure   = 7,
    NodeFolder      = 8,   // "Tables (N)" / "Views (N)" / "Functions (N)" caption row
};

// Per-theme gx tokens used by the manually-painted SchemaTreeDelegate.
// Looked up at paint time so a Light/Dark switch via Settings repaints
// against the correct palette without needing a full widget rebuild.
struct DelegatePalette {
    QColor bg1;          // row default background
    QColor bg2;          // row hover background
    QColor bg4;          // row selected background
    QColor text;         // selected / strong text
    QColor text2;        // normal text
    QColor faint;        // folder caption + disabled
    QColor mutedIcon;    // glyph tint
};

DelegatePalette delegatePalette() {
    if (ThemeManager::instance().isDark()) {
        return {QColor(0x11, 0x15, 0x1a),  // bg-1
                QColor(0x17, 0x1c, 0x22),  // bg-2
                QColor(0x2b, 0x31, 0x38),  // bg-4
                QColor(0xe5, 0xe8, 0xeb),  // text
                QColor(0xb4, 0xb8, 0xbc),  // text-2
                QColor(0x55, 0x58, 0x5c),  // faint
                QColor(0x7d, 0x81, 0x85)}; // muted
    }
    return {QColor(0xf4, 0xf5, 0xf7),  // bg-1
            QColor(0xe9, 0xeb, 0xef),  // bg-2
            QColor(0xcd, 0xd1, 0xd8),  // bg-4
            QColor(0x1c, 0x20, 0x25),  // text
            QColor(0x4a, 0x4f, 0x56),  // text-2
            QColor(0x93, 0x98, 0xa0),  // faint
            QColor(0x6a, 0x6e, 0x74)}; // muted
}

// CSV parser (RFC 4180) — used by importCsv below. Carried over verbatim
// from the pre-A2 implementation.
struct CsvParser {
    QString text;
    int pos = 0;

    bool atEnd() const { return pos >= text.size(); }

    QStringList readRow() {
        QStringList fields;
        if (atEnd()) return fields;

        QString cell;
        bool quoted = false;
        while (pos < text.size()) {
            const QChar c = text.at(pos);
            if (quoted) {
                if (c == '"') {
                    if (pos + 1 < text.size() && text.at(pos + 1) == '"') {
                        cell.append('"');
                        pos += 2;
                        continue;
                    }
                    quoted = false;
                    ++pos;
                    continue;
                }
                cell.append(c);
                ++pos;
                continue;
            }
            if (c == '"' && cell.isEmpty()) { quoted = true; ++pos; continue; }
            if (c == ',') { fields << cell; cell.clear(); ++pos; continue; }
            if (c == '\r') { ++pos; continue; }
            if (c == '\n') { ++pos; fields << cell; return fields; }
            cell.append(c);
            ++pos;
        }
        fields << cell;
        return fields;
    }
};

// Map DatabaseType → (engine pill label, pill background colour). Hex
// values precomputed from .gx-eng-* oklch in gridex.css.
struct EnginePill { QString label; QColor color; };
EnginePill enginePill(DatabaseType t) {
    switch (t) {
        case DatabaseType::PostgreSQL: return {QStringLiteral("PG"), QColor(0x3a, 0xa3, 0xd8)};
        case DatabaseType::MySQL:      return {QStringLiteral("MY"), QColor(0xd6, 0xb0, 0x4c)};
        case DatabaseType::SQLite:     return {QStringLiteral("SL"), QColor(0x82, 0xc9, 0x6a)};
        case DatabaseType::ClickHouse: return {QStringLiteral("CH"), QColor(0xe3, 0xc0, 0x4a)};
        case DatabaseType::MongoDB:    return {QStringLiteral("MG"), QColor(0x4f, 0xc4, 0x88)};
        case DatabaseType::Redis:      return {QStringLiteral("RD"), QColor(0xd6, 0x5b, 0x4c)};
        case DatabaseType::MSSQL:      return {QStringLiteral("MS"), QColor(0xa8, 0x8c, 0xe0)};
    }
    return {QStringLiteral("DB"), QColor(0x7d, 0x81, 0x85)};
}

QString fmtBigInt(qint64 n) {
    if (n >= 1'000'000'000LL) return QString::number(n / 1.0e9, 'f', 1) + QLatin1Char('B');
    if (n >= 1'000'000LL)     return QString::number(n / 1.0e6, 'f', 1) + QLatin1Char('M');
    if (n >= 1'000LL)         return QString::number(n / 1.0e3, 'f', 1) + QLatin1Char('K');
    return QString::number(n);
}

// Compact, square 24-px icon button used in the header strip and tab strip.
// Hover/checked colors come from the theme QSS via the `side-square` role
// selector so the same widget reads correctly in light and dark.
QToolButton* makeSquareBtn(const QString& glyph, const QString& tooltip,
                            QWidget* parent, int size = 24, int iconPx = 12) {
    auto* b = new QToolButton(parent);
    b->setIcon(GxIcons::glyph(glyph));
    b->setIconSize(QSize(iconPx, iconPx));
    b->setFixedSize(size, size);
    b->setAutoRaise(true);
    b->setToolTip(tooltip);
    b->setCursor(Qt::PointingHandCursor);
    b->setProperty("gxRole", QStringLiteral("side-square"));
    return b;
}

// Custom delegate that paints engine pills on connection-root rows, dim
// caps headers on folder rows ("Tables (N)"), and trailing row-count
// badges on table rows.
class SchemaTreeDelegate : public QStyledItemDelegate {
public:
    explicit SchemaTreeDelegate(QObject* parent = nullptr)
        : QStyledItemDelegate(parent) {}

    QSize sizeHint(const QStyleOptionViewItem& opt, const QModelIndex& idx) const override {
        QSize s = QStyledItemDelegate::sizeHint(opt, idx);
        const int kind = idx.data(kRoleKind).toInt();
        if (kind == NodeFolder) s.setHeight(20);
        else                    s.setHeight(std::max(s.height(), 24));
        return s;
    }

    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const override {
        QStyleOptionViewItem o = opt;
        initStyleOption(&o, idx);

        const bool isFolder = idx.data(kRoleKind).toInt() == NodeFolder
                              || idx.data(kRoleFolderLabel).toBool();
        const bool selected = (o.state & QStyle::State_Selected);
        const DelegatePalette pal = delegatePalette();

        p->save();
        p->setRenderHint(QPainter::Antialiasing, true);

        // Background — hover/selected. Folder rows never highlight.
        if (selected && !isFolder) {
            p->fillRect(o.rect, pal.bg4);
        } else if ((o.state & QStyle::State_MouseOver) && !isFolder) {
            p->fillRect(o.rect, pal.bg2);
        } else {
            p->fillRect(o.rect, pal.bg1);
        }

        // Folder rows: small all-caps muted label, no icon.
        if (isFolder) {
            QFont f = o.font;
            f.setPointSizeF(std::max(8.0, f.pointSizeF() - 2.0));
            f.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
            f.setCapitalization(QFont::AllUppercase);
            p->setFont(f);
            p->setPen(pal.faint);
            QRect r = o.rect.adjusted(o.rect.x() == 0 ? 16 : 4, 0, -8, 0);
            p->drawText(r, Qt::AlignVCenter | Qt::AlignLeft, idx.data(Qt::DisplayRole).toString());
            p->restore();
            return;
        }

        // Default branch indicator + indentation already handled by view —
        // we delegate "indented" painting to the standard pipeline by
        // calling the base for non-decorated rows when no engine pill is
        // needed. But to control text + trail consistently we paint
        // ourselves and let the view supply rect via o.rect (which already
        // honours indentation).
        const int kind = idx.data(kRoleKind).toInt();
        const QString text = idx.data(Qt::DisplayRole).toString();

        int x = o.rect.x() + 4;
        const int y = o.rect.y();
        const int h = o.rect.height();

        // Engine pill on schema-root rows (we treat NodeSchema as the
        // visible "connection" anchor since this sidebar only sees one
        // open connection — the activity bar's Connections panel renders
        // the multi-connection list).
        if (kind == NodeSchema) {
            // No pill here — the schema item is one level below the
            // (implicit) connection. Pill is painted by the bottom-bar
            // db-info strip instead. Keep the row clean.
        }

        // Icon glyph by node kind. Tree's own branch/expander is left
        // intact (Qt::style draws it before paint() is called).
        QString iconName;
        switch (kind) {
            case NodeSchema:            iconName = QStringLiteral("schema"); break;
            case NodeTable:             iconName = QStringLiteral("table");  break;
            case NodeFunctionsGroup:    iconName = QStringLiteral("folder"); break;
            case NodeProceduresGroup:   iconName = QStringLiteral("folder"); break;
            case NodeFunction:          iconName = QStringLiteral("fn");     break;
            case NodeProcedure:         iconName = QStringLiteral("fn");     break;
            default:                    iconName.clear();                    break;
        }
        if (!iconName.isEmpty()) {
            const QIcon ic = GxIcons::glyph(iconName, pal.mutedIcon.name());
            ic.paint(p, x, y + (h - 14) / 2, 14, 14);
            x += 18;
        }

        // Label.
        p->setPen(selected ? pal.text
                            : (idx.flags() & Qt::ItemIsEnabled ? pal.text2
                                                                : pal.faint));
        QFont f = o.font;
        if (kind == NodeSchema) f.setBold(true);
        p->setFont(f);

        // Trailing badge — table row count.
        QString trail;
        const QVariant rc = idx.data(kRoleRowCount);
        if (rc.isValid() && rc.toLongLong() >= 0) trail = fmtBigInt(rc.toLongLong());

        QFontMetrics fm(p->font());
        int trailW = 0;
        if (!trail.isEmpty()) {
            QFont mono(QStringLiteral("JetBrains Mono"));
            mono.setStyleHint(QFont::Monospace);
            mono.setPointSizeF(std::max(8.5, f.pointSizeF() - 1.5));
            QFontMetrics mfm(mono);
            trailW = mfm.horizontalAdvance(trail) + 8;
        }

        const QRect labelRect(x, y, std::max(0, o.rect.right() - x - trailW - 4), h);
        p->drawText(labelRect, Qt::AlignVCenter | Qt::AlignLeft,
                    fm.elidedText(text, Qt::ElideRight, labelRect.width()));

        if (!trail.isEmpty()) {
            QFont mono(QStringLiteral("JetBrains Mono"));
            mono.setStyleHint(QFont::Monospace);
            mono.setPointSizeF(std::max(8.5, f.pointSizeF() - 1.5));
            p->setFont(mono);
            p->setPen(pal.faint);
            p->drawText(QRect(o.rect.right() - trailW - 4, y, trailW, h),
                        Qt::AlignVCenter | Qt::AlignRight, trail);
        }

        p->restore();
    }
};

}  // namespace

WorkspaceSidebar::~WorkspaceSidebar() = default;

WorkspaceSidebar::WorkspaceSidebar(WorkspaceState* state,
                                   std::shared_ptr<AppDatabase> appDb,
                                   QWidget* parent)
    : QWidget(parent), state_(state), appDb_(std::move(appDb)) {
    if (appDb_) savedQueryRepo_ = std::make_unique<SavedQueryRepository>(appDb_);

    setObjectName(QStringLiteral("gxWorkspaceSidebar"));
    setFixedWidth(280);
    setAttribute(Qt::WA_StyledBackground, true);
    // Background + right border live in the theme QSS — QWidget#gxWorkspaceSidebar.

    buildUi();
    if (appDb_) loadHistoryFromDb();
    if (savedQueryRepo_) reloadSavedQueriesTree();
    if (state_) {
        connect(state_, &WorkspaceState::connectionOpened,
                this, &WorkspaceSidebar::onConnectionOpened);
        connect(state_, &WorkspaceState::connectionClosed,
                this, &WorkspaceSidebar::onConnectionClosed);
        if (state_->isOpen()) onConnectionOpened();
    }
}

// --------------------------------------------------------------------
// UI construction
// --------------------------------------------------------------------

QWidget* WorkspaceSidebar::buildHeaderStrip() {
    // .gx-side-hd — 28px, padding 6 10 6 10, border-bottom #2e3339, bg-1.
    auto* strip = new QWidget(this);
    strip->setObjectName(QStringLiteral("gxSideHd"));
    strip->setFixedHeight(28);
    strip->setAttribute(Qt::WA_StyledBackground, true);
    auto* h = new QHBoxLayout(strip);
    h->setContentsMargins(10, 6, 10, 6);
    h->setSpacing(2);

    auto* title = new QLabel(tr("SCHEMA"), strip);
    title->setObjectName(QStringLiteral("gxSideHdTitle"));
    QFont tf = title->font();
    tf.setPointSizeF(10.0);
    tf.setLetterSpacing(QFont::PercentageSpacing, 108);
    tf.setBold(true);
    title->setFont(tf);
    h->addWidget(title);
    h->addStretch();

    hdNewConnBtn_  = makeSquareBtn(QStringLiteral("plug"),    tr("New connection"), strip, 18, 11);
    hdRefreshBtn_  = makeSquareBtn(QStringLiteral("refresh"), tr("Refresh"),        strip, 18, 11);
    hdCollapseBtn_ = makeSquareBtn(QStringLiteral("x"),       tr("Collapse all"),   strip, 18, 11);
    connect(hdRefreshBtn_,  &QToolButton::clicked, this, [this] { loadSchemas(); });
    connect(hdCollapseBtn_, &QToolButton::clicked, this, [this] { if (tree_) tree_->collapseAll(); });
    h->addWidget(hdNewConnBtn_);
    h->addWidget(hdRefreshBtn_);
    h->addWidget(hdCollapseBtn_);
    return strip;
}

QWidget* WorkspaceSidebar::buildFilterRow(QWidget* parent) {
    // .gx-side-filter — height 24, margin 6 8, padding 0 8, bg-0 border-2.
    auto* row = new QWidget(parent);
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(8, 6, 8, 6);
    h->setSpacing(4);

    searchEdit_ = new QLineEdit(row);
    searchEdit_->setObjectName(QStringLiteral("gxSideFilter"));
    searchEdit_->setPlaceholderText(tr("Filter…"));
    searchEdit_->setClearButtonEnabled(false);
    searchEdit_->addAction(GxIcons::glyph(QStringLiteral("search")),
                           QLineEdit::LeadingPosition);
    connect(searchEdit_, &QLineEdit::textChanged,
            this, &WorkspaceSidebar::onSearchChanged);
    h->addWidget(searchEdit_);

    gridToggleBtn_ = makeSquareBtn(QStringLiteral("folder"), tr("Toggle Grid View"), row, 24, 12);
    gridToggleBtn_->setCheckable(true);
    connect(gridToggleBtn_, &QToolButton::toggled, this, [this](bool checked) {
        itemsViewStack_->setCurrentIndex(checked ? 1 : 0);
        searchEdit_->setPlaceholderText(checked ? tr("Filter tables…") : tr("Filter…"));
        if (checked && tableGrid_) tableGrid_->reload();
    });
    h->addWidget(gridToggleBtn_);
    return row;
}

QWidget* WorkspaceSidebar::buildBottomBar(QWidget* parent) {
    auto* bar = new QWidget(parent);
    bar->setObjectName(QStringLiteral("gxSideBottom"));
    bar->setAttribute(Qt::WA_StyledBackground, true);
    auto* v = new QVBoxLayout(bar);
    v->setContentsMargins(8, 6, 8, 6);
    v->setSpacing(4);

    auto* schemaRow = new QWidget(bar);
    auto* sh = new QHBoxLayout(schemaRow);
    sh->setContentsMargins(0, 0, 0, 0);
    sh->setSpacing(6);

    schemaCombo_ = new QComboBox(schemaRow);
    schemaCombo_->setObjectName(QStringLiteral("gxSideSchemaCombo"));
    schemaCombo_->setToolTip(tr("Active schema"));
    schemaCombo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    connect(schemaCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &WorkspaceSidebar::onSchemaChanged);
    sh->addWidget(schemaCombo_);
    sh->addStretch();
    v->addWidget(schemaRow);

    dbInfoLabel_ = new QLabel(bar);
    dbInfoLabel_->setObjectName(QStringLiteral("gxSideDbInfo"));
    dbInfoLabel_->setWordWrap(false);
    v->addWidget(dbInfoLabel_);

    auto* actionRow = new QWidget(bar);
    auto* ah = new QHBoxLayout(actionRow);
    ah->setContentsMargins(0, 0, 0, 0);
    ah->setSpacing(6);

    newTableBtn_ = new QPushButton(tr("+ New Table"), actionRow);
    newTableBtn_->setObjectName(QStringLiteral("gxSideNewTable"));
    newTableBtn_->setCursor(Qt::PointingHandCursor);
    connect(newTableBtn_, &QPushButton::clicked, this, [this] {
        const QString schema = schemaCombo_ ? schemaCombo_->currentText() : QString{};
        emit newTableRequested(schema);
    });

    disconnectBtn_ = new QPushButton(tr("Disconnect"), actionRow);
    disconnectBtn_->setObjectName(QStringLiteral("gxSideDisconnect"));
    disconnectBtn_->setCursor(Qt::PointingHandCursor);
    connect(disconnectBtn_, &QPushButton::clicked,
            this, &WorkspaceSidebar::disconnectRequested);

    ah->addWidget(newTableBtn_);
    ah->addStretch();
    ah->addWidget(disconnectBtn_);
    v->addWidget(actionRow);

    return bar;
}

QWidget* WorkspaceSidebar::buildItemsPage() {
    auto* page = new QWidget(this);
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    v->addWidget(buildFilterRow(page));

    itemsViewStack_ = new QStackedWidget(page);

    tree_ = new QTreeView(itemsViewStack_);
    tree_->setObjectName(QStringLiteral("gxSchemaTree"));
    tree_->setHeaderHidden(true);
    tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tree_->setFrameShape(QFrame::NoFrame);
    tree_->setIndentation(14);
    tree_->setUniformRowHeights(false);
    tree_->setRootIsDecorated(true);
    tree_->setAnimated(false);
    tree_->setMouseTracking(true);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    tree_->setItemDelegate(new SchemaTreeDelegate(tree_));

    model_ = new QStandardItemModel(this);
    tree_->setModel(model_);

    connect(tree_, &QTreeView::expanded,
            this, &WorkspaceSidebar::onItemExpanded);
    connect(tree_, &QTreeView::doubleClicked,
            this, &WorkspaceSidebar::onItemDoubleClicked);
    connect(tree_, &QTreeView::customContextMenuRequested,
            this, &WorkspaceSidebar::onContextMenuRequested);

    itemsViewStack_->addWidget(tree_);

    tableGrid_ = new TableGridView(state_, itemsViewStack_);
    connect(tableGrid_, &TableGridView::tableSelected,
            this, &WorkspaceSidebar::tableSelected);
    connect(tableGrid_, &TableGridView::tableDeleted,
            this, &WorkspaceSidebar::tableDeleted);
    itemsViewStack_->addWidget(tableGrid_);

    itemsViewStack_->setCurrentIndex(0);
    v->addWidget(itemsViewStack_, 1);

    v->addWidget(buildBottomBar(page));
    return page;
}

void WorkspaceSidebar::buildUi() {
    // History/Saved widgets are kept alive (but layout-less) so logQuery /
    // promptSaveQuery don't crash. They'll be re-surfaced via the
    // SidebarPanelStack History / Snippets panels in a follow-up PR.
    historyList_ = new QListWidget(this);
    historyList_->setVisible(false);
    savedTree_ = new QTreeWidget(this);
    savedTree_->setHeaderHidden(true);
    savedTree_->setVisible(false);

    // PR A2 fix: the inner 4-tab strip (Items / Queries / History / Saved)
    // was a duplicate of the activity bar's job. SidebarPanelStack now hosts
    // the History/Snippets/ERD panels at the activity-bar level — this widget
    // only ever shows the Schema (Items) page.
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    root->addWidget(buildHeaderStrip());
    root->addWidget(buildItemsPage(), 1);
}

// --------------------------------------------------------------------
// Public surface (preserved verbatim from pre-A2 API)
// --------------------------------------------------------------------

void WorkspaceSidebar::refreshTree() {
    onConnectionOpened();
}

void WorkspaceSidebar::onConnectionOpened() {
    loadSchemas();

    if (schemaCombo_ && state_ && state_->adapter()) {
        QSignalBlocker blocker(schemaCombo_);
        schemaCombo_->clear();
        try {
            const auto schemas = state_->adapter()->listSchemas(std::nullopt);
            for (const auto& s : schemas) {
                schemaCombo_->addItem(QString::fromUtf8(s.c_str()));
            }
        } catch (const GridexError&) {}
        const int pubIdx = schemaCombo_->findText(QStringLiteral("public"));
        schemaCombo_->setCurrentIndex(pubIdx >= 0 ? pubIdx : 0);
    }

    if (dbInfoLabel_ && state_) {
        const auto& cfg = state_->config();
        const auto pill = enginePill(cfg.databaseType);
        const QString host = QString::fromUtf8(cfg.displayHost().c_str());
        const QString name = QString::fromStdString(cfg.database.value_or(cfg.name));
        dbInfoLabel_->setText(QStringLiteral("%1 · %2 · %3")
                                  .arg(pill.label,
                                       name.isEmpty() ? QStringLiteral("—") : name,
                                       host));
    }
}

void WorkspaceSidebar::onConnectionClosed() {
    model_->clear();
    if (schemaCombo_) schemaCombo_->clear();
    if (dbInfoLabel_) dbInfoLabel_->clear();
}

void WorkspaceSidebar::loadSchemas() {
    model_->clear();
    if (!state_ || !state_->adapter()) return;

    try {
        const auto schemas = state_->adapter()->listSchemas(std::nullopt);
        for (const auto& s : schemas) {
            auto* schemaItem = new QStandardItem(QString::fromUtf8(s.c_str()));
            schemaItem->setData(NodeSchema, kRoleKind);
            schemaItem->setData(QString::fromUtf8(s.c_str()), kRoleSchemaName);
            schemaItem->setData(false, kRoleLoaded);

            auto* placeholder = new QStandardItem(tr("(loading…)"));
            placeholder->setData(NodePlaceholder, kRoleKind);
            placeholder->setEnabled(false);
            schemaItem->appendRow(placeholder);

            model_->appendRow(schemaItem);
        }
        if (schemas.size() == 1 && model_->rowCount() > 0) {
            tree_->expand(model_->index(0, 0));
        }
    } catch (const GridexError&) {
        auto* err = new QStandardItem(tr("(schema listing failed)"));
        err->setEnabled(false);
        model_->appendRow(err);
    }
}

QStandardItem* WorkspaceSidebar::appendFolderRow(QStandardItem* parent,
                                                  const QString& label, int count) {
    const QString text = (count >= 0)
        ? QStringLiteral("%1 (%2)").arg(label).arg(count)
        : label;
    auto* item = new QStandardItem(text);
    item->setData(NodeFolder, kRoleKind);
    item->setData(true, kRoleFolderLabel);
    item->setSelectable(false);
    item->setEnabled(false);
    parent->appendRow(item);
    return item;
}

void WorkspaceSidebar::onItemExpanded(const QModelIndex& index) {
    auto* item = model_->itemFromIndex(index);
    if (!item) return;
    if (item->data(kRoleLoaded).toBool()) return;

    const int kind = item->data(kRoleKind).toInt();
    const QString schema = item->data(kRoleSchemaName).toString();

    if (kind == NodeSchema) {
        item->removeRows(0, item->rowCount());
        loadTablesForSchema(item, schema);
        item->setData(true, kRoleLoaded);
    } else if (kind == NodeFunctionsGroup) {
        item->removeRows(0, item->rowCount());
        loadFunctionsForSchema(item, schema);
        item->setData(true, kRoleLoaded);
    } else if (kind == NodeProceduresGroup) {
        item->removeRows(0, item->rowCount());
        loadProceduresForSchema(item, schema);
        item->setData(true, kRoleLoaded);
    }
}

void WorkspaceSidebar::loadTablesForSchema(QStandardItem* schemaItem, const QString& schemaName) {
    if (!state_ || !state_->adapter()) return;

    std::vector<TableInfo> tables;
    try {
        tables = state_->adapter()->listTables(schemaName.toStdString());
    } catch (const GridexError& e) {
        auto* err = new QStandardItem(QString::fromUtf8(e.what()));
        err->setEnabled(false);
        schemaItem->appendRow(err);
        return;
    }

    // Tables (N) folder header + table rows
    if (!tables.empty()) {
        appendFolderRow(schemaItem, tr("Tables"), static_cast<int>(tables.size()));
        for (const auto& t : tables) {
            auto* tableItem = new QStandardItem(QString::fromUtf8(t.name.c_str()));
            tableItem->setData(NodeTable, kRoleKind);
            tableItem->setData(schemaName, kRoleSchemaName);
            tableItem->setData(QString::fromUtf8(t.name.c_str()), kRoleTableName);
            if (t.estimatedRowCount.has_value()) {
                tableItem->setData(static_cast<qlonglong>(*t.estimatedRowCount), kRoleRowCount);
            }
            schemaItem->appendRow(tableItem);
        }
    } else {
        appendFolderRow(schemaItem, tr("Tables"), 0);
    }

    // Functions group node — lazy-loaded on expand
    auto* fnGroup = new QStandardItem(tr("Functions"));
    fnGroup->setData(NodeFunctionsGroup, kRoleKind);
    fnGroup->setData(schemaName, kRoleSchemaName);
    fnGroup->setData(false, kRoleLoaded);
    auto* fnPlaceholder = new QStandardItem(tr("(loading…)"));
    fnPlaceholder->setData(NodePlaceholder, kRoleKind);
    fnPlaceholder->setEnabled(false);
    fnGroup->appendRow(fnPlaceholder);
    schemaItem->appendRow(fnGroup);

    // Procedures group node — lazy-loaded on expand
    auto* procGroup = new QStandardItem(tr("Procedures"));
    procGroup->setData(NodeProceduresGroup, kRoleKind);
    procGroup->setData(schemaName, kRoleSchemaName);
    procGroup->setData(false, kRoleLoaded);
    auto* procPlaceholder = new QStandardItem(tr("(loading…)"));
    procPlaceholder->setData(NodePlaceholder, kRoleKind);
    procPlaceholder->setEnabled(false);
    procGroup->appendRow(procPlaceholder);
    schemaItem->appendRow(procGroup);
}

void WorkspaceSidebar::loadFunctionsForSchema(QStandardItem* parent, const QString& schemaName) {
    if (!state_ || !state_->adapter()) return;
    try {
        const std::optional<std::string> schemaOpt =
            schemaName.isEmpty() ? std::nullopt
                                 : std::make_optional(schemaName.toStdString());
        const auto fns = state_->adapter()->listFunctions(schemaOpt);
        if (fns.empty()) {
            auto* empty = new QStandardItem(tr("(none)"));
            empty->setEnabled(false);
            parent->appendRow(empty);
            return;
        }
        for (const auto& fn : fns) {
            auto* item = new QStandardItem(QString::fromUtf8(fn.c_str()));
            item->setData(NodeFunction, kRoleKind);
            item->setData(schemaName, kRoleSchemaName);
            item->setData(QString::fromUtf8(fn.c_str()), kRoleRoutineName);
            parent->appendRow(item);
        }
    } catch (const GridexError&) {
        auto* empty = new QStandardItem(tr("(none)"));
        empty->setEnabled(false);
        parent->appendRow(empty);
    }
}

void WorkspaceSidebar::loadProceduresForSchema(QStandardItem* parent, const QString& schemaName) {
    if (!state_ || !state_->adapter()) return;
    try {
        const std::optional<std::string> schemaOpt =
            schemaName.isEmpty() ? std::nullopt
                                 : std::make_optional(schemaName.toStdString());
        const auto procs = state_->adapter()->listProcedures(schemaOpt);
        if (procs.empty()) {
            auto* empty = new QStandardItem(tr("(none)"));
            empty->setEnabled(false);
            parent->appendRow(empty);
            return;
        }
        for (const auto& p : procs) {
            auto* item = new QStandardItem(QString::fromUtf8(p.c_str()));
            item->setData(NodeProcedure, kRoleKind);
            item->setData(schemaName, kRoleSchemaName);
            item->setData(QString::fromUtf8(p.c_str()), kRoleRoutineName);
            parent->appendRow(item);
        }
    } catch (const GridexError&) {
        auto* empty = new QStandardItem(tr("(none)"));
        empty->setEnabled(false);
        parent->appendRow(empty);
    }
}

void WorkspaceSidebar::onItemDoubleClicked(const QModelIndex& index) {
    auto* item = model_->itemFromIndex(index);
    if (!item) return;
    const int kind = item->data(kRoleKind).toInt();
    const QString schema = item->data(kRoleSchemaName).toString();
    if (kind == NodeTable) {
        emit tableSelected(schema, item->data(kRoleTableName).toString());
    } else if (kind == NodeFunction) {
        emit functionSelected(schema, item->data(kRoleRoutineName).toString());
    } else if (kind == NodeProcedure) {
        emit procedureSelected(schema, item->data(kRoleRoutineName).toString());
    }
}

void WorkspaceSidebar::onSchemaChanged(int /*index*/) {
    reloadActiveSchema();
}

void WorkspaceSidebar::reloadActiveSchema() {
    if (!schemaCombo_ || schemaCombo_->count() == 0) return;
    const QString schema = schemaCombo_->currentText();
    model_->clear();
    if (!state_ || !state_->adapter()) return;
    auto* schemaItem = new QStandardItem(schema);
    schemaItem->setData(NodeSchema, kRoleKind);
    schemaItem->setData(schema, kRoleSchemaName);
    schemaItem->setData(false, kRoleLoaded);
    auto* placeholder = new QStandardItem(tr("(loading…)"));
    placeholder->setData(NodePlaceholder, kRoleKind);
    placeholder->setEnabled(false);
    schemaItem->appendRow(placeholder);
    model_->appendRow(schemaItem);
    tree_->expand(model_->index(0, 0));
}

void WorkspaceSidebar::onSearchChanged(const QString& text) {
    if (gridToggleBtn_ && gridToggleBtn_->isChecked()) {
        if (tableGrid_) tableGrid_->onSearchChanged(text);
        return;
    }

    const auto lower = text.trimmed().toLower();
    for (int i = 0; i < model_->rowCount(); ++i) {
        auto* schemaItem = model_->item(i);
        bool schemaMatches = lower.isEmpty() ||
            schemaItem->text().toLower().contains(lower);
        bool anyChildMatches = false;
        for (int j = 0; j < schemaItem->rowCount(); ++j) {
            auto* child = schemaItem->child(j);
            // Folder caption rows are kept whenever any sibling is visible.
            const bool isFolder = child->data(kRoleKind).toInt() == NodeFolder;
            bool ok;
            if (isFolder) {
                ok = true;
            } else {
                ok = lower.isEmpty() || child->text().toLower().contains(lower);
            }
            tree_->setRowHidden(j, schemaItem->index(), !ok);
            if (!isFolder) anyChildMatches = anyChildMatches || ok;
        }
        tree_->setRowHidden(i, QModelIndex(), !(schemaMatches || anyChildMatches));
    }
}

void WorkspaceSidebar::onContextMenuRequested(const QPoint& pos) {
    const QModelIndex index = tree_->indexAt(pos);
    auto* item = index.isValid() ? model_->itemFromIndex(index) : nullptr;
    const int kind = item ? item->data(kRoleKind).toInt() : 0;

    QMenu menu(this);

    auto* refreshAction = menu.addAction(tr("Refresh"));
    refreshAction->setShortcut(QKeySequence::Refresh);
    connect(refreshAction, &QAction::triggered, this, [this] { loadSchemas(); });

    if (!item || kind != NodeTable) {
        menu.addSeparator();

        auto* runSqlAct  = menu.addAction(tr("Run SQL File…"));
        auto* backupAct  = menu.addAction(tr("Backup Database…"));
        auto* restoreAct = menu.addAction(tr("Restore Database…"));
        const bool hasAdapter = state_ && state_->adapter();
        runSqlAct->setEnabled(hasAdapter);
        backupAct->setEnabled(hasAdapter);
        restoreAct->setEnabled(hasAdapter);
        connect(runSqlAct,  &QAction::triggered, this, &WorkspaceSidebar::runSqlFile);
        connect(backupAct,  &QAction::triggered, this, &WorkspaceSidebar::backupDatabase);
        connect(restoreAct, &QAction::triggered, this, &WorkspaceSidebar::restoreDatabase);

        menu.exec(tree_->viewport()->mapToGlobal(pos));
        return;
    }

    menu.addSeparator();

    const QString schema = item->data(kRoleSchemaName).toString();
    const QString table  = item->data(kRoleTableName).toString();

    auto* openAction = menu.addAction(tr("Open in New Tab"));
    connect(openAction, &QAction::triggered, this, [this, schema, table] {
        emit tableSelected(schema, table);
    });

    auto* structureAction = menu.addAction(tr("View Structure"));
    connect(structureAction, &QAction::triggered, this, [this, schema, table] {
        emit tableSelected(schema, table);
    });

    menu.addSeparator();

    auto* copyNameAction = menu.addAction(tr("Copy Table Name"));
    connect(copyNameAction, &QAction::triggered, this, [table] {
        QApplication::clipboard()->setText(table);
    });

    const QString selectSql = QStringLiteral("SELECT * FROM %1 LIMIT 100;").arg(table);
    auto* copySelectAction = menu.addAction(tr("Copy SELECT * FROM…"));
    connect(copySelectAction, &QAction::triggered, this, [selectSql] {
        QApplication::clipboard()->setText(selectSql);
    });

    auto* copyDdlAction = menu.addAction(tr("Copy CREATE TABLE…"));
    connect(copyDdlAction, &QAction::triggered, this, [this, schema, table] {
        if (!state_ || !state_->adapter()) return;
        try {
            const auto desc = state_->adapter()->describeTable(
                table.toStdString(),
                schema.isEmpty() ? std::nullopt : std::make_optional(schema.toStdString()));
            const SQLDialect dialect = sqlDialect(state_->adapter()->databaseType());
            const QString ddl = QString::fromStdString(desc.toDDL(dialect));
            QApplication::clipboard()->setText(ddl);
        } catch (const GridexError& e) {
            QMessageBox::warning(this, tr("Copy DDL Failed"), QString::fromUtf8(e.what()));
        }
    });

    menu.addSeparator();

    auto* truncateAction = menu.addAction(tr("Truncate Table…"));
    connect(truncateAction, &QAction::triggered, this, [this, schema, table] {
        if (!state_ || !state_->adapter()) return;
        const auto answer = QMessageBox::warning(
            this, tr("Truncate Table"),
            tr("Are you sure you want to truncate \"%1\"?\nAll rows will be deleted.").arg(table),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Yes) return;

        try {
            const DatabaseType dt = state_->adapter()->databaseType();
            std::string sql;
            if (dt == DatabaseType::SQLite) {
                sql = "DELETE FROM " + quoteIdentifier(sqlDialect(dt), table.toStdString());
            } else if (!schema.isEmpty()) {
                sql = "TRUNCATE TABLE "
                    + quoteIdentifier(sqlDialect(dt), schema.toStdString())
                    + "."
                    + quoteIdentifier(sqlDialect(dt), table.toStdString());
            } else {
                sql = "TRUNCATE TABLE " + quoteIdentifier(sqlDialect(dt), table.toStdString());
            }
            state_->adapter()->executeRaw(sql);
        } catch (const GridexError& e) {
            QMessageBox::critical(this, tr("Truncate Failed"), QString::fromUtf8(e.what()));
        }
    });

    auto* dropAction = menu.addAction(tr("Delete Table…"));
    connect(dropAction, &QAction::triggered, this, [this, schema, table] {
        if (!state_ || !state_->adapter()) return;
        const auto answer = QMessageBox::warning(
            this, tr("Delete Table"),
            tr("Are you sure you want to drop \"%1\"?\nThis action cannot be undone.").arg(table),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Yes) return;

        try {
            const DatabaseType dt = state_->adapter()->databaseType();
            std::string sql;
            if (!schema.isEmpty() &&
                dt != DatabaseType::SQLite &&
                dt != DatabaseType::MySQL)
            {
                sql = "DROP TABLE "
                    + quoteIdentifier(sqlDialect(dt), schema.toStdString())
                    + "."
                    + quoteIdentifier(sqlDialect(dt), table.toStdString());
            } else {
                sql = "DROP TABLE " + quoteIdentifier(sqlDialect(dt), table.toStdString());
            }
            state_->adapter()->executeRaw(sql);
            loadSchemas();
            emit tableDeleted(schema, table);
        } catch (const GridexError& e) {
            QMessageBox::critical(this, tr("Delete Table Failed"), QString::fromUtf8(e.what()));
        }
    });

    menu.addSeparator();

    auto* exportAction = menu.addAction(tr("Export Table…"));
    connect(exportAction, &QAction::triggered, this, [this, schema, table] {
        if (!state_ || !state_->adapter()) return;

        const QString path = QFileDialog::getSaveFileName(
            this, tr("Export Table — %1").arg(table),
            QStringLiteral("%1.csv").arg(table),
            tr("CSV (*.csv);;JSON (*.json);;SQL (*.sql)"));
        if (path.isEmpty()) return;

        try {
            const std::optional<std::string> schemaOpt =
                schema.isEmpty() ? std::nullopt
                                 : std::make_optional(schema.toStdString());
            const QueryResult result = state_->adapter()->fetchRows(
                table.toStdString(), schemaOpt,
                std::nullopt, std::nullopt, std::nullopt,
                10000, 0);

            const std::string filePath = path.toStdString();
            if (path.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)) {
                ExportService::exportToJson(result, filePath);
            } else if (path.endsWith(QStringLiteral(".sql"), Qt::CaseInsensitive)) {
                ExportService::exportToSql(result, table.toStdString(), filePath);
            } else {
                ExportService::exportToCsv(result, filePath);
            }
        } catch (const std::exception& e) {
            QMessageBox::critical(this, tr("Export Failed"), QString::fromUtf8(e.what()));
        }
    });

    auto* importCsvAct = menu.addAction(tr("Import CSV…"));
    connect(importCsvAct, &QAction::triggered, this, [this, schema, table] {
        importCsv(schema, table);
    });

    menu.exec(tree_->viewport()->mapToGlobal(pos));
}

// --------------------------------------------------------------------
// Run SQL File / CSV import / Backup / Restore — carried over verbatim
// from the pre-A2 sidebar. Backend behaviour is unchanged.
// --------------------------------------------------------------------

void WorkspaceSidebar::runSqlFile() {
    if (!state_ || !state_->adapter()) return;

    const QString path = QFileDialog::getOpenFileName(
        this, tr("Run SQL File"),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
        tr("SQL (*.sql);;All files (*)"));
    if (path.isEmpty()) return;

    auto* wizard = new SqlImportWizard(state_->adapter(), path, this);
    wizard->setWindowModality(Qt::WindowModal);
    wizard->setAttribute(Qt::WA_DeleteOnClose);
    connect(wizard, &QDialog::finished, this, [this] { loadSchemas(); });
    wizard->show();
}

void WorkspaceSidebar::importCsv(const QString& schema, const QString& table) {
    if (!state_ || !state_->adapter()) return;

    const QString path = QFileDialog::getOpenFileName(
        this, tr("Import CSV into %1").arg(table),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
        tr("CSV (*.csv);;All files (*)"));
    if (path.isEmpty()) return;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Import CSV"),
                             tr("Cannot open file: %1").arg(path));
        return;
    }
    QTextStream ts(&f);
    const QString body = ts.readAll();
    f.close();

    CsvParser parser{body, 0};
    const QStringList header = parser.readRow();
    if (header.isEmpty()) {
        QMessageBox::warning(this, tr("Import CSV"), tr("Empty CSV — no header row."));
        return;
    }

    std::vector<ColumnInfo> tableCols;
    try {
        const auto schemaOpt = schema.isEmpty() ? std::nullopt
                                                 : std::make_optional(schema.toStdString());
        const auto desc = state_->adapter()->describeTable(table.toStdString(), schemaOpt);
        tableCols = desc.columns;
    } catch (const GridexError& e) {
        QMessageBox::critical(this, tr("Import CSV"), QString::fromUtf8(e.what()));
        return;
    }

    std::vector<int> colMap;
    QStringList mappedNames;
    for (const auto& h : header) {
        int idx = -1;
        for (std::size_t i = 0; i < tableCols.size(); ++i) {
            if (QString::fromStdString(tableCols[i].name).compare(h, Qt::CaseInsensitive) == 0) {
                idx = static_cast<int>(i);
                break;
            }
        }
        colMap.push_back(idx);
        if (idx >= 0) mappedNames << h;
    }
    if (mappedNames.isEmpty()) {
        QMessageBox::warning(this, tr("Import CSV"),
            tr("No CSV columns match table columns. Check header names."));
        return;
    }

    const auto ok = QMessageBox::question(
        this, tr("Import CSV"),
        tr("Mapped %1 of %2 CSV columns. Continue?").arg(mappedNames.size()).arg(header.size()),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Yes);
    if (ok != QMessageBox::Yes) return;

    const DatabaseType dt = state_->adapter()->databaseType();
    const SQLDialect dialect = sqlDialect(dt);
    const std::string qTable = (!schema.isEmpty() && dt != DatabaseType::SQLite && dt != DatabaseType::MySQL)
        ? quoteIdentifier(dialect, schema.toStdString()) + "." + quoteIdentifier(dialect, table.toStdString())
        : quoteIdentifier(dialect, table.toStdString());

    std::string colList;
    for (std::size_t i = 0; i < colMap.size(); ++i) {
        if (colMap[i] < 0) continue;
        if (!colList.empty()) colList += ", ";
        colList += quoteIdentifier(dialect, header[i].toStdString());
    }

    int inserted = 0;
    QStringList errors;
    while (!parser.atEnd()) {
        const QStringList fields = parser.readRow();
        if (fields.isEmpty() || (fields.size() == 1 && fields[0].isEmpty())) continue;

        std::string values;
        for (std::size_t i = 0; i < colMap.size(); ++i) {
            if (colMap[i] < 0) continue;
            if (!values.empty()) values += ", ";
            if (i >= static_cast<std::size_t>(fields.size()) || fields[i].isNull()) {
                values += "NULL";
            } else {
                QString v = fields[i];
                v.replace('\'', QStringLiteral("''"));
                values += '\'';
                values += v.toStdString();
                values += '\'';
            }
        }
        const std::string sql = "INSERT INTO " + qTable + " (" + colList + ") VALUES (" + values + ")";
        try {
            state_->adapter()->executeRaw(sql);
            ++inserted;
        } catch (const GridexError& e) {
            errors << QString::fromUtf8(e.what());
            if (errors.size() > 10) { errors << "... (truncated)"; break; }
        } catch (const std::exception& e) {
            errors << QString::fromUtf8(e.what());
            if (errors.size() > 10) { errors << "... (truncated)"; break; }
        }
    }

    if (errors.isEmpty()) {
        QMessageBox::information(this, tr("Import CSV"),
                                 tr("Inserted %1 rows into %2.").arg(inserted).arg(table));
    } else {
        QMessageBox::warning(this, tr("Import CSV"),
            tr("Inserted %1 rows with errors:\n\n%2").arg(inserted).arg(errors.join("\n")));
    }
}

void WorkspaceSidebar::backupDatabase() {
    if (!state_ || !state_->adapter()) return;
    const ConnectionConfig& cfg = state_->config();

    QString defName = QString::fromStdString(cfg.database.value_or(cfg.name));
    if (defName.isEmpty()) defName = QStringLiteral("backup");
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Backup Database"),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
            + QStringLiteral("/") + defName + QStringLiteral(".sql"),
        tr("SQL Dump (*.sql);;All files (*)"));
    if (path.isEmpty()) return;

    std::optional<std::string> pw;
    SecretStore store;
    if (store.isAvailable()) pw = store.loadPassword(cfg.id);

    auto* dlg = new BackupProgressDialog(
        BackupProgressDialog::Mode::Backup, cfg, pw, path, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowModality(Qt::WindowModal);
    dlg->show();
    dlg->start();
}

void WorkspaceSidebar::restoreDatabase() {
    if (!state_ || !state_->adapter()) return;
    const ConnectionConfig& cfg = state_->config();

    const QString path = QFileDialog::getOpenFileName(
        this, tr("Restore Database"),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
        tr("SQL Dump (*.sql);;All files (*)"));
    if (path.isEmpty()) return;

    const auto confirm = QMessageBox::warning(
        this, tr("Restore Database"),
        tr("This will execute SQL from:\n%1\n\nExisting data may be overwritten. Continue?").arg(path),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (confirm != QMessageBox::Yes) return;

    std::optional<std::string> pw;
    SecretStore store;
    if (store.isAvailable()) pw = store.loadPassword(cfg.id);

    auto* dlg = new BackupProgressDialog(
        BackupProgressDialog::Mode::Restore, cfg, pw, path, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowModality(Qt::WindowModal);
    connect(dlg, &QDialog::finished, this, [this] { loadSchemas(); });
    dlg->show();
    dlg->start();
}

void WorkspaceSidebar::promptSaveQuery(const QString& sql) {
    if (!savedQueryRepo_) return;
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, tr("Save Query"), tr("Query name:"),
        QLineEdit::Normal, QString{}, &ok);
    if (!ok || name.trimmed().isEmpty()) return;

    const QString group = QInputDialog::getText(
        this, tr("Save Query"), tr("Group (leave blank for Default):"),
        QLineEdit::Normal, QString{}, &ok);
    if (!ok) return;

    AppDatabase::SavedQueryRecord rec;
    const auto ts = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    rec.id         = std::to_string(ts);
    rec.name       = name.trimmed().toStdString();
    rec.groupName  = group.trimmed().toStdString();
    rec.sql        = sql.toStdString();
    rec.createdAt  = std::chrono::system_clock::now();
    rec.updatedAt  = rec.createdAt;

    try {
        savedQueryRepo_->save(rec);
        reloadSavedQueriesTree();
    } catch (const std::exception& e) {
        QMessageBox::warning(this, tr("Save Failed"), QString::fromUtf8(e.what()));
    }
}

void WorkspaceSidebar::logQuery(const QString& sql, int rowCount, int elapsedMs) {
    if (appDb_) {
        AppDatabase::HistoryEntry entry;
        entry.connectionId = state_ ? state_->config().id : std::string{};
        entry.sql          = sql.toStdString();
        entry.executedAt   = std::chrono::system_clock::now();
        entry.durationMs   = elapsedMs;
        entry.rowCount     = rowCount;
        entry.succeeded    = true;
        try { appDb_->appendHistory(entry); } catch (...) {}
    }

    if (!historyList_) return;
    const QString preview = sql.simplified().left(80);
    const QString label = QStringLiteral("%1 rows · %2 ms  │  %3")
                              .arg(rowCount).arg(elapsedMs).arg(preview);
    auto* item = new QListWidgetItem(label);
    item->setData(Qt::UserRole, sql);
    item->setToolTip(sql);
    historyList_->insertItem(0, item);
    while (historyList_->count() > 200) {
        delete historyList_->takeItem(historyList_->count() - 1);
    }
}

void WorkspaceSidebar::loadHistoryFromDb() {
    if (!appDb_ || !historyList_) return;
    historyList_->clear();
    try {
        const auto entries = appDb_->listAllHistory(200);
        for (const auto& h : entries) {
            const QString sql = QString::fromStdString(h.sql);
            const QString preview = sql.simplified().left(80);
            const QString label = QStringLiteral("%1 rows · %2 ms  │  %3")
                                      .arg(h.rowCount).arg(h.durationMs).arg(preview);
            auto* item = new QListWidgetItem(label);
            item->setData(Qt::UserRole, sql);
            item->setToolTip(sql);
            historyList_->addItem(item);
        }
    } catch (...) {}
}

void WorkspaceSidebar::reloadSavedQueriesTree() {
    if (!savedTree_ || !savedQueryRepo_) return;
    savedTree_->clear();
    try {
        const auto records = savedQueryRepo_->fetchAll();
        QMap<QString, QTreeWidgetItem*> groups;
        for (const auto& r : records) {
            const QString group = r.groupName.empty()
                ? tr("Default") : QString::fromStdString(r.groupName);
            if (!groups.contains(group)) {
                auto* gi = new QTreeWidgetItem(savedTree_);
                gi->setText(0, group);
                gi->setData(0, Qt::UserRole + 1, QStringLiteral("group"));
                gi->setData(0, Qt::UserRole + 2, group);
                groups[group] = gi;
            }
            auto* qi = new QTreeWidgetItem(groups[group]);
            qi->setText(0, QString::fromStdString(r.name));
            qi->setData(0, Qt::UserRole, QString::fromStdString(r.sql));
            qi->setData(0, Qt::UserRole + 1, QStringLiteral("query"));
            qi->setData(0, Qt::UserRole + 3, QString::fromStdString(r.id));
        }
        savedTree_->expandAll();
    } catch (...) {}
}

void WorkspaceSidebar::onSavedQueryContextMenu(const QPoint& pos) {
    auto* item = savedTree_->itemAt(pos);
    QMenu menu(this);
    const QString role = item ? item->data(0, Qt::UserRole + 1).toString() : QString{};

    if (role == QStringLiteral("query")) {
        const QString sql    = item->data(0, Qt::UserRole).toString();
        const QString qid    = item->data(0, Qt::UserRole + 3).toString();
        const QString qname  = item->text(0);

        auto* runAct = menu.addAction(tr("Run"));
        connect(runAct, &QAction::triggered, this, [this, sql] {
            emit loadSavedQueryRequested(sql);
        });

        auto* editAct = menu.addAction(tr("Edit SQL…"));
        connect(editAct, &QAction::triggered, this, [this, qid, qname, sql] {
            if (!savedQueryRepo_) return;
            bool ok = false;
            const QString newSql = QInputDialog::getMultiLineText(
                this, tr("Edit Saved Query"), tr("SQL:"), sql, &ok);
            if (!ok || newSql.trimmed().isEmpty()) return;
            try {
                auto records = savedQueryRepo_->fetchAll();
                for (auto& r : records) {
                    if (QString::fromStdString(r.id) == qid) {
                        r.sql       = newSql.toStdString();
                        r.updatedAt = std::chrono::system_clock::now();
                        savedQueryRepo_->save(r);
                        break;
                    }
                }
                reloadSavedQueriesTree();
            } catch (const std::exception& e) {
                QMessageBox::warning(this, tr("Edit Failed"), QString::fromUtf8(e.what()));
            }
        });

        auto* delAct = menu.addAction(tr("Delete"));
        connect(delAct, &QAction::triggered, this, [this, qid, qname] {
            if (!savedQueryRepo_) return;
            const auto btn = QMessageBox::question(this, tr("Delete Saved Query"),
                tr("Delete \"%1\"?").arg(qname),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
            if (btn != QMessageBox::Yes) return;
            try {
                savedQueryRepo_->remove(qid.toStdString());
                reloadSavedQueriesTree();
            } catch (const std::exception& e) {
                QMessageBox::warning(this, tr("Delete Failed"), QString::fromUtf8(e.what()));
            }
        });

    } else if (role == QStringLiteral("group")) {
        const QString groupName = item->data(0, Qt::UserRole + 2).toString();

        auto* renameAct = menu.addAction(tr("Rename Group…"));
        connect(renameAct, &QAction::triggered, this, [this, groupName] {
            if (!savedQueryRepo_) return;
            bool ok = false;
            const QString newName = QInputDialog::getText(
                this, tr("Rename Group"), tr("New name:"),
                QLineEdit::Normal, groupName, &ok);
            if (!ok || newName.trimmed().isEmpty()) return;
            try {
                auto records = savedQueryRepo_->fetchAll();
                for (auto& r : records) {
                    if (QString::fromStdString(r.groupName) == groupName) {
                        r.groupName = newName.toStdString();
                        r.updatedAt = std::chrono::system_clock::now();
                        savedQueryRepo_->save(r);
                    }
                }
                reloadSavedQueriesTree();
            } catch (const std::exception& e) {
                QMessageBox::warning(this, tr("Rename Failed"), QString::fromUtf8(e.what()));
            }
        });

        auto* deleteGroupAct = menu.addAction(tr("Delete Group"));
        connect(deleteGroupAct, &QAction::triggered, this, [this, groupName] {
            if (!savedQueryRepo_) return;
            const auto btn = QMessageBox::question(this, tr("Delete Group"),
                tr("Delete group \"%1\" and move queries to Default?").arg(groupName),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
            if (btn != QMessageBox::Yes) return;
            try {
                auto records = savedQueryRepo_->fetchAll();
                for (auto& r : records) {
                    if (QString::fromStdString(r.groupName) == groupName) {
                        r.groupName = {};
                        r.updatedAt = std::chrono::system_clock::now();
                        savedQueryRepo_->save(r);
                    }
                }
                reloadSavedQueriesTree();
            } catch (const std::exception& e) {
                QMessageBox::warning(this, tr("Delete Group Failed"), QString::fromUtf8(e.what()));
            }
        });
    }

    if (!menu.isEmpty()) menu.exec(savedTree_->viewport()->mapToGlobal(pos));
}

}  // namespace gridex
