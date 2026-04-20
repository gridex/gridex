#pragma once
//
// InsertRowsTool.h — Insert one or more rows into a table.
// Requires approval (Tier 3). Mirrors macOS InsertRowsTool.swift.

#include "../MCPTool.h"

namespace DBModels
{
    class InsertRowsTool : public MCPTool
    {
    public:
        std::string name() const override { return "insert_rows"; }
        std::string description() const override
        {
            return "Insert one or more rows into a table. Requires user "
                   "approval (Tier 3). Returns the number of rows inserted.";
        }
        MCPPermissionTier tier() const override { return MCPPermissionTier::Write; }
        nlohmann::json inputSchema() const override
        {
            return {
                {"type", "object"},
                {"properties", {
                    {"connection_id", {{"type", "string"}, {"description", "Connection identifier"}}},
                    {"table_name",    {{"type", "string"}, {"description", "Table to insert into"}}},
                    {"schema",        {{"type", "string"}, {"description", "Optional schema"}}},
                    {"rows",          {{"type", "array"},
                                       {"description", "Array of row objects (column -> value)"},
                                       {"items", {{"type", "object"}}}}}
                }},
                {"required", {"connection_id", "table_name", "rows"}}
            };
        }
        MCPToolResult execute(const nlohmann::json& params,
                              MCPToolContext& ctx) override;
    };
}
