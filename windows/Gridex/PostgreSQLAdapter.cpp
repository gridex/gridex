#include <windows.h>
#include "Models/PostgreSQLAdapter.h"
#include <libpq-fe.h>
#include <algorithm>
#include <chrono>
#include <codecvt>
#include <locale>

namespace DBModels
{
    // ── UTF-8 <-> wstring helpers ─────────────────────
    std::string PostgreSQLAdapter::toUtf8(const std::wstring& wstr)
    {
        if (wstr.empty()) return {};
        int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(),
            static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
        std::string result(size, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(),
            static_cast<int>(wstr.size()), &result[0], size, nullptr, nullptr);
        return result;
    }

    std::wstring PostgreSQLAdapter::fromUtf8(const std::string& str)
    {
        if (str.empty()) return {};
        int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(),
            static_cast<int>(str.size()), nullptr, 0);
        std::wstring result(size, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, str.c_str(),
            static_cast<int>(str.size()), &result[0], size);
        return result;
    }

    std::string PostgreSQLAdapter::quoteIdentifier(const std::wstring& name)
    {
        return "\"" + toUtf8(name) + "\"";
    }

    std::string PostgreSQLAdapter::quoteLiteral(const std::wstring& value)
    {
        std::string utf8 = toUtf8(value);
        std::string escaped;
        escaped.reserve(utf8.size() + 2);
        escaped += '\'';
        for (char c : utf8)
        {
            if (c == '\'') escaped += "''";
            else escaped += c;
        }
        escaped += '\'';
        return escaped;
    }

    void PostgreSQLAdapter::ensureConnected() const
    {
        if (!connected_ || !conn_)
            throw DatabaseError(DatabaseError::Code::ConnectionFailed, "Not connected to database");
    }

    // ── Constructor / Destructor ────────────────────
    PostgreSQLAdapter::PostgreSQLAdapter() = default;

    PostgreSQLAdapter::~PostgreSQLAdapter()
    {
        if (conn_)
        {
            PQfinish(conn_);
            conn_ = nullptr;
        }
    }

    // ── Connection ──────────────────────────────────
    void PostgreSQLAdapter::connect(const ConnectionConfig& config, const std::wstring& password)
    {
        disconnect();

        // Build connection string
        std::string connStr;
        connStr += "host=" + toUtf8(config.host);
        connStr += " port=" + std::to_string(config.port);
        connStr += " dbname=" + toUtf8(config.database);
        connStr += " user=" + toUtf8(config.username);
        if (!password.empty())
            connStr += " password=" + toUtf8(password);

        // SSL mode
        switch (config.sslMode)
        {
        case SSLMode::Disabled:       connStr += " sslmode=disable"; break;
        case SSLMode::Preferred:      connStr += " sslmode=prefer"; break;
        case SSLMode::Required:       connStr += " sslmode=require"; break;
        case SSLMode::VerifyCA:       connStr += " sslmode=verify-ca"; break;
        case SSLMode::VerifyIdentity: connStr += " sslmode=verify-full"; break;
        }

        connStr += " connect_timeout=10";

        conn_ = PQconnectdb(connStr.c_str());
        if (PQstatus(conn_) != CONNECTION_OK)
        {
            std::string err = PQerrorMessage(conn_);
            PQfinish(conn_);
            conn_ = nullptr;
            throw DatabaseError(DatabaseError::Code::ConnectionFailed, err);
        }

        connected_ = true;

        // Set UTF-8 encoding
        PQsetClientEncoding(conn_, "UTF8");
    }

    void PostgreSQLAdapter::disconnect()
    {
        if (conn_)
        {
            PQfinish(conn_);
            conn_ = nullptr;
        }
        connected_ = false;
    }

    bool PostgreSQLAdapter::testConnection(const ConnectionConfig& config, const std::wstring& password)
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

    bool PostgreSQLAdapter::isConnected() const { return connected_ && conn_; }

    // ── Query Execution ─────────────────────────────
    QueryResult PostgreSQLAdapter::executeInternal(const std::string& sql)
    {
        ensureConnected();

        auto start = std::chrono::high_resolution_clock::now();

        PGresult* pgResult = PQexec(conn_, sql.c_str());
        auto end = std::chrono::high_resolution_clock::now();
        double durationMs = std::chrono::duration<double, std::milli>(end - start).count();

        QueryResult result;
        result.sql = fromUtf8(sql);
        result.executionTimeMs = durationMs;

        ExecStatusType status = PQresultStatus(pgResult);

        if (status == PGRES_TUPLES_OK)
        {
            int nCols = PQnfields(pgResult);
            int nRows = PQntuples(pgResult);

            // Column names and types — pre-reserve to avoid reallocation
            result.columnNames.reserve(nCols);
            result.columnTypes.reserve(nCols);
            for (int c = 0; c < nCols; c++)
            {
                result.columnNames.push_back(fromUtf8(PQfname(pgResult, c)));
                // Store OID as string; resolve to type name later if needed
                Oid typeOid = PQftype(pgResult, c);
                result.columnTypes.push_back(std::to_wstring(typeOid));
            }

            // PERFORMANCE: pre-reserve rows vector + pre-reserve each row's
            // hash buckets, capture column name by const-ref (no per-cell copy),
            // and move-construct the row into the result (no map deep-copy).
            // This is the dominant cost on wide tables (100rows x 30cols).
            result.rows.reserve(nRows);
            for (int r = 0; r < nRows; r++)
            {
                TableRow row;
                row.reserve(nCols);
                for (int c = 0; c < nCols; c++)
                {
                    const std::wstring& colName = result.columnNames[c];
                    if (PQgetisnull(pgResult, r, c))
                        row.emplace(colName, nullValue());
                    else
                        row.emplace(colName, fromUtf8(PQgetvalue(pgResult, r, c)));
                }
                result.rows.push_back(std::move(row));
            }

            result.totalRows = nRows;
            result.success = true;
        }
        else if (status == PGRES_COMMAND_OK)
        {
            // DML: INSERT/UPDATE/DELETE — no result rows
            const char* affected = PQcmdTuples(pgResult);
            if (affected && *affected)
                result.totalRows = std::stoi(affected);
            result.success = true;
        }
        else
        {
            result.success = false;
            result.error = fromUtf8(PQresultErrorMessage(pgResult));
        }

        PQclear(pgResult);
        return result;
    }

    QueryResult PostgreSQLAdapter::execute(const std::wstring& sql)
    {
        return executeInternal(toUtf8(sql));
    }

    QueryResult PostgreSQLAdapter::fetchRows(
        const std::wstring& table, const std::wstring& schema,
        int limit, int offset,
        const std::wstring& orderBy, bool ascending)
    {
        std::string sql = "SELECT * FROM " + quoteIdentifier(schema) + "." + quoteIdentifier(table);
        if (!orderBy.empty())
            sql += " ORDER BY " + quoteIdentifier(orderBy) + (ascending ? " ASC" : " DESC");
        sql += " LIMIT " + std::to_string(limit) + " OFFSET " + std::to_string(offset);

        auto result = executeInternal(sql);

        // Get total row count for pagination metadata
        std::string countSql = "SELECT COUNT(*) FROM " + quoteIdentifier(schema) + "." + quoteIdentifier(table);
        auto countResult = executeInternal(countSql);
        if (countResult.success && !countResult.rows.empty())
        {
            auto& firstRow = countResult.rows[0];
            auto it = firstRow.find(L"count");
            if (it != firstRow.end())
                result.totalRows = std::stoi(toUtf8(it->second));
        }

        result.currentPage = (offset / limit) + 1;
        result.pageSize = limit;
        return result;
    }

    // ── Schema Inspection ───────────────────────────
    std::vector<std::wstring> PostgreSQLAdapter::listDatabases()
    {
        auto result = executeInternal(
            "SELECT datname FROM pg_database WHERE datistemplate = false ORDER BY datname");
        std::vector<std::wstring> dbs;
        for (auto& row : result.rows)
        {
            auto it = row.find(L"datname");
            if (it != row.end()) dbs.push_back(it->second);
        }
        return dbs;
    }

    std::vector<std::wstring> PostgreSQLAdapter::listSchemas()
    {
        auto result = executeInternal(
            "SELECT schema_name FROM information_schema.schemata "
            "WHERE schema_name NOT IN ('pg_catalog','information_schema','pg_toast') "
            "ORDER BY schema_name");
        std::vector<std::wstring> schemas;
        for (auto& row : result.rows)
        {
            auto it = row.find(L"schema_name");
            if (it != row.end()) schemas.push_back(it->second);
        }
        return schemas;
    }

    std::vector<TableInfo> PostgreSQLAdapter::listTables(const std::wstring& schema)
    {
        std::string sql =
            "SELECT c.relname AS name, n.nspname AS schema, "
            "pg_catalog.obj_description(c.oid) AS comment, "
            "c.reltuples::bigint AS estimated_rows, "
            "pg_total_relation_size(c.oid) AS size_bytes "
            "FROM pg_class c "
            "JOIN pg_namespace n ON n.oid = c.relnamespace "
            "WHERE c.relkind = 'r' AND n.nspname = " + quoteLiteral(schema) +
            " ORDER BY c.relname";

        auto result = executeInternal(sql);
        std::vector<TableInfo> tables;
        for (auto& row : result.rows)
        {
            TableInfo info;
            info.name    = row[L"name"];
            info.schema  = row[L"schema"];
            info.type    = L"table";
            info.comment = row[L"comment"];
            auto estIt = row.find(L"estimated_rows");
            if (estIt != row.end() && !isNullCell(estIt->second))
                info.estimatedRows = std::stoll(toUtf8(estIt->second));
            auto sizeIt = row.find(L"size_bytes");
            if (sizeIt != row.end() && !isNullCell(sizeIt->second))
                info.sizeBytes = std::stoll(toUtf8(sizeIt->second));
            tables.push_back(info);
        }
        return tables;
    }

    std::vector<TableInfo> PostgreSQLAdapter::listViews(const std::wstring& schema)
    {
        std::string sql =
            "SELECT c.relname AS name, n.nspname AS schema, "
            "pg_catalog.obj_description(c.oid) AS comment "
            "FROM pg_class c "
            "JOIN pg_namespace n ON n.oid = c.relnamespace "
            "WHERE c.relkind IN ('v','m') AND n.nspname = " + quoteLiteral(schema) +
            " ORDER BY c.relname";

        auto result = executeInternal(sql);
        std::vector<TableInfo> views;
        for (auto& row : result.rows)
        {
            TableInfo info;
            info.name   = row[L"name"];
            info.schema = row[L"schema"];
            info.type   = L"view";
            views.push_back(info);
        }
        return views;
    }

    std::vector<ColumnInfo> PostgreSQLAdapter::describeTable(
        const std::wstring& table, const std::wstring& schema)
    {
        std::string sql =
            "SELECT c.column_name, c.data_type, c.is_nullable, c.column_default, "
            "c.ordinal_position, c.udt_name, c.character_maximum_length, "
            "c.numeric_precision, c.numeric_scale, "
            "CASE WHEN pk.column_name IS NOT NULL THEN true ELSE false END AS is_pk "
            "FROM information_schema.columns c "
            "LEFT JOIN ("
            "  SELECT ku.column_name "
            "  FROM information_schema.table_constraints tc "
            "  JOIN information_schema.key_column_usage ku ON tc.constraint_name = ku.constraint_name "
            "  WHERE tc.constraint_type = 'PRIMARY KEY' "
            "    AND tc.table_name = " + quoteLiteral(table) +
            "    AND tc.table_schema = " + quoteLiteral(schema) +
            ") pk ON pk.column_name = c.column_name "
            "WHERE c.table_name = " + quoteLiteral(table) +
            " AND c.table_schema = " + quoteLiteral(schema) +
            " ORDER BY c.ordinal_position";

        auto result = executeInternal(sql);
        std::vector<ColumnInfo> columns;
        for (auto& row : result.rows)
        {
            ColumnInfo col;
            col.name = row[L"column_name"];

            // Build display type (e.g. "varchar(255)", "numeric(12,2)")
            std::wstring udtName  = row[L"udt_name"];
            std::wstring dataType = row[L"data_type"];
            if (dataType == L"character varying")
            {
                auto maxLen = row[L"character_maximum_length"];
                col.dataType = !isNullCell(maxLen)
                    ? std::wstring(L"varchar(") + maxLen + L")"
                    : L"varchar";
            }
            else if (dataType == L"numeric")
            {
                auto prec  = row[L"numeric_precision"];
                auto scale = row[L"numeric_scale"];
                col.dataType = !isNullCell(prec)
                    ? std::wstring(L"numeric(") + prec + L"," + scale + L")"
                    : L"numeric";
            }
            else if (dataType == L"USER-DEFINED")
            {
                col.dataType = udtName;
            }
            else
            {
                col.dataType = dataType;
            }

            col.nullable     = (row[L"is_nullable"] == L"YES");
            col.defaultValue = !isNullCell(row[L"column_default"]) ? row[L"column_default"] : L"";
            col.isPrimaryKey = (row[L"is_pk"] == L"t" || row[L"is_pk"] == L"true");
            auto ordIt = row.find(L"ordinal_position");
            if (ordIt != row.end() && !isNullCell(ordIt->second))
                col.ordinalPosition = std::stoi(toUtf8(ordIt->second));
            columns.push_back(col);
        }

        // Mark FK columns using listForeignKeys
        auto fks = listForeignKeys(table, schema);
        for (auto& fk : fks)
        {
            for (auto& col : columns)
            {
                if (col.name == fk.column)
                {
                    col.isForeignKey       = true;
                    col.fkReferencedTable  = fk.referencedTable;
                    col.fkReferencedColumn = fk.referencedColumn;
                }
            }
        }

        return columns;
    }

    std::vector<IndexInfo> PostgreSQLAdapter::listIndexes(
        const std::wstring& table, const std::wstring& schema)
    {
        std::string sql =
            "SELECT i.relname AS index_name, "
            "am.amname AS algorithm, "
            "ix.indisunique AS is_unique, "
            "ix.indisprimary AS is_primary, "
            "array_to_string(ARRAY("
            "  SELECT a.attname FROM unnest(ix.indkey) WITH ORDINALITY AS k(attnum, ord) "
            "  JOIN pg_attribute a ON a.attrelid = t.oid AND a.attnum = k.attnum "
            "  ORDER BY k.ord"
            "), ', ') AS columns "
            "FROM pg_class t "
            "JOIN pg_namespace n ON n.oid = t.relnamespace "
            "JOIN pg_index ix ON t.oid = ix.indrelid "
            "JOIN pg_class i ON i.oid = ix.indexrelid "
            "JOIN pg_am am ON am.oid = i.relam "
            "WHERE t.relname = " + quoteLiteral(table) +
            " AND n.nspname = " + quoteLiteral(schema) +
            " ORDER BY i.relname";

        auto result = executeInternal(sql);
        std::vector<IndexInfo> indexes;
        for (auto& row : result.rows)
        {
            IndexInfo idx;
            idx.name      = row[L"index_name"];
            idx.columns   = row[L"columns"];
            idx.isUnique  = (row[L"is_unique"] == L"t");
            idx.isPrimary = (row[L"is_primary"] == L"t");
            idx.algorithm = row[L"algorithm"];
            indexes.push_back(idx);
        }
        return indexes;
    }

    std::vector<ForeignKeyInfo> PostgreSQLAdapter::listForeignKeys(
        const std::wstring& table, const std::wstring& schema)
    {
        std::string sql =
            "SELECT conname AS name, "
            "a.attname AS column_name, "
            "cf.relname AS ref_table, "
            "af.attname AS ref_column, "
            "CASE confupdtype WHEN 'c' THEN 'CASCADE' WHEN 'n' THEN 'SET NULL' "
            "  WHEN 'd' THEN 'SET DEFAULT' WHEN 'r' THEN 'RESTRICT' ELSE 'NO ACTION' END AS on_update, "
            "CASE confdeltype WHEN 'c' THEN 'CASCADE' WHEN 'n' THEN 'SET NULL' "
            "  WHEN 'd' THEN 'SET DEFAULT' WHEN 'r' THEN 'RESTRICT' ELSE 'NO ACTION' END AS on_delete "
            "FROM pg_constraint co "
            "JOIN pg_class c ON c.oid = co.conrelid "
            "JOIN pg_namespace n ON n.oid = c.relnamespace "
            "JOIN pg_attribute a ON a.attrelid = c.oid AND a.attnum = ANY(co.conkey) "
            "JOIN pg_class cf ON cf.oid = co.confrelid "
            "JOIN pg_attribute af ON af.attrelid = cf.oid AND af.attnum = ANY(co.confkey) "
            "WHERE co.contype = 'f' "
            "AND c.relname = " + quoteLiteral(table) +
            " AND n.nspname = " + quoteLiteral(schema) +
            " ORDER BY conname";

        auto result = executeInternal(sql);
        std::vector<ForeignKeyInfo> fks;
        for (auto& row : result.rows)
        {
            ForeignKeyInfo fk;
            fk.name            = row[L"name"];
            fk.column          = row[L"column_name"];
            fk.referencedTable  = row[L"ref_table"];
            fk.referencedColumn = row[L"ref_column"];
            fk.onUpdate        = row[L"on_update"];
            fk.onDelete        = row[L"on_delete"];
            fks.push_back(fk);
        }
        return fks;
    }

    std::vector<std::wstring> PostgreSQLAdapter::listFunctions(const std::wstring& schema)
    {
        std::string sql =
            "SELECT p.proname AS name "
            "FROM pg_proc p "
            "JOIN pg_namespace n ON n.oid = p.pronamespace "
            "WHERE n.nspname = " + quoteLiteral(schema) +
            " AND p.prokind = 'f' "
            "ORDER BY p.proname";

        auto result = executeInternal(sql);
        std::vector<std::wstring> funcs;
        for (auto& row : result.rows)
        {
            auto it = row.find(L"name");
            if (it != row.end()) funcs.push_back(it->second);
        }
        return funcs;
    }

    std::wstring PostgreSQLAdapter::getFunctionSource(
        const std::wstring& name, const std::wstring& schema)
    {
        std::string sql =
            "SELECT pg_get_functiondef(p.oid) AS source "
            "FROM pg_proc p "
            "JOIN pg_namespace n ON n.oid = p.pronamespace "
            "WHERE p.proname = " + quoteLiteral(name) +
            " AND n.nspname = " + quoteLiteral(schema) +
            " LIMIT 1";

        auto result = executeInternal(sql);
        if (!result.rows.empty())
        {
            auto it = result.rows[0].find(L"source");
            if (it != result.rows[0].end()) return it->second;
        }
        return L"";
    }

    std::wstring PostgreSQLAdapter::getCreateTableSQL(
        const std::wstring& table, const std::wstring& schema)
    {
        // Build DDL from describeTable output — basic columns + NOT NULL + DEFAULT + PK
        auto columns = describeTable(table, schema);
        if (columns.empty()) return L"";

        std::wstring sql = L"CREATE TABLE \"" + schema + L"\".\"" + table + L"\" (\n";
        std::vector<std::wstring> pkCols;
        for (size_t i = 0; i < columns.size(); i++)
        {
            const auto& col = columns[i];
            sql += L"    \"" + col.name + L"\" " + col.dataType;
            if (!col.nullable) sql += L" NOT NULL";
            if (!col.defaultValue.empty())
                sql += L" DEFAULT " + col.defaultValue;
            if (i + 1 < columns.size() || std::any_of(columns.begin(), columns.end(),
                [](const ColumnInfo& c) { return c.isPrimaryKey; }))
                sql += L",";
            sql += L"\n";
            if (col.isPrimaryKey) pkCols.push_back(col.name);
        }
        if (!pkCols.empty())
        {
            sql += L"    PRIMARY KEY (";
            for (size_t i = 0; i < pkCols.size(); i++)
            {
                if (i > 0) sql += L", ";
                sql += L"\"" + pkCols[i] + L"\"";
            }
            sql += L")\n";
        }
        sql += L")";
        return sql;
    }

    // ── DML ─────────────────────────────────────────
    QueryResult PostgreSQLAdapter::insertRow(
        const std::wstring& table, const std::wstring& schema,
        const TableRow& values)
    {
        std::string sql = "INSERT INTO " + quoteIdentifier(schema) + "." + quoteIdentifier(table) + " (";
        std::string valsSql;
        bool first = true;
        for (auto& [col, val] : values)
        {
            // Blank cell → omit so SERIAL / DEFAULT / NOT NULL DEFAULT
            // apply. Users wanting explicit SQL NULL still get the null
            // sentinel path which emits NULL below.
            if (!isNullCell(val) && val.empty()) continue;
            if (!first) { sql += ", "; valsSql += ", "; }
            sql     += quoteIdentifier(col);
            valsSql += isNullCell(val) ? "NULL" : quoteLiteral(val);
            first = false;
        }
        // All columns blank → PostgreSQL's canonical all-defaults form.
        if (first)
            sql = "INSERT INTO " + quoteIdentifier(schema) + "." + quoteIdentifier(table) + " DEFAULT VALUES";
        else
            sql += ") VALUES (" + valsSql + ")";
        return executeInternal(sql);
    }

    QueryResult PostgreSQLAdapter::updateRow(
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
            // Correct SQL for NULL match is IS NULL, not = NULL
            if (isNullCell(val))
                sql += quoteIdentifier(col) + " IS NULL";
            else
                sql += quoteIdentifier(col) + " = " + quoteLiteral(val);
            first = false;
        }
        return executeInternal(sql);
    }

    QueryResult PostgreSQLAdapter::deleteRow(
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
    void PostgreSQLAdapter::beginTransaction()    { executeInternal("BEGIN"); }
    void PostgreSQLAdapter::commitTransaction()   { executeInternal("COMMIT"); }
    void PostgreSQLAdapter::rollbackTransaction() { executeInternal("ROLLBACK"); }

    // ── Server Info ─────────────────────────────────
    std::wstring PostgreSQLAdapter::serverVersion()
    {
        auto result = executeInternal("SHOW server_version");
        if (!result.rows.empty())
        {
            auto it = result.rows[0].find(L"server_version");
            if (it != result.rows[0].end()) return it->second;
        }
        return L"Unknown";
    }

    std::wstring PostgreSQLAdapter::currentDatabase()
    {
        auto result = executeInternal("SELECT current_database()");
        if (!result.rows.empty())
        {
            auto it = result.rows[0].find(L"current_database");
            if (it != result.rows[0].end()) return it->second;
        }
        return L"";
    }
}
