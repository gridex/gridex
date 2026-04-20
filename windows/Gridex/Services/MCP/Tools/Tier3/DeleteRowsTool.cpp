//
// DeleteRowsTool.cpp
//

#include "DeleteRowsTool.h"
#include "../MCPToolHelpers.h"
#include "../../Security/MCPIdentifierValidator.h"
#include "../../Security/MCPRowCountEstimator.h"

namespace DBModels
{
    MCPToolResult DeleteRowsTool::execute(const nlohmann::json& params, MCPToolContext& ctx)
    {
        const auto connId = MCPTool::extractConnectionId(params);
        auto ts = MCPIdentifierValidator::extractTableAndSchema(params);

        if (!params.contains("where") || !params["where"].is_string())
            throw MCPToolError::invalidParameters(
                "where is required. Bare DELETE without WHERE is not allowed.");
        const auto whereUtf8 = params["where"].get<std::string>();

        {
            const auto v = ctx.permissionEngine.validateWhereClause(whereUtf8);
            if (const auto* msg = v.errorMessage())
                throw MCPToolError::permissionDenied(*msg);
        }

        const auto perm = ctx.checkPermission(tier(), connId);
        if (const auto* msg = perm.errorMessage())
            throw MCPToolError::permissionDenied(*msg);

        auto [adapter, config] = ctx.getAdapter(connId);
        const std::wstring tableW = MCPToolHelpers::fromUtf8(ts.table);
        const std::wstring schemaW = ts.schema.has_value()
            ? MCPToolHelpers::fromUtf8(*ts.schema) : std::wstring{};
        const std::wstring whereW = MCPToolHelpers::fromUtf8(whereUtf8);

        std::wstring qTable;
        try
        {
            qTable = (schemaW.empty())
                ? adapter->quoteSqlIdentifier(tableW)
                : adapter->quoteSqlIdentifier(schemaW) + L"." + adapter->quoteSqlIdentifier(tableW);
        }
        catch (...) { qTable = tableW; }

        const std::wstring sql = L"DELETE FROM " + qTable + L" WHERE " + whereW;

        if (perm.requiresUserApproval())
        {
            const int est = MCPRowCountEstimator::estimate(
                adapter, qTable, whereW, config);
            const std::wstring details =
                L"SQL:\n" + sql +
                L"\n\nEstimated rows to delete: " + std::to_wstring(est) +
                L"\n\n⚠ This operation cannot be undone!";
            const auto clientW = MCPToolHelpers::fromUtf8(
                ctx.client.name.empty() ? std::string{"AI client"} : ctx.client.name);
            const bool ok = ctx.requestApproval(
                MCPToolHelpers::fromUtf8(this->name()),
                L"Delete rows from '" + tableW + L"' where " + whereW,
                details, connId, clientW);
            if (!ok) throw MCPToolError::permissionDenied("User denied the operation.");
        }

        QueryResult r;
        try { r = adapter->execute(sql); }
        catch (const std::exception& e)
        { throw MCPToolError::queryFailed(e.what()); }
        if (!r.success) throw MCPToolError::queryFailed(MCPToolHelpers::toUtf8(r.error));

        return MCPToolResult::text(
            "Deleted " + std::to_string(r.totalRows) + " row(s) from '" + ts.table + "'.");
    }
}
