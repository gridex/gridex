#pragma once

#include <memory>
#include <QWidget>
#include <string>
#include <unordered_map>

class QFrame;
class QLabel;
class QPushButton;
class QSplitter;
class QStackedWidget;

namespace gridex {

class AppDatabase;
class DataGridView;
class DetailsPanel;
class FunctionDetailView;
class RedisKeyDetailView;
class MongoCollectionView;
class ERDiagramView;
class QueryEditorView;
class SecretStore;
class StatusBarView;
class WorkspaceSidebar;
class WorkspaceState;
class WorkspaceTabBar;

// Workspace: [Sidebar 280] | [Main: toolbar + infoBar + TabBar + content + StatusBar] | [DetailsPanel ~340]
// Item 1: 28px connection info bar between tab bar and content.
// Item 7: sidebar-toggle + connection indicator icon buttons left of tab bar.
class WorkspaceView : public QWidget {
    Q_OBJECT

public:
    explicit WorkspaceView(WorkspaceState*              state,
                           SecretStore*                 secretStore = nullptr,
                           std::shared_ptr<AppDatabase> appDb       = nullptr,
                           QWidget*                     parent      = nullptr);

    // Detach the internal Schema sidebar from WorkspaceView's splitter so
    // a host (MainWindow) can mount it inside the activity-bar-driven
    // SidebarPanelStack. After this returns, WorkspaceView's splitter has
    // 2 children (main + details); all internal signal wiring against
    // `sidebar_` remains intact — the pointer doesn't change.
    WorkspaceSidebar* takeSidebar();

public slots:
    void onNewQueryTab();
    void onNewErDiagramTab();
    void toggleDetailsPanel();
    void toggleSidebar();

    // Toolbar-driven actions that target the *current* tab. Each one
    // looks up mainStack_->currentWidget(), casts to the expected view,
    // and routes the trigger. A no-op when the cast fails (e.g. user is
    // on a DataGridView while Run is pressed).
    void triggerActiveRun();
    void triggerActiveExport();
    void openDatabaseSwitcher();

signals:
    void disconnectRequested();

private slots:
    void onTableSelected(const QString& schema, const QString& table);
    void onTableDeleted(const QString& schema, const QString& table);
    void onNewTableRequested(const QString& schema);
    void onOpenDatabaseSwitcher();
    void onFunctionSelected(const QString& schema, const QString& name);
    void onProcedureSelected(const QString& schema, const QString& name);
    void onTabSelected(const QString& id);
    void onTabCloseRequested(const QString& id);
    void onConnectionOpened();
    void onConnectionClosed();

private:
    void buildUi();
    void closeAllTabs();

    WorkspaceState*              state_        = nullptr;
    SecretStore*                 secretStore_  = nullptr;
    std::shared_ptr<AppDatabase> appDb_;
    QSplitter*        rootSplitter_ = nullptr;
    WorkspaceSidebar* sidebar_      = nullptr;
    QFrame*           leftDiv_      = nullptr;
    WorkspaceTabBar*  tabBar_       = nullptr;
    QStackedWidget*   mainStack_    = nullptr;
    QLabel*           welcome_      = nullptr;
    QLabel*           infoBar_      = nullptr;   // Item 1: connection info
    StatusBarView*    statusBar_    = nullptr;

    // Right details panel
    DetailsPanel*     detailsPanel_ = nullptr;
    QFrame*           detailsDiv_   = nullptr;
    QPushButton*      detailsBtn_   = nullptr;

    // Item 7: left toolbar icon buttons
    QPushButton*      sidebarBtn_   = nullptr;
    QPushButton*      connDotBtn_   = nullptr;

    // ER Diagram toolbar button (shown only when connected)
    QPushButton*      erDiagramBtn_     = nullptr;

    // Database switcher button (shown only when connected)
    QPushButton*      dbSwitcherBtn_    = nullptr;

    std::unordered_map<std::string, QWidget*> tabWidgets_;
};

}
