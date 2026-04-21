#pragma once
#include "DatabaseAdapter.h"
#include <string>

// Forward declare libpq types to avoid including libpq-fe.h in header
struct pg_conn;
typedef struct pg_conn PGconn;

namespace DBModels
{
    class PostgreSQLAdapter : public DatabaseAdapter
    {
    public:
        PostgreSQLAdapter();
        ~PostgreSQLAdapter() override;

        // ── Connection ──────────────────────────────
        void connect(const ConnectionConfig& config, const std::wstring& password) override;
        void disconnect() override;
        bool testConnection(const ConnectionConfig& config, const std::wstring& password) override;
        bool isConnected() const override;

        // ── Query Execution ─────────────────────────
        QueryResult execute(const std::wstring& sql) override;
        QueryResult fetchRows(
            const std::wstring& table, const std::wstring& schema,
            int limit, int offset,
            const std::wstring& orderBy, bool ascending) override;

        // ── Schema Inspection ───────────────────────
        std::vector<std::wstring> listDatabases() override;
        std::vector<std::wstring> listSchemas() override;
        std::vector<TableInfo> listTables(const std::wstring& schema) override;
        std::vector<TableInfo> listViews(const std::wstring& schema) override;
        std::vector<ColumnInfo> describeTable(
            const std::wstring& table, const std::wstring& schema) override;
        std::vector<IndexInfo> listIndexes(
            const std::wstring& table, const std::wstring& schema) override;
        std::vector<ForeignKeyInfo> listForeignKeys(
            const std::wstring& table, const std::wstring& schema) override;
        std::vector<std::wstring> listFunctions(const std::wstring& schema) override;
        std::wstring getFunctionSource(
            const std::wstring& name, const std::wstring& schema) override;
        std::wstring getCreateTableSQL(
            const std::wstring& table, const std::wstring& schema) override;

        // ── Data Manipulation ───────────────────────
        QueryResult insertRow(
            const std::wstring& table, const std::wstring& schema,
            const TableRow& values) override;
        QueryResult updateRow(
            const std::wstring& table, const std::wstring& schema,
            const TableRow& setValues, const TableRow& whereValues) override;
        QueryResult deleteRow(
            const std::wstring& table, const std::wstring& schema,
            const TableRow& whereValues) override;

        // ── Transactions ────────────────────────────
        void beginTransaction() override;
        void commitTransaction() override;
        void rollbackTransaction() override;

        // ── Server Info ─────────────────────────────
        std::wstring serverVersion() override;
        std::wstring currentDatabase() override;

        // ── SQL string assembly ─────────────────────
        std::wstring quoteSqlLiteral(const std::wstring& value) const override;
        std::wstring quoteSqlIdentifier(const std::wstring& name) const override;

        // Getters so hosts can open a second "side" connection with the
        // same credentials (used by the EE connection monitor to avoid
        // serializing its pg_stat_activity polling against the user's
        // foreground SQL editor queries on the primary PGconn).
        const ConnectionConfig& getConnectionConfig() const { return storedConfig_; }
        const std::wstring& getStoredPassword() const { return storedPassword_; }

    private:
        PGconn* conn_ = nullptr;
        bool connected_ = false;
        ConnectionConfig storedConfig_;
        std::wstring storedPassword_;

        // Helper: convert wstring to UTF-8 for libpq
        static std::string toUtf8(const std::wstring& wstr);
        // Helper: convert UTF-8 to wstring
        static std::wstring fromUtf8(const std::string& str);
        // Helper: quote identifier (double-quotes)
        static std::string quoteIdentifier(const std::wstring& name);
        // Helper: quote literal (single-quotes with escaping)
        static std::string quoteLiteral(const std::wstring& value);
        // Helper: ensure connected, throw if not
        void ensureConnected() const;
        // Helper: execute a query and return result, throw on error
        QueryResult executeInternal(const std::string& sql);
    };
}
