#pragma once
#include <string>
#include <vector>
#include <ShlObj.h>
#include <sqlite3.h>
#include <wincrypt.h>
#include "ConnectionConfig.h"

#pragma comment(lib, "Crypt32.lib")

namespace DBModels
{
    class ConnectionStore
    {
    public:
        // Load all saved connections
        static std::vector<ConnectionConfig> Load()
        {
            auto dbPath = GetDbPath();
            sqlite3* db = nullptr;
            if (sqlite3_open16(dbPath.c_str(), &db) != SQLITE_OK)
                return {};

            EnsureTable(db);

            std::vector<ConnectionConfig> results;
            const char* sql =
                "SELECT id, name, type, host, port, database_name, username, "
                "password_enc, ssl_enabled, ssl_mode, color_tag, grp, file_path, "
                "sort_order, created_at, last_connected_at, connection_uri, "
                "mongo_options, "
                "ssh_host, ssh_port, ssh_username, ssh_password_enc, "
                "ssh_auth_method, ssh_key_path, mcp_mode, tag "
                "FROM connections ORDER BY sort_order, name";

            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK)
            {
                while (sqlite3_step(stmt) == SQLITE_ROW)
                {
                    ConnectionConfig c;
                    c.id = ColText16(stmt, 0);
                    c.name = ColText16(stmt, 1);
                    c.databaseType = static_cast<DatabaseType>(sqlite3_column_int(stmt, 2));
                    c.host = ColText16(stmt, 3);
                    c.port = static_cast<uint16_t>(sqlite3_column_int(stmt, 4));
                    c.database = ColText16(stmt, 5);
                    c.username = ColText16(stmt, 6);
                    c.password = DecryptPassword(ColText8(stmt, 7));
                    c.sslEnabled = sqlite3_column_int(stmt, 8) != 0;
                    c.sslMode = static_cast<SSLMode>(sqlite3_column_int(stmt, 9));
                    int ct = sqlite3_column_int(stmt, 10);
                    if (ct >= 0) c.colorTag = static_cast<ColorTag>(ct);
                    c.group = ColText16(stmt, 11);
                    c.filePath = ColText16(stmt, 12);
                    // Column 16-17: MongoDB fields
                    c.connectionUri = ColText16(stmt, 16);
                    c.mongoOptions = ColText16(stmt, 17);
                    // Column 18-23: SSH tunnel fields
                    {
                        auto sshHost = ColText16(stmt, 18);
                        if (!sshHost.empty())
                        {
                            SSHTunnelConfig ssh;
                            ssh.host = sshHost;
                            ssh.port = static_cast<uint16_t>(sqlite3_column_int(stmt, 19));
                            if (ssh.port == 0) ssh.port = 22;
                            ssh.username = ColText16(stmt, 20);
                            ssh.password = DecryptPassword(ColText8(stmt, 21));
                            ssh.authMethod = static_cast<SSHAuthMethod>(sqlite3_column_int(stmt, 22));
                            ssh.keyPath = ColText16(stmt, 23);
                            c.sshConfig = ssh;
                        }
                    }
                    // Column 24: per-connection MCP mode. Defaults to
                    // Locked (0) for rows created before the migration.
                    c.mcpMode = static_cast<MCPConnectionMode>(sqlite3_column_int(stmt, 24));
                    // Column 25: free-form environment tag. Empty
                    // string for legacy rows pre-migration.
                    c.tag = ColText16(stmt, 25);
                    results.push_back(c);
                }
                sqlite3_finalize(stmt);
            }

            sqlite3_close(db);
            return results;
        }

        // Save (insert or update) a single connection
        static void Save(const ConnectionConfig& c)
        {
            auto dbPath = GetDbPath();
            sqlite3* db = nullptr;
            if (sqlite3_open16(dbPath.c_str(), &db) != SQLITE_OK)
                return;

            EnsureTable(db);

            const char* sql =
                "INSERT OR REPLACE INTO connections "
                "(id, name, type, host, port, database_name, username, "
                "password_enc, ssl_enabled, ssl_mode, color_tag, grp, file_path, "
                "sort_order, created_at, last_connected_at, connection_uri, "
                "mongo_options, "
                "ssh_host, ssh_port, ssh_username, ssh_password_enc, "
                "ssh_auth_method, ssh_key_path, mcp_mode, tag) "
                "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,0, "
                "COALESCE((SELECT created_at FROM connections WHERE id=?), datetime('now')), "
                "datetime('now'), ?, ?, ?,?,?,?,?,?,?,?)";

            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK)
            {
                auto id8 = ToUtf8(c.id);
                BindText(stmt, 1, id8);
                BindText(stmt, 2, ToUtf8(c.name));
                sqlite3_bind_int(stmt, 3, static_cast<int>(c.databaseType));
                BindText(stmt, 4, ToUtf8(c.host));
                sqlite3_bind_int(stmt, 5, c.port);
                BindText(stmt, 6, ToUtf8(c.database));
                BindText(stmt, 7, ToUtf8(c.username));
                BindText(stmt, 8, EncryptPassword(c.password));
                sqlite3_bind_int(stmt, 9, c.sslEnabled ? 1 : 0);
                sqlite3_bind_int(stmt, 10, static_cast<int>(c.sslMode));
                sqlite3_bind_int(stmt, 11, c.colorTag.has_value()
                    ? static_cast<int>(c.colorTag.value()) : -1);
                BindText(stmt, 12, ToUtf8(c.group));
                BindText(stmt, 13, ToUtf8(c.filePath));
                BindText(stmt, 14, id8); // for COALESCE created_at
                BindText(stmt, 15, ToUtf8(c.connectionUri)); // MongoDB URI
                BindText(stmt, 16, ToUtf8(c.mongoOptions)); // MongoDB options
                // SSH tunnel fields (17-22)
                if (c.sshConfig.has_value())
                {
                    BindText(stmt, 17, ToUtf8(c.sshConfig->host));
                    sqlite3_bind_int(stmt, 18, c.sshConfig->port);
                    BindText(stmt, 19, ToUtf8(c.sshConfig->username));
                    BindText(stmt, 20, EncryptPassword(c.sshConfig->password));
                    sqlite3_bind_int(stmt, 21, static_cast<int>(c.sshConfig->authMethod));
                    BindText(stmt, 22, ToUtf8(c.sshConfig->keyPath));
                }
                else
                {
                    BindText(stmt, 17, "");
                    sqlite3_bind_int(stmt, 18, 0);
                    BindText(stmt, 19, "");
                    BindText(stmt, 20, "");
                    sqlite3_bind_int(stmt, 21, 0);
                    BindText(stmt, 22, "");
                }
                // Placeholder 23: per-connection MCP mode. Defaults
                // to Locked so new rows never auto-expose data to AI.
                sqlite3_bind_int(stmt, 23, static_cast<int>(c.mcpMode));
                // Placeholder 24: free-form environment tag.
                BindText(stmt, 24, ToUtf8(c.tag));
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
            }

            sqlite3_close(db);
        }

        // Save all connections (bulk)
        static void SaveAll(const std::vector<ConnectionConfig>& connections)
        {
            for (auto& c : connections)
                Save(c);
        }

        // Delete a connection by id
        static void Delete(const std::wstring& id)
        {
            auto dbPath = GetDbPath();
            sqlite3* db = nullptr;
            if (sqlite3_open16(dbPath.c_str(), &db) != SQLITE_OK)
                return;

            const char* sql = "DELETE FROM connections WHERE id=?";
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK)
            {
                BindText(stmt, 1, ToUtf8(id));
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
            }
            sqlite3_close(db);
        }

    private:
        static void EnsureTable(sqlite3* db)
        {
            const char* sql =
                "CREATE TABLE IF NOT EXISTS connections ("
                "  id TEXT PRIMARY KEY,"
                "  name TEXT NOT NULL,"
                "  type INTEGER NOT NULL DEFAULT 0,"
                "  host TEXT DEFAULT '',"
                "  port INTEGER DEFAULT 5432,"
                "  database_name TEXT DEFAULT '',"
                "  username TEXT DEFAULT '',"
                "  password_enc TEXT DEFAULT '',"
                "  ssl_enabled INTEGER DEFAULT 0,"
                "  ssl_mode INTEGER DEFAULT 0,"
                "  color_tag INTEGER DEFAULT -1,"
                "  grp TEXT DEFAULT '',"
                "  file_path TEXT DEFAULT '',"
                "  sort_order INTEGER DEFAULT 0,"
                "  created_at TEXT DEFAULT (datetime('now')),"
                "  last_connected_at TEXT,"
                "  connection_uri TEXT DEFAULT ''"
                ")";
            sqlite3_exec(db, sql, nullptr, nullptr, nullptr);

            // Migration: add connection_uri + mongo_options for MongoDB support
            sqlite3_exec(db,
                "ALTER TABLE connections ADD COLUMN connection_uri TEXT DEFAULT ''",
                nullptr, nullptr, nullptr);
            sqlite3_exec(db,
                "ALTER TABLE connections ADD COLUMN mongo_options TEXT DEFAULT ''",
                nullptr, nullptr, nullptr);
            // Migration: SSH tunnel fields
            sqlite3_exec(db, "ALTER TABLE connections ADD COLUMN ssh_host TEXT DEFAULT ''", nullptr, nullptr, nullptr);
            sqlite3_exec(db, "ALTER TABLE connections ADD COLUMN ssh_port INTEGER DEFAULT 22", nullptr, nullptr, nullptr);
            sqlite3_exec(db, "ALTER TABLE connections ADD COLUMN ssh_username TEXT DEFAULT ''", nullptr, nullptr, nullptr);
            sqlite3_exec(db, "ALTER TABLE connections ADD COLUMN ssh_password_enc TEXT DEFAULT ''", nullptr, nullptr, nullptr);
            sqlite3_exec(db, "ALTER TABLE connections ADD COLUMN ssh_auth_method INTEGER DEFAULT 0", nullptr, nullptr, nullptr);
            sqlite3_exec(db, "ALTER TABLE connections ADD COLUMN ssh_key_path TEXT DEFAULT ''", nullptr, nullptr, nullptr);
            // Migration: per-connection MCP access mode (0=Locked, 1=ReadOnly, 2=ReadWrite).
            // Default 0 keeps existing connections firewalled from AI until the
            // user explicitly opens them up in the MCP Connections tab.
            sqlite3_exec(db, "ALTER TABLE connections ADD COLUMN mcp_mode INTEGER DEFAULT 0", nullptr, nullptr, nullptr);
            // Migration: free-form environment tag (Production / Staging /
            // Development / Testing / Local / "" for None) picked in the
            // ConnectionFormDialog's TagCombo. Was rendered in XAML but
            // never persisted — first run after this migration adds the
            // column with empty default; existing connections get '' too.
            sqlite3_exec(db, "ALTER TABLE connections ADD COLUMN tag TEXT DEFAULT ''", nullptr, nullptr, nullptr);
        }

        static std::wstring GetDbPath()
        {
            wchar_t* appData = nullptr;
            SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData);
            std::wstring dir = std::wstring(appData) + L"\\Gridex";
            CoTaskMemFree(appData);
            CreateDirectoryW(dir.c_str(), nullptr);
            return dir + L"\\gridex.db";
        }

        // ── DPAPI password encryption ───────────────
        static std::string EncryptPassword(const std::wstring& password)
        {
            if (password.empty()) return "";
            std::string utf8 = ToUtf8(password);

            DATA_BLOB input;
            input.pbData = reinterpret_cast<BYTE*>(utf8.data());
            input.cbData = static_cast<DWORD>(utf8.size());

            DATA_BLOB output = {};
            if (!CryptProtectData(&input, nullptr, nullptr, nullptr, nullptr, 0, &output))
                return "";

            // Encode as hex
            std::string hex;
            hex.reserve(output.cbData * 2);
            for (DWORD i = 0; i < output.cbData; i++)
            {
                char buf[3];
                snprintf(buf, sizeof(buf), "%02x", output.pbData[i]);
                hex += buf;
            }
            LocalFree(output.pbData);
            return hex;
        }

        static std::wstring DecryptPassword(const std::string& hexEnc)
        {
            if (hexEnc.empty()) return L"";

            // Decode hex
            std::vector<BYTE> bytes;
            bytes.reserve(hexEnc.size() / 2);
            for (size_t i = 0; i + 1 < hexEnc.size(); i += 2)
            {
                BYTE b = static_cast<BYTE>(std::stoi(hexEnc.substr(i, 2), nullptr, 16));
                bytes.push_back(b);
            }

            DATA_BLOB input;
            input.pbData = bytes.data();
            input.cbData = static_cast<DWORD>(bytes.size());

            DATA_BLOB output = {};
            if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, 0, &output))
                return L"";

            std::string utf8(reinterpret_cast<char*>(output.pbData), output.cbData);
            LocalFree(output.pbData);
            return FromUtf8(utf8);
        }

        // ── Helpers ─────────────────────────────────
        static std::string ToUtf8(const std::wstring& w)
        {
            if (w.empty()) return {};
            int sz = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
            std::string r(sz, '\0');
            WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &r[0], sz, nullptr, nullptr);
            return r;
        }

        static std::wstring FromUtf8(const std::string& s)
        {
            if (s.empty()) return {};
            int sz = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
            std::wstring r(sz, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &r[0], sz);
            return r;
        }

        static std::wstring ColText16(sqlite3_stmt* stmt, int col)
        {
            auto* text = sqlite3_column_text(stmt, col);
            if (!text) return L"";
            return FromUtf8(reinterpret_cast<const char*>(text));
        }

        static std::string ColText8(sqlite3_stmt* stmt, int col)
        {
            auto* text = sqlite3_column_text(stmt, col);
            if (!text) return "";
            return reinterpret_cast<const char*>(text);
        }

        static void BindText(sqlite3_stmt* stmt, int idx, const std::string& val)
        {
            sqlite3_bind_text(stmt, idx, val.c_str(), (int)val.size(), SQLITE_TRANSIENT);
        }
    };
}
