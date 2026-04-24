#pragma once

#include <QMainWindow>
#include <memory>

class QAction;
class QLabel;
class QStackedWidget;

namespace gridex {

class AppDatabase;
class AppConnectionRepository;
class ConnectionListViewModel;
class ConnectionSidebar;
class HomeBrandingPanel;
class MCPConnectionProvider;
class MCPWindow;
class SecretStore;
class UpdateService;
class WorkspaceState;
class WorkspaceView;
namespace mcp { class MCPServer; }

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void onAddConnection();
    void onEditConnection(const QString& id);
    void onRemoveConnection(const QString& id);
    void onConnectionSelected(const QString& id);
    void onNewQueryTab();
    void onDisconnect();
    void onVmError(const QString& message);
    void onBackupRequested();
    void onRestoreRequested();
    void onOpenPreferences();
    void onShowAbout();
    void onShowShortcuts();
    void onImportConnections();
    void onExportConnections();
    void onOpenMCPServer();
    void onCheckForUpdates();

private:
    void setupMenuBar();
    void setupCentralLayout();
    void setupStatusBar();
    void wireBackend();
    void updateWorkspaceActions();

    // Backend (owned)
    std::shared_ptr<AppDatabase> appDb_;
    std::unique_ptr<AppConnectionRepository> repo_;
    std::unique_ptr<SecretStore> secretStore_;
    std::unique_ptr<ConnectionListViewModel> viewModel_;
    std::unique_ptr<WorkspaceState> workspace_;

    // MCP
    std::unique_ptr<MCPConnectionProvider>  mcpProvider_;
    std::unique_ptr<mcp::MCPServer>         mcpServer_;
    MCPWindow*                              mcpWindow_ = nullptr;  // owned by Qt parent when shown

    // Auto-update (AppImage self-replace; no-op for other distros).
    std::unique_ptr<UpdateService> updateService_;

    // Menu actions that toggle enabled state with connection
    QAction* newQueryAction_    = nullptr;
    QAction* disconnectAction_  = nullptr;

    // UI (owned by Qt parent)
    QStackedWidget*    stack_ = nullptr;
    HomeBrandingPanel* brandPanel_ = nullptr;
    ConnectionSidebar* connectionsPanel_ = nullptr;
    WorkspaceView*     workspaceView_ = nullptr;
};

}
