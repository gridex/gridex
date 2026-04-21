#pragma once
#include "DatabaseAdapter.h"
#include <string>
#include <memory>

// Forward-declare httplib types to keep this header lightweight.
// The full httplib.h (WinSock + OpenSSL chain) is only pulled in
// by ClickHouseAdapter.cpp via the Impl pimpl pattern.
namespace httplib { class Client; }

namespace DBModels
{
    // ClickHouse adapter — communicates via the ClickHouse HTTP interface
    // (default port 8123).  No native clickhouse-cpp client is used so
    // there is no abseil/protobuf build dependency.
    //
    // Schema mapping: ClickHouse has no sub-database schemas.
    // listSchemas() returns a single entry equal to the connected
    // database name so the sidebar tree shows one "schema" node that
    // contains the tables, matching the PG adapter's UX contract.
    class ClickHouseAdapter : public DatabaseAdapter
    {
    public:
        ClickHouseAdapter();
        ~ClickHouseAdapter() override;

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
        // ClickHouse has no index/FK concepts — returns empty vectors.
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
        // ClickHouse UPDATE: ALTER TABLE t UPDATE col=val WHERE ...
        QueryResult updateRow(
            const std::wstring& table, const std::wstring& schema,
            const TableRow& setValues, const TableRow& whereValues) override;
        // ClickHouse DELETE: ALTER TABLE t DELETE WHERE ...
        QueryResult deleteRow(
            const std::wstring& table, const std::wstring& schema,
            const TableRow& whereValues) override;

        // ── Transactions ────────────────────────────
        // ClickHouse has no classic transactions; these are no-ops
        // that succeed silently (mutations are applied immediately).
        void beginTransaction() override;
        void commitTransaction() override;
        void rollbackTransaction() override;

        // ── Server Info ─────────────────────────────
        std::wstring serverVersion() override;
        std::wstring currentDatabase() override;

        // ── SQL string assembly ─────────────────────
        std::wstring quoteSqlLiteral(const std::wstring& value) const override;
        std::wstring quoteSqlIdentifier(const std::wstring& name) const override;

    private:
        // Pimpl: keeps httplib headers out of all translation units
        // that include this header (saves ~2 s compile time per TU).
        struct Impl;
        std::unique_ptr<Impl> impl_;

        bool connected_ = false;
        std::wstring currentDb_;

        // ── Helpers ──────────────────────────────────
        static std::string  toUtf8(const std::wstring& wstr);
        static std::wstring fromUtf8(const std::string& str);
        // Backtick-quote an identifier for ClickHouse SQL.
        static std::string  quoteIdentifier(const std::wstring& name);
        // Single-quote a string literal with escaping.
        static std::string  quoteLiteral(const std::wstring& value);
        void ensureConnected() const;

        // Execute a raw UTF-8 SQL string, parse JSONCompact response.
        QueryResult executeInternal(const std::string& sql);
    };
}
