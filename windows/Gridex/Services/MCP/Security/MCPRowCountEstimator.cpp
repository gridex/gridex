//
// MCPRowCountEstimator.cpp
//

#include "MCPRowCountEstimator.h"
#include <stdexcept>

namespace DBModels { namespace MCPRowCountEstimator {

static bool isSQLDatabase(DatabaseType t)
{
    // Redis / MongoDB have no SQL COUNT(*) equivalent in this shape.
    return t == DatabaseType::PostgreSQL ||
           t == DatabaseType::MySQL      ||
           t == DatabaseType::SQLite     ||
           t == DatabaseType::MSSQLServer;
}

int estimate(const std::shared_ptr<DatabaseAdapter>& adapter,
             const std::wstring& qualifiedTable,
             const std::wstring& whereClause,
             const ConnectionConfig& config)
{
    if (!adapter || !isSQLDatabase(config.databaseType))
        return 0;

    const std::wstring sql =
        L"SELECT COUNT(*) AS cnt FROM " + qualifiedTable +
        L" WHERE " + whereClause;

    try
    {
        QueryResult r = adapter->execute(sql);
        if (!r.success || r.rows.empty() || r.columnNames.empty())
            return 0;
        const auto& row = r.rows[0];
        // Row is unordered_map<wstring,wstring> keyed by column name.
        const auto it = row.find(r.columnNames[0]);
        if (it == row.end()) return 0;
        const std::wstring& val = it->second;
        try { return std::stoi(val); } catch (...) { return 0; }
    }
    catch (...)
    {
        return 0;
    }
}

}} // namespace
