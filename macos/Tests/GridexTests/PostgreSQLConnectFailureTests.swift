// PostgreSQLConnectFailureTests.swift
//
// Connect failures must surface quickly with the server's message, not after the
// connection pool's ~70s retry window as "ConnectionPoolError error 1".
// Skipped unless a PG without SSL is reachable on 127.0.0.1:55440:
//   docker run -d --rm --name gridex-pg-auth -e POSTGRES_PASSWORD=rootpw -p 127.0.0.1:55440:5432 postgres:16

import XCTest
@testable import Gridex

final class PostgreSQLConnectFailureTests: XCTestCase {
    private let port = 55440

    private func config(database: String = "postgres", port: Int? = nil, sslMode: SSLMode = .disabled) -> ConnectionConfig {
        ConnectionConfig(
            name: "pg-connect-failure", databaseType: .postgresql, host: "127.0.0.1", port: port ?? self.port,
            database: database, username: "postgres", sslEnabled: sslMode != .disabled, sslMode: sslMode
        )
    }

    /// Connects and returns the error message and how long the failure took.
    private func failure(_ config: ConnectionConfig, password: String) async -> (message: String, seconds: Double)? {
        let adapter = PostgreSQLAdapter()
        let start = Date()
        defer { Task { try? await adapter.disconnect() } }
        do {
            try await adapter.connect(config: config, password: password)
            return nil
        } catch {
            return (error.localizedDescription, Date().timeIntervalSince(start))
        }
    }

    func test_wrongPassword_failsFastWithServerMessage() async throws {
        try skipIfNoServer()
        let outcome = await failure(config(), password: "wrong")
        let result = try XCTUnwrap(outcome, "connect should fail")
        XCTAssertLessThan(result.seconds, 5)
        XCTAssertTrue(result.message.contains("password authentication failed"), result.message)
        XCTAssertTrue(result.message.contains("28P01"), result.message)
    }

    func test_unknownDatabase_failsFastWithServerMessage() async throws {
        try skipIfNoServer()
        let outcome = await failure(config(database: "gx_does_not_exist"), password: "rootpw")
        let result = try XCTUnwrap(outcome, "connect should fail")
        XCTAssertLessThan(result.seconds, 5)
        XCTAssertTrue(result.message.contains("does not exist"), result.message)
    }

    func test_closedPort_failsFast() async throws {
        try skipIfNoServer()
        let outcome = await failure(config(port: 1), password: "rootpw")
        let result = try XCTUnwrap(outcome, "connect should fail")
        XCTAssertLessThan(result.seconds, 5)
        XCTAssertTrue(result.message.contains("Cannot reach 127.0.0.1:1"), result.message)
    }

    func test_sslRequired_onServerWithoutSSL_failsFast() async throws {
        try skipIfNoServer()
        let outcome = await failure(config(sslMode: .required), password: "rootpw")
        let result = try XCTUnwrap(outcome, "connect should fail")
        XCTAssertLessThan(result.seconds, 5)
        XCTAssertTrue(result.message.contains("does not support SSL"), result.message)
    }

    func test_validCredentials_connect_includingPreferredFallback() async throws {
        try skipIfNoServer()
        for mode in [SSLMode.disabled, .preferred] {
            let adapter = PostgreSQLAdapter()
            try await adapter.connect(config: config(sslMode: mode), password: "rootpw")
            let result = try await adapter.executeRaw(sql: "SELECT 1")
            XCTAssertEqual(result.rows.first?.first?.intValue, 1, "\(mode)")
            try await adapter.disconnect()
        }
    }

    private func skipIfNoServer() throws {
        let fd = socket(AF_INET, SOCK_STREAM, 0)
        defer { close(fd) }
        var addr = sockaddr_in()
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = in_port_t(port).bigEndian
        addr.sin_addr.s_addr = inet_addr("127.0.0.1")
        let rc = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                Darwin.connect(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        try XCTSkipIf(rc != 0, "PG not reachable on 127.0.0.1:\(port) — skipping")
    }
}
