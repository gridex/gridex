#include <windows.h>

// httplib pulls in WinSock2 — include before any other Windows headers
// to avoid winsock.h vs winsock2.h ordering conflicts.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>

#pragma warning(push)
#pragma warning(disable: 4996)  // deprecation inside httplib
#include <httplib.h>
#pragma warning(pop)

#include <nlohmann/json.hpp>

#include "Models/ClickHouseAdapter.h"
#include <chrono>
#include <sstream>
#include <algorithm>

namespace DBModels
{
    // ── Pimpl struct ──────────────────────────────────────────────────────────
    // Owns the single httplib::Client kept alive for the session.
    // One client = one persistent TCP connection (httplib keep-alive default).
    struct ClickHouseAdapter::Impl
    {
        std::unique_ptr<httplib::Client> client;
        std::string host;       // UTF-8
        int port = 8123;
        std::string user;
        std::string password;
        std::string database;   // current DB sent as query param
    };

    // ── UTF-8 <-> wstring helpers ─────────────────────────────────────────────
    std::string ClickHouseAdapter::toUtf8(const std::wstring& wstr)
    {
        if (wstr.empty()) return {};
        int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(),
            static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
        std::string result(size, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(),
            static_cast<int>(wstr.size()), &result[0], size, nullptr, nullptr);
        return result;
    }

    std::wstring ClickHouseAdapter::fromUtf8(const std::string& str)
    {
        if (str.empty()) return {};
        int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(),
            static_cast<int>(str.size()), nullptr, 0);
        std::wstring result(size, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, str.c_str(),
            static_cast<int>(str.size()), &result[0], size);
        return result;
    }

    // Backtick-quote an identifier (ClickHouse / MySQL style).
    std::string ClickHouseAdapter::quoteIdentifier(const std::wstring& name)
    {
        std::string utf8 = toUtf8(name);
        // Escape any backtick that already appears inside the name.
        std::string escaped;
        escaped.reserve(utf8.size() + 2);
        escaped += '`';
        for (char c : utf8)
        {
            if (c == '`') escaped += "``";
            else escaped += c;
        }
        escaped += '`';
        return escaped;
    }

    // Single-quote a string literal with single-quote escaping.
    std::string ClickHouseAdapter::quoteLiteral(const std::wstring& value)
    {
        std::string utf8 = toUtf8(value);
        std::string escaped;
        escaped.reserve(utf8.size() + 2);
        escaped += '\'';
        for (char c : utf8)
        {
            if (c == '\'') escaped += "\\'";  // ClickHouse accepts \'
            else if (c == '\\') escaped += "\\\\";
            else escaped += c;
        }
        escaped += '\'';
        return escaped;
    }

    // Public wstring-returning variants used by callers that build wide SQL.
    std::wstring ClickHouseAdapter::quoteSqlLiteral(const std::wstring& value) const
    {
        return fromUtf8(quoteLiteral(value));
    }

    std::wstring ClickHouseAdapter::quoteSqlIdentifier(const std::wstring& name) const
    {
        return fromUtf8(quoteIdentifier(name));
    }

    void ClickHouseAdapter::ensureConnected() const
    {
        if (!connected_ || !impl_ || !impl_->client)
            throw DatabaseError(DatabaseError::Code::ConnectionFailed,
                "Not connected to ClickHouse");
    }

    // ── Constructor / Destructor ──────────────────────────────────────────────
    ClickHouseAdapter::ClickHouseAdapter()
        : impl_(std::make_unique<Impl>())
    {}

    ClickHouseAdapter::~ClickHouseAdapter() { disconnect(); }

    // ── Connection ────────────────────────────────────────────────────────────
    void ClickHouseAdapter::connect(const ConnectionConfig& config,
                                    const std::wstring& password)
    {
        disconnect();

        impl_->host     = toUtf8(config.host);
        impl_->port     = config.port > 0 ? config.port : 8123;
        impl_->user     = toUtf8(config.username);
        impl_->password = toUtf8(password);
        impl_->database = toUtf8(config.database);
        currentDb_      = config.database;

        // Build httplib client — plain or TLS depending on sslEnabled.
        // httplib::Client auto-detects http:// vs https:// from the
        // scheme prefix, but we construct explicitly for clarity.
        if (config.sslEnabled && config.sslMode != SSLMode::Disabled)
        {
            // SSLClient is the TLS variant; same API as Client.
            impl_->client = std::make_unique<httplib::SSLClient>(
                impl_->host, impl_->port);
            // Skip cert verification for VerifyIdentity=false modes
            if (config.sslMode == SSLMode::Preferred ||
                config.sslMode == SSLMode::Required)
            {
                impl_->client->enable_server_certificate_verification(false);
            }
        }
        else
        {
            impl_->client = std::make_unique<httplib::Client>(
                impl_->host, impl_->port);
        }

        impl_->client->set_connection_timeout(10);
        impl_->client->set_read_timeout(30);

        // Verify connectivity with a trivial query.
        auto probe = executeInternal("SELECT 1");
        if (!probe.success)
            throw DatabaseError(DatabaseError::Code::ConnectionFailed,
                toUtf8(probe.error));

        connected_ = true;
    }

    void ClickHouseAdapter::disconnect()
    {
        connected_ = false;
        if (impl_)
            impl_->client.reset();
    }

    bool ClickHouseAdapter::testConnection(const ConnectionConfig& config,
                                           const std::wstring& password)
    {
        try
        {
            connect(config, password);
            disconnect();
            return true;
        }
        catch (...)
        {
            disconnect();
            return false;
        }
    }

    bool ClickHouseAdapter::isConnected() const
    {
        return connected_ && impl_ && impl_->client;
    }

    // ── Core HTTP query execution ─────────────────────────────────────────────
    // Appends "FORMAT JSONCompact" to SELECT-like statements so results come
    // back as:
    //   { "meta": [{"name":"col","type":"T"},...],
    //     "data": [[v1,v2,...], ...],
    //     "rows": N }
    // DDL / DML (ALTER, INSERT, etc.) gets no FORMAT suffix.
    static bool needsFormat(const std::string& sql)
    {
        // Quick heuristic: trim leading whitespace, check first keyword.
        size_t i = 0;
        while (i < sql.size() && std::isspace((unsigned char)sql[i])) ++i;
        if (i >= sql.size()) return false;
        // Convert first 10 chars to uppercase for comparison
        std::string head = sql.substr(i, 10);
        std::transform(head.begin(), head.end(), head.begin(),
            [](unsigned char c) { return (char)std::toupper(c); });
        return head.rfind("SELECT", 0) == 0 ||
               head.rfind("SHOW",   0) == 0 ||
               head.rfind("DESC",   0) == 0 ||
               head.rfind("EXPLAIN",0) == 0 ||
               head.rfind("WITH",   0) == 0;
    }

    QueryResult ClickHouseAdapter::executeInternal(const std::string& sql)
    {
        QueryResult result;
        result.sql = fromUtf8(sql);

        if (!impl_ || !impl_->client)
        {
            result.success = false;
            result.error   = L"No active connection";
            return result;
        }

        // Build query string: database param + output format
        std::string body = sql;
        bool isSelect = needsFormat(sql);
        if (isSelect) body += " FORMAT JSONCompact";

        // Query params
        std::string params = "?";
        if (!impl_->database.empty())
            params += "database=" + impl_->database + "&";
        params += "default_format=JSONCompact";

        // Build request with auth headers
        httplib::Headers headers = {
            { "X-ClickHouse-User",   impl_->user },
            { "X-ClickHouse-Key",    impl_->password },
            { "Content-Type",        "text/plain; charset=utf-8" }
        };

        auto t0 = std::chrono::high_resolution_clock::now();
        auto res = impl_->client->Post(params.c_str(), headers, body, "text/plain");
        auto t1 = std::chrono::high_resolution_clock::now();
        result.executionTimeMs =
            std::chrono::duration<double, std::milli>(t1 - t0).count();

        if (!res)
        {
            // Network-level failure
            result.success = false;
            result.error   = L"HTTP request failed (network error)";
            return result;
        }

        if (res->status == 401)
        {
            result.success = false;
            result.error   = fromUtf8(res->body);
            throw DatabaseError(DatabaseError::Code::AuthenticationFailed,
                "ClickHouse authentication failed: " + res->body);
        }

        if (res->status != 200)
        {
            result.success = false;
            // ClickHouse puts the error text in the body
            result.error = fromUtf8(res->body);
            return result;
        }

        // DDL / DML returns empty body on success
        if (!isSelect || res->body.empty())
        {
            result.success  = true;
            result.totalRows = 0;
            return result;
        }

        // Parse JSONCompact response
        try
        {
            auto j = nlohmann::json::parse(res->body);

            // Column metadata
            auto& meta = j["meta"];
            result.columnNames.reserve(meta.size());
            result.columnTypes.reserve(meta.size());
            for (auto& col : meta)
            {
                result.columnNames.push_back(fromUtf8(col["name"].get<std::string>()));
                result.columnTypes.push_back(fromUtf8(col["type"].get<std::string>()));
            }

            // Row data
            auto& data = j["data"];
            result.rows.reserve(data.size());
            for (auto& rowArr : data)
            {
                TableRow row;
                row.reserve(result.columnNames.size());
                for (size_t c = 0; c < result.columnNames.size() && c < rowArr.size(); ++c)
                {
                    const auto& cell = rowArr[c];
                    if (cell.is_null())
                        row.emplace(result.columnNames[c], nullValue());
                    else if (cell.is_string())
                        row.emplace(result.columnNames[c], fromUtf8(cell.get<std::string>()));
                    else
                        // Numbers, booleans — serialize to string
                        row.emplace(result.columnNames[c], fromUtf8(cell.dump()));
                }
                result.rows.push_back(std::move(row));
            }

            result.totalRows = static_cast<int>(result.rows.size());
            if (j.contains("rows"))
                result.totalRows = j["rows"].get<int>();

            result.success = true;
        }
        catch (const std::exception& ex)
        {
            result.success = false;
            result.error   = fromUtf8(std::string("JSON parse error: ") + ex.what());
        }

        return result;
    }

    QueryResult ClickHouseAdapter::execute(const std::wstring& sql)
    {
        ensureConnected();
        return executeInternal(toUtf8(sql));
    }

    QueryResult ClickHouseAdapter::fetchRows(
        const std::wstring& table, const std::wstring& schema,
        int limit, int offset,
        const std::wstring& orderBy, bool ascending)
    {
        ensureConnected();

        // schema == database in ClickHouse (see listSchemas() note)
        std::string db = schema.empty() ? impl_->database : toUtf8(schema);

        std::string sql = "SELECT * FROM " + "`" + db + "`" + "." + quoteIdentifier(table);
        if (!orderBy.empty())
            sql += " ORDER BY " + quoteIdentifier(orderBy) + (ascending ? " ASC" : " DESC");
        sql += " LIMIT " + std::to_string(limit) + " OFFSET " + std::to_string(offset);

        auto result = executeInternal(sql);

        // Fetch total row count for pagination metadata.
        // ClickHouse COUNT(*) is fast even on large tables (stored in system.tables).
        std::string countSql = "SELECT COUNT(*) AS cnt FROM `" + db + "`." +
                               quoteIdentifier(table);
        auto countResult = executeInternal(countSql);
        if (countResult.success && !countResult.rows.empty())
        {
            auto it = countResult.rows[0].find(L"cnt");
            if (it != countResult.rows[0].end() && !isNullCell(it->second))
            {
                try { result.totalRows = std::stoi(toUtf8(it->second)); }
                catch (...) {}
            }
        }

        result.currentPage = (offset / limit) + 1;
        result.pageSize    = limit;
        return result;
    }

    // ── Schema Inspection ─────────────────────────────────────────────────────
    std::vector<std::wstring> ClickHouseAdapter::listDatabases()
    {
        ensureConnected();
        auto result = executeInternal(
            "SELECT name FROM system.databases ORDER BY name");
        std::vector<std::wstring> dbs;
        for (auto& row : result.rows)
        {
            auto it = row.find(L"name");
            if (it != row.end()) dbs.push_back(it->second);
        }
        return dbs;
    }

    // ClickHouse has no sub-database schemas.  We expose one "schema"
    // entry equal to the connected database so the sidebar tree is
    // consistent with the PG adapter (schema node → tables list).
    std::vector<std::wstring> ClickHouseAdapter::listSchemas()
    {
        ensureConnected();
        return { currentDb_ };
    }

    std::vector<TableInfo> ClickHouseAdapter::listTables(const std::wstring& schema)
    {
        ensureConnected();
        std::string db = schema.empty() ? impl_->database : toUtf8(schema);

        std::string sql =
            "SELECT name, engine, total_rows "
            "FROM system.tables "
            "WHERE database = " + quoteLiteral(fromUtf8(db)) +
            " AND engine NOT LIKE '%View%' "
            "ORDER BY name";

        auto result = executeInternal(sql);
        std::vector<TableInfo> tables;
        for (auto& row : result.rows)
        {
            TableInfo info;
            info.name   = row[L"name"];
            info.schema = fromUtf8(db);
            info.type   = L"table";
            // engine maps to a comment-like extra field
            auto engIt = row.find(L"engine");
            if (engIt != row.end()) info.comment = engIt->second;
            auto rowsIt = row.find(L"total_rows");
            if (rowsIt != row.end() && !isNullCell(rowsIt->second))
            {
                try { info.estimatedRows = std::stoll(toUtf8(rowsIt->second)); }
                catch (...) {}
            }
            tables.push_back(info);
        }
        return tables;
    }

    std::vector<TableInfo> ClickHouseAdapter::listViews(const std::wstring& schema)
    {
        ensureConnected();
        std::string db = schema.empty() ? impl_->database : toUtf8(schema);

        std::string sql =
            "SELECT name, engine "
            "FROM system.tables "
            "WHERE database = " + quoteLiteral(fromUtf8(db)) +
            " AND engine LIKE '%View%' "
            "ORDER BY name";

        auto result = executeInternal(sql);
        std::vector<TableInfo> views;
        for (auto& row : result.rows)
        {
            TableInfo info;
            info.name   = row[L"name"];
            info.schema = fromUtf8(db);
            info.type   = L"view";
            views.push_back(info);
        }
        return views;
    }

    std::vector<ColumnInfo> ClickHouseAdapter::describeTable(
        const std::wstring& table, const std::wstring& schema)
    {
        ensureConnected();
        std::string db = schema.empty() ? impl_->database : toUtf8(schema);

        std::string sql =
            "SELECT name, type, default_expression, is_in_primary_key "
            "FROM system.columns "
            "WHERE database = " + quoteLiteral(fromUtf8(db)) +
            " AND table = " + quoteLiteral(table) +
            " ORDER BY position";

        auto result = executeInternal(sql);
        std::vector<ColumnInfo> columns;
        int pos = 1;
        for (auto& row : result.rows)
        {
            ColumnInfo col;
            col.name            = row[L"name"];
            col.dataType        = row[L"type"];
            // ClickHouse nullable columns: type is "Nullable(T)"
            col.nullable        = (col.dataType.find(L"Nullable") != std::wstring::npos);
            col.ordinalPosition = pos++;

            auto defIt = row.find(L"default_expression");
            if (defIt != row.end() && !isNullCell(defIt->second))
                col.defaultValue = defIt->second;

            auto pkIt = row.find(L"is_in_primary_key");
            if (pkIt != row.end())
                col.isPrimaryKey = (pkIt->second == L"1" || pkIt->second == L"true");

            // FK detection: ClickHouse has no FKs — always false.
            col.isForeignKey = false;
            columns.push_back(col);
        }
        return columns;
    }

    // ClickHouse has no index concept in the relational sense.
    std::vector<IndexInfo> ClickHouseAdapter::listIndexes(
        const std::wstring& /*table*/, const std::wstring& /*schema*/)
    {
        return {};
    }

    // ClickHouse has no foreign key constraints.
    std::vector<ForeignKeyInfo> ClickHouseAdapter::listForeignKeys(
        const std::wstring& /*table*/, const std::wstring& /*schema*/)
    {
        return {};
    }

    std::vector<std::wstring> ClickHouseAdapter::listFunctions(
        const std::wstring& /*schema*/)
    {
        ensureConnected();
        // Return built-in function names for auto-complete / documentation.
        // Scoped by origin='System' to avoid noise from user-defined functions.
        auto result = executeInternal(
            "SELECT name FROM system.functions "
            "WHERE origin = 'System' ORDER BY name");
        std::vector<std::wstring> funcs;
        for (auto& row : result.rows)
        {
            auto it = row.find(L"name");
            if (it != row.end()) funcs.push_back(it->second);
        }
        return funcs;
    }

    // ClickHouse does not expose source for built-in functions.
    std::wstring ClickHouseAdapter::getFunctionSource(
        const std::wstring& /*name*/, const std::wstring& /*schema*/)
    {
        return L"";
    }

    std::wstring ClickHouseAdapter::getCreateTableSQL(
        const std::wstring& table, const std::wstring& schema)
    {
        ensureConnected();
        std::string db = schema.empty() ? impl_->database : toUtf8(schema);

        // SHOW CREATE TABLE returns a single row with the DDL string.
        std::string sql = "SHOW CREATE TABLE `" + db + "`." + quoteIdentifier(table);
        auto result = executeInternal(sql);
        if (!result.rows.empty())
        {
            // Column name varies by CH version: "statement" or "CREATE TABLE ..."
            for (auto& [k, v] : result.rows[0])
                if (!isNullCell(v) && !v.empty()) return v;
        }
        return L"";
    }

    // ── Data Manipulation ─────────────────────────────────────────────────────
    QueryResult ClickHouseAdapter::insertRow(
        const std::wstring& table, const std::wstring& schema,
        const TableRow& values)
    {
        ensureConnected();
        std::string db = schema.empty() ? impl_->database : toUtf8(schema);

        std::string sql = "INSERT INTO `" + db + "`." + quoteIdentifier(table) + " (";
        std::string vals;
        bool first = true;
        for (auto& [col, val] : values)
        {
            if (!isNullCell(val) && val.empty()) continue;  // skip blank → DEFAULT
            if (!first) { sql += ", "; vals += ", "; }
            sql  += quoteIdentifier(col);
            vals += isNullCell(val) ? "NULL" : quoteLiteral(val);
            first = false;
        }
        if (first)
        {
            // No values provided — ClickHouse does not support DEFAULT VALUES
            // syntax; emit an explicit empty insert (will fail on NOT NULL cols,
            // which is the correct behaviour).
            sql = "INSERT INTO `" + db + "`." + quoteIdentifier(table) + " VALUES ()";
        }
        else
        {
            sql += ") VALUES (" + vals + ")";
        }
        return executeInternal(sql);
    }

    // ClickHouse UPDATE syntax: ALTER TABLE t UPDATE col=val WHERE ...
    // (Unlike standard SQL which uses UPDATE t SET col=val WHERE ...).
    QueryResult ClickHouseAdapter::updateRow(
        const std::wstring& table, const std::wstring& schema,
        const TableRow& setValues, const TableRow& whereValues)
    {
        ensureConnected();
        std::string db = schema.empty() ? impl_->database : toUtf8(schema);

        std::string sql = "ALTER TABLE `" + db + "`." + quoteIdentifier(table) + " UPDATE ";
        bool first = true;
        for (auto& [col, val] : setValues)
        {
            if (!first) sql += ", ";
            sql += quoteIdentifier(col) + " = " +
                   (isNullCell(val) ? "NULL" : quoteLiteral(val));
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

    // ClickHouse DELETE syntax: ALTER TABLE t DELETE WHERE ...
    QueryResult ClickHouseAdapter::deleteRow(
        const std::wstring& table, const std::wstring& schema,
        const TableRow& whereValues)
    {
        ensureConnected();
        std::string db = schema.empty() ? impl_->database : toUtf8(schema);

        std::string sql = "ALTER TABLE `" + db + "`." + quoteIdentifier(table) + " DELETE WHERE ";
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

    // ── Transactions (no-ops) ─────────────────────────────────────────────────
    // ClickHouse does not support classic ACID transactions by default.
    // Mutations (ALTER ... UPDATE/DELETE) are applied asynchronously in
    // the background; BEGIN/COMMIT/ROLLBACK would fail if sent.
    void ClickHouseAdapter::beginTransaction()    { /* no-op */ }
    void ClickHouseAdapter::commitTransaction()   { /* no-op */ }
    void ClickHouseAdapter::rollbackTransaction() { /* no-op */ }

    // ── Server Info ───────────────────────────────────────────────────────────
    std::wstring ClickHouseAdapter::serverVersion()
    {
        ensureConnected();
        auto result = executeInternal("SELECT version() AS ver");
        if (!result.rows.empty())
        {
            auto it = result.rows[0].find(L"ver");
            if (it != result.rows[0].end()) return it->second;
        }
        return L"Unknown";
    }

    std::wstring ClickHouseAdapter::currentDatabase()
    {
        return currentDb_;
    }
}
