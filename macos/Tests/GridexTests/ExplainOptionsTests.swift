// ExplainOptionsTests.swift
// Pure-logic tests for the new ExplainOptions struct + the PG `EXPLAIN (...)`
// option-list builder it powers. No infra needed.

import XCTest
@testable import Gridex

final class ExplainOptionsTests: XCTestCase {

    private let sql = "SELECT * FROM profile"

    // MARK: - Default value pins legacy behaviour

    func test_default_buildsLegacyShape() {
        // The previous hardcoded query was
        //   "EXPLAIN (ANALYZE false, COSTS true, FORMAT TEXT) <sql>"
        // The default value of ExplainOptions must reproduce it exactly so
        // every existing call site stays byte-compatible.
        let out = DatabaseType.postgresql.explainSQL(for: sql)
        XCTAssertEqual(out, "EXPLAIN (ANALYZE false, COSTS true, FORMAT TEXT) \(sql)")
    }

    // MARK: - Always-emitted vs opt-in toggles

    func test_costsCanBeTurnedOff() {
        var opts = ExplainOptions.default
        opts.costs = false
        let out = DatabaseType.postgresql.explainSQL(for: sql, options: opts)
        XCTAssertEqual(out, "EXPLAIN (ANALYZE false, COSTS false, FORMAT TEXT) \(sql)")
    }

    func test_offToggles_doNotAppearInOutput() {
        // Buffers / Memory / Settings / Summary / Verbose / WAL all default
        // false, must not bloat the option list.
        let out = DatabaseType.postgresql.explainSQL(for: sql)!
        XCTAssertFalse(out.contains("BUFFERS"))
        XCTAssertFalse(out.contains("MEMORY"))
        XCTAssertFalse(out.contains("SETTINGS"))
        XCTAssertFalse(out.contains("SUMMARY"))
        XCTAssertFalse(out.contains("VERBOSE"))
        XCTAssertFalse(out.contains("WAL"))
        XCTAssertFalse(out.contains("GENERIC_PLAN"))
        XCTAssertFalse(out.contains("SERIALIZE"))
    }

    func test_onToggles_appearWithTrue() {
        var opts = ExplainOptions.default
        opts.buffers     = true
        opts.memory      = true
        opts.settings    = true
        opts.summary     = true
        opts.verbose     = true
        let out = DatabaseType.postgresql.explainSQL(for: sql, options: opts)!
        for piece in ["BUFFERS true", "MEMORY true", "SETTINGS true",
                      "SUMMARY true", "VERBOSE true"] {
            XCTAssertTrue(out.contains(piece), "missing '\(piece)' in: \(out)")
        }
    }

    // MARK: - Cross-disable: ANALYZE-dependent options

    func test_timing_requiresAnalyze_sanitizedOut() {
        // User toggles Timing on without enabling Analyze. PG would 400 with
        // "EXPLAIN option TIMING requires ANALYZE". `sanitized()` (called
        // inside the builder) drops the offending flag pre-flight.
        var opts = ExplainOptions.default
        opts.analyze = false
        opts.timing = true
        let out = DatabaseType.postgresql.explainSQL(for: sql, options: opts)!
        XCTAssertFalse(out.contains("TIMING"), "TIMING must be stripped when ANALYZE is off")
    }

    func test_wal_requiresAnalyze_sanitizedOut() {
        var opts = ExplainOptions.default
        opts.wal = true
        let out = DatabaseType.postgresql.explainSQL(for: sql, options: opts)!
        XCTAssertFalse(out.contains("WAL"))
    }

    func test_serialize_requiresAnalyze_sanitizedOut() {
        var opts = ExplainOptions.default
        opts.serialize = .text
        let out = DatabaseType.postgresql.explainSQL(for: sql, options: opts)!
        XCTAssertFalse(out.contains("SERIALIZE"))
    }

    func test_canEnable_returnsFalse_whenAnalyzeOff() {
        var opts = ExplainOptions.default
        opts.analyze = false
        for dep in ExplainOptions.AnalyzeDependency.allCases {
            XCTAssertFalse(opts.canEnable(dep), "\(dep) must be disabled when analyze=false")
        }
    }

    func test_canEnable_returnsTrue_whenAnalyzeOn() {
        var opts = ExplainOptions.default
        opts.analyze = true
        for dep in ExplainOptions.AnalyzeDependency.allCases {
            XCTAssertTrue(opts.canEnable(dep), "\(dep) must be enabled when analyze=true")
        }
    }

    func test_analyzeOn_keepsDependentOptions() {
        var opts = ExplainOptions.default
        opts.analyze = true
        opts.timing = true
        opts.wal = true
        opts.serialize = .binary
        let out = DatabaseType.postgresql.explainSQL(for: sql, options: opts)!
        XCTAssertTrue(out.contains("ANALYZE true"))
        XCTAssertTrue(out.contains("TIMING true"))
        XCTAssertTrue(out.contains("WAL true"))
        XCTAssertTrue(out.contains("SERIALIZE BINARY"))
    }

    // MARK: - Format submenu

    func test_format_changesOutputClause() {
        for fmt in ExplainOptions.Format.allCases {
            var opts = ExplainOptions.default
            opts.format = fmt
            let out = DatabaseType.postgresql.explainSQL(for: sql, options: opts)!
            XCTAssertTrue(out.contains("FORMAT \(fmt.rawValue)"),
                          "missing 'FORMAT \(fmt.rawValue)' in: \(out)")
        }
    }

    // MARK: - Version gating

    func test_versionGating_minMajor() {
        XCTAssertEqual(ExplainOptions.VersionGated.settings.minPostgresMajor,    12)
        XCTAssertEqual(ExplainOptions.VersionGated.wal.minPostgresMajor,         13)
        XCTAssertEqual(ExplainOptions.VersionGated.genericPlan.minPostgresMajor, 16)
        XCTAssertEqual(ExplainOptions.VersionGated.memory.minPostgresMajor,      17)
        XCTAssertEqual(ExplainOptions.VersionGated.serialize.minPostgresMajor,   17)
    }

    func test_isAvailable_respectsServerVersion() {
        // PG 12 server: settings yes, wal/generic/memory/serialize no.
        XCTAssertTrue(ExplainOptions.isAvailable(.settings, on: 12))
        XCTAssertFalse(ExplainOptions.isAvailable(.wal, on: 12))
        XCTAssertFalse(ExplainOptions.isAvailable(.memory, on: 12))

        // PG 17 server: every gated option allowed.
        for opt in ExplainOptions.VersionGated.allCases {
            XCTAssertTrue(ExplainOptions.isAvailable(opt, on: 17),
                          "\(opt) must be available on PG 17")
        }
    }

    func test_isAvailable_nilVersion_permitsEverything() {
        // When server version is unknown, we default to permissive — let the
        // server tell the user. Matches the rest of the codebase, which doesn't
        // pre-detect server features.
        for opt in ExplainOptions.VersionGated.allCases {
            XCTAssertTrue(ExplainOptions.isAvailable(opt, on: nil))
        }
    }

    // MARK: - Other engines unchanged by options

    func test_otherEngines_ignoreOptions() {
        var opts = ExplainOptions.default
        opts.analyze = true
        opts.buffers = true
        opts.format  = .json
        // MySQL / SQLite / MSSQL / ClickHouse have no per-option syntax.
        XCTAssertEqual(DatabaseType.mysql.explainSQL(for: sql, options: opts),
                       "EXPLAIN \(sql)")
        XCTAssertEqual(DatabaseType.sqlite.explainSQL(for: sql, options: opts),
                       "EXPLAIN QUERY PLAN \(sql)")
        XCTAssertEqual(DatabaseType.mssql.explainSQL(for: sql, options: opts),
                       "SET SHOWPLAN_TEXT ON; \(sql); SET SHOWPLAN_TEXT OFF")
        XCTAssertEqual(DatabaseType.clickhouse.explainSQL(for: sql, options: opts),
                       "EXPLAIN \(sql)")
        XCTAssertNil(DatabaseType.mongodb.explainSQL(for: sql, options: opts))
        XCTAssertNil(DatabaseType.redis.explainSQL(for: sql, options: opts))
    }

    // MARK: - Codable persistence

    func test_codable_roundTripsAllFields() throws {
        var opts = ExplainOptions.default
        opts.analyze     = true
        opts.buffers     = true
        opts.genericPlan = true
        opts.memory      = true
        opts.settings    = true
        opts.summary     = true
        opts.timing      = true
        opts.verbose     = true
        opts.wal         = true
        opts.serialize   = .binary
        opts.format      = .json

        let data = try JSONEncoder().encode(opts)
        let restored = try JSONDecoder().decode(ExplainOptions.self, from: data)
        XCTAssertEqual(restored, opts)
    }

    // MARK: - PG version banner parsing

    func test_parsePostgresMajor_examples() {
        XCTAssertEqual(QueryEditorView.parsePostgresMajor(from:
            "PostgreSQL 16.2 on aarch64-apple-darwin"), 16)
        XCTAssertEqual(QueryEditorView.parsePostgresMajor(from:
            "PostgreSQL 9.6.24 on x86_64-pc-linux-gnu"), 9)
        XCTAssertEqual(QueryEditorView.parsePostgresMajor(from:
            "PostgreSQL 17rc1"), 17)
        XCTAssertNil(QueryEditorView.parsePostgresMajor(from: "garbage"))
        XCTAssertNil(QueryEditorView.parsePostgresMajor(from: ""))
    }
}
