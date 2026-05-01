#pragma once
#include <string>
#include <optional>
#include <cstdint>
#include "DatabaseType.h"
#include "ColorTag.h"
#include "MCP/MCPConnectionMode.h"

namespace DBModels
{
    enum class SSHAuthMethod
    {
        Password,
        PrivateKey,
        KeyWithPassphrase
    };

    enum class SSLMode
    {
        Preferred,
        Disabled,
        Required,
        VerifyCA,
        VerifyIdentity
    };

    struct SSHTunnelConfig
    {
        std::wstring host;
        uint16_t port = 22;
        std::wstring username;
        std::wstring password;  // SSH password (separate from DB password)
        SSHAuthMethod authMethod = SSHAuthMethod::Password;
        std::wstring keyPath;
    };

    struct ConnectionConfig
    {
        std::wstring id;
        std::wstring name;
        DatabaseType databaseType = DatabaseType::PostgreSQL;
        std::wstring host;
        uint16_t port = 5432;
        std::wstring database;
        std::wstring username;
        std::wstring password;  // stored encrypted in JSON via DPAPI
        bool sslEnabled = false;
        SSLMode sslMode = SSLMode::Preferred;
        std::optional<ColorTag> colorTag;
        // Free-form environment tag picked from the form's TagCombo
        // (Production / Staging / Development / Testing / Local).
        // Empty = "None". Stored as a plain string so we can extend
        // the dropdown later without a schema migration.
        std::wstring tag;
        std::wstring group;
        std::wstring filePath;  // SQLite file path
        std::wstring connectionUri;  // MongoDB URI (mongodb:// or mongodb+srv://)
        // Arbitrary URI query params for MongoDB form-based connections.
        // Appended as ?key=val&key=val when building the URI from
        // host/port/user/pass fields. Common values:
        //   authSource=admin
        //   replicaSet=rs0
        //   tls=true
        //   authMechanism=SCRAM-SHA-256
        std::wstring mongoOptions;
        std::optional<SSHTunnelConfig> sshConfig;

        // Per-connection MCP access gate. Default Locked — every new
        // connection must be explicitly opened up in MCPConnectionsView.
        // Mirrors mac's ConnectionConfig.mcpMode.
        MCPConnectionMode mcpMode = MCPConnectionMode::Locked;

        std::wstring subtitle() const
        {
            if (databaseType == DatabaseType::SQLite)
                return filePath.empty() ? L"No file selected" : filePath;
            // MongoDB: show truncated URI if provided
            if (databaseType == DatabaseType::MongoDB && !connectionUri.empty())
            {
                auto uri = connectionUri;
                return uri.size() > 60 ? uri.substr(0, 57) + L"..." : uri;
            }
            std::wstring result = host;
            if (port > 0) result += L":" + std::to_wstring(port);
            if (!database.empty()) result += L"/" + database;
            return result.empty() ? L"Not configured" : result;
        }
    };
}
