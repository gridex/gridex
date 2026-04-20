#pragma once
//
// ListSchemasTool.h — List schemas (Postgres/MSSQL) or databases
// (MySQL / Redis) for a connection. Mirrors
// macos/Services/MCP/Tools/Tier1/ListSchemasTool.swift.

#include "../MCPTool.h"

namespace DBModels
{
    class ListSchemasTool : public MCPTool
    {
    public:
        std::string name() const override { return "list_schemas"; }
        std::string description() const override
        {
            return "List schemas (PostgreSQL / MSSQL) or databases "
                   "(MySQL / MongoDB) available on a connection.";
        }
        MCPPermissionTier tier() const override { return MCPPermissionTier::Schema; }
        nlohmann::json inputSchema() const override
        {
            return {
                {"type", "object"},
                {"properties", {
                    {"connection_id", {{"type", "string"}, {"description", "Connection identifier"}}}
                }},
                {"required", {"connection_id"}}
            };
        }
        MCPToolResult execute(const nlohmann::json& params,
                              MCPToolContext& ctx) override;
    };
}
