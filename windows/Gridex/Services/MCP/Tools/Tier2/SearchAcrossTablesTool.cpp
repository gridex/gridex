//
// SearchAcrossTablesTool.cpp
//

#include "SearchAcrossTablesTool.h"
#include "../MCPToolHelpers.h"
#include <algorithm>

namespace DBModels
{
    namespace
    {
        std::wstring toLower(std::wstring s)
        {
            std::transform(s.begin(), s.end(), s.begin(), ::towlower);
            return s;
        }
        bool containsCI(const std::wstring& hay, const std::wstring& needle)
        {
            return toLower(hay).find(toLower(needle)) != std::wstring::npos;
        }
    }

    MCPToolResult SearchAcrossTablesTool::execute(const nlohmann::json& params, MCPToolContext& ctx)
    {
        const auto connId = MCPTool::extractConnectionId(params);

        if (!params.contains("keyword") || !params["keyword"].is_string())
            throw MCPToolError::invalidParameters("keyword is required");
        const auto kwUtf8 = params["keyword"].get<std::string>();
        const std::wstring kwW = MCPToolHelpers::fromUtf8(kwUtf8);

        const std::wstring schemaW = (params.contains("schema") && params["schema"].is_string())
            ? MCPToolHelpers::fromUtf8(params["schema"].get<std::string>()) : std::wstring{};

        const auto perm = ctx.checkPermission(tier(), connId);
        if (const auto* msg = perm.errorMessage())
            throw MCPToolError::permissionDenied(*msg);

        auto [adapter, config] = ctx.getAdapter(connId);

        nlohmann::json matches = nlohmann::json::array();
        for (const auto& t : adapter->listTables(schemaW))
        {
            if (containsCI(t.name, kwW))
                matches.push_back({
                    {"type",  "table"},
                    {"table", MCPToolHelpers::toUtf8(t.name)},
                    {"match", MCPToolHelpers::toUtf8(t.name)}
                });

            if (!t.comment.empty() && containsCI(t.comment, kwW))
                matches.push_back({
                    {"type",    "table_comment"},
                    {"table",   MCPToolHelpers::toUtf8(t.name)},
                    {"comment", MCPToolHelpers::toUtf8(t.comment)}
                });

            // describeTable can be slow per-table, but Tier-2 read
            // sits behind a rate limiter — fine for exploration.
            for (const auto& col : adapter->describeTable(t.name, schemaW))
            {
                if (containsCI(col.name, kwW))
                    matches.push_back({
                        {"type",      "column"},
                        {"table",     MCPToolHelpers::toUtf8(t.name)},
                        {"column",    MCPToolHelpers::toUtf8(col.name)},
                        {"data_type", MCPToolHelpers::toUtf8(col.dataType)}
                    });
                if (!col.comment.empty() && containsCI(col.comment, kwW))
                    matches.push_back({
                        {"type",    "column_comment"},
                        {"table",   MCPToolHelpers::toUtf8(t.name)},
                        {"column",  MCPToolHelpers::toUtf8(col.name)},
                        {"comment", MCPToolHelpers::toUtf8(col.comment)}
                    });
            }
        }

        if (matches.empty())
            return MCPToolResult::text("No matches found for '" + kwUtf8 + "'.");

        return MCPToolResult::text(
            "Found " + std::to_string(matches.size()) +
            " match(es) for '" + kwUtf8 + "':\n" + matches.dump(2));
    }
}
