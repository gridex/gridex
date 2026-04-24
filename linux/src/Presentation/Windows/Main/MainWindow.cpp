#include "Presentation/Windows/Main/MainWindow.h"

#include <chrono>
#include <thread>

#include <QAction>
#include <QApplication>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QAbstractButton>
#include <QMessageBox>
#include <QPushButton>
#include <QProgressDialog>
#include <QSettings>
#include <QStackedWidget>
#include <QTimer>
#include <QStandardPaths>
#include <QStatusBar>
#include <QString>
#include <QWidget>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "Core/Errors/GridexError.h"
#include "Core/Models/Database/ConnectionConfigJson.h"
#include "Data/Adapters/AdapterFactory.h"
#include "Data/Keychain/SecretStore.h"
#include "Data/Persistence/AppConnectionRepository.h"
#include "Data/Persistence/AppDatabase.h"
#include "Domain/UseCases/Connection/TestConnection.h"
#include "Presentation/ViewModels/ConnectionListViewModel.h"
#include "Services/Export/DatabaseDumpRunner.h"
#include "Services/SSH/SSHTunnelManager.h"
#include "Presentation/ViewModels/WorkspaceState.h"
#include "Presentation/Views/ConnectionForm/ConnectionFormDialog.h"
#include "Presentation/Views/Home/HomeBrandingPanel.h"
#include "Presentation/Views/Main/WorkspaceView.h"
#include "App/MCP/MCPConnectionProvider.h"
#include "Presentation/Views/MCP/MCPWindow.h"
#include "Presentation/Views/Settings/SettingsDialog.h"
#include "Presentation/Views/Sidebar/ConnectionSidebar.h"
#include "Presentation/Views/TabBar/WorkspaceTabBar.h"
#include "Services/MCP/MCPServer.h"
#include "Services/MCP/Security/ApprovalGate.h"
#include "Services/Update/UpdateService.h"

namespace gridex {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("Gridex"));
    // Match macOS HomeView initial size (900x500 — focused connection picker).
    resize(900, 500);

    wireBackend();
    setupMenuBar();
    setupCentralLayout();
    setupStatusBar();

    viewModel_->reload();
}

MainWindow::~MainWindow() = default;

void MainWindow::wireBackend() {
    appDb_ = std::make_shared<AppDatabase>();
    try {
        appDb_->open();  // default path: $XDG_DATA_HOME/gridex/app.sqlite
    } catch (const GridexError& e) {
        QMessageBox::critical(this, tr("Storage error"),
                              tr("Failed to open app database: %1").arg(QString::fromUtf8(e.what())));
    }
    repo_ = std::make_unique<AppConnectionRepository>(appDb_);
    secretStore_ = std::make_unique<SecretStore>();
    viewModel_ = std::make_unique<ConnectionListViewModel>(repo_.get());
    workspace_ = std::make_unique<WorkspaceState>();
    connect(viewModel_.get(), &ConnectionListViewModel::errorOccurred,
            this, &MainWindow::onVmError);

    // MCP server (stays stopped until user presses Start in the MCP window).
    mcpProvider_ = std::make_unique<MCPConnectionProvider>(repo_.get(), secretStore_.get());
    std::shared_ptr<mcp::IMCPConnectionProvider> providerShim(mcpProvider_.get(), [](auto*){});
    mcpServer_ = std::make_unique<mcp::MCPServer>(
        providerShim, "1.0.0", mcp::MCPTransportMode::InProcess);

    // Approval dialog callback — runs on the GUI main thread via QMetaObject.
    mcpServer_->approvalGate().setDialogCallback(
        [this](const std::string& tool,
               const std::string& description,
               const std::string& details,
               const std::string& connectionId,
               const mcp::MCPAuditClient& client) -> mcp::ApprovalResult {
            mcp::ApprovalResult result = mcp::ApprovalResult::Denied;
            QMetaObject::invokeMethod(this, [&]() {
                QMessageBox box(this);
                box.setWindowTitle(tr("%1 wants to:").arg(QString::fromStdString(client.name)));
                box.setText(QString::fromStdString(description));
                box.setInformativeText(QString::fromStdString(details)
                    + "\n\nConnection: " + QString::fromStdString(connectionId.substr(0, 8)) + "…");
                box.setIcon(QMessageBox::Warning);
                auto* denyBtn    = static_cast<QAbstractButton*>(box.addButton(tr("Deny"), QMessageBox::RejectRole));
                auto* onceBtn    = static_cast<QAbstractButton*>(box.addButton(tr("Approve Once"), QMessageBox::AcceptRole));
                auto* sessionBtn = static_cast<QAbstractButton*>(box.addButton(tr("Approve for Session"), QMessageBox::AcceptRole));
                (void)tool;
                box.exec();
                QAbstractButton* clicked = box.clickedButton();
                if (clicked == sessionBtn) result = mcp::ApprovalResult::ApprovedForSession;
                else if (clicked == onceBtn) result = mcp::ApprovalResult::Approved;
                else result = mcp::ApprovalResult::Denied;
                (void)denyBtn;
            }, Qt::BlockingQueuedConnection);
            return result;
        });

    // Apply saved rate limits / audit retention.
    {
        QSettings s;
        mcp::RateLimits l;
        l.queriesPerMinute = s.value("mcp.rateLimit.queriesPerMinute", 60).toInt();
        l.queriesPerHour   = s.value("mcp.rateLimit.queriesPerHour",   1000).toInt();
        l.writesPerMinute  = s.value("mcp.rateLimit.writesPerMinute",  10).toInt();
        l.ddlPerMinute     = s.value("mcp.rateLimit.ddlPerMinute",     1).toInt();
        mcpServer_->rateLimiter().setLimits(l);
        mcpServer_->auditLogger().setMaxFileSize(
            static_cast<qint64>(s.value("mcp.audit.maxSizeMB", 100).toInt()) * 1024 * 1024);
    }

    // Auto-update: silent background check 3s after launch. Notifies via
    // message box only when a new version is available; failures are
    // swallowed so offline users aren't nagged.
    updateService_ = std::make_unique<UpdateService>();
    connect(updateService_.get(), &UpdateService::updateChecked, this,
            [this](const UpdateCheckResult& r) {
        if (r.errorMessage.isEmpty() && r.hasUpdate) {
            QMessageBox box(this);
            box.setIcon(QMessageBox::Information);
            box.setWindowTitle(tr("Update available"));
            box.setText(tr("Gridex %1 is available (you have %2).").arg(r.newVersion, r.currentVersion));
            if (!r.notes.isEmpty()) box.setInformativeText(r.notes);
            auto* install = box.addButton(tr("Install"), QMessageBox::AcceptRole);
            box.addButton(tr("Later"), QMessageBox::RejectRole);
            box.exec();
            if (box.clickedButton() == install) {
                updateService_->downloadAndApply(r.downloadUrl, r.sha256);
            }
        }
    });
    connect(updateService_.get(), &UpdateService::errorOccurred, this,
            [this](const QString& msg) {
        QMessageBox::warning(this, tr("Update failed"), msg);
    });
    QTimer::singleShot(3000, this, [this] { updateService_->checkForUpdate(); });
}

void MainWindow::setupMenuBar() {
    // ---- File menu (matches macOS CommandGroup) ----
    auto* fileMenu = menuBar()->addMenu(tr("&File"));
    auto* newConn = fileMenu->addAction(tr("New &Connection..."));
    newConn->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_T));
    connect(newConn, &QAction::triggered, this, &MainWindow::onAddConnection);

    newQueryAction_ = fileMenu->addAction(tr("New &Query"));
    newQueryAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N));
    newQueryAction_->setEnabled(false);
    connect(newQueryAction_, &QAction::triggered, this, &MainWindow::onNewQueryTab);

    auto* closeTabAction = fileMenu->addAction(tr("Close &Tab"));
    closeTabAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_W));
    closeTabAction->setEnabled(true);
    connect(closeTabAction, &QAction::triggered, this, [this] {
        if (!workspaceView_ || !workspace_ || !workspace_->isOpen()) return;
        // Delegate to the tab bar to close the currently active tab.
        auto* bar = workspaceView_->findChild<WorkspaceTabBar*>();
        if (bar && !bar->activeTabId().isEmpty()) {
            bar->tabCloseRequested(bar->activeTabId());
        }
    });

    fileMenu->addSeparator();

    disconnectAction_ = fileMenu->addAction(tr("&Disconnect"));
    disconnectAction_->setEnabled(false);
    connect(disconnectAction_, &QAction::triggered, this, &MainWindow::onDisconnect);

    fileMenu->addSeparator();
    auto* importConns = fileMenu->addAction(tr("&Import Connections..."));
    connect(importConns, &QAction::triggered, this, &MainWindow::onImportConnections);
    auto* exportConns = fileMenu->addAction(tr("&Export Connections..."));
    connect(exportConns, &QAction::triggered, this, &MainWindow::onExportConnections);

    fileMenu->addSeparator();
    auto* prefs = fileMenu->addAction(tr("&Preferences..."));
    prefs->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Comma));
    connect(prefs, &QAction::triggered, this, &MainWindow::onOpenPreferences);

    fileMenu->addSeparator();
    auto* quit = fileMenu->addAction(tr("&Quit"));
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, qApp, &QApplication::quit);

    // ---- Query menu ----
    auto* queryMenu = menuBar()->addMenu(tr("&Query"));
    auto* runQuery = queryMenu->addAction(tr("&Run Query"));
    runQuery->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return));
    runQuery->setEnabled(false);  // wired in Phase 3d+ when QueryEditor is active

    auto* toolsMenu = menuBar()->addMenu(tr("&Tools"));
    auto* mcpAction = toolsMenu->addAction(tr("MCP Server…"));
    connect(mcpAction, &QAction::triggered, this, &MainWindow::onOpenMCPServer);

    auto* helpMenu = menuBar()->addMenu(tr("&Help"));
    auto* shortcuts = helpMenu->addAction(tr("Keyboard &Shortcuts"));
    shortcuts->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Slash));
    connect(shortcuts, &QAction::triggered, this, &MainWindow::onShowShortcuts);
    helpMenu->addSeparator();
    auto* checkUpdatesAction = helpMenu->addAction(tr("Check for &Updates…"));
    connect(checkUpdatesAction, &QAction::triggered, this, &MainWindow::onCheckForUpdates);
    helpMenu->addSeparator();
    auto* about = helpMenu->addAction(tr("&About Gridex"));
    connect(about, &QAction::triggered, this, &MainWindow::onShowAbout);
}

void MainWindow::updateWorkspaceActions() {
    const bool open = workspace_ && workspace_->isOpen();
    newQueryAction_->setEnabled(open);
    disconnectAction_->setEnabled(open);
}

void MainWindow::onNewQueryTab() {
    if (!workspace_ || !workspace_->isOpen()) return;
    if (workspaceView_) workspaceView_->onNewQueryTab();
}

void MainWindow::onDisconnect() {
    if (!workspace_) return;
    workspace_->close();
    stack_->setCurrentIndex(0);
    resize(900, 500);
    updateWorkspaceActions();
}

void MainWindow::setupCentralLayout() {
    // Matches macOS MainView routing: HomeView vs WorkspaceView stacked.
    stack_ = new QStackedWidget(this);

    auto* home = new QWidget(stack_);
    auto* h = new QHBoxLayout(home);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(0);

    brandPanel_ = new HomeBrandingPanel(home);
    connect(brandPanel_, &HomeBrandingPanel::newConnectionRequested,
            this, &MainWindow::onAddConnection);
    connect(brandPanel_, &HomeBrandingPanel::newGroupRequested,
            this, [this] { statusBar()->showMessage(tr("Use right-click on a connection to move it to a group"), 3000); });
    connect(brandPanel_, &HomeBrandingPanel::backupRequested,
            this, &MainWindow::onBackupRequested);
    connect(brandPanel_, &HomeBrandingPanel::restoreRequested,
            this, &MainWindow::onRestoreRequested);
    h->addWidget(brandPanel_);

    auto* divider = new QFrame(home);
    divider->setFrameShape(QFrame::VLine);
    h->addWidget(divider);

    connectionsPanel_ = new ConnectionSidebar(viewModel_.get(), appDb_, home);
    connect(connectionsPanel_, &ConnectionSidebar::addConnectionRequested,
            this, &MainWindow::onAddConnection);
    connect(connectionsPanel_, &ConnectionSidebar::newGroupRequested,
            this, [this] {
                bool ok = false;
                const QString name = QInputDialog::getText(this, tr("New Group"), tr("Group name:"),
                    QLineEdit::Normal, QString{}, &ok);
                if (!ok || name.trimmed().isEmpty()) return;
                statusBar()->showMessage(tr("Group \"%1\" ready — move connections into it via right-click").arg(name), 4000);
            });
    connect(connectionsPanel_, &ConnectionSidebar::editConnectionRequested,
            this, &MainWindow::onEditConnection);
    connect(connectionsPanel_, &ConnectionSidebar::removeConnectionRequested,
            this, &MainWindow::onRemoveConnection);
    connect(connectionsPanel_, &ConnectionSidebar::connectionSelected,
            this, &MainWindow::onConnectionSelected);
    h->addWidget(connectionsPanel_, 1);

    stack_->addWidget(home);

    // Workspace page (built once; listens to WorkspaceState signals).
    workspaceView_ = new WorkspaceView(workspace_.get(), secretStore_.get(), appDb_, stack_);
    connect(workspaceView_, &WorkspaceView::disconnectRequested, this, &MainWindow::onDisconnect);
    connect(workspace_.get(), &WorkspaceState::connectionOpened, this, [this] {
        stack_->setCurrentIndex(1);
        resize(1280, 800);
        updateWorkspaceActions();
    });
    connect(workspace_.get(), &WorkspaceState::connectionClosed, this, [this] {
        stack_->setCurrentIndex(0);
        updateWorkspaceActions();
    });
    stack_->addWidget(workspaceView_);

    setCentralWidget(stack_);
}

void MainWindow::setupStatusBar() {
    statusBar()->showMessage(tr("Ready"));
}

void MainWindow::onAddConnection() {
    ConnectionFormDialog dialog(this);
    connect(&dialog, &ConnectionFormDialog::testRequested, this,
            [this, dlg = &dialog](const ConnectionConfig& cfg,
                                  const std::optional<std::string>& pw) {
                const auto start = std::chrono::steady_clock::now();
                try {
                    ConnectionConfig effective = cfg;
                    std::unique_ptr<SSHTunnelManager> tmpTunnel;

                    // If SSH configured, create a temporary tunnel for the test.
                    if (cfg.sshConfig) {
                        tmpTunnel = std::make_unique<SSHTunnelManager>();
                        auto sshPw = dlg->sshPassword();
                        const auto localPort = tmpTunnel->establish(
                            "test-probe", *cfg.sshConfig,
                            cfg.host.value_or("localhost"),
                            cfg.port.value_or(defaultPort(cfg.databaseType)),
                            sshPw);
                        effective.host = "127.0.0.1";
                        effective.port = static_cast<int>(localPort);
                        // Small delay to let relay thread call accept().
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }

                    auto adapter = createAdapter(cfg.databaseType);
                    adapter->connect(effective, pw);
                    const auto ver = adapter->serverVersion();
                    adapter->disconnect();

                    const auto ms = static_cast<int>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start).count());
                    dlg->showTestResult(true,
                        tr("Connected! %1 (%2ms)")
                            .arg(QString::fromUtf8(ver.c_str())).arg(ms));
                } catch (const std::exception& e) {
                    dlg->showTestResult(false, QString::fromUtf8(e.what()));
                }
            });

    const int result = dialog.exec();
    if (result == ConnectionFormDialog::Cancelled) return;

    auto cfg = dialog.config();
    viewModel_->upsert(cfg);

    if (dialog.storeInKeychain()) {
        if (auto pw = dialog.password()) {
            try { secretStore_->savePassword(cfg.id, *pw); }
            catch (const std::exception& e) {
                statusBar()->showMessage(tr("DB credentials not saved: %1").arg(e.what()), 5000);
            }
        }
        // Save SSH password separately if SSH is configured.
        if (auto sshPw = dialog.sshPassword(); sshPw && cfg.sshConfig) {
            try { secretStore_->saveSSHPassword(cfg.id, *sshPw); }
            catch (const std::exception& e) {
                statusBar()->showMessage(tr("SSH credentials not saved: %1").arg(e.what()), 5000);
            }
        }
    }
    const auto msg = result == ConnectionFormDialog::Connect
                         ? tr("Connected to \"%1\"")
                         : tr("Saved connection \"%1\"");
    statusBar()->showMessage(msg.arg(QString::fromUtf8(cfg.name.c_str())), 3000);
}

void MainWindow::onEditConnection(const QString& id) {
    const auto existing = viewModel_->find(id.toStdString());
    if (!existing) return;

    ConnectionFormDialog dialog(this);
    dialog.setConfig(*existing);
    try {
        if (auto pw = secretStore_->loadPassword(existing->id)) dialog.setPassword(*pw);
    } catch (...) {}
    // Load SSH password too.
    try {
        if (auto sshPw = secretStore_->loadSSHPassword(existing->id)) dialog.setSshPassword(*sshPw);
    } catch (...) {}

    connect(&dialog, &ConnectionFormDialog::testRequested, this,
            [this, dlg = &dialog](const ConnectionConfig& cfg,
                                  const std::optional<std::string>& pw) {
                const auto start = std::chrono::steady_clock::now();
                try {
                    ConnectionConfig effective = cfg;
                    std::unique_ptr<SSHTunnelManager> tmpTunnel;
                    if (cfg.sshConfig) {
                        tmpTunnel = std::make_unique<SSHTunnelManager>();
                        auto sshPw = dlg->sshPassword();
                        const auto localPort = tmpTunnel->establish(
                            "test-probe", *cfg.sshConfig,
                            cfg.host.value_or("localhost"),
                            cfg.port.value_or(defaultPort(cfg.databaseType)),
                            sshPw);
                        effective.host = "127.0.0.1";
                        effective.port = static_cast<int>(localPort);
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                    auto adapter = createAdapter(cfg.databaseType);
                    adapter->connect(effective, pw);
                    const auto ver = adapter->serverVersion();
                    adapter->disconnect();
                    const auto ms = static_cast<int>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start).count());
                    dlg->showTestResult(true,
                        tr("Connected! %1 (%2ms)")
                            .arg(QString::fromUtf8(ver.c_str())).arg(ms));
                } catch (const std::exception& e) {
                    dlg->showTestResult(false, QString::fromUtf8(e.what()));
                }
            });

    const int result = dialog.exec();
    if (result == ConnectionFormDialog::Cancelled) return;
    auto cfg = dialog.config();
    viewModel_->upsert(cfg);

    if (dialog.storeInKeychain()) {
        if (auto pw = dialog.password()) {
            try { secretStore_->savePassword(cfg.id, *pw); }
            catch (const std::exception& e) {
                statusBar()->showMessage(tr("DB credentials not saved: %1").arg(e.what()), 5000);
            }
        }
        if (auto sshPw = dialog.sshPassword(); sshPw && cfg.sshConfig) {
            try { secretStore_->saveSSHPassword(cfg.id, *sshPw); }
            catch (const std::exception& e) {
                statusBar()->showMessage(tr("SSH credentials not saved: %1").arg(e.what()), 5000);
            }
        }
    }
    statusBar()->showMessage(tr("Updated connection \"%1\"").arg(QString::fromUtf8(cfg.name.c_str())), 3000);
}

void MainWindow::onRemoveConnection(const QString& id) {
    const auto existing = viewModel_->find(id.toStdString());
    if (!existing) return;

    const auto btn = QMessageBox::question(
        this, tr("Delete connection"),
        tr("Delete connection \"%1\"? Stored credentials will also be removed.")
            .arg(QString::fromUtf8(existing->name.c_str())));
    if (btn != QMessageBox::Yes) return;

    try { secretStore_->removePassword(existing->id); } catch (...) {}
    viewModel_->remove(existing->id);
    statusBar()->showMessage(tr("Deleted"), 2000);
}

void MainWindow::onConnectionSelected(const QString& id) {
    const auto existing = viewModel_->find(id.toStdString());
    if (!existing) return;
    try {
        workspace_->open(*existing, std::nullopt, secretStore_.get());
        statusBar()->showMessage(
            tr("Connected to \"%1\"")
                .arg(QString::fromUtf8(existing->name.c_str())),
            3000);
    } catch (const GridexError& e) {
        QMessageBox::critical(this, tr("Connection failed"),
                              QString::fromUtf8(e.what()));
    } catch (const std::exception& e) {
        QMessageBox::critical(this, tr("Connection failed"),
                              QString::fromUtf8(e.what()));
    } catch (...) {
        QMessageBox::critical(this, tr("Connection failed"),
                              tr("Unknown error occurred during connection."));
    }
}

void MainWindow::onVmError(const QString& message) {
    statusBar()->showMessage(message, 6000);
}

void MainWindow::onOpenPreferences() {
    SettingsDialog dlg(secretStore_.get(), this);
    dlg.exec();
}

void MainWindow::onOpenMCPServer() {
    if (!mcpWindow_) {
        mcpWindow_ = new MCPWindow(mcpServer_.get(), repo_.get(), this);
        mcpWindow_->setAttribute(Qt::WA_DeleteOnClose);
        connect(mcpWindow_, &QObject::destroyed, this, [this] { mcpWindow_ = nullptr; });
    }
    mcpWindow_->show();
    mcpWindow_->raise();
    mcpWindow_->activateWindow();
}

void MainWindow::onCheckForUpdates() {
    if (!updateService_) return;
    auto* progress = new QProgressDialog(tr("Checking for updates…"), QString(), 0, 0, this);
    progress->setWindowTitle(tr("Gridex Update"));
    progress->setModal(true);
    progress->setMinimumWidth(360);
    progress->show();

    auto conn = std::make_shared<QMetaObject::Connection>();
    *conn = connect(updateService_.get(), &UpdateService::updateChecked, this,
                    [this, progress, conn](const UpdateCheckResult& r) {
        QObject::disconnect(*conn);
        progress->close();
        progress->deleteLater();

        if (!r.errorMessage.isEmpty()) {
            QMessageBox::warning(this, tr("Update check failed"), r.errorMessage);
            return;
        }
        if (!r.hasUpdate) {
            QMessageBox::information(this, tr("You're up to date"),
                tr("Gridex %1 is the latest version.").arg(r.currentVersion));
            return;
        }
        QMessageBox box(this);
        box.setIcon(QMessageBox::Information);
        box.setWindowTitle(tr("Update available"));
        box.setText(tr("Gridex %1 is available (you have %2).").arg(r.newVersion, r.currentVersion));
        if (!r.notes.isEmpty()) box.setInformativeText(r.notes);
        auto* install = box.addButton(tr("Install"), QMessageBox::AcceptRole);
        box.addButton(tr("Later"), QMessageBox::RejectRole);
        box.exec();
        if (box.clickedButton() == install) {
            auto* dlProgress = new QProgressDialog(tr("Downloading…"), tr("Cancel"), 0, 0, this);
            dlProgress->setWindowTitle(tr("Gridex Update"));
            dlProgress->setMinimumWidth(360);
            dlProgress->setModal(true);
            connect(updateService_.get(), &UpdateService::statusChanged,
                    dlProgress, &QProgressDialog::setLabelText);
            connect(updateService_.get(), &UpdateService::errorOccurred, this,
                    [this, dlProgress](const QString& msg) {
                dlProgress->close();
                QMessageBox::warning(this, tr("Update failed"), msg);
            });
            dlProgress->show();
            updateService_->downloadAndApply(r.downloadUrl, r.sha256);
        }
    });
    updateService_->checkForUpdate();
}

void MainWindow::onShowAbout() {
    QMessageBox::about(this, tr("About Gridex"), tr(
        "<h2>Gridex</h2>"
        "<p><b>AI-Native Database IDE</b> (Linux build)</p>"
        "<p>Version %1 — 2026</p>").arg(UpdateService::currentVersion()) + tr(
        "<p>Connects to PostgreSQL, MySQL, SQLite, Redis, MongoDB, MSSQL with "
        "SSH tunnel support. Ships an AI chat assistant that speaks your "
        "schema.</p>"
        "<p>© 2026 Vurakit. MIT-licensed third-party components.</p>"));
}

void MainWindow::onShowShortcuts() {
    const QString html = tr(
        "<h3>Keyboard Shortcuts</h3>"
        "<table cellpadding='4' cellspacing='2'>"
        "<tr><th align='left'>Key</th><th align='left'>Action</th></tr>"
        "<tr><td><b>Ctrl+T</b></td><td>New Connection</td></tr>"
        "<tr><td><b>Ctrl+Shift+N</b></td><td>New Query Tab</td></tr>"
        "<tr><td><b>Ctrl+W</b></td><td>Close Tab</td></tr>"
        "<tr><td><b>Ctrl+Enter</b></td><td>Run Query</td></tr>"
        "<tr><td><b>Ctrl+Space</b></td><td>Autocomplete</td></tr>"
        "<tr><td><b>Ctrl+S</b></td><td>Commit pending changes</td></tr>"
        "<tr><td><b>Delete</b></td><td>Delete selected row(s)</td></tr>"
        "<tr><td><b>F5</b></td><td>Refresh table list</td></tr>"
        "<tr><td><b>Ctrl+,</b></td><td>Preferences</td></tr>"
        "<tr><td><b>Ctrl+/</b></td><td>This shortcut list</td></tr>"
        "<tr><td><b>Ctrl+Q</b></td><td>Quit</td></tr>"
        "</table>");
    QMessageBox box(this);
    box.setWindowTitle(tr("Keyboard Shortcuts"));
    box.setTextFormat(Qt::RichText);
    box.setText(html);
    box.setStandardButtons(QMessageBox::Ok);
    box.exec();
}

// ---- Backup / Restore (Home screen buttons) ----

namespace {

// Shows a picker over saved connections. Returns nullopt on cancel.
std::optional<ConnectionConfig> pickConnection(QWidget* parent,
                                               const std::vector<ConnectionConfig>& all,
                                               const QString& title) {
    if (all.empty()) {
        QMessageBox::information(parent, title,
            QObject::tr("No saved connections. Add one first."));
        return std::nullopt;
    }
    QStringList labels;
    labels.reserve(static_cast<int>(all.size()));
    for (const auto& c : all) {
        const QString name = QString::fromStdString(c.name);
        const QString host = QString::fromStdString(c.displayHost());
        labels << QString("%1 — %2").arg(name, host);
    }
    bool ok = false;
    const QString picked = QInputDialog::getItem(
        parent, title, QObject::tr("Select a connection:"),
        labels, 0, /*editable*/ false, &ok);
    if (!ok) return std::nullopt;
    const int idx = labels.indexOf(picked);
    if (idx < 0) return std::nullopt;
    return all[static_cast<std::size_t>(idx)];
}

}  // namespace

void MainWindow::onBackupRequested() {
    const auto cfg = pickConnection(this, viewModel_->connections(), tr("Backup Database"));
    if (!cfg) return;

    if (!DatabaseDumpRunner::isSupported(cfg->databaseType)) {
        QMessageBox::warning(this, tr("Backup"),
            tr("Backup is not supported for this database type."));
        return;
    }

    QString defName = QString::fromStdString(cfg->database.value_or(cfg->name));
    if (defName.isEmpty()) defName = "backup";
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Backup Database — %1").arg(QString::fromStdString(cfg->name)),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
            + "/" + defName + ".sql",
        tr("SQL Dump (*.sql);;All files (*)"));
    if (path.isEmpty()) return;

    std::optional<std::string> pw;
    if (secretStore_ && secretStore_->isAvailable()) pw = secretStore_->loadPassword(cfg->id);

    statusBar()->showMessage(tr("Backing up…"), 0);
    QApplication::processEvents();
    const auto r = DatabaseDumpRunner::backup(*cfg, pw, path.toStdString());
    statusBar()->clearMessage();

    if (!r.success) {
        QMessageBox::critical(this, tr("Backup Failed"),
            QString::fromStdString(r.errorOutput.empty()
                ? ("Exit code " + std::to_string(r.exitCode))
                : r.errorOutput));
        return;
    }
    QMessageBox::information(this, tr("Backup"),
        tr("Database dumped to:\n%1").arg(path));
}

void MainWindow::onRestoreRequested() {
    const auto cfg = pickConnection(this, viewModel_->connections(), tr("Restore Database"));
    if (!cfg) return;

    if (!DatabaseDumpRunner::isSupported(cfg->databaseType)) {
        QMessageBox::warning(this, tr("Restore"),
            tr("Restore is not supported for this database type."));
        return;
    }

    const QString path = QFileDialog::getOpenFileName(
        this, tr("Restore Database — %1").arg(QString::fromStdString(cfg->name)),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
        tr("SQL Dump (*.sql);;All files (*)"));
    if (path.isEmpty()) return;

    const auto confirm = QMessageBox::warning(
        this, tr("Restore Database"),
        tr("This will execute SQL against \"%1\" from:\n%2\n\nExisting data may be overwritten. Continue?")
            .arg(QString::fromStdString(cfg->name), path),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (confirm != QMessageBox::Yes) return;

    std::optional<std::string> pw;
    if (secretStore_ && secretStore_->isAvailable()) pw = secretStore_->loadPassword(cfg->id);

    statusBar()->showMessage(tr("Restoring…"), 0);
    QApplication::processEvents();
    const auto r = DatabaseDumpRunner::restore(*cfg, pw, path.toStdString());
    statusBar()->clearMessage();

    if (!r.success) {
        QMessageBox::critical(this, tr("Restore Failed"),
            QString::fromStdString(r.errorOutput.empty()
                ? ("Exit code " + std::to_string(r.exitCode))
                : r.errorOutput));
        return;
    }
    QMessageBox::information(this, tr("Restore"),
        tr("Database restored from:\n%1").arg(path));
}

void MainWindow::onExportConnections() {
    const auto& conns = viewModel_->connections();
    if (conns.empty()) {
        QMessageBox::information(this, tr("Export Connections"),
            tr("No connections to export."));
        return;
    }

    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export Connections"),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
            + "/gridex-connections.json",
        tr("JSON (*.json)"));
    if (path.isEmpty()) return;

    QJsonArray arr;
    for (const auto& c : conns) {
        QJsonObject obj;
        obj[QStringLiteral("id")]           = QString::fromStdString(c.id);
        obj[QStringLiteral("name")]         = QString::fromStdString(c.name);
        obj[QStringLiteral("databaseType")] = QString::fromUtf8(rawValue(c.databaseType).data(),
                                                static_cast<qsizetype>(rawValue(c.databaseType).size()));
        obj[QStringLiteral("sslEnabled")]   = c.sslEnabled;
        if (c.host)     obj[QStringLiteral("host")]     = QString::fromStdString(*c.host);
        if (c.port)     obj[QStringLiteral("port")]     = *c.port;
        if (c.database) obj[QStringLiteral("database")] = QString::fromStdString(*c.database);
        if (c.username) obj[QStringLiteral("username")] = QString::fromStdString(*c.username);
        if (c.group)    obj[QStringLiteral("group")]    = QString::fromStdString(*c.group);
        if (c.filePath) obj[QStringLiteral("filePath")] = QString::fromStdString(*c.filePath);
        obj[QStringLiteral("password")] = QStringLiteral("__RE_PROMPT__");
        if (c.sshConfig) {
            QJsonObject ssh;
            ssh[QStringLiteral("host")]      = QString::fromStdString(c.sshConfig->host);
            ssh[QStringLiteral("port")]      = c.sshConfig->port;
            ssh[QStringLiteral("username")]  = QString::fromStdString(c.sshConfig->username);
            const auto am = rawValue(c.sshConfig->authMethod);
            ssh[QStringLiteral("authMethod")] = QString::fromUtf8(am.data(),
                                                    static_cast<qsizetype>(am.size()));
            if (c.sshConfig->keyPath)
                ssh[QStringLiteral("keyPath")] = QString::fromStdString(*c.sshConfig->keyPath);
            ssh[QStringLiteral("sshPassword")] = QStringLiteral("__RE_PROMPT__");
            obj[QStringLiteral("sshConfig")] = ssh;
        }
        arr.append(obj);
    }

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::critical(this, tr("Export Failed"),
            tr("Cannot write to: %1").arg(path));
        return;
    }
    f.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
    f.close();
    statusBar()->showMessage(
        tr("Exported %1 connection(s) to %2").arg(conns.size()).arg(path), 4000);
}

void MainWindow::onImportConnections() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Import Connections"),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
        tr("JSON (*.json);;All files (*)"));
    if (path.isEmpty()) return;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::critical(this, tr("Import Failed"),
            tr("Cannot open: %1").arg(path));
        return;
    }
    const QByteArray raw = f.readAll();
    f.close();

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
    if (err.error != QJsonParseError::NoError) {
        QMessageBox::critical(this, tr("Import Failed"),
            tr("Invalid JSON: %1").arg(err.errorString()));
        return;
    }
    if (!doc.isArray()) {
        QMessageBox::critical(this, tr("Import Failed"),
            tr("Expected a JSON array of connections."));
        return;
    }

    int imported = 0;
    QStringList errors;
    for (const auto& val : doc.array()) {
        if (!val.isObject()) continue;
        const QJsonObject obj = val.toObject();

        // Build a stripped JSON string for the existing decoder (no password fields)
        QJsonObject stripped = obj;
        stripped.remove(QStringLiteral("password"));
        stripped.remove(QStringLiteral("sshPassword"));
        if (stripped.contains(QStringLiteral("sshConfig"))) {
            QJsonObject ssh = stripped[QStringLiteral("sshConfig")].toObject();
            ssh.remove(QStringLiteral("sshPassword"));
            stripped[QStringLiteral("sshConfig")] = ssh;
        }

        try {
            const std::string cfgJson = QJsonDocument(stripped).toJson(QJsonDocument::Compact).toStdString();
            const ConnectionConfig cfg = json::decode(cfgJson);
            viewModel_->upsert(cfg);

            const QString pw = obj.value(QStringLiteral("password")).toString();
            if (pw == QStringLiteral("__RE_PROMPT__")) {
                bool ok = false;
                const QString entered = QInputDialog::getText(
                    this,
                    tr("Password for \"%1\"").arg(QString::fromStdString(cfg.name)),
                    tr("Enter password (leave blank to skip):"),
                    QLineEdit::Password, QString{}, &ok);
                if (ok && !entered.isEmpty() && secretStore_) {
                    try { secretStore_->savePassword(cfg.id, entered.toStdString()); }
                    catch (...) {}
                }
            }
            ++imported;
        } catch (const std::exception& e) {
            errors << QString::fromUtf8(e.what());
        }
    }

    if (errors.isEmpty()) {
        QMessageBox::information(this, tr("Import Connections"),
            tr("Imported %1 connection(s).").arg(imported));
    } else {
        QMessageBox::warning(this, tr("Import Connections"),
            tr("Imported %1 connection(s) with errors:\n\n%2")
                .arg(imported).arg(errors.join("\n")));
    }
}

}
