#pragma once
//
// MCPRowCountEstimator.h
// Gridex
//
// Best-effort "SELECT COUNT(*) ..." estimate used by write tools
// to show an affected-rows number in the approval dialog. Mirrors
// macos/Services/MCP/Security/MCPRowCountEstimator.swift.
//
// Swallows every error and returns 0 — this is a UX aid, not a
// correctness check. Callers MUST still validate the WHERE clause
// via MCPPermissionEngine::validateWhereClause BEFORE calling.

#include <string>
#include <memory>
#include "../../../Models/DatabaseAdapter.h"
#include "../../../Models/ConnectionConfig.h"

namespace DBModels
{
    namespace MCPRowCountEstimator
    {
        // `qualifiedTable` must already be quoted (caller used
        // `adapter->quoteSqlIdentifier`). `whereClause` is untrusted
        // but gated by validateWhereClause upstream.
        int estimate(const std::shared_ptr<DatabaseAdapter>& adapter,
                     const std::wstring& qualifiedTable,
                     const std::wstring& whereClause,
                     const ConnectionConfig& config);
    }
}
