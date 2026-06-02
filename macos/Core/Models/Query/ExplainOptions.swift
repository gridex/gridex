// ExplainOptions.swift
// Gridex
//
// User-toggleable EXPLAIN options. Currently shaped for PostgreSQL — the only
// engine with a rich option list (`EXPLAIN (FOO true, BAR false, ...)`).
// MySQL / SQL Server / SQLite / ClickHouse have no per-option syntax, so the
// builder ignores the flags for those engines and returns the engine's plain
// `EXPLAIN` form.
//
// Cross-disable rules and version gating live here so the UI and the SQL
// builder share a single source of truth — a future toolbar that doesn't
// honour the rules can't accidentally produce a server-rejected query.

import Foundation

struct ExplainOptions: Equatable, Codable, Sendable {

    // MARK: - PG flag toggles

    /// Execute the query (modifies data!) and report actual times.
    /// Off → planner output only, no side effects.
    var analyze: Bool = false

    /// Buffer-pool usage statistics.
    /// PG 9.0+, before PG 13 only meaningful with ANALYZE.
    var buffers: Bool = false

    /// Show estimated planner costs. Default ON in PG.
    var costs: Bool = true

    /// Show plan for a prepared statement with generic parameter values.
    /// PG 16+.
    var genericPlan: Bool = false

    /// Per-node memory usage during planning.
    /// PG 17+.
    var memory: Bool = false

    /// Modified non-default GUC settings used to plan the query.
    /// PG 12+.
    var settings: Bool = false

    /// Summary information (planning + execution time).
    /// Default ON when ANALYZE is set.
    var summary: Bool = false

    /// Per-node actual time output.
    /// Requires ANALYZE.
    var timing: Bool = false

    /// More-detailed per-node output (output columns, schema-qualified names).
    var verbose: Bool = false

    /// Per-node WAL usage.
    /// PG 13+, requires ANALYZE.
    var wal: Bool = false

    // MARK: - Multi-value enums

    enum Format: String, Codable, Sendable, CaseIterable, Identifiable {
        case text = "TEXT"
        case json = "JSON"
        case yaml = "YAML"
        case xml  = "XML"

        var id: String { rawValue }
        var displayName: String { rawValue.capitalized }
    }

    enum Serialize: String, Codable, Sendable, CaseIterable, Identifiable {
        case off    = "OFF"     // do not include SERIALIZE in the option list
        case none   = "NONE"
        case text   = "TEXT"
        case binary = "BINARY"

        var id: String { rawValue }
        var displayName: String {
            self == .off ? "Off" : rawValue.capitalized
        }
    }

    /// Output format. PG default is TEXT.
    var format: Format = .text

    /// SERIALIZE option (PG 17+, requires ANALYZE).
    /// `.off` means don't include the option at all.
    var serialize: Serialize = .off

    // MARK: - Defaults

    /// Matches the legacy hard-coded `EXPLAIN (ANALYZE false, COSTS true, FORMAT TEXT)`
    /// — used when callers don't pass options.
    static let `default` = ExplainOptions()

    /// "Profile" preset — what a senior dev would tick before debugging a slow
    /// SELECT: actual times, buffer hits/reads, schema-qualified output, plus
    /// the planning/execution summary. Does NOT change FORMAT — caller keeps
    /// whatever they already set so JSON-tree users don't get downgraded to
    /// TEXT after applying the preset.
    ///
    /// **Caution**: enabling ANALYZE means the query is actually executed.
    /// Safe for SELECT, dangerous for INSERT/UPDATE/DELETE (will modify data).
    /// The UI surfaces this preset as "Profile (SELECT)" to make the
    /// expectation explicit.
    static func profilePreset(currentFormat: Format) -> ExplainOptions {
        var opts = ExplainOptions.default
        opts.analyze = true
        opts.buffers = true
        opts.verbose = true
        opts.summary = true
        opts.format  = currentFormat
        return opts
    }
}

// MARK: - Cross-disable rules

extension ExplainOptions {
    /// PG enforces these constraints server-side; surfacing them in the model
    /// lets the UI grey out impossible combinations before sending the query.

    /// Options that are valid only when `analyze == true`.
    /// Server rejects them with `EXPLAIN option X requires ANALYZE` otherwise.
    enum AnalyzeDependency: CaseIterable {
        case timing, wal, serialize
    }

    /// True when the toggle for `dep` should be enabled in the UI given the
    /// current `analyze` setting.
    func canEnable(_ dep: AnalyzeDependency) -> Bool {
        analyze
    }

    /// Apply cross-disable rules — call before sending. Force-clears any
    /// option that is invalid given the current settings.
    func sanitized() -> ExplainOptions {
        var copy = self
        if !copy.analyze {
            copy.timing = false
            copy.wal = false
            copy.serialize = .off
        }
        return copy
    }
}

// MARK: - Version gating

extension ExplainOptions {
    /// Each option is keyed to the minimum PG major version on which it
    /// becomes available. Older servers reject the option as a syntax error.
    enum VersionGated: Hashable, CaseIterable {
        case settings      // PG 12+
        case wal           // PG 13+
        case genericPlan   // PG 16+
        case memory        // PG 17+
        case serialize     // PG 17+

        var minPostgresMajor: Int {
            switch self {
            case .settings:    return 12
            case .wal:         return 13
            case .genericPlan: return 16
            case .memory:      return 17
            case .serialize:   return 17
            }
        }

        var displayName: String {
            switch self {
            case .settings:    return "Settings"
            case .wal:         return "WAL"
            case .genericPlan: return "Generic Plan"
            case .memory:      return "Memory"
            case .serialize:   return "Serialize"
            }
        }
    }

    /// True when option is supported on a PG server reporting major version
    /// `serverMajor`. Pass nil when the version is unknown — we permit the
    /// option in that case and let the server tell the user (consistent with
    /// the rest of the codebase, which doesn't pre-detect server features).
    static func isAvailable(_ option: VersionGated, on serverMajor: Int?) -> Bool {
        guard let v = serverMajor else { return true }
        return v >= option.minPostgresMajor
    }
}

// MARK: - Persistence keying

extension ExplainOptions {
    /// UserDefaults key for storing the last-used options per connection.
    /// Connection-scoped so a user's PG 17 server can keep Memory enabled
    /// without polluting a sibling PG 12 connection.
    static func userDefaultsKey(connectionId: UUID) -> String {
        "explainOptions.\(connectionId.uuidString)"
    }
}
