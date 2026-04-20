//
// GetSampleRowsTool.cpp
//

#include "GetSampleRowsTool.h"
#include "../MCPToolHelpers.h"
#include <algorithm>

namespace DBModels
{
    MCPToolResult GetSampleRowsTool::execute(const nlohmann::json& params, MCPToolContext& ctx)
    {
        const auto connId = MCPTool::extractConnectionId(params);

        if (!params.contains("table_name") || !params["table_name"].is_string())
            throw MCPToolError::invalidParameters("table_name is required");
        const auto tableUtf8 = params["table_name"].get<std::string>();
        const std::wstring tableW = MCPToolHelpers::fromUtf8(tableUtf8);

        const std::wstring schemaW = (params.contains("schema") && params["schema"].is_string())
            ? MCPToolHelpers::fromUtf8(params["schema"].get<std::string>()) : std::wstring{};

        int limit = 10;
        if (params.contains("limit") && params["limit"].is_number_integer())
            limit = (std::min)(100, (std::max)(1, params["limit"].get<int>()));

        const auto perm = ctx.checkPermission(tier(), connId);
        if (const auto* msg = perm.errorMessage())
            throw MCPToolError::permissionDenied(*msg);

        auto [adapter, config] = ctx.getAdapter(connId);

        // fetchRows signature: (table, schema, limit, offset, orderBy, ascending).
        QueryResult r = adapter->fetchRows(tableW, schemaW, limit, 0, L"", true);
        if (!r.success)
            throw MCPToolError::queryFailed(MCPToolHelpers::toUtf8(r.error));

        if (r.rows.empty())
            return MCPToolResult::text("Table '" + tableUtf8 + "' is empty.");

        // Columns header summary.
        std::string cols = "Columns: ";
        for (size_t i = 0; i < r.columnNames.size(); ++i)
        {
            if (i) cols += ", ";
            cols += MCPToolHelpers::toUtf8(r.columnNames[i]);
            if (i < r.columnTypes.size())
                cols += " (" + MCPToolHelpers::toUtf8(r.columnTypes[i]) + ")";
        }

        nlohmann::json rows = nlohmann::json::array();
        for (const auto& row : r.rows)
        {
            nlohmann::json o = nlohmann::json::object();
            for (const auto& cname : r.columnNames)
            {
                auto it = row.find(cname);
                const std::wstring v = (it != row.end()) ? it->second : L"";
                o[MCPToolHelpers::toUtf8(cname)] =
                    isNullCell(v) ? nlohmann::json(nullptr)
                                  : nlohmann::json(MCPToolHelpers::toUtf8(v));
            }
            rows.push_back(std::move(o));
        }

        return MCPToolResult::text(
            "Sample " + std::to_string(r.rows.size()) + " row(s) from '" +
            tableUtf8 + "':\n" + cols + "\n\n" + rows.dump(2));
    }
}
