#pragma once
//
// ListRelationshipsTool.h — FK graph for a table: outgoing (this
// table references others) + incoming (others reference this).
// Mirrors macOS ListRelationshipsTool.swift.
//
// Incoming is O(N_tables * M_FKs) — we scan every table's FK set.
// Fine for <200 tables; above that, add a schema filter param.

#include "../MCPTool.h"

namespace DBModels
{
    class ListRelationshipsTool : public MCPTool
    {
    public:
        std::string name() const override { return "list_relationships"; }
        std::string description() const override
        {
            return "List foreign-key relationships for a table. Returns both "
                   "outgoing (this -> other) and incoming (other -> this) "
                   "references.";
        }
        MCPPermissionTier tier() const override { return MCPPermissionTier::Schema; }
        nlohmann::json inputSchema() const override
        {
            return {
                {"type", "object"},
                {"properties", {
                    {"connection_id", {{"type", "string"}, {"description", "Connection identifier"}}},
                    {"table_name",    {{"type", "string"}, {"description", "Name of the table"}}},
                    {"schema",        {{"type", "string"}, {"description", "Optional schema"}}}
                }},
                {"required", {"connection_id", "table_name"}}
            };
        }
        MCPToolResult execute(const nlohmann::json& params,
                              MCPToolContext& ctx) override;
    };
}
