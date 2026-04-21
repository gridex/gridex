#include <windows.h>
#include "Models/ConnectionManager.h"
#include "Models/PostgreSQLAdapter.h"
#include "Models/MySQLAdapter.h"
#include "Models/SQLiteAdapter.h"
#include "Models/RedisAdapter.h"
#include "Models/MongoDBAdapter.h"
#include "Models/MSSQLAdapter.h"
#include "Models/ClickHouseAdapter.h"

namespace DBModels
{
    // Helper: if SSH tunnel is configured, establish it and rewrite
    // the connection config to point at localhost:localPort instead
    // of the remote DB host. Returns the (possibly modified) config.
    static ConnectionConfig applySSHTunnel(
        const ConnectionConfig& config,
        const std::wstring& password,
        std::unique_ptr<SSHTunnelService>& tunnel)
    {
        if (!config.sshConfig.has_value())
            return config;

        auto& ssh = config.sshConfig.value();
        if (ssh.host.empty())
            return config; // SSH not actually configured

        // Use SSH-specific password from config. Falls back to DB
        // password only if SSH password is empty (backward compat).
        auto sshPass = ssh.password.empty() ? password : ssh.password;

        auto remoteHost = std::string(
            config.host.begin(), config.host.end());
        int remotePort = config.port > 0 ? config.port : 5432;

        tunnel = std::make_unique<SSHTunnelService>();
        auto localPort = tunnel->establish(
            ssh, sshPass, remoteHost, remotePort);

        // Rewrite config to connect through tunnel
        ConnectionConfig tunneled = config;
        tunneled.host = L"127.0.0.1";
        tunneled.port = localPort;
        tunneled.sshConfig.reset(); // don't recurse
        return tunneled;
    }

    std::shared_ptr<DatabaseAdapter> ConnectionManager::connect(
        const ConnectionConfig& config,
        const std::wstring& password)
    {
        // Disconnect existing connection + tear down old tunnel
        disconnect();

        // Establish SSH tunnel if configured
        auto effectiveConfig = applySSHTunnel(config, password, sshTunnel_);

        activeAdapter_ = createAdapter(config.databaseType);
        activeConfig_ = config; // store ORIGINAL config (not tunneled)
        activeAdapter_->connect(effectiveConfig, password);

        return activeAdapter_;
    }

    void ConnectionManager::disconnect()
    {
        if (activeAdapter_)
        {
            if (activeAdapter_->isConnected())
                activeAdapter_->disconnect();
            activeAdapter_.reset();
        }
        // Tear down SSH tunnel after DB disconnect
        if (sshTunnel_)
        {
            sshTunnel_->close();
            sshTunnel_.reset();
        }
    }

    bool ConnectionManager::testConnection(
        const ConnectionConfig& config,
        const std::wstring& password)
    {
        // Test with SSH tunnel if configured
        std::unique_ptr<SSHTunnelService> testTunnel;
        auto effectiveConfig = applySSHTunnel(config, password, testTunnel);

        auto adapter = createAdapter(config.databaseType);
        bool ok = adapter->testConnection(effectiveConfig, password);

        if (testTunnel) testTunnel->close();
        return ok;
    }

    bool ConnectionManager::testConnectionWithError(
        const ConnectionConfig& config,
        const std::wstring& password,
        std::wstring& outError)
    {
        outError.clear();

        std::unique_ptr<SSHTunnelService> testTunnel;
        auto effectiveConfig = applySSHTunnel(config, password, testTunnel);

        auto adapter = createAdapter(config.databaseType);

        // Call adapter->connect directly instead of the bool
        // testConnection shortcut: connect() throws DatabaseError with
        // the driver's own message, which is exactly what we want to
        // surface in the UI.
        bool ok = false;
        try
        {
            adapter->connect(effectiveConfig, password);
            auto result = adapter->execute(L"SELECT 1");
            adapter->disconnect();
            ok = result.success;
            if (!ok) outError = result.error;
        }
        catch (const std::exception& e)
        {
            const std::string w = e.what();
            outError.assign(w.begin(), w.end());
        }
        catch (...)
        {
            outError = L"Unknown error";
        }

        if (testTunnel) testTunnel->close();
        return ok;
    }

    std::shared_ptr<DatabaseAdapter> ConnectionManager::createAdapter(DatabaseType type)
    {
        switch (type)
        {
        case DatabaseType::PostgreSQL:
            return std::make_shared<PostgreSQLAdapter>();
        case DatabaseType::MySQL:
            return std::make_shared<MySQLAdapter>();
        case DatabaseType::SQLite:
            return std::make_shared<SQLiteAdapter>();
        case DatabaseType::Redis:
            return std::make_shared<RedisAdapter>();
        case DatabaseType::MongoDB:
            return std::make_shared<MongoDBAdapter>();
        case DatabaseType::MSSQLServer:
            return std::make_shared<MSSQLAdapter>();
        case DatabaseType::ClickHouse:
            return std::make_shared<ClickHouseAdapter>();
        default:
            throw DatabaseError(DatabaseError::Code::ConnectionFailed,
                "Unsupported database type");
        }
    }
}
