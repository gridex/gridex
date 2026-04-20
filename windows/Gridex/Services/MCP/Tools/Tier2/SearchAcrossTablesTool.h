#pragma once
//
// SearchAcrossTablesTool.h — Keyword search over table names,
// column names, and column comments in a connection. Mirrors
// macOS SearchAcrossTablesTool.swift. Useful for the AI to
// discover relevant data.

#include "../MCPTool.h"

namespace DBModels
{
    class SearchAcrossTablesTool : public MCPTool
    {
    public:
        std::string name() const override { return "search_across_tables"; }
        std::string description() const override
        {
            return "Search a keyword across table names, column names, "
                   "and column/table comments. Useful when the AI is "
                   "exploring an unfamiliar schema.";
        }
        MCPPermissionTier tier() const override { return MCPPermissionTier::Read; }
        nlohmann::json inputSchema() const override
        {
            return {
                {"type", "object"},
                {"properties", {
                    {"connection_id", {{"type", "string"}, {"description", "Connection identifier"}}},
                    {"keyword",       {{"type", "string"}, {"description", "Keyword to search for"}}},
                    {"schema",        {{"type", "string"}, {"description", "Optional schema filter"}}}
                }},
                {"required", {"connection_id", "keyword"}}
            };
        }
        MCPToolResult execute(const nlohmann::json& params,
                              MCPToolContext& ctx) override;
    };
}
