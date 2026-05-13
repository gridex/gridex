#include "Presentation/Views/Main/WorkspaceView.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include "Core/Enums/DatabaseType.h"
#include "Data/Keychain/SecretStore.h"
#include "Data/Persistence/AppDatabase.h"
#include "Presentation/ViewModels/WorkspaceState.h"
#include "Presentation/Views/CreateTable/CreateTableDialog.h"
#include "Presentation/Views/DataGrid/DataGridView.h"
#include "Presentation/Views/Redis/RedisKeyDetailView.h"
#include "Presentation/Views/MongoDB/MongoCollectionView.h"
#include "Data/Adapters/Redis/RedisAdapter.h"
#include "Data/Adapters/MongoDB/MongodbAdapter.h"
#include "Presentation/Views/DatabaseSwitcher/DatabaseSwitcherDialog.h"
#include "Presentation/Views/FunctionDetail/FunctionDetailView.h"
#include "Presentation/Views/Details/DetailsPanel.h"
#include "Presentation/Views/ERDiagram/ERDiagramView.h"
#include "Presentation/Views/QueryEditor/QueryEditorView.h"
#include "Presentation/Views/Sidebar/WorkspaceSidebar.h"
#include "Presentation/Views/StatusBar/StatusBarView.h"
#include "Presentation/Views/TabBar/WorkspaceTabBar.h"

namespace gridex {

WorkspaceView::WorkspaceView(WorkspaceState* state, SecretStore* secretStore,
                             std::shared_ptr<AppDatabase> appDb, QWidget* parent)
    : QWidget(parent), state_(state), secretStore_(secretStore), appDb_(std::move(appDb)) {
    buildUi();
    if (state_) {
        connect(state_, &WorkspaceState::connectionOpened,
                this, &WorkspaceView::onConnectionOpened);
        connect(state_, &WorkspaceState::connectionClosed,
                this, &WorkspaceView::onConnectionClosed);
    }
}

void WorkspaceView::buildUi() {
    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // Horizontal splitter: [sidebar] | [mainHost] | [details]. Replaces the
    // old fixed-width boxes so the user can drag to resize each pane.
    rootSplitter_ = new QSplitter(Qt::Horizontal, this);
    rootSplitter_->setChildrenCollapsible(false);
    rootSplitter_->setHandleWidth(4);
    outer->addWidget(rootSplitter_);
    auto* root = rootSplitter_;

    // ---- Left sidebar ----
    sidebar_ = new WorkspaceSidebar(state_, appDb_, this);
    sidebar_->setMinimumWidth(180);
    connect(sidebar_, &WorkspaceSidebar::tableSelected,
            this, &WorkspaceView::onTableSelected);
    connect(sidebar_, &WorkspaceSidebar::tableDeleted,
            this, &WorkspaceView::onTableDeleted);
    connect(sidebar_, &WorkspaceSidebar::newTableRequested,
            this, &WorkspaceView::onNewTableRequested);
    connect(sidebar_, &WorkspaceSidebar::disconnectRequested,
            this, &WorkspaceView::disconnectRequested);
    connect(sidebar_, &WorkspaceSidebar::loadSavedQueryRequested, this,
            [this](const QString& sql) {
                onNewQueryTab();
                // The new tab's editor is now current in mainStack_
                auto* qe = qobject_cast<QueryEditorView*>(mainStack_->currentWidget());
                if (qe) qe->setSql(sql);
            });
    connect(sidebar_, &WorkspaceSidebar::functionSelected,
            this, &WorkspaceView::onFunctionSelected);
    connect(sidebar_, &WorkspaceSidebar::procedureSelected,
            this, &WorkspaceView::onProcedureSelected);
    root->addWidget(sidebar_);

    // leftDiv_ retained as a no-op placeholder so toggleSidebar still
    // references something; splitter handle does the actual separator.
    leftDiv_ = new QFrame(this);
    leftDiv_->setVisible(false);

    // ---- Center: toolbar row + TabBar + infoBar + content + StatusBar ----
    auto* mainHost = new QWidget(this);
    auto* mv = new QVBoxLayout(mainHost);
    mv->setContentsMargins(0, 0, 0, 0);
    mv->setSpacing(0);

    // Top bar: [sidebarBtn] [connDotBtn] [tabBar ...] [detailsBtn]
    auto* topBar = new QWidget(mainHost);
    topBar->setFixedHeight(38);
    auto* topH = new QHBoxLayout(topBar);
    topH->setContentsMargins(0, 0, 4, 0);
    topH->setSpacing(0);

    // Item 7: sidebar toggle button (☰)
    sidebarBtn_ = new QPushButton(QStringLiteral("☰"), topBar);
    sidebarBtn_->setFixedSize(28, 38);
    sidebarBtn_->setCheckable(true);
    sidebarBtn_->setChecked(true);
    sidebarBtn_->setToolTip(tr("Toggle Sidebar"));
    sidebarBtn_->setCursor(Qt::PointingHandCursor);
    sidebarBtn_->setProperty("toolbarIcon", true);
    connect(sidebarBtn_, &QPushButton::clicked, this, &WorkspaceView::toggleSidebar);
    topH->addWidget(sidebarBtn_);

    // Item 7: connection indicator dot button (●)
    connDotBtn_ = new QPushButton(QStringLiteral("●"), topBar);
    connDotBtn_->setObjectName(QStringLiteral("connDot"));
    connDotBtn_->setFixedSize(28, 38);
    connDotBtn_->setEnabled(false);
    connDotBtn_->setToolTip(tr("Connection status"));
    connDotBtn_->setProperty("connected", false);
    topH->addWidget(connDotBtn_);

    // ER Diagram button (hidden until a connection is open)
    erDiagramBtn_ = new QPushButton(QStringLiteral("⬡ ER"), topBar);
    erDiagramBtn_->setFixedSize(48, 38);
    erDiagramBtn_->setToolTip(tr("Open ER Diagram for current schema"));
    erDiagramBtn_->setVisible(false);
    erDiagramBtn_->setCursor(Qt::PointingHandCursor);
    erDiagramBtn_->setProperty("toolbarIcon", true);
    connect(erDiagramBtn_, &QPushButton::clicked,
            this, &WorkspaceView::onNewErDiagramTab);
    topH->addWidget(erDiagramBtn_);

    // Database switcher button (hidden until a connection is open)
    dbSwitcherBtn_ = new QPushButton(QStringLiteral("⬡ DB ▾"), topBar);
    dbSwitcherBtn_->setFixedHeight(38);
    dbSwitcherBtn_->setMinimumWidth(60);
    dbSwitcherBtn_->setToolTip(tr("Switch database"));
    dbSwitcherBtn_->setVisible(false);
    dbSwitcherBtn_->setCursor(Qt::PointingHandCursor);
    dbSwitcherBtn_->setProperty("toolbarIcon", true);
    connect(dbSwitcherBtn_, &QPushButton::clicked,
            this, &WorkspaceView::onOpenDatabaseSwitcher);
    topH->addWidget(dbSwitcherBtn_);

    tabBar_ = new WorkspaceTabBar(topBar);
    connect(tabBar_, &WorkspaceTabBar::newTabRequested,
            this, &WorkspaceView::onNewQueryTab);
    connect(tabBar_, &WorkspaceTabBar::tabSelected,
            this, &WorkspaceView::onTabSelected);
    connect(tabBar_, &WorkspaceTabBar::tabCloseRequested,
            this, &WorkspaceView::onTabCloseRequested);
    topH->addWidget(tabBar_, 1);

    // Toggle right panel
    detailsBtn_ = new QPushButton(QStringLiteral("⊟"), topBar);
    detailsBtn_->setCheckable(true);
    detailsBtn_->setChecked(true);
    detailsBtn_->setToolTip(tr("Toggle Details Panel"));
    detailsBtn_->setCursor(Qt::PointingHandCursor);
    detailsBtn_->setFixedSize(32, 38);
    detailsBtn_->setProperty("toolbarIcon", true);
    connect(detailsBtn_, &QPushButton::clicked, this, &WorkspaceView::toggleDetailsPanel);
    topH->addWidget(detailsBtn_);

    mv->addWidget(topBar);
    // The legacy toolbar buttons (sidebar toggle ☰, connection dot ●,
    // ER, DB switcher, details ⊟) have moved to GxToolbar / activity
    // bar / shortcuts. We HIDE them individually so the WorkspaceTabBar
    // — which lives in the same row — stays visible.
    sidebarBtn_->hide();
    connDotBtn_->hide();
    erDiagramBtn_->hide();
    dbSwitcherBtn_->hide();
    detailsBtn_->hide();
    topBar->setFixedHeight(30);
    topH->setContentsMargins(0, 0, 0, 0);

    // Item 1: 28px connection info bar (hidden until connection opens)
    infoBar_ = new QLabel(mainHost);
    infoBar_->setFixedHeight(28);
    infoBar_->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    infoBar_->setContentsMargins(12, 0, 12, 0);
    infoBar_->setVisible(false);
    mv->addWidget(infoBar_);

    mainStack_ = new QStackedWidget(mainHost);
    welcome_ = new QLabel(tr("Connected.\n\nDouble-click a table or press [+] to open a query tab."),
                          mainStack_);
    welcome_->setAlignment(Qt::AlignCenter);
    mainStack_->addWidget(welcome_);
    mainStack_->setCurrentWidget(welcome_);
    mv->addWidget(mainStack_, 1);

    statusBar_ = new StatusBarView(mainHost);
    mv->addWidget(statusBar_);
    // Hidden — MainWindow's GxStatusBar is the single source of status
    // truth in the new IDE shell. Setter calls below (setConnection,
    // setSchema, setRowCount, setQueryTime) still run so we can wire
    // them to GxStatusBar later without touching call sites.
    statusBar_->hide();

    root->addWidget(mainHost);

    // ---- Right DetailsPanel ----
    detailsDiv_ = new QFrame(this);
    detailsDiv_->setVisible(false);

    detailsPanel_ = new DetailsPanel(secretStore_, state_, this);
    detailsPanel_->setMinimumWidth(220);
    root->addWidget(detailsPanel_);

    // Initial widths + stretch: only mainHost grows on window resize.
    root->setStretchFactor(0, 0);
    root->setStretchFactor(1, 1);
    root->setStretchFactor(2, 0);
    root->setSizes({280, 800, 340});
}

void WorkspaceView::triggerActiveRun() {
    if (!mainStack_) return;
    if (auto* qe = qobject_cast<QueryEditorView*>(mainStack_->currentWidget())) {
        qe->onRunClicked();
    }
}

void WorkspaceView::triggerActiveExport() {
    if (!mainStack_) return;
    if (auto* qe = qobject_cast<QueryEditorView*>(mainStack_->currentWidget())) {
        qe->exportResultAsCsv();
    }
}

void WorkspaceView::openDatabaseSwitcher() {
    onOpenDatabaseSwitcher();
}

void WorkspaceView::toggleDetailsPanel() {
    const bool show = !detailsPanel_->isVisible();
    detailsPanel_->setVisible(show);
    detailsDiv_->setVisible(show);
    detailsBtn_->setChecked(show);
}

void WorkspaceView::toggleSidebar() {
    // Sidebar may have been re-homed into SidebarPanelStack — guard against
    // a null/reparented sidebar so this no longer crashes on the toggle.
    if (!sidebar_) return;
    const bool show = !sidebar_->isVisible();
    sidebar_->setVisible(show);
    if (leftDiv_) leftDiv_->setVisible(show);
    if (sidebarBtn_) sidebarBtn_->setChecked(show);
}

WorkspaceSidebar* WorkspaceView::takeSidebar() {
    // Pull `sidebar_` out of the QSplitter without resetting its pointer —
    // callers (the SidebarPanelStack host) will reparent + show it inside
    // their own layout. The internal wires made in buildUi() stay live
    // because `sidebar_` is still the same object.
    auto* s = sidebar_;
    if (s && rootSplitter_) {
        s->setParent(nullptr);
    }
    return s;
}

void WorkspaceView::onTableSelected(const QString& schema, const QString& table) {
    if (!state_ || !state_->adapter()) return;
    const auto qualified = schema.isEmpty() ? table : (schema + "." + table);

    for (const auto& [id, w] : tabWidgets_) {
        if (w->property("gridex_table").toString() == qualified) {
            tabBar_->setActiveTab(QString::fromStdString(id));
            return;
        }
    }

    // MongoDB collections get a specialised document view.
    const bool isMongo = state_->adapter()->databaseType() == DatabaseType::MongoDB;
    if (isMongo) {
        auto* mongo = static_cast<MongodbAdapter*>(state_->adapter());
        auto* mv = new MongoCollectionView(mongo, table, mainStack_);
        mv->setProperty("gridex_table", qualified);
        mainStack_->addWidget(mv);
        const auto tabId = tabBar_->addTab(table);
        tabWidgets_[tabId.toStdString()] = mv;
        mainStack_->setCurrentWidget(mv);
        statusBar_->setSchema(schema);
        return;
    }

    // Redis keys get a specialised view; all other adapters use DataGridView.
    const bool isRedis = state_->adapter()->databaseType() == DatabaseType::Redis;
    if (isRedis) {
        auto* redis = static_cast<RedisAdapter*>(state_->adapter());
        auto* rv = new RedisKeyDetailView(redis, table, mainStack_);
        rv->setProperty("gridex_table", qualified);
        mainStack_->addWidget(rv);

        connect(rv, &RedisKeyDetailView::keyDeleted, this,
                [this](const QString& key) {
                    // Close the tab and refresh the sidebar.
                    const auto qualified2 = key;
                    std::vector<std::string> toRemove;
                    for (const auto& [id, w] : tabWidgets_) {
                        if (w->property("gridex_table").toString() == qualified2)
                            toRemove.push_back(id);
                    }
                    for (const auto& id : toRemove) {
                        auto it = tabWidgets_.find(id);
                        if (it != tabWidgets_.end()) {
                            mainStack_->removeWidget(it->second);
                            it->second->deleteLater();
                            tabWidgets_.erase(it);
                        }
                        tabBar_->removeTab(QString::fromStdString(id));
                    }
                    if (tabWidgets_.empty()) mainStack_->setCurrentWidget(welcome_);
                    if (sidebar_) sidebar_->refreshTree();
                });

        const auto tabId = tabBar_->addTab(table);
        tabWidgets_[tabId.toStdString()] = rv;
        mainStack_->setCurrentWidget(rv);
        statusBar_->setSchema(schema);
        return;
    }

    auto* dg = new DataGridView(mainStack_);
    dg->setProperty("gridex_table", qualified);
    dg->setAdapter(state_->adapter());
    mainStack_->addWidget(dg);

    connect(dg, &DataGridView::rowCountChanged, this,
            [this](int rows, int ms) {
                statusBar_->setRowCount(tr("%1 rows").arg(rows));
                statusBar_->setQueryTime(tr("%1ms").arg(ms));
            });

    connect(dg, &DataGridView::rowSelected, this,
            [this](const std::vector<std::pair<std::string, std::string>>& fields) {
                std::vector<DetailsPanel::FieldEntry> entries;
                entries.reserve(fields.size());
                for (const auto& [col, val] : fields)
                    entries.push_back({col, val});
                detailsPanel_->setSelectedRow(entries);
            });

    connect(detailsPanel_, &DetailsPanel::fieldEdited, dg,
            [dg](int columnIndex, const QString& newValue) {
                auto* tv = dg->findChild<QTableView*>();
                if (!tv) return;
                const auto currentRow = tv->currentIndex().row();
                if (currentRow < 0) return;
                auto* model = tv->model();
                if (!model) return;
                const auto idx = model->index(currentRow, columnIndex);
                model->setData(idx, newValue, Qt::EditRole);
            });

    // Tab label is just the table name — the schema is implicit (visible
    // in the sidebar tree and the status bar). Full schema-qualified name
    // still lives in the `gridex_table` property used for de-dup.
    const auto tabId = tabBar_->addTab(table);
    tabWidgets_[tabId.toStdString()] = dg;
    mainStack_->setCurrentWidget(dg);
    dg->loadTable(schema, table);
    statusBar_->setSchema(schema);
}

void WorkspaceView::onNewQueryTab() {
    if (!state_ || !state_->adapter()) return;
    auto* qe = new QueryEditorView(mainStack_);
    qe->setAdapter(state_->adapter());
    mainStack_->addWidget(qe);

    connect(qe, &QueryEditorView::queryExecuted, this,
            [this](const QString& sql, int rows, int ms) {
                statusBar_->setRowCount(tr("%1 rows").arg(rows));
                statusBar_->setQueryTime(tr("%1ms").arg(ms));
                if (sidebar_) sidebar_->logQuery(sql, rows, ms);
            });
    connect(qe, &QueryEditorView::saveQueryRequested, this,
            [this](const QString& sql) {
                if (sidebar_) sidebar_->promptSaveQuery(sql);
            });

    const auto tabId = tabBar_->addTab(tr("Query"));
    tabWidgets_[tabId.toStdString()] = qe;
    mainStack_->setCurrentWidget(qe);
}

void WorkspaceView::onNewErDiagramTab() {
    if (!state_ || !state_->adapter()) return;

    // Determine the active schema from the status bar context (use nullopt for default).
    std::optional<std::string> schema;
    try {
        const auto db = state_->adapter()->currentDatabase();
        // listSchemas to check; leave schema as nullopt to use adapter default.
        (void)db;
    } catch (...) {}

    auto* er = new ERDiagramView(mainStack_);
    mainStack_->addWidget(er);

    const auto tabId = tabBar_->addTab(tr("ER Diagram"));
    tabWidgets_[tabId.toStdString()] = er;
    mainStack_->setCurrentWidget(er);

    // Load asynchronously — ERDiagramView::loadSchema is synchronous but schema
    // inspection may be slow; defer to next event loop tick so the tab appears first.
    QTimer::singleShot(0, er, [er, adapter = state_->adapter(), schema] {
        er->loadSchema(adapter, schema);
    });
}

void WorkspaceView::onTabSelected(const QString& id) {
    auto it = tabWidgets_.find(id.toStdString());
    if (it != tabWidgets_.end()) mainStack_->setCurrentWidget(it->second);
}

void WorkspaceView::onTabCloseRequested(const QString& id) {
    auto it = tabWidgets_.find(id.toStdString());
    if (it != tabWidgets_.end()) {
        mainStack_->removeWidget(it->second);
        it->second->deleteLater();
        tabWidgets_.erase(it);
    }
    tabBar_->removeTab(id);
    if (tabWidgets_.empty()) mainStack_->setCurrentWidget(welcome_);
}

void WorkspaceView::onTableDeleted(const QString& schema, const QString& table) {
    const auto qualified = schema.isEmpty() ? table : (schema + "." + table);
    // Close any open tab that shows the deleted table
    std::vector<std::string> toRemove;
    for (const auto& [id, w] : tabWidgets_) {
        if (w->property("gridex_table").toString() == qualified) {
            toRemove.push_back(id);
        }
    }
    for (const auto& id : toRemove) {
        auto it = tabWidgets_.find(id);
        if (it != tabWidgets_.end()) {
            mainStack_->removeWidget(it->second);
            it->second->deleteLater();
            tabWidgets_.erase(it);
        }
        tabBar_->removeTab(QString::fromStdString(id));
    }
    if (tabWidgets_.empty()) mainStack_->setCurrentWidget(welcome_);
}

void WorkspaceView::onNewTableRequested(const QString& schema) {
    if (!state_ || !state_->adapter()) return;
    auto* dlg = new CreateTableDialog(state_->adapter(), schema, this);
    connect(dlg, &CreateTableDialog::tableCreated, this,
            [this](const QString& /*tableName*/) {
                if (sidebar_) sidebar_->refreshTree();
            });
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->exec();
}

void WorkspaceView::onFunctionSelected(const QString& schema, const QString& name) {
    if (!state_ || !state_->adapter()) return;
    const auto qualified = (schema.isEmpty() ? name : (schema + "." + name))
                           + QStringLiteral(" [fn]");
    for (const auto& [id, w] : tabWidgets_) {
        if (w->property("gridex_routine").toString() == qualified) {
            tabBar_->setActiveTab(QString::fromStdString(id));
            return;
        }
    }
    auto* view = new FunctionDetailView(state_->adapter(), schema, name,
                                        false, mainStack_);
    view->setProperty("gridex_routine", qualified);
    mainStack_->addWidget(view);
    const auto tabId = tabBar_->addTab(name);
    tabWidgets_[tabId.toStdString()] = view;
    mainStack_->setCurrentWidget(view);
}

void WorkspaceView::onProcedureSelected(const QString& schema, const QString& name) {
    if (!state_ || !state_->adapter()) return;
    const auto qualified = (schema.isEmpty() ? name : (schema + "." + name))
                           + QStringLiteral(" [proc]");
    for (const auto& [id, w] : tabWidgets_) {
        if (w->property("gridex_routine").toString() == qualified) {
            tabBar_->setActiveTab(QString::fromStdString(id));
            return;
        }
    }
    auto* view = new FunctionDetailView(state_->adapter(), schema, name,
                                        true, mainStack_);
    view->setProperty("gridex_routine", qualified);
    mainStack_->addWidget(view);
    const auto tabId = tabBar_->addTab(name);
    tabWidgets_[tabId.toStdString()] = view;
    mainStack_->setCurrentWidget(view);
}

void WorkspaceView::onOpenDatabaseSwitcher() {
    if (!state_ || !state_->adapter()) return;

    QString currentDb;
    try {
        const auto db = state_->adapter()->currentDatabase();
        if (db) currentDb = QString::fromStdString(*db);
    } catch (...) {}

    auto* dlg = new DatabaseSwitcherDialog(state_->adapter(), currentDb, this);
    connect(dlg, &DatabaseSwitcherDialog::databaseSelected, this,
            [this](const QString& dbName) {
                if (!state_ || !state_->adapter()) return;
                // Switch database: execute USE <db> for MySQL;
                // for PostgreSQL/SQLite reconnect with modified config.
                const DatabaseType dt = state_->adapter()->databaseType();
                try {
                    if (dt == DatabaseType::MySQL) {
                        state_->adapter()->executeRaw(
                            "USE `" + dbName.toStdString() + "`");
                    } else {
                        // Reconnect silently — no HomeView flip.
                        state_->switchDatabase(dbName.toStdString(), secretStore_);
                    }
                } catch (...) {}

                // Update button label and reload sidebar
                dbSwitcherBtn_->setText(
                    QStringLiteral("⬡ %1 ▾").arg(dbName));
                if (sidebar_) sidebar_->refreshTree();
            });
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->exec();
}

void WorkspaceView::onConnectionOpened() {
    if (!state_) return;
    const auto& cfg = state_->config();
    const QString name = QString::fromUtf8(cfg.name.c_str());
    const QString host = QString::fromUtf8(cfg.displayHost().c_str());

    welcome_->setText(tr("Connected to %1.\n\nDouble-click a table or press [+] to open a query tab.")
                          .arg(name));
    mainStack_->setCurrentWidget(welcome_);
    statusBar_->setConnection(tr("%1 — %2").arg(name, host));
    statusBar_->setSchema({});
    statusBar_->setRowCount({});
    statusBar_->setQueryTime({});
    detailsPanel_->clearSelectedRow();

    // Item 1: populate info bar.
    // serverVersion() may throw; fall back gracefully.
    QString version;
    try {
        if (state_->adapter()) {
            version = QString::fromStdString(state_->adapter()->serverVersion());
        }
    } catch (...) {}

    const QString dbType = QString::fromUtf8(
        displayName(cfg.databaseType).data(),
        static_cast<qsizetype>(displayName(cfg.databaseType).size()));
    const QString user = cfg.username
        ? QString::fromUtf8(cfg.username->c_str()) : QString{};
    const QString database = cfg.database
        ? QString::fromUtf8(cfg.database->c_str()) : QString{};

    QString infoText = QStringLiteral("● %1 · %2").arg(name, dbType);
    if (!version.isEmpty()) infoText += QStringLiteral(" %1").arg(version);
    if (!user.isEmpty())     infoText += QStringLiteral(" · %1").arg(user);
    if (!database.isEmpty()) infoText += QStringLiteral(" · %1").arg(database);

    infoBar_->setText(infoText);
    infoBar_->setVisible(true);

    // ER-diagram + database-switcher buttons live on GxToolbar now —
    // leave the legacy buttons hidden so they don't leak back into the
    // tab strip when a connection opens.
    erDiagramBtn_->setVisible(false);
    dbSwitcherBtn_->setVisible(false);

    // Connection dot → active (Catppuccin green via style.qss).
    connDotBtn_->setProperty("connected", true);
    connDotBtn_->style()->unpolish(connDotBtn_);
    connDotBtn_->style()->polish(connDotBtn_);
}

void WorkspaceView::onConnectionClosed() {
    closeAllTabs();
    detailsPanel_->clearSelectedRow();
    welcome_->setText(tr("Disconnected."));
    mainStack_->setCurrentWidget(welcome_);
    statusBar_->setConnection({});
    statusBar_->setSchema({});
    statusBar_->setRowCount({});
    statusBar_->setQueryTime({});

    // Item 1: hide info bar on disconnect.
    infoBar_->setVisible(false);

    // Hide ER Diagram button when disconnected.
    erDiagramBtn_->setVisible(false);

    // Hide database switcher button when disconnected.
    dbSwitcherBtn_->setVisible(false);

    // Connection dot → inactive (gray via style.qss).
    connDotBtn_->setProperty("connected", false);
    connDotBtn_->style()->unpolish(connDotBtn_);
    connDotBtn_->style()->polish(connDotBtn_);
}

void WorkspaceView::closeAllTabs() {
    for (auto& [id, w] : tabWidgets_) {
        mainStack_->removeWidget(w);
        w->deleteLater();
        tabBar_->removeTab(QString::fromStdString(id));
    }
    tabWidgets_.clear();
}

}
