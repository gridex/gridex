#include <QApplication>
#include <QSettings>
#include <QStringList>
#include <QStyleFactory>
#include <QThread>
#include <cstdio>
#include <exception>
#include <memory>

#include "App/MCP/MCPConnectionProvider.h"
#include "Data/Keychain/SecretStore.h"
#include "Data/Persistence/AppConnectionRepository.h"
#include "Data/Persistence/AppDatabase.h"
#include "Presentation/Theme/ThemeManager.h"
#include "Presentation/Views/ExplainVisualizer/ExplainExtension.h"
#include "Presentation/Windows/Main/MainWindow.h"
#include "Services/MCP/MCPServer.h"

namespace {

void seedConnectionModesFromSettings(gridex::mcp::MCPServer& server,
                                     gridex::AppConnectionRepository& repo) {
    QSettings s;
    for (const auto& c : repo.fetchAll()) {
        QString id = QString::fromStdString(c.id);
        QString raw = s.value(QStringLiteral("mcp.connectionMode.") + id).toString();
        auto m = gridex::mcpConnectionModeFromRaw(raw.toStdString());
        if (m) server.setConnectionMode(c.id, *m);
    }
}

int runMcpStdio(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Gridex"));
    QApplication::setOrganizationName(QStringLiteral("Vurakit"));
    QApplication::setOrganizationDomain(QStringLiteral("vurakit.com"));

    auto db = std::make_shared<gridex::AppDatabase>();
    db->open();

    auto repo        = std::make_unique<gridex::AppConnectionRepository>(db);
    auto secretStore = std::make_unique<gridex::SecretStore>();
    auto provider    = std::make_unique<gridex::MCPConnectionProvider>(repo.get(), secretStore.get());

    std::shared_ptr<gridex::mcp::IMCPConnectionProvider> shim(provider.get(), [](auto*){});
    auto server = std::make_unique<gridex::mcp::MCPServer>(
        shim, "1.0.0", gridex::mcp::MCPTransportMode::Stdio);

    seedConnectionModesFromSettings(*server, *repo);

    QSettings s;
    gridex::mcp::RateLimits rl;
    rl.queriesPerMinute = s.value("mcp.rateLimit.queriesPerMinute", 60).toInt();
    rl.queriesPerHour   = s.value("mcp.rateLimit.queriesPerHour",   1000).toInt();
    rl.writesPerMinute  = s.value("mcp.rateLimit.writesPerMinute",  10).toInt();
    rl.ddlPerMinute     = s.value("mcp.rateLimit.ddlPerMinute",     1).toInt();
    server->rateLimiter().setLimits(rl);

    server->start();
    // Background watcher quits Qt when stdin EOF causes the transport to stop.
    QThread* watcher = QThread::create([&]() {
        while (server->isRunning()) QThread::msleep(250);
        QMetaObject::invokeMethod(qApp, "quit", Qt::QueuedConnection);
    });
    watcher->start();
    int code = QApplication::exec();
    server->stop();
    watcher->wait(1000);
    return code;
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        QStringList args;
        for (int i = 1; i < argc; ++i) args << QString::fromUtf8(argv[i]);
        if (args.contains(QStringLiteral("--mcp-stdio"))) {
            return runMcpStdio(argc, argv);
        }

        QApplication app(argc, argv);

        QApplication::setApplicationName(QStringLiteral("Gridex"));
        QApplication::setOrganizationName(QStringLiteral("Vurakit"));
        QApplication::setOrganizationDomain(QStringLiteral("vurakit.com"));

        if (auto* fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
            QApplication::setStyle(fusion);
        }

        gridex::ThemeManager::instance().apply(&app);

        gridex::explain::registerQueryEditorExtension();

        gridex::MainWindow window;
        window.show();

        return QApplication::exec();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FATAL: %s\n", e.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "FATAL: unknown exception\n");
        return 1;
    }
}
