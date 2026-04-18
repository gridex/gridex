#include <windows.h>
#include "Models/MySQLAdapter.h"
#include <mysql/mysql.h>
#include <chrono>

namespace DBModels
{
    // ── UTF-8 helpers (same pattern as PostgreSQLAdapter) ──
    std::string MySQLAdapter::toUtf8(const std::wstring& wstr)
    {
        if (wstr.empty()) return {};
        int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(),
            static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
        std::string result(size, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(),
            static_cast<int>(wstr.size()), &result[0], size, nullptr, nullptr);
        return result;
    }

    std::wstring MySQLAdapter::fromUtf8(const std::string& str)
    {
        if (str.empty()) return {};
        int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(),
            static_cast<int>(str.size()), nullptr, 0);
        std::wstring result(size, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, str.c_str(),
            static_cast<int>(str.size()), &result[0], size);
        return result;
    }

    std::string MySQLAdapter::quoteIdentifier(const std::wstring& name)
    {
        return "`" + toUtf8(name) + "`";
    }

    std::string MySQLAdapter::quoteLiteral(const std::wstring& value)
    {
        std::string utf8 = toUtf8(value);
        std::string escaped;
        escaped.reserve(utf8.size() + 2);
        escaped += '\'';
        for (char c : utf8)
        {
            if (c == '\'') escaped += "''";
            else if (c == '\\') escaped += "\\\\";
            else escaped += c;
        }
        escaped += '\'';
        return escaped;
    }

    void MySQLAdapter::ensureConnected() const
    {
        if (!connected_ || !conn_)
            throw DatabaseError(DatabaseError::Code::ConnectionFailed, "Not connected to MySQL");
    }

    // ── Constructor / Destructor ────────────────────
    MySQLAdapter::MySQLAdapter() = default;

    MySQLAdapter::~MySQLAdapter()
    {
        if (conn_)
        {
            mysql_close(conn_);
            conn_ = nullptr;
        }
    }

    // ── Connection ──────────────────────────────────
    void MySQLAdapter::connect(const ConnectionConfig& config, const std::wstring& password)
    {
        disconnect();

        conn_ = mysql_init(nullptr);
        if (!conn_)
            throw DatabaseError(DatabaseError::Code::ConnectionFailed, "mysql_init() failed");

        // Set connection timeout
        unsigned int timeout = 10;
        mysql_options(conn_, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);

        // Set UTF-8
        mysql_options(conn_, MYSQL_SET_CHARSET_NAME, "utf8mb4");

        // Point libmariadb at the bundled auth plugin DLLs. Without this
        // the library looks for plugins in a compile-time vcpkg path
        // that doesn't exist on customer machines → any auth method
        // beyond the built-in mysql_native_password (e.g. MySQL 8's
        // default caching_sha2_password, MariaDB's client_ed25519)
        // fails with "Authentication plugin '...' cannot be loaded".
        // Plugins are copied to <exe_dir>/plugins/mariadb/ by
        // build-unpackaged.ps1.
        {
            wchar_t exePath[MAX_PATH] = {};
            DWORD len = ::GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            if (len > 0 && len < MAX_PATH)
            {
                std::wstring dir(exePath);
                auto slash = dir.find_last_of(L"\\/");
                if (slash != std::wstring::npos) dir.resize(slash);
                std::wstring pluginDirW = dir + L"\\plugins\\mariadb";
                auto pluginDir = toUtf8(pluginDirW);
                mysql_options(conn_, MYSQL_PLUGIN_DIR, pluginDir.c_str());
            }
        }

        // SSL policy.
        //
        // MariaDB Connector/C 3.4 (vcpkg build) auto-upgrades the
        // connection to TLS whenever the server advertises CLIENT_SSL —
        // even if the caller never touched any SSL option. The client's
        // default cert-verify flag is TRUE, so a self-signed / untrusted
        // server cert then bombs with "Certificate verification failure:
        // The certificate is NOT trusted", turning what the user picked
        // as "Disabled" into a verification failure instead of a plain
        // TCP connect. macOS MySQLNIO doesn't have this auto-upgrade so
        // the same server works there with Disabled.
        //
        // Pragmatic fix: always disable cert verification UNLESS the
        // user explicitly picked VerifyCA / VerifyIdentity. That covers
        // Disabled (may or may not get SSL silently, but won't fail on
        // cert), Preferred, and Required.
        {
            bool wantSSL =
                (config.sslMode != DBModels::SSLMode::Disabled);
            unsigned char enforce = wantSSL ? 1 : 0;
            mysql_options(conn_, MYSQL_OPT_SSL_ENFORCE, &enforce);

            bool verifyCert =
                (config.sslMode == DBModels::SSLMode::VerifyCA ||
                 config.sslMode == DBModels::SSLMode::VerifyIdentity);
            unsigned char verify = verifyCert ? 1 : 0;
            mysql_options(conn_, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &verify);

            if (wantSSL)
            {
                mysql_ssl_set(conn_, nullptr, nullptr, nullptr, nullptr, nullptr);
            }
        }

        // MariaDB Connector/C 3.4+ defaults its restricted_auth list to
        // a subset that on some vcpkg builds **excludes**
        // mysql_native_password — the most common MySQL auth plugin and
        // the one MySQLNIO (macOS Gridex) has no problem with. On
        // Windows this caused "Authentication plugin 'mysql_native_password'
        // couldn't be found in restricted_auth plugin list." whenever a
        // server's user account wasn't on caching_sha2_password.
        //
        // NOTE: passing "" would mean "whitelist = nothing" → block
        // every plugin, which is worse. Pass an explicit whitelist
        // covering every plugin libmariadb supports and that real
        // servers actually use. Mirrors the mysql CLI behaviour.
        static const char* kAllowedAuthPlugins =
            "mysql_native_password,"
            "caching_sha2_password,"
            "sha256_password,"
            "mysql_clear_password,"
            "dialog,"
            "auth_gssapi_client,"
            "client_ed25519";
        mysql_optionsv(conn_, MARIADB_OPT_RESTRICTED_AUTH,
                       (void*)kAllowedAuthPlugins);

        auto host = toUtf8(config.host);
        auto user = toUtf8(config.username);
        auto pass = toUtf8(password);
        auto db   = toUtf8(config.database);

        MYSQL* result = mysql_real_connect(
            conn_,
            host.c_str(),
            user.c_str(),
            pass.empty() ? nullptr : pass.c_str(),
            db.empty() ? nullptr : db.c_str(),
            config.port,
            nullptr,  // unix socket
            CLIENT_MULTI_STATEMENTS
        );

        if (!result)
        {
            std::string err = mysql_error(conn_);
            mysql_close(conn_);
            conn_ = nullptr;
            throw DatabaseError(DatabaseError::Code::ConnectionFailed, err);
        }

        connected_ = true;
    }

    void MySQLAdapter::disconnect()
    {
        if (conn_)
        {
            mysql_close(conn_);
            conn_ = nullptr;
        }
        connected_ = false;
    }

    bool MySQLAdapter::testConnection(const ConnectionConfig& config, const std::wstring& password)
    {
        try
        {
            connect(config, password);
            auto result = execute(L"SELECT 1");
            disconnect();
            return result.success;
        }
        catch (...)
        {
            disconnect();
            return false;
        }
    }

    bool MySQLAdapter::isConnected() const { return connected_ && conn_; }

    // ── Query Execution ─────────────────────────────
    QueryResult MySQLAdapter::executeInternal(const std::string& sql)
    {
        ensureConnected();

        auto start = std::chrono::high_resolution_clock::now();
        int rc = mysql_real_query(conn_, sql.c_str(), static_cast<unsigned long>(sql.size()));
        auto end = std::chrono::high_resolution_clock::now();
        double durationMs = std::chrono::duration<double, std::milli>(end - start).count();

        QueryResult result;
        result.sql = fromUtf8(sql);
        result.executionTimeMs = durationMs;

        if (rc != 0)
        {
            result.success = false;
            result.error = fromUtf8(mysql_error(conn_));
            return result;
        }

        MYSQL_RES* res = mysql_store_result(conn_);
        if (res)
        {
            unsigned int nCols = mysql_num_fields(res);
            MYSQL_FIELD* fields = mysql_fetch_fields(res);

            // Pre-reserve to avoid vector reallocation
            result.columnNames.reserve(nCols);
            result.columnTypes.reserve(nCols);
            for (unsigned int c = 0; c < nCols; c++)
            {
                result.columnNames.push_back(fromUtf8(fields[c].name));
                result.columnTypes.push_back(fromUtf8(fields[c].name)); // simplified
            }

            // PERFORMANCE: pre-reserve, const-ref colName, move-construct rows
            // (matches PostgreSQLAdapter optimization)
            const auto totalRows = mysql_num_rows(res);
            result.rows.reserve(static_cast<size_t>(totalRows));
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(res)))
            {
                unsigned long* lengths = mysql_fetch_lengths(res);
                TableRow tableRow;
                tableRow.reserve(nCols);
                for (unsigned int c = 0; c < nCols; c++)
                {
                    const std::wstring& colName = result.columnNames[c];
                    if (row[c])
                        tableRow.emplace(colName, fromUtf8(std::string(row[c], lengths[c])));
                    else
                        tableRow.emplace(colName, nullValue());
                }
                result.rows.push_back(std::move(tableRow));
            }

            result.totalRows = static_cast<int>(mysql_num_rows(res));
            result.success = true;
            mysql_free_result(res);
        }
        else
        {
            // No result set — DML statement
            my_ulonglong affected = mysql_affected_rows(conn_);
            result.totalRows = static_cast<int>(affected);
            result.success = true;
        }

        // Clear any remaining result sets (CLIENT_MULTI_STATEMENTS)
        while (mysql_next_result(conn_) == 0)
        {
            MYSQL_RES* extra = mysql_store_result(conn_);
            if (extra) mysql_free_result(extra);
        }

        return result;
    }

    QueryResult MySQLAdapter::execute(const std::wstring& sql)
    {
        return executeInternal(toUtf8(sql));
    }

    QueryResult MySQLAdapter::fetchRows(
        const std::wstring& table, const std::wstring& schema,
        int limit, int offset,
        const std::wstring& orderBy, bool ascending)
    {
        // MySQL uses database name instead of schema
        std::string sql = "SELECT * FROM " + quoteIdentifier(schema) + "." + quoteIdentifier(table);
        if (!orderBy.empty())
            sql += " ORDER BY " + quoteIdentifier(orderBy) + (ascending ? " ASC" : " DESC");
        sql += " LIMIT " + std::to_string(limit) + " OFFSET " + std::to_string(offset);

        auto result = executeInternal(sql);

        // Get total count
        std::string countSql = "SELECT COUNT(*) AS cnt FROM " +
            quoteIdentifier(schema) + "." + quoteIdentifier(table);
        auto countResult = executeInternal(countSql);
        if (countResult.success && !countResult.rows.empty())
        {
            auto it = countResult.rows[0].find(L"cnt");
            if (it != countResult.rows[0].end() && !isNullCell(it->second))
                result.totalRows = std::stoi(toUtf8(it->second));
        }

        result.currentPage = (offset / limit) + 1;
        result.pageSize = limit;
        return result;
    }

    // ── Schema Inspection ───────────────────────────
    std::vector<std::wstring> MySQLAdapter::listDatabases()
    {
        auto result = executeInternal("SHOW DATABASES");
        std::vector<std::wstring> dbs;
        for (auto& row : result.rows)
        {
            auto it = row.find(L"Database");
            if (it != row.end()) dbs.push_back(it->second);
        }
        return dbs;
    }

    std::vector<std::wstring> MySQLAdapter::listSchemas()
    {
        return listDatabases(); // MySQL schemas = databases
    }

    std::vector<TableInfo> MySQLAdapter::listTables(const std::wstring& schema)
    {
        std::string sql =
            "SELECT TABLE_NAME, TABLE_COMMENT, TABLE_ROWS, DATA_LENGTH + INDEX_LENGTH AS size_bytes "
            "FROM INFORMATION_SCHEMA.TABLES "
            "WHERE TABLE_SCHEMA = " + quoteLiteral(schema) +
            " AND TABLE_TYPE = 'BASE TABLE' ORDER BY TABLE_NAME";

        auto result = executeInternal(sql);
        std::vector<TableInfo> tables;
        for (auto& row : result.rows)
        {
            TableInfo info;
            info.name = row[L"TABLE_NAME"];
            info.schema = schema;
            info.type = L"table";
            info.comment = row[L"TABLE_COMMENT"];
            auto estIt = row.find(L"TABLE_ROWS");
            if (estIt != row.end() && !isNullCell(estIt->second))
                info.estimatedRows = std::stoll(toUtf8(estIt->second));
            auto sizeIt = row.find(L"size_bytes");
            if (sizeIt != row.end() && !isNullCell(sizeIt->second))
                info.sizeBytes = std::stoll(toUtf8(sizeIt->second));
            tables.push_back(info);
        }
        return tables;
    }

    std::vector<TableInfo> MySQLAdapter::listViews(const std::wstring& schema)
    {
        std::string sql =
            "SELECT TABLE_NAME FROM INFORMATION_SCHEMA.VIEWS "
            "WHERE TABLE_SCHEMA = " + quoteLiteral(schema) + " ORDER BY TABLE_NAME";

        auto result = executeInternal(sql);
        std::vector<TableInfo> views;
        for (auto& row : result.rows)
        {
            TableInfo info;
            info.name = row[L"TABLE_NAME"];
            info.schema = schema;
            info.type = L"view";
            views.push_back(info);
        }
        return views;
    }

    std::vector<ColumnInfo> MySQLAdapter::describeTable(
        const std::wstring& table, const std::wstring& schema)
    {
        std::string sql =
            "SELECT COLUMN_NAME, COLUMN_TYPE, IS_NULLABLE, COLUMN_DEFAULT, "
            "COLUMN_KEY, ORDINAL_POSITION, COLUMN_COMMENT "
            "FROM INFORMATION_SCHEMA.COLUMNS "
            "WHERE TABLE_SCHEMA = " + quoteLiteral(schema) +
            " AND TABLE_NAME = " + quoteLiteral(table) +
            " ORDER BY ORDINAL_POSITION";

        auto result = executeInternal(sql);
        std::vector<ColumnInfo> columns;
        for (auto& row : result.rows)
        {
            ColumnInfo col;
            col.name = row[L"COLUMN_NAME"];
            col.dataType = row[L"COLUMN_TYPE"];
            col.nullable = (row[L"IS_NULLABLE"] == L"YES");
            col.defaultValue = !isNullCell(row[L"COLUMN_DEFAULT"]) ? row[L"COLUMN_DEFAULT"] : L"";
            col.isPrimaryKey = (row[L"COLUMN_KEY"] == L"PRI");
            col.comment = row[L"COLUMN_COMMENT"];
            auto ordIt = row.find(L"ORDINAL_POSITION");
            if (ordIt != row.end() && !isNullCell(ordIt->second))
                col.ordinalPosition = std::stoi(toUtf8(ordIt->second));
            columns.push_back(col);
        }

        // Mark FK columns
        auto fks = listForeignKeys(table, schema);
        for (auto& fk : fks)
            for (auto& col : columns)
                if (col.name == fk.column)
                {
                    col.isForeignKey = true;
                    col.fkReferencedTable = fk.referencedTable;
                    col.fkReferencedColumn = fk.referencedColumn;
                }

        return columns;
    }

    std::vector<IndexInfo> MySQLAdapter::listIndexes(
        const std::wstring& table, const std::wstring& schema)
    {
        std::string sql =
            "SELECT INDEX_NAME, GROUP_CONCAT(COLUMN_NAME ORDER BY SEQ_IN_INDEX) AS columns, "
            "NOT NON_UNIQUE AS is_unique, INDEX_TYPE AS algorithm "
            "FROM INFORMATION_SCHEMA.STATISTICS "
            "WHERE TABLE_SCHEMA = " + quoteLiteral(schema) +
            " AND TABLE_NAME = " + quoteLiteral(table) +
            " GROUP BY INDEX_NAME, NON_UNIQUE, INDEX_TYPE ORDER BY INDEX_NAME";

        auto result = executeInternal(sql);
        std::vector<IndexInfo> indexes;
        for (auto& row : result.rows)
        {
            IndexInfo idx;
            idx.name = row[L"INDEX_NAME"];
            idx.columns = row[L"columns"];
            idx.isUnique = (row[L"is_unique"] == L"1");
            idx.isPrimary = (idx.name == L"PRIMARY");
            idx.algorithm = row[L"algorithm"];
            indexes.push_back(idx);
        }
        return indexes;
    }

    std::vector<ForeignKeyInfo> MySQLAdapter::listForeignKeys(
        const std::wstring& table, const std::wstring& schema)
    {
        std::string sql =
            "SELECT CONSTRAINT_NAME, COLUMN_NAME, REFERENCED_TABLE_NAME, "
            "REFERENCED_COLUMN_NAME "
            "FROM INFORMATION_SCHEMA.KEY_COLUMN_USAGE "
            "WHERE TABLE_SCHEMA = " + quoteLiteral(schema) +
            " AND TABLE_NAME = " + quoteLiteral(table) +
            " AND REFERENCED_TABLE_NAME IS NOT NULL ORDER BY CONSTRAINT_NAME";

        auto result = executeInternal(sql);
        std::vector<ForeignKeyInfo> fks;
        for (auto& row : result.rows)
        {
            ForeignKeyInfo fk;
            fk.name = row[L"CONSTRAINT_NAME"];
            fk.column = row[L"COLUMN_NAME"];
            fk.referencedTable = row[L"REFERENCED_TABLE_NAME"];
            fk.referencedColumn = row[L"REFERENCED_COLUMN_NAME"];
            fk.onUpdate = L"RESTRICT";
            fk.onDelete = L"RESTRICT";
            fks.push_back(fk);
        }
        return fks;
    }

    std::vector<std::wstring> MySQLAdapter::listFunctions(const std::wstring& schema)
    {
        std::string sql =
            "SELECT ROUTINE_NAME FROM INFORMATION_SCHEMA.ROUTINES "
            "WHERE ROUTINE_SCHEMA = " + quoteLiteral(schema) +
            " AND ROUTINE_TYPE = 'FUNCTION' ORDER BY ROUTINE_NAME";

        auto result = executeInternal(sql);
        std::vector<std::wstring> funcs;
        for (auto& row : result.rows)
        {
            auto it = row.find(L"ROUTINE_NAME");
            if (it != row.end()) funcs.push_back(it->second);
        }
        return funcs;
    }

    std::wstring MySQLAdapter::getFunctionSource(
        const std::wstring& name, const std::wstring& schema)
    {
        std::string sql = "SHOW CREATE FUNCTION " + quoteIdentifier(schema) + "." + quoteIdentifier(name);
        auto result = executeInternal(sql);
        if (!result.rows.empty())
        {
            auto it = result.rows[0].find(L"Create Function");
            if (it != result.rows[0].end()) return it->second;
        }
        return L"";
    }

    std::wstring MySQLAdapter::getCreateTableSQL(
        const std::wstring& table, const std::wstring& schema)
    {
        std::string sql = "SHOW CREATE TABLE " +
            quoteIdentifier(schema) + "." + quoteIdentifier(table);
        auto result = executeInternal(sql);
        if (!result.rows.empty())
        {
            auto it = result.rows[0].find(L"Create Table");
            if (it != result.rows[0].end()) return it->second;
        }
        return L"";
    }

    // ── DML ─────────────────────────────────────────
    QueryResult MySQLAdapter::insertRow(
        const std::wstring& table, const std::wstring& schema,
        const TableRow& values)
    {
        std::string sql = "INSERT INTO " + quoteIdentifier(schema) + "." + quoteIdentifier(table) + " (";
        std::string valsSql;
        bool first = true;
        for (auto& [col, val] : values)
        {
            // Blank cell (user left it empty in the new-row UI) → omit the
            // column so DB defaults / AUTO_INCREMENT / NULL-by-default kick
            // in. MySQL otherwise rejects '' for INT columns. Users who
            // actually want SQL NULL get the null sentinel, which is kept
            // below and emitted as NULL.
            if (!isNullCell(val) && val.empty()) continue;
            if (!first) { sql += ", "; valsSql += ", "; }
            sql += quoteIdentifier(col);
            valsSql += isNullCell(val) ? "NULL" : quoteLiteral(val);
            first = false;
        }
        // All columns blank → use MySQL's all-defaults insert form so we
        // don't produce invalid "INSERT INTO t () VALUES ()".
        if (first)
            sql = "INSERT INTO " + quoteIdentifier(schema) + "." + quoteIdentifier(table) + " () VALUES ()";
        else
            sql += ") VALUES (" + valsSql + ")";
        return executeInternal(sql);
    }

    QueryResult MySQLAdapter::updateRow(
        const std::wstring& table, const std::wstring& schema,
        const TableRow& setValues, const TableRow& whereValues)
    {
        std::string sql = "UPDATE " + quoteIdentifier(schema) + "." + quoteIdentifier(table) + " SET ";
        bool first = true;
        for (auto& [col, val] : setValues)
        {
            if (!first) sql += ", ";
            sql += quoteIdentifier(col) + " = " + (isNullCell(val) ? "NULL" : quoteLiteral(val));
            first = false;
        }
        sql += " WHERE ";
        first = true;
        for (auto& [col, val] : whereValues)
        {
            if (!first) sql += " AND ";
            if (isNullCell(val))
                sql += quoteIdentifier(col) + " IS NULL";
            else
                sql += quoteIdentifier(col) + " = " + quoteLiteral(val);
            first = false;
        }
        return executeInternal(sql);
    }

    QueryResult MySQLAdapter::deleteRow(
        const std::wstring& table, const std::wstring& schema,
        const TableRow& whereValues)
    {
        std::string sql = "DELETE FROM " + quoteIdentifier(schema) + "." + quoteIdentifier(table) + " WHERE ";
        bool first = true;
        for (auto& [col, val] : whereValues)
        {
            if (!first) sql += " AND ";
            if (isNullCell(val))
                sql += quoteIdentifier(col) + " IS NULL";
            else
                sql += quoteIdentifier(col) + " = " + quoteLiteral(val);
            first = false;
        }
        return executeInternal(sql);
    }

    // ── Transactions ────────────────────────────────
    void MySQLAdapter::beginTransaction()    { executeInternal("START TRANSACTION"); }
    void MySQLAdapter::commitTransaction()   { executeInternal("COMMIT"); }
    void MySQLAdapter::rollbackTransaction() { executeInternal("ROLLBACK"); }

    // ── Server Info ─────────────────────────────────
    std::wstring MySQLAdapter::serverVersion()
    {
        ensureConnected();
        const char* ver = mysql_get_server_info(conn_);
        return ver ? fromUtf8(ver) : L"Unknown";
    }

    std::wstring MySQLAdapter::currentDatabase()
    {
        auto result = executeInternal("SELECT DATABASE() AS db");
        if (!result.rows.empty())
        {
            auto it = result.rows[0].find(L"db");
            if (it != result.rows[0].end()) return it->second;
        }
        return L"";
    }
}
