#pragma once
//
// MCPBuiltinTools.h
// Gridex
//
// Single entry point that registers every shipped MCP tool into
// an MCPToolRegistry. Keeps MCPServer.cpp free of per-tool
// includes — when Phase 5 expands beyond 4 tools, just grow the
// cpp here.

#include "MCPToolRegistry.h"

namespace DBModels
{
    namespace MCPBuiltinTools
    {
        void registerAll(MCPToolRegistry& registry);
    }
}
