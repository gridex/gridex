// DatabaseType+Explain.swift
// Gridex
//
// Per-engine EXPLAIN syntax. PostgreSQL/MySQL/SQLite/MSSQL/ClickHouse all
// disagree on the keyword and the option-list shape; MongoDB and Redis don't
// have a SQL EXPLAIN at all. Centralising here so the query editor button,
// the MCP `explain_query` tool, and any future caller don't drift apart.

import Foundation

extension DatabaseType {
    /// Build the database-specific EXPLAIN SQL for `sql`. Returns nil when the
    /// engine has no SQL EXPLAIN (Mongo, Redis) or when `sql` is blank.
    func explainSQL(for sql: String) -> String? {
        let trimmed = sql.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return nil }

        switch self {
        case .postgresql:
            // ANALYZE=false → planner output without executing the query.
            return "EXPLAIN (ANALYZE false, COSTS true, FORMAT TEXT) \(trimmed)"
        case .mysql:
            return "EXPLAIN \(trimmed)"
        case .sqlite:
            return "EXPLAIN QUERY PLAN \(trimmed)"
        case .mssql:
            // SET-toggled showplan; statements between the toggles return plan rows.
            return "SET SHOWPLAN_TEXT ON; \(trimmed); SET SHOWPLAN_TEXT OFF"
        case .clickhouse:
            return "EXPLAIN \(trimmed)"
        case .mongodb, .redis:
            return nil
        }
    }
}
