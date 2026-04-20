#pragma once
//
// DescribeTableTool.h
// Gridex
//
// Detailed table structure: columns, PK, FKs, indexes. Mirrors
// macos DescribeTableTool.swift.

#include "../MCPTool.h"

namespace DBModels
{
    class DescribeTableTool : public MCPTool
    {
    public:
        std::string name() const override { return "describe_table"; }
        std::string description() const override
        {
            return "Get detailed structure of a table including columns, data "
                   "types, indexes, primary keys, foreign keys, and constraints.";
        }
        MCPPermissionTier tier() const override { return MCPPermissionTier::Schema; }
        nlohmann::json inputSchema() const override
        {
            return {
                {"type", "object"},
                {"properties", {
                    {"connection_id", {{"type", "string"}, {"description", "Connection identifier"}}},
                    {"table_name",    {{"type", "string"}, {"description", "Name of the table to describe"}}},
                    {"schema",        {{"type", "string"}, {"description", "Optional schema name"}}}
                }},
                {"required", {"connection_id", "table_name"}}
            };
        }
        MCPToolResult execute(const nlohmann::json& params,
                              MCPToolContext& ctx) override;
    };
}
