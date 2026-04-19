// MCPRowCountEstimator.swift
// Gridex
//
// Pre-approval row count estimation for write tools.
//
// The estimate runs against the same (already validated & quoted) SQL shape
// that the write tool will execute, so it never reconstructs SQL from
// user-supplied substrings — avoiding the "parse to rebuild COUNT(*)"
// injection pattern.

import Foundation

enum MCPRowCountEstimator {
    static func estimate(
        adapter: any DatabaseAdapter,
        qualifiedTable: String,
        whereClause: String,
        config: ConnectionConfig
    ) async -> Int {
        guard config.databaseType.isSQL else { return 0 }
        let sql = "SELECT COUNT(*) FROM \(qualifiedTable) WHERE \(whereClause)"
        guard let result = try? await adapter.executeRaw(sql: sql),
              let count = result.rows.first?.first?.intValue else {
            return 0
        }
        return count
    }
}
