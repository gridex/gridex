// MSSQLAdapter.cpp — SQL Server connection, query execution, schema
// inspection, CRUD, and transactions via Windows ODBC API.
//
// Zero external dependencies: uses sql.h / sqlext.h from Windows SDK
// and links against odbc32.lib (ships with every Windows install).

#include <windows.h>
#include <sql.h>
#include <sqlext.h>
#include "Models/MSSQLAdapter.h"

#include <algorithm>
#include <chrono>
#include <set>
#include <sstream>

namespace DBModels
{
    // ── UTF-8 helpers ─────────────────────────────────────────
    std::string MSSQLAdapter::toUtf8(const std::wstring& s)
    {
        if (s.empty()) return {};
        int sz = WideCharToMultiByte(CP_UTF8, 0, s.c_str(),
            static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
        std::string out(sz, '\0');
        WideCharToMultiByte(CP_UTF8, 0, s.c_str(),
            static_cast<int>(s.size()), &out[0], sz, nullptr, nullptr);
        return out;
    }

    std::wstring MSSQLAdapter::fromUtf8(const std::string& s)
    {
        if (s.empty()) return {};
        int sz = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
            static_cast<int>(s.size()), nullptr, 0);
        std::wstring out(sz, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
            static_cast<int>(s.size()), &out[0], sz);
        return out;
    }

    // SQL Server uses [bracket] quoting for identifiers
    std::string MSSQLAdapter::quoteIdentifier(const std::wstring& name)
    {
        auto utf8 = toUtf8(name);
        std::string escaped;
        escaped.reserve(utf8.size() + 2);
        escaped += '[';
        for (char c : utf8)
        {
            if (c == ']') escaped += "]]";
            else escaped += c;
        }
        escaped += ']';
        return escaped;
    }

    // SQL Server uses N'...' for Unicode string literals
    std::string MSSQLAdapter::quoteLiteral(const std::wstring& value)
    {
        auto utf8 = toUtf8(value);
        std::string escaped;
        escaped.reserve(utf8.size() + 3);
        escaped += "N'";
        for (char c : utf8)
        {
            if (c == '\'') escaped += "''";
            else escaped += c;
        }
        escaped += '\'';
        return escaped;
    }

    void MSSQLAdapter::ensureConnected() const
    {
        if (!connected_ || !hDbc_)
            throw DatabaseError(DatabaseError::Code::ConnectionFailed, "Not connected to SQL Server");
    }

    // Extract diagnostic message from an ODBC handle
    std::string MSSQLAdapter::odbcError(short handleType, void* handle)
    {
        SQLWCHAR state[6], msg[1024];
        SQLINTEGER nativeErr;
        SQLSMALLINT msgLen;
        if (SQLGetDiagRecW(handleType, handle, 1, state, &nativeErr,
                           msg, 1024, &msgLen) == SQL_SUCCESS)
        {
            return toUtf8(std::wstring(msg, msgLen));
        }
        return "Unknown ODBC error";
    }

    // Try ODBC drivers in preference order
    std::string MSSQLAdapter::findDriver()
    {
        // Check available drivers via SQLDrivers
        SQLHENV env;
        if (SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env) != SQL_SUCCESS)
            return "{ODBC Driver 18 for SQL Server}"; // fallback guess

        SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0);

        const char* candidates[] = {
            "ODBC Driver 18 for SQL Server",
            "ODBC Driver 17 for SQL Server",
            "SQL Server"
        };

        SQLWCHAR driverDesc[256], driverAttr[256];
        SQLSMALLINT descLen, attrLen;
        SQLUSMALLINT dir = SQL_FETCH_FIRST;

        std::string found;
        while (SQLDriversW(env, dir, driverDesc, 256, &descLen,
                           driverAttr, 256, &attrLen) == SQL_SUCCESS)
        {
            auto name = toUtf8(std::wstring(driverDesc, descLen));
            for (auto& c : candidates)
            {
                if (name == c && found.empty())
                {
                    found = std::string("{") + c + "}";
                    break;
                }
            }
            dir = SQL_FETCH_NEXT;
        }
        SQLFreeHandle(SQL_HANDLE_ENV, env);
        return found.empty() ? "{ODBC Driver 18 for SQL Server}" : found;
    }

    // ── Constructor / Destructor ───────────────────────────────
    MSSQLAdapter::MSSQLAdapter() = default;

    MSSQLAdapter::~MSSQLAdapter()
    {
        disconnect();
    }

    // ── Connection ────────────────────────────────────────────
    void MSSQLAdapter::connect(
        const ConnectionConfig& config, const std::wstring& password)
    {
        disconnect();

        auto driver = findDriver();
        auto host = toUtf8(config.host.empty() ? L"127.0.0.1" : config.host);
        auto db   = toUtf8(config.database.empty() ? L"master" : config.database);
        auto user = toUtf8(config.username.empty() ? L"sa" : config.username);
        auto pass = toUtf8(password);

        // Build ODBC connection string
        std::ostringstream cs;
        cs << "DRIVER=" << driver << ";";
        cs << "SERVER=" << host << "," << (config.port > 0 ? config.port : 1433) << ";";
        cs << "DATABASE=" << db << ";";
        cs << "UID=" << user << ";";
        cs << "PWD=" << pass << ";";

        // SSL/Encryption.
        // The legacy "{SQL Server}" driver (shipped with Windows since
        // 2000) does NOT understand Encrypt= or TrustServerCertificate=
        // and throws "[DBNETLIB]SSL Security error" if we include them.
        // Only emit encryption flags for modern ODBC Driver 17/18+.
        bool isModernDriver = (driver.find("ODBC Driver") != std::string::npos);
        if (isModernDriver)
        {
            switch (config.sslMode)
            {
            case SSLMode::Disabled:
                cs << "Encrypt=no;";
                break;
            case SSLMode::Required:
            case SSLMode::VerifyCA:
            case SSLMode::VerifyIdentity:
                cs << "Encrypt=yes;TrustServerCertificate=no;";
                break;
            default: // Preferred
                cs << "Encrypt=yes;TrustServerCertificate=yes;";
                break;
            }
        }

        cs << "Connection Timeout=15;";

        auto connStr = cs.str();
        auto wConnStr = fromUtf8(connStr);

        // Allocate ODBC environment
        if (SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &hEnv_) != SQL_SUCCESS)
            throw DatabaseError(DatabaseError::Code::ConnectionFailed, "SQLAllocHandle(ENV) failed");
        SQLSetEnvAttr(hEnv_, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0);

        // Allocate connection
        if (SQLAllocHandle(SQL_HANDLE_DBC, hEnv_, &hDbc_) != SQL_SUCCESS)
        {
            SQLFreeHandle(SQL_HANDLE_ENV, hEnv_); hEnv_ = nullptr;
            throw DatabaseError(DatabaseError::Code::ConnectionFailed, "SQLAllocHandle(DBC) failed");
        }

        // Connect
        SQLWCHAR outStr[1024];
        SQLSMALLINT outLen;
        auto rc = SQLDriverConnectW(hDbc_, nullptr,
            (SQLWCHAR*)wConnStr.c_str(), SQL_NTS,
            outStr, 1024, &outLen, SQL_DRIVER_NOPROMPT);

        if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO)
        {
            auto err = odbcError(SQL_HANDLE_DBC, hDbc_);
            SQLFreeHandle(SQL_HANDLE_DBC, hDbc_); hDbc_ = nullptr;
            SQLFreeHandle(SQL_HANDLE_ENV, hEnv_); hEnv_ = nullptr;
            throw DatabaseError(DatabaseError::Code::ConnectionFailed,
                "SQL Server connection failed: " + err);
        }

        connected_ = true;
        currentDb_ = db;
    }

    void MSSQLAdapter::disconnect()
    {
        if (hDbc_)
        {
            if (connected_) SQLDisconnect(hDbc_);
            SQLFreeHandle(SQL_HANDLE_DBC, hDbc_);
            hDbc_ = nullptr;
        }
        if (hEnv_)
        {
            SQLFreeHandle(SQL_HANDLE_ENV, hEnv_);
            hEnv_ = nullptr;
        }
        connected_ = false;
        currentDb_.clear();
    }

    bool MSSQLAdapter::testConnection(
        const ConnectionConfig& config, const std::wstring& password)
    {
        try
        {
            connect(config, password);
            auto r = executeInternal("SELECT 1");
            disconnect();
            return r.success;
        }
        catch (...) { disconnect(); return false; }
    }

    bool MSSQLAdapter::isConnected() const
    {
        return connected_ && hDbc_ != nullptr;
    }

    // ── Query Execution ───────────────────────────────────────
    QueryResult MSSQLAdapter::executeInternal(const std::string& sql)
    {
        ensureConnected();

        SQLHSTMT hStmt = nullptr;
        if (SQLAllocHandle(SQL_HANDLE_STMT, hDbc_, &hStmt) != SQL_SUCCESS)
            throw DatabaseError(DatabaseError::Code::QueryFailed, "SQLAllocHandle(STMT) failed");

        auto wSql = fromUtf8(sql);

        auto start = std::chrono::high_resolution_clock::now();
        auto rc = SQLExecDirectW(hStmt, (SQLWCHAR*)wSql.c_str(), SQL_NTS);
        auto end = std::chrono::high_resolution_clock::now();

        QueryResult result;
        result.sql = fromUtf8(sql);
        result.executionTimeMs =
            std::chrono::duration<double, std::milli>(end - start).count();

        if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO && rc != SQL_NO_DATA)
        {
            result.success = false;
            result.error = fromUtf8(odbcError(SQL_HANDLE_STMT, hStmt));
            SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
            return result;
        }

        // Check if result set exists
        SQLSMALLINT nCols = 0;
        SQLNumResultCols(hStmt, &nCols);

        if (nCols > 0)
        {
            // SELECT-type query — fetch column names
            result.columnNames.reserve(nCols);
            result.columnTypes.reserve(nCols);
            for (SQLSMALLINT i = 1; i <= nCols; i++)
            {
                SQLWCHAR colName[256];
                SQLSMALLINT nameLen, dataType, decDigits, nullable;
                SQLULEN colSize;
                SQLDescribeColW(hStmt, i, colName, 256, &nameLen,
                    &dataType, &colSize, &decDigits, &nullable);
                result.columnNames.push_back(std::wstring(colName, nameLen));
                result.columnTypes.push_back(std::to_wstring(dataType));
            }

            // Fetch rows
            while (SQLFetch(hStmt) == SQL_SUCCESS)
            {
                TableRow row;
                row.reserve(nCols);
                for (SQLSMALLINT i = 1; i <= nCols; i++)
                {
                    SQLWCHAR buf[4096];
                    SQLLEN indicator;
                    auto fetchRc = SQLGetData(hStmt, i, SQL_C_WCHAR,
                        buf, sizeof(buf), &indicator);

                    const auto& colName = result.columnNames[i - 1];
                    if (indicator == SQL_NULL_DATA)
                        row.emplace(colName, nullValue());
                    else if (fetchRc == SQL_SUCCESS || fetchRc == SQL_SUCCESS_WITH_INFO)
                        row.emplace(colName, std::wstring(buf));
                    else
                        row.emplace(colName, L"");
                }
                result.rows.push_back(std::move(row));
            }
            result.totalRows = static_cast<int>(result.rows.size());
            result.success = true;
        }
        else
        {
            // DML: count affected rows
            SQLLEN rowCount = 0;
            SQLRowCount(hStmt, &rowCount);
            result.totalRows = static_cast<int>(rowCount);
            result.success = true;
        }

        SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
        return result;
    }

    QueryResult MSSQLAdapter::execute(const std::wstring& sql)
    {
        return executeInternal(toUtf8(sql));
    }

    QueryResult MSSQLAdapter::fetchRows(
        const std::wstring& table, const std::wstring& schema,
        int limit, int offset,
        const std::wstring& orderBy, bool ascending)
    {
        auto sch = schema.empty() ? L"dbo" : schema;
        std::string sql = "SELECT * FROM " +
            quoteIdentifier(sch) + "." + quoteIdentifier(table);

        // OFFSET/FETCH requires ORDER BY in SQL Server
        if (!orderBy.empty())
            sql += " ORDER BY " + quoteIdentifier(orderBy) + (ascending ? " ASC" : " DESC");
        else
            sql += " ORDER BY (SELECT NULL)";

        sql += " OFFSET " + std::to_string(offset) + " ROWS";
        sql += " FETCH NEXT " + std::to_string(limit) + " ROWS ONLY";

        auto result = executeInternal(sql);

        // Get total row count for pagination
        std::string countSql = "SELECT COUNT(*) FROM " +
            quoteIdentifier(sch) + "." + quoteIdentifier(table);
        auto countResult = executeInternal(countSql);
        if (countResult.success && !countResult.rows.empty())
        {
            auto& firstRow = countResult.rows[0];
            if (!firstRow.empty())
                result.totalRows = std::stoi(toUtf8(firstRow.begin()->second));
        }

        result.currentPage = (limit > 0) ? (offset / limit) + 1 : 1;
        result.pageSize = limit;
        return result;
    }

    // ── Schema Inspection ─────────────────────────────────────
    std::vector<std::wstring> MSSQLAdapter::listDatabases()
    {
        auto result = executeInternal(
            "SELECT name FROM sys.databases "
            "WHERE name NOT IN ('master','tempdb','model','msdb') "
            "ORDER BY name");
        std::vector<std::wstring> dbs;
        for (auto& row : result.rows)
        {
            auto it = row.find(L"name");
            if (it != row.end()) dbs.push_back(it->second);
        }
        return dbs;
    }

    std::vector<std::wstring> MSSQLAdapter::listSchemas()
    {
        auto result = executeInternal(
            "SELECT SCHEMA_NAME FROM INFORMATION_SCHEMA.SCHEMATA "
            "WHERE SCHEMA_NAME NOT IN ("
            "'sys','INFORMATION_SCHEMA','guest','db_owner','db_accessadmin',"
            "'db_securityadmin','db_ddladmin','db_backupoperator',"
            "'db_datareader','db_datawriter','db_denydatareader','db_denydatawriter'"
            ") ORDER BY SCHEMA_NAME");
        std::vector<std::wstring> schemas;
        for (auto& row : result.rows)
        {
            auto it = row.find(L"SCHEMA_NAME");
            if (it != row.end()) schemas.push_back(it->second);
        }
        return schemas;
    }

    std::vector<TableInfo> MSSQLAdapter::listTables(const std::wstring& schema)
    {
        auto sch = schema.empty() ? "dbo" : toUtf8(schema);
        auto result = executeInternal(
            "SELECT TABLE_NAME FROM INFORMATION_SCHEMA.TABLES "
            "WHERE TABLE_SCHEMA = '" + sch + "' AND TABLE_TYPE = 'BASE TABLE' "
            "ORDER BY TABLE_NAME");
        std::vector<TableInfo> tables;
        for (auto& row : result.rows)
        {
            TableInfo t;
            auto it = row.find(L"TABLE_NAME");
            if (it != row.end()) t.name = it->second;
            t.type = L"table";
            tables.push_back(t);
        }
        return tables;
    }

    std::vector<TableInfo> MSSQLAdapter::listViews(const std::wstring& schema)
    {
        auto sch = schema.empty() ? "dbo" : toUtf8(schema);
        auto result = executeInternal(
            "SELECT TABLE_NAME FROM INFORMATION_SCHEMA.VIEWS "
            "WHERE TABLE_SCHEMA = '" + sch + "' "
            "ORDER BY TABLE_NAME");
        std::vector<TableInfo> views;
        for (auto& row : result.rows)
        {
            TableInfo t;
            auto it = row.find(L"TABLE_NAME");
            if (it != row.end()) t.name = it->second;
            t.type = L"view";
            views.push_back(t);
        }
        return views;
    }

    std::vector<ColumnInfo> MSSQLAdapter::describeTable(
        const std::wstring& table, const std::wstring& schema)
    {
        auto sch = schema.empty() ? "dbo" : toUtf8(schema);
        auto tbl = toUtf8(table);

        // Column metadata
        auto result = executeInternal(
            "SELECT c.COLUMN_NAME, c.DATA_TYPE, c.IS_NULLABLE, "
            "c.COLUMN_DEFAULT, c.CHARACTER_MAXIMUM_LENGTH, c.ORDINAL_POSITION, "
            "COLUMNPROPERTY(OBJECT_ID('" + sch + "." + tbl + "'), c.COLUMN_NAME, 'IsIdentity') AS is_identity "
            "FROM INFORMATION_SCHEMA.COLUMNS c "
            "WHERE c.TABLE_SCHEMA = '" + sch + "' AND c.TABLE_NAME = '" + tbl + "' "
            "ORDER BY c.ORDINAL_POSITION");

        // Primary key columns
        auto pkResult = executeInternal(
            "SELECT ku.COLUMN_NAME "
            "FROM INFORMATION_SCHEMA.TABLE_CONSTRAINTS tc "
            "JOIN INFORMATION_SCHEMA.KEY_COLUMN_USAGE ku "
            "  ON tc.CONSTRAINT_NAME = ku.CONSTRAINT_NAME "
            "WHERE tc.TABLE_SCHEMA = '" + sch + "' "
            "  AND tc.TABLE_NAME = '" + tbl + "' "
            "  AND tc.CONSTRAINT_TYPE = 'PRIMARY KEY'");

        std::set<std::wstring> pkCols;
        for (auto& row : pkResult.rows)
        {
            auto it = row.find(L"COLUMN_NAME");
            if (it != row.end()) pkCols.insert(it->second);
        }

        std::vector<ColumnInfo> cols;
        for (auto& row : result.rows)
        {
            ColumnInfo col;
            auto nameIt = row.find(L"COLUMN_NAME");
            if (nameIt != row.end()) col.name = nameIt->second;

            auto typeIt = row.find(L"DATA_TYPE");
            if (typeIt != row.end()) col.dataType = typeIt->second;

            // Append length for char types
            auto lenIt = row.find(L"CHARACTER_MAXIMUM_LENGTH");
            if (lenIt != row.end() && !isNullCell(lenIt->second) && !lenIt->second.empty())
            {
                if (lenIt->second == L"-1")
                    col.dataType += L"(MAX)";
                else
                    col.dataType += L"(" + lenIt->second + L")";
            }

            auto nullIt = row.find(L"IS_NULLABLE");
            col.nullable = (nullIt != row.end() && nullIt->second == L"YES");

            auto defIt = row.find(L"COLUMN_DEFAULT");
            if (defIt != row.end() && !isNullCell(defIt->second))
                col.defaultValue = defIt->second;

            col.isPrimaryKey = pkCols.count(col.name) > 0;

            auto identIt = row.find(L"is_identity");
            if (identIt != row.end() && identIt->second == L"1")
                col.comment = L"IDENTITY";

            auto ordIt = row.find(L"ORDINAL_POSITION");
            if (ordIt != row.end())
                try { col.ordinalPosition = std::stoi(toUtf8(ordIt->second)); } catch (...) {}

            cols.push_back(col);
        }
        return cols;
    }

    std::vector<IndexInfo> MSSQLAdapter::listIndexes(
        const std::wstring& table, const std::wstring& schema)
    {
        // Stub — can be enhanced with sys.indexes + sys.index_columns
        return {};
    }

    std::vector<ForeignKeyInfo> MSSQLAdapter::listForeignKeys(
        const std::wstring& table, const std::wstring& schema)
    {
        auto sch = schema.empty() ? "dbo" : toUtf8(schema);
        auto tbl = toUtf8(table);

        auto result = executeInternal(
            "SELECT "
            "  fk.CONSTRAINT_NAME, "
            "  cu.COLUMN_NAME, "
            "  pk.TABLE_SCHEMA AS ref_schema, "
            "  pk.TABLE_NAME AS ref_table, "
            "  pt.COLUMN_NAME AS ref_column "
            "FROM INFORMATION_SCHEMA.REFERENTIAL_CONSTRAINTS fk "
            "JOIN INFORMATION_SCHEMA.KEY_COLUMN_USAGE cu "
            "  ON fk.CONSTRAINT_NAME = cu.CONSTRAINT_NAME "
            "JOIN INFORMATION_SCHEMA.TABLE_CONSTRAINTS pk "
            "  ON fk.UNIQUE_CONSTRAINT_NAME = pk.CONSTRAINT_NAME "
            "JOIN INFORMATION_SCHEMA.KEY_COLUMN_USAGE pt "
            "  ON pk.CONSTRAINT_NAME = pt.CONSTRAINT_NAME "
            "WHERE cu.TABLE_SCHEMA = '" + sch + "' "
            "  AND cu.TABLE_NAME = '" + tbl + "'");

        std::vector<ForeignKeyInfo> fks;
        for (auto& row : result.rows)
        {
            ForeignKeyInfo fk;
            auto it = row.find(L"CONSTRAINT_NAME");
            if (it != row.end()) fk.name = it->second;
            it = row.find(L"COLUMN_NAME");
            if (it != row.end()) fk.column = it->second;
            it = row.find(L"ref_table");
            if (it != row.end()) fk.referencedTable = it->second;
            it = row.find(L"ref_column");
            if (it != row.end()) fk.referencedColumn = it->second;
            fks.push_back(fk);
        }
        return fks;
    }

    std::vector<std::wstring> MSSQLAdapter::listFunctions(const std::wstring& schema)
    {
        auto sch = schema.empty() ? "dbo" : toUtf8(schema);
        // Return BOTH functions and stored procedures — sidebar shows
        // them under "Functions" group. The ROUTINE_TYPE prefix lets
        // users tell them apart at a glance.
        auto result = executeInternal(
            "SELECT ROUTINE_NAME, ROUTINE_TYPE FROM INFORMATION_SCHEMA.ROUTINES "
            "WHERE ROUTINE_SCHEMA = '" + sch + "' "
            "ORDER BY ROUTINE_TYPE, ROUTINE_NAME");
        std::vector<std::wstring> funcs;
        for (auto& row : result.rows)
        {
            auto nameIt = row.find(L"ROUTINE_NAME");
            auto typeIt = row.find(L"ROUTINE_TYPE");
            if (nameIt == row.end()) continue;
            // Prefix with [SP] or [FN] so user can distinguish
            std::wstring prefix;
            if (typeIt != row.end() && typeIt->second == L"PROCEDURE")
                prefix = L"[SP] ";
            else if (typeIt != row.end() && typeIt->second == L"FUNCTION")
                prefix = L"[FN] ";
            funcs.push_back(prefix + nameIt->second);
        }
        return funcs;
    }

    std::wstring MSSQLAdapter::getFunctionSource(
        const std::wstring& name, const std::wstring& schema)
    {
        auto sch = schema.empty() ? "dbo" : toUtf8(schema);
        auto result = executeInternal(
            "SELECT ROUTINE_DEFINITION FROM INFORMATION_SCHEMA.ROUTINES "
            "WHERE ROUTINE_SCHEMA = '" + sch + "' "
            "  AND ROUTINE_NAME = '" + toUtf8(name) + "'");
        if (!result.rows.empty())
        {
            auto it = result.rows[0].find(L"ROUTINE_DEFINITION");
            if (it != result.rows[0].end()) return it->second;
        }
        return {};
    }

    std::wstring MSSQLAdapter::getCreateTableSQL(
        const std::wstring& table, const std::wstring& schema)
    {
        // Best-effort reconstruct from column metadata
        auto cols = describeTable(table, schema);
        if (cols.empty()) return {};

        auto sch = schema.empty() ? L"dbo" : schema;
        std::wstring sql = L"CREATE TABLE [" + sch + L"].[" + table + L"] (\n";
        for (size_t i = 0; i < cols.size(); i++)
        {
            auto& c = cols[i];
            sql += L"  [" + c.name + L"] " + c.dataType;
            if (c.comment == L"IDENTITY") sql += L" IDENTITY(1,1)";
            if (!c.nullable) sql += L" NOT NULL";
            if (!c.defaultValue.empty()) sql += L" DEFAULT " + c.defaultValue;
            if (i + 1 < cols.size()) sql += L",";
            sql += L"\n";
        }
        sql += L")";
        return sql;
    }

    // ── Data Manipulation ─────────────────────────────────────
    QueryResult MSSQLAdapter::insertRow(
        const std::wstring& table, const std::wstring& schema,
        const TableRow& values)
    {
        auto sch = schema.empty() ? L"dbo" : schema;
        std::string sql = "INSERT INTO " +
            quoteIdentifier(sch) + "." + quoteIdentifier(table) + " (";
        std::string vals = "VALUES (";
        bool first = true;
        for (auto& [col, val] : values)
        {
            // Skip SQL NULL (already the case) and blank cells — blank
            // means "use IDENTITY / DEFAULT / nullable default"; emitting
            // '' would bomb on INT columns and fight IDENTITY_INSERT.
            if (isNullCell(val) || val.empty()) continue;
            if (!first) { sql += ", "; vals += ", "; }
            sql += quoteIdentifier(col);
            vals += quoteLiteral(val);
            first = false;
        }
        if (first)
            sql = "INSERT INTO " + quoteIdentifier(sch) + "." +
                  quoteIdentifier(table) + " DEFAULT VALUES";
        else
            sql += ") " + vals + ")";
        return executeInternal(sql);
    }

    QueryResult MSSQLAdapter::updateRow(
        const std::wstring& table, const std::wstring& schema,
        const TableRow& setValues, const TableRow& whereValues)
    {
        auto sch = schema.empty() ? L"dbo" : schema;
        std::string sql = "UPDATE " +
            quoteIdentifier(sch) + "." + quoteIdentifier(table) + " SET ";
        bool first = true;
        for (auto& [col, val] : setValues)
        {
            if (!first) sql += ", ";
            sql += quoteIdentifier(col) + " = ";
            sql += isNullCell(val) ? "NULL" : quoteLiteral(val);
            first = false;
        }
        sql += " WHERE ";
        first = true;
        for (auto& [col, val] : whereValues)
        {
            if (!first) sql += " AND ";
            sql += quoteIdentifier(col) + " = " + quoteLiteral(val);
            first = false;
        }
        return executeInternal(sql);
    }

    QueryResult MSSQLAdapter::deleteRow(
        const std::wstring& table, const std::wstring& schema,
        const TableRow& whereValues)
    {
        auto sch = schema.empty() ? L"dbo" : schema;
        std::string sql = "DELETE FROM " +
            quoteIdentifier(sch) + "." + quoteIdentifier(table) + " WHERE ";
        bool first = true;
        for (auto& [col, val] : whereValues)
        {
            if (!first) sql += " AND ";
            sql += quoteIdentifier(col) + " = " + quoteLiteral(val);
            first = false;
        }
        return executeInternal(sql);
    }

    // ── Transactions ──────────────────────────────────────────
    void MSSQLAdapter::beginTransaction()
    {
        ensureConnected();
        executeInternal("BEGIN TRANSACTION");
    }

    void MSSQLAdapter::commitTransaction()
    {
        ensureConnected();
        executeInternal("COMMIT");
    }

    void MSSQLAdapter::rollbackTransaction()
    {
        ensureConnected();
        executeInternal("ROLLBACK");
    }

    // ── Server Info ───────────────────────────────────────────
    std::wstring MSSQLAdapter::serverVersion()
    {
        try
        {
            auto result = executeInternal(
                "SELECT SERVERPROPERTY('ProductVersion') AS ver");
            if (!result.rows.empty())
            {
                auto it = result.rows[0].find(L"ver");
                if (it != result.rows[0].end()) return it->second;
            }
        }
        catch (...) {}
        return L"Unknown";
    }

    std::wstring MSSQLAdapter::currentDatabase()
    {
        return fromUtf8(currentDb_);
    }
}
