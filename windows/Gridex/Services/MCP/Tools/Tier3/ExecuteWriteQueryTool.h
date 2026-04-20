#pragma once
//
// ExecuteWriteQueryTool.h — Run one arbitrary write SQL statement
// (INSERT / UPDATE / DELETE). Requires approval. Multi-statement
// input is rejected; SELECT / WITH are redirected at the query
// tool. Mirrors macOS ExecuteWriteQueryTool.swift.

#include "../MCPTool.h"

namespace DBModels
{
    class ExecuteWriteQueryTool : public MCPTool
    {
    public:
        std::string name() const override { return "execute_write_query"; }
        std::string description() const override
        {
            return "Execute one write SQL statement (INSERT/UPDATE/DELETE). "
                   "Requires user approval. Multi-statement input is rejected; "
                   "use the 'query' tool for SELECT / WITH.";
        }
        MCPPermissionTier tier() const override { return MCPPermissionTier::Write; }
        nlohmann::json inputSchema() const override
        {
            return {
                {"type", "object"},
                {"properties", {
                    {"connection_id", {{"type", "string"}, {"description", "Connection identifier"}}},
                    {"sql",           {{"type", "string"}, {"description", "A single write SQL statement"}}}
                }},
                {"required", {"connection_id", "sql"}}
            };
        }
        MCPToolResult execute(const nlohmann::json& params,
                              MCPToolContext& ctx) override;
    };
}
