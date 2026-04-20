//
// ListSchemasTool.cpp
//

#include "ListSchemasTool.h"
#include "../MCPToolHelpers.h"

namespace DBModels
{
    static bool supportsSchemas(DatabaseType t)
    {
        return t == DatabaseType::PostgreSQL || t == DatabaseType::MSSQLServer;
    }

    MCPToolResult ListSchemasTool::execute(const nlohmann::json& params, MCPToolContext& ctx)
    {
        const auto connId = MCPTool::extractConnectionId(params);

        const auto perm = ctx.checkPermission(tier(), connId);
        if (const auto* msg = perm.errorMessage())
            throw MCPToolError::permissionDenied(*msg);

        auto [adapter, config] = ctx.getAdapter(connId);

        nlohmann::json arr = nlohmann::json::array();
        std::wstring kind = L"Schemas";
        if (supportsSchemas(config.databaseType))
        {
            for (const auto& s : adapter->listSchemas())
                arr.push_back(MCPToolHelpers::toUtf8(s));
        }
        else
        {
            kind = L"Databases";
            for (const auto& d : adapter->listDatabases())
                arr.push_back(MCPToolHelpers::toUtf8(d));
        }

        if (arr.empty())
            return MCPToolResult::text("No " + MCPToolHelpers::toUtf8(kind) + " found.");

        return MCPToolResult::text(
            MCPToolHelpers::toUtf8(kind) + ": " + arr.dump());
    }
}
