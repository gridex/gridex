#pragma once
//
// QueryTool.h
// Gridex
//
// Execute a SQL query. In read-only mode, only SELECT is allowed —
// enforced via MCPPermissionEngine::validateReadOnlyQuery. Mirrors
// macos Tier2/QueryTool.swift.

#include "../MCPTool.h"

namespace DBModels
{
    class QueryTool : public MCPTool
    {
    public:
        std::string name() const override { return "query"; }
        std::string description() const override
        {
            return "Execute a SQL query. In read-only mode, only SELECT statements "
                   "are allowed. Returns rows with metadata (column types, row "
                   "count, execution time).";
        }
        MCPPermissionTier tier() const override { return MCPPermissionTier::Read; }
        nlohmann::json inputSchema() const override
        {
            return {
                {"type", "object"},
                {"properties", {
                    {"connection_id", {{"type", "string"}, {"description", "Connection identifier"}}},
                    {"sql",           {{"type", "string"}, {"description", "SQL query to execute"}}},
                    {"row_limit",     {
                        {"type", "integer"},
                        {"description", "Maximum rows to return (default 1000, max 10000)"},
                        {"default", 1000}, {"maximum", 10000}
                    }}
                }},
                {"required", {"connection_id", "sql"}}
            };
        }
        MCPToolResult execute(const nlohmann::json& params,
                              MCPToolContext& ctx) override;
    };
}
