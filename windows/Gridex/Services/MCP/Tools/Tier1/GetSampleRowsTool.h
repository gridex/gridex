#pragma once
//
// GetSampleRowsTool.h — Return up to N rows from a table so the
// AI can understand the data shape. Mirrors macOS
// GetSampleRowsTool.swift. Defaults to 10, hard-caps at 100.

#include "../MCPTool.h"

namespace DBModels
{
    class GetSampleRowsTool : public MCPTool
    {
    public:
        std::string name() const override { return "get_sample_rows"; }
        std::string description() const override
        {
            return "Get sample rows from a table so the AI can see the "
                   "data shape. Default limit 10, max 100.";
        }
        MCPPermissionTier tier() const override { return MCPPermissionTier::Schema; }
        nlohmann::json inputSchema() const override
        {
            return {
                {"type", "object"},
                {"properties", {
                    {"connection_id", {{"type", "string"}, {"description", "Connection identifier"}}},
                    {"table_name",    {{"type", "string"}, {"description", "Name of the table"}}},
                    {"schema",        {{"type", "string"}, {"description", "Optional schema"}}},
                    {"limit",         {{"type", "integer"}, {"default", 10},
                                       {"minimum", 1}, {"maximum", 100}}}
                }},
                {"required", {"connection_id", "table_name"}}
            };
        }
        MCPToolResult execute(const nlohmann::json& params,
                              MCPToolContext& ctx) override;
    };
}
