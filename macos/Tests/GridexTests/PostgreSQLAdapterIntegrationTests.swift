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
        XCTAssertEqual(names.filter { $0 == "app_view" }.count, 1)
        XCTAssertTrue(names.contains("hg_t_audit_log"), "pg_class-backed listTables should include physical tables hidden by information_schema.tables")
    }
}
