//
// ExplainQueryTool.cpp
//

#include "ExplainQueryTool.h"
#include "../MCPToolHelpers.h"

namespace DBModels
{
    MCPToolResult ExplainQueryTool::execute(const nlohmann::json& params, MCPToolContext& ctx)
    {
        const auto connId = MCPTool::extractConnectionId(params);

        if (!params.contains("sql") || !params["sql"].is_string())
            throw MCPToolError::invalidParameters("sql is required");
        const auto sqlUtf8 = params["sql"].get<std::string>();

        const auto perm = ctx.checkPermission(tier(), connId);
        if (const auto* msg = perm.errorMessage())
            throw MCPToolError::permissionDenied(*msg);

        auto [adapter, config] = ctx.getAdapter(connId);
        const std::wstring sqlW = MCPToolHelpers::fromUtf8(sqlUtf8);

        std::wstring explainSQL;
        switch (config.databaseType)
        {
            case DatabaseType::PostgreSQL:
                explainSQL = L"EXPLAIN (ANALYZE false, COSTS true, FORMAT TEXT) " + sqlW;
                break;
            case DatabaseType::MySQL:
                explainSQL = L"EXPLAIN " + sqlW;
                break;
            case DatabaseType::SQLite:
                explainSQL = L"EXPLAIN QUERY PLAN " + sqlW;
                break;
            case DatabaseType::MSSQLServer:
                // SET SHOWPLAN_TEXT is a session flag — bracket the
                // query with it. Multi-statement; the adapter passes
                // this through to the driver unchanged.
                explainSQL = L"SET SHOWPLAN_TEXT ON; " + sqlW + L"; SET SHOWPLAN_TEXT OFF";
                break;
            default:
                return MCPToolResult::error(
                    "EXPLAIN is not supported for " +
                    MCPToolHelpers::toUtf8(DatabaseTypeDisplayName(config.databaseType)) +
                    " connections.");
        }

        QueryResult r;
        try { r = adapter->execute(explainSQL); }
        catch (const std::exception& e)
        { throw MCPToolError::queryFailed(e.what()); }
        if (!r.success) throw MCPToolError::queryFailed(MCPToolHelpers::toUtf8(r.error));

        std::string out = "Query Plan for: " + sqlUtf8 + "\n\n";
        if (r.rows.empty())
        {
            out += "No plan information available.";
        }
        else
        {
            for (const auto& row : r.rows)
            {
                bool first = true;
                for (const auto& cname : r.columnNames)
                {
                    auto it = row.find(cname);
                    if (it == row.end()) continue;
                    if (!first) out += " | ";
                    first = false;
                    out += MCPToolHelpers::toUtf8(it->second);
                }
                out += "\n";
            }
        }
        return MCPToolResult::text(out);
    }
}
