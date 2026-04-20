#pragma once
//
// ListTablesTool.h
// Gridex
//
// List tables in a connection's specified schema (or all schemas if
// omitted). Mirrors macos ListTablesTool.swift.

#include "../MCPTool.h"

namespace DBModels
{
    class ListTablesTool : public MCPTool
    {
    public:
        std::string name() const override { return "list_tables"; }
        std::string description() const override
        {
            return "List all tables in a database connection. Returns table "
                   "names, schemas, and approximate row counts.";
        }
        MCPPermissionTier tier() const override { return MCPPermissionTier::Schema; }
        nlohmann::json inputSchema() const override
        {
            return {
                {"type", "object"},
                {"properties", {
                    {"connection_id", {{"type", "string"}, {"description", "Connection identifier"}}},
                    {"schema",        {{"type", "string"}, {"description", "Optional schema/database filter"}}}
                }},
                {"required", {"connection_id"}}
            };
        }
        MCPToolResult execute(const nlohmann::json& params,
                              MCPToolContext& ctx) override;
    };
}
