#pragma once
//
// MCPToolRegistry.h
// Gridex
//
// Name → MCPTool mapping, populated at MCPServer startup with
// all 13 tools (6 schema / 3 read / 4 write). Mirrors
// macos/Services/MCP/Tools/MCPToolRegistry.swift.

#include <string>
#include <memory>
#include <unordered_map>
#include <vector>
#include <mutex>
#include "MCPTool.h"

namespace DBModels
{
    class MCPToolRegistry
    {
    public:
        // Registers built-in tools (Phase 5 populates).
        MCPToolRegistry();

        void registerTool(std::shared_ptr<MCPTool> tool);
        void unregisterTool(const std::string& name);

        std::shared_ptr<MCPTool> get(const std::string& name) const;

        // All tool definitions for tools/list response.
        std::vector<MCPToolDefinition> definitions() const;

    private:
        mutable std::mutex mtx_;
        std::unordered_map<std::string, std::shared_ptr<MCPTool>> tools_;
    };
}
