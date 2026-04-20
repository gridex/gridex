#pragma once
//
// MCPConnectionMode.h
// Gridex
//
// MCP access mode for database connections. Mirrors
// macos/Core/Enums/MCPConnectionMode.swift 1:1 so the on-disk raw
// strings ("locked" / "read_only" / "read_write") are identical
// between platforms — this keeps audit JSONL grep-friendly and
// makes future cloud-sync trivial.

#include <string>

namespace DBModels
{
    enum class MCPConnectionMode
    {
        Locked = 0,
        ReadOnly = 1,
        ReadWrite = 2
    };

    inline std::wstring mcpDisplayName(MCPConnectionMode mode)
    {
        switch (mode)
        {
            case MCPConnectionMode::Locked:    return L"Locked";
            case MCPConnectionMode::ReadOnly:  return L"Read-only";
            case MCPConnectionMode::ReadWrite: return L"Read-write";
        }
        return L"Unknown";
    }

    inline std::wstring mcpDescription(MCPConnectionMode mode)
    {
        switch (mode)
        {
            case MCPConnectionMode::Locked:
                return L"AI cannot access this connection";
            case MCPConnectionMode::ReadOnly:
                return L"AI can query but not modify (recommended for production)";
            case MCPConnectionMode::ReadWrite:
                return L"AI can modify with your approval (use for dev only)";
        }
        return L"";
    }

    // Cross-platform raw string used in audit JSONL + REST payloads.
    // MUST match macOS Swift rawValue ("locked", "read_only", "read_write").
    inline std::string mcpRawString(MCPConnectionMode mode)
    {
        switch (mode)
        {
            case MCPConnectionMode::Locked:    return "locked";
            case MCPConnectionMode::ReadOnly:  return "read_only";
            case MCPConnectionMode::ReadWrite: return "read_write";
        }
        return "locked";
    }

    inline MCPConnectionMode mcpModeFromRawString(const std::string& raw)
    {
        if (raw == "read_only")  return MCPConnectionMode::ReadOnly;
        if (raw == "read_write") return MCPConnectionMode::ReadWrite;
        return MCPConnectionMode::Locked;
    }

    // Per-tier allowance. Matches Swift `allowsTierN` exactly — do
    // not drift; the permission engine in Phase 2 relies on these.
    inline bool mcpAllowsTier1(MCPConnectionMode m) { return m != MCPConnectionMode::Locked; }
    inline bool mcpAllowsTier2(MCPConnectionMode m) { return m != MCPConnectionMode::Locked; }
    inline bool mcpAllowsTier3(MCPConnectionMode m) { return m == MCPConnectionMode::ReadWrite; }
    inline bool mcpAllowsTier4(MCPConnectionMode m) { return m == MCPConnectionMode::ReadWrite; }
    inline bool mcpAllowsTier5(MCPConnectionMode m) { return m != MCPConnectionMode::Locked; }
}
