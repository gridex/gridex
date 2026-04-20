#pragma once
//
// ExplainQueryTool.h — Return the EXPLAIN plan for a SQL query
// without executing it. Mirrors macOS ExplainQueryTool.swift.
// Dialect-specific prefix: EXPLAIN for PG/MySQL, EXPLAIN QUERY PLAN
// for SQLite, SHOWPLAN_TEXT for MSSQL. Redis/Mongo return an error.

#include "../MCPTool.h"

namespace DBModels
{
    class ExplainQueryTool : public MCPTool
    {
    public:
        std::string name() const override { return "explain_query"; }
        std::string description() const override
        {
            return "Get the EXPLAIN plan for a SQL query without executing "
                   "it. Helps the AI reason about performance.";
        }
        MCPPermissionTier tier() const override { return MCPPermissionTier::Read; }
        nlohmann::json inputSchema() const override
        {
            return {
                {"type", "object"},
                {"properties", {
                    {"connection_id", {{"type", "string"}, {"description", "Connection identifier"}}},
                    {"sql",           {{"type", "string"}, {"description", "SQL query to explain"}}}
                }},
                {"required", {"connection_id", "sql"}}
            };
        }
        MCPToolResult execute(const nlohmann::json& params,
                              MCPToolContext& ctx) override;
    };
}
