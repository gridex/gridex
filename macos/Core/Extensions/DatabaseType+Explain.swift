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
    ///
    /// `options` shapes the PG `EXPLAIN (...)` option list. Other engines have
    /// no per-option syntax, so the flags are ignored and we return their
    /// plain `EXPLAIN` form.
    ///
    /// The default value preserves the previous hardcoded behaviour
    /// (`EXPLAIN (ANALYZE false, COSTS true, FORMAT TEXT) ...`), so legacy
    /// call sites stay byte-compatible.
    func explainSQL(for sql: String, options: ExplainOptions = .default) -> String? {
        let trimmed = sql.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return nil }

        switch self {
        case .postgresql:
            return Self.buildPostgresExplain(sql: trimmed, options: options.sanitized())
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

    // MARK: - Postgres option-list builder

    /// Produce `EXPLAIN (FOO true, BAR false, FORMAT JSON, ...) <sql>`.
    /// ANALYZE / COSTS / FORMAT are always emitted (matches the legacy default
    /// + makes the planner output reproducible regardless of server-side
    /// defaults). Boolean toggles only appear when on. SERIALIZE only appears
    /// when not `.off`.
    private static func buildPostgresExplain(sql: String, options: ExplainOptions) -> String {
        var parts: [String] = [
            "ANALYZE \(options.analyze)",
            "COSTS \(options.costs)",
        ]

        if options.buffers     { parts.append("BUFFERS true") }
        if options.genericPlan { parts.append("GENERIC_PLAN true") }
        if options.memory      { parts.append("MEMORY true") }
        if options.settings    { parts.append("SETTINGS true") }
        if options.summary     { parts.append("SUMMARY true") }
        if options.timing      { parts.append("TIMING true") }
        if options.verbose     { parts.append("VERBOSE true") }
        if options.wal         { parts.append("WAL true") }

        if options.serialize != .off {
            parts.append("SERIALIZE \(options.serialize.rawValue)")
        }

        // FORMAT always last per PG convention (purely cosmetic; server doesn't care).
        parts.append("FORMAT \(options.format.rawValue)")

        return "EXPLAIN (\(parts.joined(separator: ", "))) \(sql)"
    }
}
