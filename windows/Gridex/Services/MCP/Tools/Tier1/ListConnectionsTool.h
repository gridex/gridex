#pragma once
//
// ListConnectionsTool.h
// Gridex
//
// Returns every non-Locked connection visible to the AI client.
// Mirrors macos Tier1/ListConnectionsTool.swift.

#include "../MCPTool.h"

namespace DBModels
{
    class ListConnectionsTool : public MCPTool
    {
    public:
        std::string name() const override { return "list_connections"; }
        std::string description() const override
        {
            return "List all configured database connections that MCP can access. "
                   "Locked connections are omitted. Returns id, name, type, host, "
                   "and per-connection mcp_mode.";
        }
        MCPPermissionTier tier() const override { return MCPPermissionTier::Schema; }
        nlohmann::json inputSchema() const override
        {
            return {
                {"type", "object"},
                {"properties", nlohmann::json::object()}
            };
        }
        MCPToolResult execute(const nlohmann::json& params,
                              MCPToolContext& ctx) override;
    };
}
