// MySQLTLSIPHostTests.swift
//
// TLS to an IP-address host (e.g. 127.0.0.1) used to fail with
// NIOSSLExtraError.cannotUseIPAddressInSNI because the IP was sent as SNI.
// Skipped unless a TLS-capable MySQL/MariaDB is reachable on 127.0.0.1:13320:
//   docker run -d --rm --name gridex-mysql-tls -e MYSQL_ROOT_PASSWORD=rootpw -p 127.0.0.1:13320:3306 mysql:8.4

import XCTest
@testable import Gridex

final class MySQLTLSIPHostTests: XCTestCase {
    private let port = 13320

    func test_ipHost_connectsOverTLS_preferredAndRequired() async throws {
        try skipIfNoServer()
        for mode in [SSLMode.preferred, .required] {
            let adapter = MySQLAdapter()
            let config = ConnectionConfig(
                name: "mysql-tls-ip", databaseType: .mysql, host: "127.0.0.1", port: port,
                database: nil, username: "root", sslEnabled: true, sslMode: mode
            )
            do {
                try await adapter.connect(config: config, password: "rootpw")
                let result = try await adapter.executeRaw(sql: "SHOW SESSION STATUS LIKE 'Ssl_cipher'")
                let cipher = result.rows.first.flatMap { $0.count > 1 ? $0[1].stringValue : nil } ?? ""
                XCTAssertFalse(cipher.isEmpty, "\(mode): connection is not encrypted")
                try await adapter.disconnect()
            } catch {
                try? await adapter.disconnect()
                XCTFail("\(mode): \(error)")
            }
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
        try XCTSkipIf(rc != 0, "MySQL not reachable on 127.0.0.1:\(port) — skipping")
    }
}
