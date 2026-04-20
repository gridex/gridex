//
// ListTablesTool.cpp
//

#include "ListTablesTool.h"
#include "../MCPToolHelpers.h"

namespace DBModels
{
    MCPToolResult ListTablesTool::execute(const nlohmann::json& params, MCPToolContext& ctx)
    {
        const auto connId = MCPTool::extractConnectionId(params);

        const auto perm = ctx.checkPermission(tier(), connId);
        if (const auto* msg = perm.errorMessage())
            throw MCPToolError::permissionDenied(*msg);

        std::wstring schema;
        if (params.contains("schema") && params["schema"].is_string())
            schema = MCPToolHelpers::fromUtf8(params["schema"].get<std::string>());

        auto [adapter, config] = ctx.getAdapter(connId);
        auto tables = adapter->listTables(schema);

        nlohmann::json arr = nlohmann::json::array();
        for (const auto& t : tables)
        {
            nlohmann::json e = {
                {"name", MCPToolHelpers::toUtf8(t.name)},
                {"type", MCPToolHelpers::toUtf8(t.type.empty() ? L"table" : t.type)}
            };
            if (!t.schema.empty())
                e["schema"] = MCPToolHelpers::toUtf8(t.schema);
            if (t.estimatedRows > 0)
                e["estimated_rows"] = t.estimatedRows;
            arr.push_back(std::move(e));
        }

        if (arr.empty())
        {
            std::string suffix = schema.empty()
                ? ""
                : " in schema '" + MCPToolHelpers::toUtf8(schema) + "'";
            return MCPToolResult::text("No tables found" + suffix + ".");
        }
        return MCPToolResult::text(
            "Found " + std::to_string(arr.size()) + " table(s):\n" + arr.dump(2));
    }
}
