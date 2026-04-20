//
// ListRelationshipsTool.cpp
//

#include "ListRelationshipsTool.h"
#include "../MCPToolHelpers.h"

namespace DBModels
{
    MCPToolResult ListRelationshipsTool::execute(const nlohmann::json& params, MCPToolContext& ctx)
    {
        const auto connId = MCPTool::extractConnectionId(params);

        if (!params.contains("table_name") || !params["table_name"].is_string())
            throw MCPToolError::invalidParameters("table_name is required");
        const auto tableUtf8 = params["table_name"].get<std::string>();
        const std::wstring tableW = MCPToolHelpers::fromUtf8(tableUtf8);

        const std::wstring schemaW = (params.contains("schema") && params["schema"].is_string())
            ? MCPToolHelpers::fromUtf8(params["schema"].get<std::string>()) : std::wstring{};

        const auto perm = ctx.checkPermission(tier(), connId);
        if (const auto* msg = perm.errorMessage())
            throw MCPToolError::permissionDenied(*msg);

        auto [adapter, config] = ctx.getAdapter(connId);

        nlohmann::json result;
        result["table"] = tableUtf8;

        // Outgoing: this table's FKs pointing at others.
        nlohmann::json out = nlohmann::json::array();
        for (const auto& fk : adapter->listForeignKeys(tableW, schemaW))
        {
            out.push_back({
                {"name",              MCPToolHelpers::toUtf8(fk.name)},
                {"columns",           MCPToolHelpers::toUtf8(fk.column)},
                {"references_table",  MCPToolHelpers::toUtf8(fk.referencedTable)},
                {"references_columns",MCPToolHelpers::toUtf8(fk.referencedColumn)}
            });
        }
        result["outgoing"] = out.empty() ? nlohmann::json("None") : out;

        // Incoming: walk all tables and grab FKs whose target is this
        // table. Linear in total-tables * avg-FKs-per-table; acceptable
        // for typical schemas, and the MCP call is one-shot.
        nlohmann::json in = nlohmann::json::array();
        for (const auto& t : adapter->listTables(schemaW))
        {
            if (t.name == tableW) continue;
            for (const auto& fk : adapter->listForeignKeys(t.name, schemaW))
            {
                if (fk.referencedTable == tableW)
                {
                    in.push_back({
                        {"from_table",   MCPToolHelpers::toUtf8(t.name)},
                        {"from_columns", MCPToolHelpers::toUtf8(fk.column)},
                        {"to_columns",   MCPToolHelpers::toUtf8(fk.referencedColumn)}
                    });
                }
            }
        }
        result["incoming"] = in.empty() ? nlohmann::json("None") : in;

        return MCPToolResult::text(result.dump(2));
    }
}
