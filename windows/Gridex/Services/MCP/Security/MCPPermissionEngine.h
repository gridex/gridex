#pragma once
//
// MCPPermissionEngine.h
// Gridex
//
// Thread-safe per-connection permission gate. Mirrors
// macos/Services/MCP/Security/MCPPermissionEngine.swift.
//
// Two primary responsibilities:
//   1. Map connectionId → MCPConnectionMode, combined with tier to
//      emit Allowed | RequiresApproval | Denied.
//   2. Syntactic validators for SELECT-only enforcement
//      (validateReadOnlyQuery) and UPDATE/DELETE WHERE clauses
//      (validateWhereClause).

#include <string>
#include <unordered_map>
#include <mutex>
#include "../../../Models/MCP/MCPConnectionMode.h"
#include "../../../Models/MCP/MCPPermissionTier.h"
#include "MCPPermissionResult.h"

namespace DBModels
{
    class MCPPermissionEngine
    {
    public:
        // Mode bookkeeping. `connectionId` is the same wstring id
        // used by ConnectionConfig / ConnectionStore.
        void setMode(const std::wstring& connectionId, MCPConnectionMode mode);
        MCPConnectionMode getMode(const std::wstring& connectionId) const;
        void removeMode(const std::wstring& connectionId);

        // Core gate. Takes a snapshot of the mode and runs the
        // Swift decision table.
        MCPPermissionResult checkPermission(MCPPermissionTier tier,
                                            const std::wstring& connectionId) const;
        MCPPermissionResult checkPermission(MCPPermissionTier tier,
                                            MCPConnectionMode mode) const;

        // SELECT-only enforcement for Tier 2 queries in read-only
        // mode. Runs MCPSQLSanitizer first, then:
        //   - rejects multi-statement (`;` anywhere except trailing)
        //   - requires prefix ∈ {SELECT, SHOW, EXPLAIN, DESCRIBE, DESC, WITH}
        //   - blocks 30+ dangerous keywords via regex
        MCPPermissionResult validateReadOnlyQuery(const std::string& sql) const;

        // Mandatory WHERE validator for Tier 3 write tools.
        // Rejects bare clauses, `;`, SQL comments, and trivial tautologies.
        MCPPermissionResult validateWhereClause(const std::string& whereClause) const;

    private:
        mutable std::mutex mtx_;
        std::unordered_map<std::wstring, MCPConnectionMode> modes_;
    };
}
