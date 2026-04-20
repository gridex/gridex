#pragma once
//
// UpdateRowsTool.h — UPDATE rows matching a WHERE clause.
// Requires approval (Tier 3). WHERE is mandatory — bare UPDATE
// is always rejected. Mirrors macOS UpdateRowsTool.swift.

#include "../MCPTool.h"

namespace DBModels
{
    class UpdateRowsTool : public MCPTool
    {
    public:
        std::string name() const override { return "update_rows"; }
        std::string description() const override
        {
            return "Update rows matching a WHERE clause. Requires user "
                   "approval. WHERE is MANDATORY — bare UPDATE is rejected.";
        }
        MCPPermissionTier tier() const override { return MCPPermissionTier::Write; }
        nlohmann::json inputSchema() const override
        {
            return {
                {"type", "object"},
                {"properties", {
                    {"connection_id", {{"type", "string"}, {"description", "Connection identifier"}}},
                    {"table_name",    {{"type", "string"}, {"description", "Table to update"}}},
                    {"schema",        {{"type", "string"}, {"description", "Optional schema"}}},
                    {"set",           {{"type", "object"},
                                       {"description", "Column -> value pairs to update"}}},
                    {"where",         {{"type", "string"},
                                       {"description", "WHERE clause (required). Must not contain ';', '--', '/*', '*/'."}}}
                }},
                {"required", {"connection_id", "table_name", "set", "where"}}
            };
        }
        MCPToolResult execute(const nlohmann::json& params,
                              MCPToolContext& ctx) override;
    };
}
