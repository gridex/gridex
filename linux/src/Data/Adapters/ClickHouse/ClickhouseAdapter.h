#pragma once

// ClickHouse adapter — speaks ClickHouse's HTTP interface (port 8123/8443).
// Uses Qt Network with a blocking event loop (matches the synchronous shape
// of IDatabaseAdapter). JSONCompact for SELECTs, plain statements for DDL/DML.
//
// SSL / TLS:
//   * Plain HTTP when ConnectionConfig::sslEnabled is false (port 8123).
//   * HTTPS via Qt's default trust store when sslEnabled is true (port 8443).
//   * Custom CA pinning + mTLS via PKCS#12 — TODO; not implemented in this
//     port. Use sslEnabled + the system store for ClickHouse Cloud which
//     fronts via a public CA.
//
// Mutations:
//   ClickHouse implements UPDATE/DELETE as `ALTER TABLE ... UPDATE/DELETE`
//   on MergeTree-family engines. We pre-check `system.tables.engine` and
//   refuse the operation with a clear error for engines that can't mutate
//   (Log / View / Dictionary / etc).

#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "Core/Models/Database/ConnectionConfig.h"
#include "Core/Protocols/Database/IDatabaseAdapter.h"

class QNetworkAccessManager;

namespace gridex {

class ClickhouseAdapter : public IDatabaseAdapter {
public:
    ClickhouseAdapter();
    ~ClickhouseAdapter() override;

    // Identity
    DatabaseType databaseType() const noexcept override { return DatabaseType::ClickHouse; }
    bool isConnected() const noexcept override;

    // Lifecycle
    void connect(const ConnectionConfig& config, const std::optional<std::string>& password) override;
    void disconnect() override;
    bool testConnection(const ConnectionConfig& config, const std::optional<std::string>& password) override;

    // Query
    QueryResult execute(const std::string& query, const std::vector<QueryParameter>& parameters) override;
    QueryResult executeRaw(const std::string& sql) override;

    // Schema
    std::vector<std::string> listDatabases() override;
    std::vector<std::string> listSchemas(const std::optional<std::string>& database) override;
    std::vector<TableInfo>  listTables(const std::optional<std::string>& schema) override;
    std::vector<ViewInfo>   listViews(const std::optional<std::string>& schema) override;
    TableDescription        describeTable(const std::string& name,
                                          const std::optional<std::string>& schema) override;
    std::vector<IndexInfo>      listIndexes(const std::string& table,
                                            const std::optional<std::string>& schema) override;
    std::vector<ForeignKeyInfo> listForeignKeys(const std::string& table,
                                                const std::optional<std::string>& schema) override;
    std::vector<std::string>    listFunctions(const std::optional<std::string>& schema) override;
    std::string getFunctionSource(const std::string& name,
                                  const std::optional<std::string>& schema) override;

    // DML
    QueryResult insertRow(const std::string& table,
                          const std::optional<std::string>& schema,
                          const std::unordered_map<std::string, RowValue>& values) override;
    QueryResult updateRow(const std::string& table,
                          const std::optional<std::string>& schema,
                          const std::unordered_map<std::string, RowValue>& set,
                          const std::unordered_map<std::string, RowValue>& where) override;
    QueryResult deleteRow(const std::string& table,
                          const std::optional<std::string>& schema,
                          const std::unordered_map<std::string, RowValue>& where) override;

    // Transactions are no-ops for ClickHouse (no traditional ACID transactions).
    void beginTransaction() override {}
    void commitTransaction() override {}
    void rollbackTransaction() override {}

    // Pagination
    QueryResult fetchRows(const std::string& table,
                          const std::optional<std::string>& schema,
                          const std::optional<std::vector<std::string>>& columns,
                          const std::optional<FilterExpression>& where,
                          const std::optional<std::vector<QuerySortDescriptor>>& orderBy,
                          int limit, int offset) override;

    // Server info
    std::string serverVersion() override;
    std::optional<std::string> currentDatabase() override;

private:
    struct HttpResponse {
        int status = 0;
        std::string body;
        std::string summaryJson;  // X-ClickHouse-Summary header
    };

    HttpResponse postSql(const std::string& sql, bool readOnly = false,
                         const std::optional<std::string>& databaseOverride = std::nullopt);
    QueryResult parseJsonCompact(const std::string& body, double elapsedMs);
    void ensureMutable(const std::string& db, const std::string& table, const std::string& op);
    std::string resolveDb(const std::optional<std::string>& schema) const;
    std::string qualifiedName(const std::string& db, const std::string& table) const;
    std::string inlineValue(const RowValue& v) const;
    std::string escapeLiteral(const std::string& s) const;
    bool serverSupportsIsInPrimaryKey() const;
    void rebuildHttpDefaults();

    std::unique_ptr<QNetworkAccessManager> nam_;
    mutable std::mutex stateMu_;
    bool connected_ = false;
    std::optional<ConnectionConfig> config_;
    std::optional<std::string> password_;
    std::optional<std::string> currentDb_;
    std::string serverVersionCache_;

    // Connection-derived (cached so we don't rebuild URL components every call).
    std::string scheme_;
    std::string host_;
    int port_ = 8123;
    std::string username_;
};

}  // namespace gridex
