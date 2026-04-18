#include <windows.h>
#include "Models/SQLiteAdapter.h"
#include <sqlite3.h>
#include <chrono>

namespace DBModels
{
    // ── UTF-8 helpers ───────────────────────────────
    std::string SQLiteAdapter::toUtf8(const std::wstring& wstr)
    {
        if (wstr.empty()) return {};
        int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(),
            static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
        std::string result(size, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(),
            static_cast<int>(wstr.size()), &result[0], size, nullptr, nullptr);
        return result;
    }

    std::wstring SQLiteAdapter::fromUtf8(const std::string& str)
    {
        if (str.empty()) return {};
        int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(),
            static_cast<int>(str.size()), nullptr, 0);
        std::wstring result(size, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, str.c_str(),
            static_cast<int>(str.size()), &result[0], size);
        return result;
    }

    std::string SQLiteAdapter::quoteIdentifier(const std::wstring& name)
    {
        return "\"" + toUtf8(name) + "\"";
    }

    std::string SQLiteAdapter::quoteLiteral(const std::wstring& value)
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

    void SQLiteAdapter::ensureConnected() const
    {
        if (!connected_ || !db_)
            throw DatabaseError(DatabaseError::Code::ConnectionFailed, "Not connected to SQLite database");
    }

    // ── Constructor / Destructor ────────────────────
    SQLiteAdapter::SQLiteAdapter() = default;

    SQLiteAdapter::~SQLiteAdapter()
    {
        if (db_)
        {
            sqlite3_close(db_);
            db_ = nullptr;
        }
    }

    // ── Connection ──────────────────────────────────
    void SQLiteAdapter::connect(const ConnectionConfig& config, const std::wstring& /*password*/)
    {
        disconnect();

        filePath_ = config.filePath;
        std::string path = toUtf8(filePath_);

        int rc = sqlite3_open(path.c_str(), &db_);
        if (rc != SQLITE_OK)
        {
            std::string err = db_ ? sqlite3_errmsg(db_) : "sqlite3_open failed";
            if (db_) { sqlite3_close(db_); db_ = nullptr; }
            throw DatabaseError(DatabaseError::Code::ConnectionFailed, err);
        }

        connected_ = true;

        // Enable WAL mode and foreign keys
        executeInternal("PRAGMA journal_mode=WAL");
        executeInternal("PRAGMA foreign_keys=ON");
    }

    void SQLiteAdapter::disconnect()
    {
        if (db_)
        {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        connected_ = false;
    }

    bool SQLiteAdapter::testConnection(const ConnectionConfig& config, const std::wstring& password)
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

    bool SQLiteAdapter::isConnected() const { return connected_ && db_; }

    // ── Query Execution ─────────────────────────────
    QueryResult SQLiteAdapter::executeInternal(const std::string& sql)
    {
        ensureConnected();

        auto start = std::chrono::high_resolution_clock::now();

        QueryResult result;
        result.sql = fromUtf8(sql);

        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_, sql.c_str(), static_cast<int>(sql.size()), &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            result.success = false;
            result.error = fromUtf8(sqlite3_errmsg(db_));
            auto end = std::chrono::high_resolution_clock::now();
            result.executionTimeMs = std::chrono::duration<double, std::milli>(end - start).count();
            return result;
        }

        // Get column info
        int nCols = sqlite3_column_count(stmt);
        result.columnNames.reserve(nCols);
        result.columnTypes.reserve(nCols);
        for (int c = 0; c < nCols; c++)
        {
            const char* name = sqlite3_column_name(stmt, c);
            result.columnNames.push_back(fromUtf8(name ? name : ""));
            const char* type = sqlite3_column_decltype(stmt, c);
            result.columnTypes.push_back(fromUtf8(type ? type : "TEXT"));
        }

        // Execute and fetch rows.
        // PERFORMANCE: pre-reserve hash buckets per row, capture colName by
        // const-ref (no per-cell wstring copy), move-construct rows into vector
        // (matches PostgreSQL/MySQL adapter optimization).
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
        {
            TableRow row;
            row.reserve(nCols);
            for (int c = 0; c < nCols; c++)
            {
                const std::wstring& colName = result.columnNames[c];
                if (sqlite3_column_type(stmt, c) == SQLITE_NULL)
                {
                    row.emplace(colName, nullValue());
                }
                else
                {
                    const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, c));
                    row.emplace(colName, text ? fromUtf8(text) : L"");
                }
            }
            result.rows.push_back(std::move(row));
        }

        auto end = std::chrono::high_resolution_clock::now();
        result.executionTimeMs = std::chrono::duration<double, std::milli>(end - start).count();

        if (rc == SQLITE_DONE)
        {
            result.success = true;
            if (nCols > 0)
                result.totalRows = static_cast<int>(result.rows.size());
            else
                result.totalRows = sqlite3_changes(db_);
        }
        else
        {
            result.success = false;
            result.error = fromUtf8(sqlite3_errmsg(db_));
        }

        sqlite3_finalize(stmt);
        return result;
    }

    QueryResult SQLiteAdapter::execute(const std::wstring& sql)
    {
        return executeInternal(toUtf8(sql));
    }

    QueryResult SQLiteAdapter::fetchRows(
        const std::wstring& table, const std::wstring& /*schema*/,
        int limit, int offset,
        const std::wstring& orderBy, bool ascending)
    {
        // SQLite has no schema concept — table name only
        std::string sql = "SELECT * FROM " + quoteIdentifier(table);
        if (!orderBy.empty())
            sql += " ORDER BY " + quoteIdentifier(orderBy) + (ascending ? " ASC" : " DESC");
        sql += " LIMIT " + std::to_string(limit) + " OFFSET " + std::to_string(offset);

        auto result = executeInternal(sql);

        // Get total count
        std::string countSql = "SELECT COUNT(*) AS cnt FROM " + quoteIdentifier(table);
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
    std::vector<std::wstring> SQLiteAdapter::listDatabases()
    {
        // SQLite is single-database — return file name
        std::vector<std::wstring> dbs;
        if (!filePath_.empty())
        {
            auto pos = filePath_.find_last_of(L"\\/");
            dbs.push_back(pos != std::wstring::npos ? filePath_.substr(pos + 1) : filePath_);
        }
        return dbs;
    }

    std::vector<std::wstring> SQLiteAdapter::listSchemas()
    {
        return { L"main" }; // SQLite default schema
    }

    std::vector<TableInfo> SQLiteAdapter::listTables(const std::wstring& /*schema*/)
    {
        auto result = executeInternal(
            "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name");
        std::vector<TableInfo> tables;
        for (auto& row : result.rows)
        {
            TableInfo info;
            info.name = row[L"name"];
            info.schema = L"main";
            info.type = L"table";
            tables.push_back(info);
        }
        return tables;
    }

    std::vector<TableInfo> SQLiteAdapter::listViews(const std::wstring& /*schema*/)
    {
        auto result = executeInternal(
            "SELECT name FROM sqlite_master WHERE type='view' ORDER BY name");
        std::vector<TableInfo> views;
        for (auto& row : result.rows)
        {
            TableInfo info;
            info.name = row[L"name"];
            info.schema = L"main";
            info.type = L"view";
            views.push_back(info);
        }
        return views;
    }

    std::vector<ColumnInfo> SQLiteAdapter::describeTable(
        const std::wstring& table, const std::wstring& /*schema*/)
    {
        std::string sql = "PRAGMA table_info(" + quoteIdentifier(table) + ")";
        auto result = executeInternal(sql);
        std::vector<ColumnInfo> columns;
        for (auto& row : result.rows)
        {
            ColumnInfo col;
            col.name = row[L"name"];
            col.dataType = row[L"type"];
            col.nullable = (row[L"notnull"] == L"0");
            col.defaultValue = !isNullCell(row[L"dflt_value"]) ? row[L"dflt_value"] : L"";
            col.isPrimaryKey = (row[L"pk"] != L"0");
            auto cidIt = row.find(L"cid");
            if (cidIt != row.end() && !isNullCell(cidIt->second))
                col.ordinalPosition = std::stoi(toUtf8(cidIt->second)) + 1;
            columns.push_back(col);
        }

        // Mark FK columns
        auto fks = listForeignKeys(table, L"main");
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

    std::vector<IndexInfo> SQLiteAdapter::listIndexes(
        const std::wstring& table, const std::wstring& /*schema*/)
    {
        std::string sql = "PRAGMA index_list(" + quoteIdentifier(table) + ")";
        auto result = executeInternal(sql);
        std::vector<IndexInfo> indexes;
        for (auto& row : result.rows)
        {
            IndexInfo idx;
            idx.name = row[L"name"];
            idx.isUnique = (row[L"unique"] == L"1");
            idx.algorithm = L"BTREE";

            // Get columns for this index
            std::string colSql = "PRAGMA index_info(" + quoteIdentifier(idx.name) + ")";
            auto colResult = executeInternal(colSql);
            std::wstring cols;
            for (auto& cr : colResult.rows)
            {
                if (!cols.empty()) cols += L", ";
                cols += cr[L"name"];
            }
            idx.columns = cols;
            indexes.push_back(idx);
        }
        return indexes;
    }

    std::vector<ForeignKeyInfo> SQLiteAdapter::listForeignKeys(
        const std::wstring& table, const std::wstring& /*schema*/)
    {
        std::string sql = "PRAGMA foreign_key_list(" + quoteIdentifier(table) + ")";
        auto result = executeInternal(sql);
        std::vector<ForeignKeyInfo> fks;
        for (auto& row : result.rows)
        {
            ForeignKeyInfo fk;
            fk.name = std::wstring(L"fk_") + row[L"id"];
            fk.column = row[L"from"];
            fk.referencedTable = row[L"table"];
            fk.referencedColumn = row[L"to"];
            fk.onUpdate = row[L"on_update"];
            fk.onDelete = row[L"on_delete"];
            fks.push_back(fk);
        }
        return fks;
    }

    std::vector<std::wstring> SQLiteAdapter::listFunctions(const std::wstring& /*schema*/)
    {
        return {}; // SQLite has no user-defined functions via SQL
    }

    std::wstring SQLiteAdapter::getFunctionSource(
        const std::wstring& /*name*/, const std::wstring& /*schema*/)
    {
        return L""; // Not applicable for SQLite
    }

    std::wstring SQLiteAdapter::getCreateTableSQL(
        const std::wstring& table, const std::wstring& /*schema*/)
    {
        // sqlite_master stores the original CREATE TABLE statement
        std::string sql =
            "SELECT sql FROM sqlite_master WHERE type='table' AND name=" +
            quoteLiteral(table);
        auto result = execute(fromUtf8(sql));
        if (!result.rows.empty())
        {
            auto it = result.rows[0].find(L"sql");
            if (it != result.rows[0].end()) return it->second;
        }
        return L"";
    }

    // ── DML ─────────────────────────────────────────
    QueryResult SQLiteAdapter::insertRow(
        const std::wstring& table, const std::wstring& /*schema*/,
        const TableRow& values)
    {
        std::string sql = "INSERT INTO " + quoteIdentifier(table) + " (";
        std::string valsSql;
        bool first = true;
        for (auto& [col, val] : values)
        {
            // Blank cell → omit so INTEGER PRIMARY KEY autoincrement and
            // column DEFAULT expressions fire. Explicit SQL NULL still
            // travels via the null sentinel.
            if (!isNullCell(val) && val.empty()) continue;
            if (!first) { sql += ", "; valsSql += ", "; }
            sql += quoteIdentifier(col);
            valsSql += isNullCell(val) ? "NULL" : quoteLiteral(val);
            first = false;
        }
        if (first)
            sql = "INSERT INTO " + quoteIdentifier(table) + " DEFAULT VALUES";
        else
            sql += ") VALUES (" + valsSql + ")";
        return executeInternal(sql);
    }

    QueryResult SQLiteAdapter::updateRow(
        const std::wstring& table, const std::wstring& /*schema*/,
        const TableRow& setValues, const TableRow& whereValues)
    {
        std::string sql = "UPDATE " + quoteIdentifier(table) + " SET ";
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

    QueryResult SQLiteAdapter::deleteRow(
        const std::wstring& table, const std::wstring& /*schema*/,
        const TableRow& whereValues)
    {
        std::string sql = "DELETE FROM " + quoteIdentifier(table) + " WHERE ";
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
    void SQLiteAdapter::beginTransaction()    { executeInternal("BEGIN TRANSACTION"); }
    void SQLiteAdapter::commitTransaction()   { executeInternal("COMMIT"); }
    void SQLiteAdapter::rollbackTransaction() { executeInternal("ROLLBACK"); }

    // ── Server Info ─────────────────────────────────
    std::wstring SQLiteAdapter::serverVersion()
    {
        return fromUtf8(sqlite3_libversion());
    }

    std::wstring SQLiteAdapter::currentDatabase()
    {
        return filePath_;
    }
}
