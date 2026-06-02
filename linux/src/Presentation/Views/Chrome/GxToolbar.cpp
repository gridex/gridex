#include "Presentation/Views/Chrome/GxToolbar.h"

#include <QAction>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QSizePolicy>
#include <QToolButton>
#include <QWidget>

#include "Presentation/Views/Chrome/GxIcons.h"

namespace gridex {

namespace {

QToolButton* makeBtn(QToolBar* bar, QAction* a, const char* kind = nullptr) {
    auto* btn = new QToolButton(bar);
    btn->setDefaultAction(a);
    btn->setToolButtonStyle(Qt::ToolButtonIconOnly);
    btn->setAutoRaise(true);
    btn->setIconSize({14, 14});
    btn->setFocusPolicy(Qt::NoFocus);
    // Qt picks up tooltips from the default action automatically, but
    // some styles (Fusion) suppress them when the action also has a
    // shortcut. Force-copy so every button consistently shows on hover.
    btn->setToolTip(a->toolTip());
    if (kind) btn->setProperty("gxKind", kind);
    return btn;
}

QFrame* makeDivider(QWidget* parent) {
    auto* d = new QFrame(parent);
    d->setObjectName(QStringLiteral("gxTbDiv"));
    d->setFixedSize(1, 22);
    return d;
}

}  // namespace

GxToolbar::GxToolbar(QWidget* parent) : QToolBar(parent) {
    setObjectName(QStringLiteral("gxToolbar"));
    setMovable(false);
    setFloatable(false);
    setIconSize({14, 14});
    setContentsMargins(6, 4, 6, 4);
    setFixedHeight(36);

    // ── Actions (only those backed by working features) ────────────
    // Wired:
    //   plug      — onAddConnection
    //   sql-new   — onNewQueryTab
    //   play      — triggerActiveRun on the workspace
    //   commit    — adapter()->commitTransaction()
    //   refresh   — sidebar.refreshTree()
    //   export    — triggerActiveExport on the workspace
    //   erd       — onNewErDiagramTab
    // Stubs kept for API compatibility but never reach the layout:
    //   openFile / save / runAll / stop / rollback / explain

    newConnAction_   = new QAction(GxIcons::glyph("plug"),     QString(), this);
    newConnAction_->setToolTip(tr("Connect (Ctrl+T)"));
    newConnAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_T));

    newQueryAction_  = new QAction(GxIcons::glyph("sql-new"),  QString(), this);
    newQueryAction_->setToolTip(tr("New query (Ctrl+Shift+N)"));
    newQueryAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N));
    newQueryAction_->setEnabled(false);

    runAction_       = new QAction(GxIcons::glyph("play", "#11151a"), QString(), this);
    runAction_->setToolTip(tr("Run statement (Ctrl+Enter)"));
    runAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return));
    runAction_->setEnabled(false);

    commitAction_    = new QAction(GxIcons::glyph("commit"),   QString(), this);
    commitAction_->setToolTip(tr("Commit transaction"));
    commitAction_->setEnabled(false);

    refreshAction_   = new QAction(GxIcons::glyph("refresh"),  QString(), this);
    refreshAction_->setToolTip(tr("Refresh schema (F5)"));
    refreshAction_->setShortcut(QKeySequence(Qt::Key_F5));
    refreshAction_->setEnabled(false);

    exportAction_    = new QAction(GxIcons::glyph("export"),   QString(), this);
    exportAction_->setToolTip(tr("Export results to CSV"));
    exportAction_->setEnabled(false);

    erdAction_       = new QAction(GxIcons::glyph("erd"),      QString(), this);
    erdAction_->setToolTip(tr("ER diagram"));
    erdAction_->setEnabled(false);

    switchDbAction_  = new QAction(GxIcons::glyph("db"),       QString(), this);
    switchDbAction_->setToolTip(tr("Switch database"));
    switchDbAction_->setEnabled(false);

    // API-only stubs (never added to layout — features not implemented).
    openFileAction_  = new QAction(this); openFileAction_->setEnabled(false);
    saveAction_      = new QAction(this); saveAction_->setEnabled(false);
    runAllAction_    = new QAction(this); runAllAction_->setEnabled(false);
    stopAction_      = new QAction(this); stopAction_->setEnabled(false);
    rollbackAction_  = new QAction(this); rollbackAction_->setEnabled(false);
    explainAction_   = new QAction(this); explainAction_->setEnabled(false);

    // ── Layout ─────────────────────────────────────────────────────
    // [plug · sql-new] | [play · commit] | [refresh · export · erd] | spacer | search
    addWidget(makeBtn(this, newConnAction_));
    addWidget(makeBtn(this, newQueryAction_));
    addWidget(makeDivider(this));

    addWidget(makeBtn(this, runAction_, "primary"));
    addWidget(makeBtn(this, commitAction_));
    addWidget(makeDivider(this));

    addWidget(makeBtn(this, refreshAction_));
    addWidget(makeBtn(this, exportAction_));
    addWidget(makeBtn(this, erdAction_));
    addWidget(makeBtn(this, switchDbAction_));

    auto* spacer = new QWidget(this);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    spacer->setAttribute(Qt::WA_TransparentForMouseEvents);
    addWidget(spacer);

    // ── Search field (.gx-tb-search) ───────────────────────────────
    auto* wrap = new QWidget(this);
    wrap->setObjectName(QStringLiteral("gxTbSearch"));
    wrap->setAttribute(Qt::WA_StyledBackground, true);
    auto* sw = new QHBoxLayout(wrap);
    sw->setContentsMargins(8, 0, 8, 0);
    sw->setSpacing(6);

    auto* icon = new QLabel(wrap);
    icon->setPixmap(GxIcons::pixmap(QStringLiteral("search"), QString(), 11));
    icon->setFixedSize(11, 11);
    icon->setAttribute(Qt::WA_TransparentForMouseEvents);
    icon->setAttribute(Qt::WA_TranslucentBackground);
    sw->addWidget(icon);

    search_ = new QLineEdit(wrap);
    search_->setObjectName(QStringLiteral("gxTbSearchInput"));
    search_->setPlaceholderText(tr("Search tables, columns, snippets…  Ctrl+P"));
    search_->setClearButtonEnabled(false);
    sw->addWidget(search_, 1);

    addWidget(wrap);

    connect(search_, &QLineEdit::returnPressed, this, [this] {
        emit searchSubmitted(search_->text());
    });
}

}  // namespace gridex
