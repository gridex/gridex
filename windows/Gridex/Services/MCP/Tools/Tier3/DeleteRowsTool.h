#pragma once
//
// DeleteRowsTool.h — DELETE rows matching a WHERE clause.
// Requires approval (Tier 3). WHERE is mandatory — bare DELETE
// is always rejected. Mirrors macOS DeleteRowsTool.swift.

#include "../MCPTool.h"

namespace DBModels
{
    class DeleteRowsTool : public MCPTool
    {
    public:
        std::string name() const override { return "delete_rows"; }
        std::string description() const override
        {
            return "Delete rows matching a WHERE clause. Requires user "
                   "approval. WHERE is MANDATORY.";
        }
        MCPPermissionTier tier() const override { return MCPPermissionTier::Write; }
        nlohmann::json inputSchema() const override
        {
            return {
                {"type", "object"},
                {"properties", {
                    {"connection_id", {{"type", "string"}, {"description", "Connection identifier"}}},
                    {"table_name",    {{"type", "string"}, {"description", "Table to delete from"}}},
                    {"schema",        {{"type", "string"}, {"description", "Optional schema"}}},
                    {"where",         {{"type", "string"},
                                       {"description", "WHERE clause (required). Must not contain ';', '--', '/*', '*/'."}}}
                }},
                {"required", {"connection_id", "table_name", "where"}}
            };
        }
        MCPToolResult execute(const nlohmann::json& params,
                              MCPToolContext& ctx) override;
    };
}
