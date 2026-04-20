//
// ListConnectionsTool.cpp
//

#include "ListConnectionsTool.h"
#include "../MCPToolHelpers.h"
#include "../../../../Models/ConnectionStore.h"

namespace DBModels
{
    MCPToolResult ListConnectionsTool::execute(const nlohmann::json&, MCPToolContext& ctx)
    {
        const auto configs = ConnectionStore::Load();
        nlohmann::json arr = nlohmann::json::array();

        for (const auto& c : configs)
        {
            // Hide Locked rows entirely — mac parity.
            const auto mode = ctx.permissionEngine.getMode(c.id);
            if (mode == MCPConnectionMode::Locked) continue;

            nlohmann::json entry = {
                {"id",        MCPToolHelpers::toUtf8(c.id)},
                {"name",      MCPToolHelpers::toUtf8(c.name)},
                {"type",      MCPToolHelpers::toUtf8(DatabaseTypeDisplayName(c.databaseType))},
                {"host",      MCPToolHelpers::toUtf8(c.host)},
                {"database",  MCPToolHelpers::toUtf8(c.database)},
                {"mcp_mode",  mcpRawString(mode)}
            };
            arr.push_back(std::move(entry));
        }

        if (arr.empty())
            return MCPToolResult::text(
                "No connections available for MCP access. All connections are "
                "either locked or none exist.");

        return MCPToolResult::text(arr.dump(2));
    }
}
