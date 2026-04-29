// PostgreSQLAdapterIntegrationTests.swift
//
// Integration tests for PR #43 against two local Postgres instances:
//   :55434  ssl=off  (no-SSL)
//   :55435  ssl=on   (cert CN=localhost, signed by /tmp/gridex-pg-certs/ca.crt)
//
// Skipped automatically when those instances aren't reachable (CI / dev machines
// without test PG running). To bring them up locally, see scripts/test-pg-setup.sh
// (added in this PR) or the inline shell in PR #43's review thread.

import XCTest
@testable import Gridex

final class PostgreSQLAdapterIntegrationTests: XCTestCase {

    // MARK: - Endpoints

    private let noSSLPort = 5432  // :55434 in shell, but Swift tests open it via 127.0.0.1
    private let sslPort   = 5432  // overridden below
    private let caPath    = "/tmp/gridex-pg-certs/ca.crt"

    private func makeBaseConfig(host: String, port: Int, sslMode: SSLMode,
                                caPath: String? = nil) -> ConnectionConfig {
        ConnectionConfig(
            id: UUID(),
            name: "test",
            databaseType: .postgresql,
            host: host,
            port: port,
            database: "postgres",
            username: "postgres",
            sslEnabled: sslMode != .disabled,
            sslMode: sslMode,
            sslCACertPath: caPath
        )
    }

    private func skipIfNoServer(port: Int) throws {
        // Fast TCP probe — skip the test if the test PG isn't running.
        let s = socket(AF_INET, SOCK_STREAM, 0)
        defer { close(s) }
        var addr = sockaddr_in()
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = in_port_t(port).bigEndian
        addr.sin_addr.s_addr = inet_addr("127.0.0.1")
        let rc = withUnsafePointer(to: &addr) { ptr -> Int32 in
            ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                connect(s, sa, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        try XCTSkipIf(rc != 0,
                      "PG not reachable on 127.0.0.1:\(port) — skipping integration test")
    }

    // MARK: - Mode × server matrix

    // ---- vs no-SSL server (:55434) ----

    func test_disabled_vs_noSSLServer_succeeds() async throws {
        try skipIfNoServer(port: 55434)
        let cfg = makeBaseConfig(host: "127.0.0.1", port: 55434, sslMode: .disabled)
        let adapter = PostgreSQLAdapter()
        try await adapter.connect(config: cfg, password: nil)
        XCTAssertTrue(adapter.isConnected)
        try await adapter.disconnect()
    }

    func test_preferred_vs_noSSLServer_succeeds_overPlaintext() async throws {
        try skipIfNoServer(port: 55434)
        // libpq prefer: try TLS, fall back to plaintext when server refuses.
        let cfg = makeBaseConfig(host: "127.0.0.1", port: 55434, sslMode: .preferred)
        let adapter = PostgreSQLAdapter()
        try await adapter.connect(config: cfg, password: nil)
        XCTAssertTrue(adapter.isConnected, "preferred must fall back to plaintext")
        try await adapter.disconnect()
    }

    func test_required_vs_noSSLServer_FAILS() async throws {
        try skipIfNoServer(port: 55434)
        // libpq REQUIRED must reject a server that can't TLS.
        // This is the security regression PR #43 is fixing — pre-PR it silently
        // succeeded over plaintext because adapter used .prefer(tls).
        let cfg = makeBaseConfig(host: "127.0.0.1", port: 55434, sslMode: .required)
        let adapter = PostgreSQLAdapter()
        do {
            try await adapter.connect(config: cfg, password: nil)
            try? await adapter.disconnect()
            XCTFail("REQUIRED must reject a server with ssl=off; pre-PR this silently succeeded over plaintext")
        } catch {
            // expected
        }
    }

    func test_verifyCA_vs_noSSLServer_FAILS() async throws {
        try skipIfNoServer(port: 55434)
        let cfg = makeBaseConfig(host: "127.0.0.1", port: 55434,
                                 sslMode: .verifyCA, caPath: caPath)
        let adapter = PostgreSQLAdapter()
        do {
            try await adapter.connect(config: cfg, password: nil)
            try? await adapter.disconnect()
            XCTFail("VERIFY_CA must reject a server with ssl=off")
        } catch { /* expected */ }
    }

    func test_verifyIdentity_vs_noSSLServer_FAILS() async throws {
        try skipIfNoServer(port: 55434)
        let cfg = makeBaseConfig(host: "127.0.0.1", port: 55434,
                                 sslMode: .verifyIdentity, caPath: caPath)
        let adapter = PostgreSQLAdapter()
        do {
            try await adapter.connect(config: cfg, password: nil)
            try? await adapter.disconnect()
            XCTFail("VERIFY_IDENTITY must reject a server with ssl=off")
        } catch { /* expected */ }
    }

    // ---- vs SSL server (:55435) ----

    func test_disabled_vs_SSLServer_works() async throws {
        try skipIfNoServer(port: 55435)
        // PG with ssl=on still accepts plaintext clients by default (pg_hba allows both).
        let cfg = makeBaseConfig(host: "127.0.0.1", port: 55435, sslMode: .disabled)
        let adapter = PostgreSQLAdapter()
        try await adapter.connect(config: cfg, password: nil)
        XCTAssertTrue(adapter.isConnected)
        try await adapter.disconnect()
    }

    func test_required_vs_SSLServer_succeeds() async throws {
        try skipIfNoServer(port: 55435)
        let cfg = makeBaseConfig(host: "127.0.0.1", port: 55435, sslMode: .required)
        let adapter = PostgreSQLAdapter()
        try await adapter.connect(config: cfg, password: nil)
        XCTAssertTrue(adapter.isConnected)
        try await adapter.disconnect()
    }

    func test_verifyCA_withCorrectCA_succeeds_evenAtIPHost() async throws {
        try skipIfNoServer(port: 55435)
        // verifyCA: chain must validate, but hostname mismatch is OK
        // (cert is for "localhost", connecting via "127.0.0.1").
        let cfg = makeBaseConfig(host: "127.0.0.1", port: 55435,
                                 sslMode: .verifyCA, caPath: caPath)
        let adapter = PostgreSQLAdapter()
        try await adapter.connect(config: cfg, password: nil)
        XCTAssertTrue(adapter.isConnected)
        try await adapter.disconnect()
    }

    func test_verifyIdentity_withMismatchedHostname_FAILS() async throws {
        try skipIfNoServer(port: 55435)
        // Cert CN=localhost, connect via 127.0.0.1 → must reject (matches psql verify-full).
        let cfg = makeBaseConfig(host: "127.0.0.1", port: 55435,
                                 sslMode: .verifyIdentity, caPath: caPath)
        let adapter = PostgreSQLAdapter()
        do {
            try await adapter.connect(config: cfg, password: nil)
            try? await adapter.disconnect()
            XCTFail("VERIFY_IDENTITY must reject when host doesn't match cert SAN/CN")
        } catch { /* expected */ }
    }

    func test_verifyIdentity_withMatchingHostname_succeeds() async throws {
        try skipIfNoServer(port: 55435)
        let cfg = makeBaseConfig(host: "localhost", port: 55435,
                                 sslMode: .verifyIdentity, caPath: caPath)
        let adapter = PostgreSQLAdapter()
        try await adapter.connect(config: cfg, password: nil)
        XCTAssertTrue(adapter.isConnected)
        try await adapter.disconnect()
    }

    func test_verifyCA_withWrongCA_FAILS() async throws {
        try skipIfNoServer(port: 55435)
        // Provide an unrelated cert as the CA — must reject.
        let cfg = makeBaseConfig(host: "127.0.0.1", port: 55435,
                                 sslMode: .verifyCA,
                                 caPath: "/tmp/gridex-pg-certs/server-wrongcn.crt")
        let adapter = PostgreSQLAdapter()
        do {
            try await adapter.connect(config: cfg, password: nil)
            try? await adapter.disconnect()
            XCTFail("VERIFY_CA must reject when supplied CA does not sign the server cert")
        } catch { /* expected */ }
    }

    // MARK: - Bug #1 — race fix

    func test_connect_doesNotRaiseConnectionPoolError_under_repeatedRapidConnects() async throws {
        try skipIfNoServer(port: 55434)
        // Pre-PR: spawning Task { run() } and immediately querying could surface
        // _ConnectionPoolModule.ConnectionPoolError on slow handshakes.
        // Repeat 20 times; if even one fails for a reason other than expected
        // server-side error, the gate didn't hold.
        for i in 1...20 {
            let cfg = makeBaseConfig(host: "127.0.0.1", port: 55434, sslMode: .disabled)
            let adapter = PostgreSQLAdapter()
            do {
                try await adapter.connect(config: cfg, password: nil)
                try await adapter.disconnect()
            } catch {
                XCTFail("iteration \(i) raised: \(error)")
                return
            }
        }
    }

    // MARK: - HighGo Secure regressions

    // Regression test for HighGo Secure 4.5+: information_schema.tables can return
    // duplicate rows for the same physical table, which made the macOS sidebar
    // render repeated names. listTables must dedupe by reading pg_class directly.
    //
    // Target instance requirements:
    //   - Must allow non-SSL connections (test connects with sslMode = .disabled).
    //   - Schema `public` must contain at least one user-visible table whose name
    //     starts with `hg_` (e.g. the HighGo audit module's `hg_t_audit_log`),
    //     i.e. the HighGo audit module must be enabled.
    //
    // Run with: GRIDEX_HIGHGO_HOST=... GRIDEX_HIGHGO_PORT=... GRIDEX_HIGHGO_DATABASE=...
    //            GRIDEX_HIGHGO_USER=... GRIDEX_HIGHGO_PASSWORD=... swift test ...
    func test_listTables_usesPgCatalogOnHighGoSecure() async throws {
        let env = ProcessInfo.processInfo.environment
        guard let host = env["GRIDEX_HIGHGO_HOST"],
              let port = Int(env["GRIDEX_HIGHGO_PORT"] ?? ""),
              let database = env["GRIDEX_HIGHGO_DATABASE"],
              let username = env["GRIDEX_HIGHGO_USER"],
              let password = env["GRIDEX_HIGHGO_PASSWORD"] else {
            throw XCTSkip("Set GRIDEX_HIGHGO_* to run the HighGo Secure catalog regression test")
        }

        let cfg = ConnectionConfig(
            id: UUID(),
            name: "highgo-catalog-regression",
            databaseType: .postgresql,
            host: host,
            port: port,
            database: database,
            username: username,
            sslEnabled: false,
            sslMode: .disabled
        )
        let adapter = PostgreSQLAdapter()
        try await adapter.connect(config: cfg, password: password)

        let tables: [TableInfo]
        do {
            tables = try await adapter.listTables(schema: "public")
            try await adapter.disconnect()
        } catch {
            try? await adapter.disconnect()
            throw error
        }

        let names = tables.map(\.name)
        XCTAssertEqual(names.count, Set(names).count, "HighGo information_schema.tables can duplicate rows; listTables must return unique physical tables")
        XCTAssertTrue(
            names.contains(where: { $0.hasPrefix("hg_") }),
            "pg_class-backed listTables should include HighGo-managed physical tables (hg_*) that information_schema.tables can hide"
        )
    }

    // MARK: - Vanilla PG regression — partition children must not surface as siblings

    // Declarative partitioning (PG 11+): a partition child has relkind='r' and
    // relispartition=true. The parent (relkind='p') already represents its
    // children's data in the UI, so leaking children would double-count rows
    // and clutter the sidebar. The original information_schema.tables query
    // returned only 'BASE TABLE' parents; switching to pg_class introduced
    // the leak unless we also filter `NOT relispartition`.
    func test_listTables_excludesPartitionChildren_onVanillaPG() async throws {
        try skipIfNoServer(port: 55434)

        let cfg = makeBaseConfig(host: "127.0.0.1", port: 55434, sslMode: .disabled)
        let adapter = PostgreSQLAdapter()
        try await adapter.connect(config: cfg, password: nil)

        // Clean previous fixtures, then create a partitioned table + one partition.
        let prefix = "gridex_part_test_\(UUID().uuidString.prefix(8).lowercased())"
        let plain = "\(prefix)_plain"
        let parent = "\(prefix)_parent"
        let child = "\(prefix)_p1"
        do {
            _ = try await adapter.executeRaw(sql: "CREATE TABLE \(plain) (id int)")
            _ = try await adapter.executeRaw(sql: "CREATE TABLE \(parent) (id int) PARTITION BY RANGE (id)")
            _ = try await adapter.executeRaw(sql: "CREATE TABLE \(child) PARTITION OF \(parent) FOR VALUES FROM (0) TO (100)")

            let names = try await adapter.listTables(schema: "public").map(\.name)

            XCTAssertTrue(names.contains(plain),
                "ordinary table \(plain) must appear")
            XCTAssertTrue(names.contains(parent),
                "partitioned-table parent \(parent) must appear")
            XCTAssertFalse(names.contains(child),
                "partition child \(child) must NOT appear as a sibling — it would double-count the parent's data")
        } catch {
            // Best-effort cleanup, then rethrow.
            _ = try? await adapter.executeRaw(sql: "DROP TABLE IF EXISTS \(parent) CASCADE")
            _ = try? await adapter.executeRaw(sql: "DROP TABLE IF EXISTS \(plain)")
            try? await adapter.disconnect()
            throw error
        }

        _ = try? await adapter.executeRaw(sql: "DROP TABLE IF EXISTS \(parent) CASCADE")
        _ = try? await adapter.executeRaw(sql: "DROP TABLE IF EXISTS \(plain)")
        try await adapter.disconnect()
    }

    // MARK: - Issue #49 — Explain button server-side regression

    // The query editor button used to send "EXPLAIN QUERY PLAN <sql>" to every
    // engine — that's SQLite syntax, Postgres rejects it with SQLSTATE 42601
    // ("syntax error at or near \"QUERY\""). After the fix, the editor goes
    // through DatabaseType.explainSQL(for:) which emits the engine-correct form.
    // This test asserts the PG-specific shape actually parses and executes.
    func test_explainSQL_postgresShape_runsWithoutSyntaxError() async throws {
        try skipIfNoServer(port: 55434)

        let cfg = makeBaseConfig(host: "127.0.0.1", port: 55434, sslMode: .disabled)
        let adapter = PostgreSQLAdapter()
        try await adapter.connect(config: cfg, password: nil)

        // Make sure there's something to plan against.
        let table = "gridex_explain_test_\(UUID().uuidString.prefix(8).lowercased())"
        do {
            _ = try await adapter.executeRaw(sql: "CREATE TABLE \(table) (id int)")

            guard let explainSQL = DatabaseType.postgresql.explainSQL(for: "SELECT * FROM \(table)") else {
                XCTFail("PG must produce a non-nil EXPLAIN string")
                return
            }
            // The literal regression: the SQL we send must NOT be the SQLite
            // form. (Belt-and-braces against a future contributor accidentally
            // re-introducing the same hardcode.)
            XCTAssertFalse(explainSQL.uppercased().contains("QUERY PLAN"))

            let result = try await adapter.executeRaw(sql: explainSQL)
            XCTAssertFalse(result.rows.isEmpty, "EXPLAIN must return at least one plan row")
        } catch {
            _ = try? await adapter.executeRaw(sql: "DROP TABLE IF EXISTS \(table)")
            try? await adapter.disconnect()
            throw error
        }

        _ = try? await adapter.executeRaw(sql: "DROP TABLE IF EXISTS \(table)")
        try await adapter.disconnect()
    }

    // MARK: - listViews + primaryKeyColumns migration off information_schema.*

    // Same anti-pattern PR #47 fixed for listTables: information_schema.{views,
    // table_constraints, key_column_usage} are privilege-filtered per the SQL
    // standard and HighGo Secure 4.5+ duplicates rows. These tests pin the new
    // pg_class / pg_constraint queries against a vanilla PG so a future
    // contributor can't silently regress to information_schema.
    func test_listViews_returnsViewsAndMaterializedViews() async throws {
        try skipIfNoServer(port: 55434)

        let cfg = makeBaseConfig(host: "127.0.0.1", port: 55434, sslMode: .disabled)
        let adapter = PostgreSQLAdapter()
        try await adapter.connect(config: cfg, password: nil)

        let suffix = UUID().uuidString.prefix(8).lowercased()
        let table = "src_\(suffix)"
        let view = "v_\(suffix)"
        let matview = "mv_\(suffix)"
        do {
            _ = try await adapter.executeRaw(sql: "CREATE TABLE \(table) (id int)")
            _ = try await adapter.executeRaw(sql: "CREATE VIEW \(view) AS SELECT id FROM \(table)")
            _ = try await adapter.executeRaw(sql: "CREATE MATERIALIZED VIEW \(matview) AS SELECT id FROM \(table) WITH NO DATA")

            let views = try await adapter.listViews(schema: "public")
            let names = views.map(\.name)

            XCTAssertTrue(names.contains(view), "ordinary view must appear")
            XCTAssertTrue(names.contains(matview), "materialized view must appear")

            let v = views.first { $0.name == view }
            let m = views.first { $0.name == matview }
            XCTAssertEqual(v?.isMaterialized, false)
            XCTAssertEqual(m?.isMaterialized, true)
            XCTAssertNotNil(v?.definition, "pg_get_viewdef must return a body")
            XCTAssertNotNil(m?.definition, "pg_get_viewdef must work for matviews too")
        } catch {
            _ = try? await adapter.executeRaw(sql: "DROP MATERIALIZED VIEW IF EXISTS \(matview)")
            _ = try? await adapter.executeRaw(sql: "DROP VIEW IF EXISTS \(view)")
            _ = try? await adapter.executeRaw(sql: "DROP TABLE IF EXISTS \(table) CASCADE")
            try? await adapter.disconnect()
            throw error
        }

        _ = try? await adapter.executeRaw(sql: "DROP MATERIALIZED VIEW IF EXISTS \(matview)")
        _ = try? await adapter.executeRaw(sql: "DROP VIEW IF EXISTS \(view)")
        _ = try? await adapter.executeRaw(sql: "DROP TABLE IF EXISTS \(table) CASCADE")
        try await adapter.disconnect()
    }

    func test_primaryKeyColumns_simpleAndCompound_orderPreserved() async throws {
        try skipIfNoServer(port: 55434)

        let cfg = makeBaseConfig(host: "127.0.0.1", port: 55434, sslMode: .disabled)
        let adapter = PostgreSQLAdapter()
        try await adapter.connect(config: cfg, password: nil)

        let suffix = UUID().uuidString.prefix(8).lowercased()
        let simple = "pk_simple_\(suffix)"
        let compound = "pk_compound_\(suffix)"
        do {
            _ = try await adapter.executeRaw(sql: "CREATE TABLE \(simple) (id int PRIMARY KEY, name text)")
            // Intentionally declare PK as (b, a) — column order in the constraint
            // is NOT the table column order. The new query must follow the
            // constraint declaration order, matching pre-PR information_schema
            // behavior.
            _ = try await adapter.executeRaw(sql: "CREATE TABLE \(compound) (a int, b int, c int, PRIMARY KEY (b, a))")

            let simplePK = try await adapter.primaryKeyColumns(table: simple, schema: "public")
            XCTAssertEqual(simplePK, ["id"])

            let compoundPK = try await adapter.primaryKeyColumns(table: compound, schema: "public")
            XCTAssertEqual(compoundPK, ["b", "a"],
                "compound PK must preserve declaration order (b, a) — not column order (a, b)")
        } catch {
            _ = try? await adapter.executeRaw(sql: "DROP TABLE IF EXISTS \(compound)")
            _ = try? await adapter.executeRaw(sql: "DROP TABLE IF EXISTS \(simple)")
            try? await adapter.disconnect()
            throw error
        }

        _ = try? await adapter.executeRaw(sql: "DROP TABLE IF EXISTS \(compound)")
        _ = try? await adapter.executeRaw(sql: "DROP TABLE IF EXISTS \(simple)")
        try await adapter.disconnect()
    }

    func test_primaryKeyColumns_returnsEmpty_whenTableHasNoPK() async throws {
        try skipIfNoServer(port: 55434)

        let cfg = makeBaseConfig(host: "127.0.0.1", port: 55434, sslMode: .disabled)
        let adapter = PostgreSQLAdapter()
        try await adapter.connect(config: cfg, password: nil)

        let table = "no_pk_\(UUID().uuidString.prefix(8).lowercased())"
        do {
            _ = try await adapter.executeRaw(sql: "CREATE TABLE \(table) (id int, name text)")
            let pk = try await adapter.primaryKeyColumns(table: table, schema: "public")
            XCTAssertEqual(pk, [])
        } catch {
            _ = try? await adapter.executeRaw(sql: "DROP TABLE IF EXISTS \(table)")
            try? await adapter.disconnect()
            throw error
        }

        _ = try? await adapter.executeRaw(sql: "DROP TABLE IF EXISTS \(table)")
        try await adapter.disconnect()
    }
}
