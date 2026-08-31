// MySQLAdapterIntegrationTests.swift
//
// Integration tests against the local MySQL fixture in
// Tests/Fixtures/mysql-empty-caching-sha2.

import XCTest
@testable import Gridex

final class MySQLAdapterIntegrationTests: XCTestCase {
    private func skipIfNoServer(port: Int) throws {
        let socketDescriptor = socket(AF_INET, SOCK_STREAM, 0)
        defer { close(socketDescriptor) }
        var address = sockaddr_in()
        address.sin_family = sa_family_t(AF_INET)
        address.sin_port = in_port_t(port).bigEndian
        address.sin_addr.s_addr = inet_addr("127.0.0.1")
        let result = withUnsafePointer(to: &address) { pointer -> Int32 in
            pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) { socketAddress in
                connect(
                    socketDescriptor,
                    socketAddress,
                    socklen_t(MemoryLayout<sockaddr_in>.size)
                )
            }
        }
        try XCTSkipIf(
            result != 0,
            "MySQL not reachable on 127.0.0.1:\(port) — skipping integration test"
        )
    }

    func test_emptyPassword_connectsToNonTLSCachingSHA2Account() async throws {
        try skipIfNoServer(port: 13307)
        let config = ConnectionConfig(
            name: "mysql-empty-auth",
            databaseType: .mysql,
            host: "127.0.0.1",
            port: 13307,
            database: "gridex_empty_auth",
            username: "gridex_empty",
            sslEnabled: false,
            sslMode: .disabled
        )
        let adapter = MySQLAdapter()
        do {
            try await adapter.connect(config: config, password: "")
            let result = try await adapter.executeRaw(sql: "SELECT 1 AS value")
            XCTAssertEqual(result.rows.first?.first?.intValue, 1)
            try await adapter.disconnect()
        } catch {
            try? await adapter.disconnect()
            throw error
        }
    }
}
