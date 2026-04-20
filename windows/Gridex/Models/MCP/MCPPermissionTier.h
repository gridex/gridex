#pragma once
//
// MCPPermissionTier.h
// Gridex
//
// Permission tiers for MCP tools. Mirrors
// macos/Core/Enums/MCPPermissionTier.swift.
//
// Tier numbering is part of the client-facing audit contract — do
// NOT renumber. New tiers append at the end.

#include <string>

namespace DBModels
{
    enum class MCPPermissionTier
    {
        Schema   = 1, // Schema introspection (read-only, no approval)
        Read     = 2, // Query execution (read, no approval by default)
        Write    = 3, // Data modification (REQUIRES approval)
        DDL      = 4, // Schema change (CRITICAL approval)
        Advanced = 5  // Reserved for future (EXPLAIN, etc.)
    };

    inline std::wstring mcpTierDisplayName(MCPPermissionTier t)
    {
        switch (t)
        {
            case MCPPermissionTier::Schema:   return L"Schema";
            case MCPPermissionTier::Read:     return L"Read";
            case MCPPermissionTier::Write:    return L"Write";
            case MCPPermissionTier::DDL:      return L"DDL";
            case MCPPermissionTier::Advanced: return L"Advanced";
        }
        return L"";
    }

    inline bool mcpTierRequiresApproval(MCPPermissionTier t)
    {
        return t == MCPPermissionTier::Write
            || t == MCPPermissionTier::DDL
            || t == MCPPermissionTier::Advanced;
    }

    inline bool mcpTierIsReadOnly(MCPPermissionTier t)
    {
        return t == MCPPermissionTier::Schema || t == MCPPermissionTier::Read;
    }
}
