// UserManagementIntegrationTests.swift
//
// Exercises UserManageable against real servers, logging in as the created user to
// check what each access preset actually allows. Skipped when the servers aren't up.
// To run locally:
//   docker run -d --rm --name gridex-users-pg -e POSTGRES_PASSWORD=rootpw -p 127.0.0.1:55440:5432 postgres:16
//   docker run -d --rm --name gridex-users-mysql -e MYSQL_ROOT_PASSWORD=rootpw -p 127.0.0.1:13320:3306 mysql:8.4
//   docker run -d --rm --name gridex-users-mariadb -e MARIADB_ROOT_PASSWORD=rootpw -p 127.0.0.1:13321:3306 mariadb:11

import XCTest
@testable import Gridex

final class UserManagementIntegrationTests: XCTestCase {
    private let password = #"pa'ss\wo"rd$1"#
    private let newPassword = #"n3w'\pass"#

    // MARK: - PostgreSQL

    func test_postgres_userLifecycleAndAccessPresets() async throws {
        let port = 55440
        try skipIfNoServer(port: port)
        let admin = try await connect(.postgresql, port: port, user: "postgres", password: "rootpw", database: "postgres")
        defer { Task { try? await admin.disconnect() } }
        let users = try XCTUnwrap(admin as? UserManageable)
        let name = "gx test-user"

        _ = try? await admin.executeRaw(sql: "DROP TABLE IF EXISTS gx_t, gx_t2")
        if let existing = try await users.listUsers().first(where: { $0.name == name }) {
            try await users.dropUser(existing)
        }
        _ = try await admin.executeRaw(sql: "CREATE TABLE gx_t (id serial PRIMARY KEY, v text)")

        try await users.createUser(name: name, host: nil, password: password)
        let listed = try await users.listUsers()
        let user = try XCTUnwrap(listed.first { $0.name == name })
        XCTAssertTrue(user.canLogin)
        XCTAssertFalse(user.isSuperuser)
        XCTAssertEqual(listed.first { $0.name == "postgres" }?.isSuperuser, true)

        func expect(_ allowed: Bool, _ sql: String, _ pw: String? = nil, _ message: String = "", line: UInt = #line) async {
            let ok = await canRun(sql, .postgresql, port: port, user: name, password: pw ?? password, database: "postgres")
            XCTAssertEqual(ok, allowed, "\(sql) \(message)", line: line)
        }

        try await users.setAccess(.none, for: user, database: "postgres")
        await expect(false, "SELECT * FROM gx_t")

        try await users.setAccess(.readOnly, for: user, database: "postgres")
        await expect(true, "SELECT * FROM gx_t")
        await expect(false, "INSERT INTO gx_t (v) VALUES ('x')")

        try await users.setAccess(.readWrite, for: user, database: "postgres")
        await expect(true, "INSERT INTO gx_t (v) VALUES ('x')", nil, "needs table + sequence privileges")
        await expect(false, "CREATE TABLE gx_t2 (id int)")
        _ = try await admin.executeRaw(sql: "CREATE TABLE gx_t3 (id int)")
        await expect(true, "SELECT * FROM gx_t3", nil, "default privileges cover tables created later")
        _ = try await admin.executeRaw(sql: "DROP TABLE gx_t3")

        try await users.setAccess(.full, for: user, database: "postgres")
        await expect(true, "CREATE TABLE gx_t2 (id int)")
        _ = try await admin.executeRaw(sql: "DROP TABLE gx_t2")  // role can't be dropped while it owns objects

        try await users.setAccess(.readOnly, for: user, database: "postgres")
        await expect(false, "INSERT INTO gx_t (v) VALUES ('x')", nil, "downgrade must revoke write")

        try await users.changePassword(for: user, to: newPassword)
        await expect(false, "SELECT 1")
        await expect(true, "SELECT 1", newPassword)

        try await users.dropUser(user)  // still holds grants + default privileges
        let remaining = try await users.listUsers()
        XCTAssertNil(remaining.first { $0.name == name })
        _ = try await admin.executeRaw(sql: "DROP TABLE gx_t")
    }

    // MARK: - MySQL / MariaDB

    func test_mysql_userLifecycleAndAccessPresets() async throws {
        try await runMySQLScenario(port: 13320)
    }

    func test_mariadb_userLifecycleAndAccessPresets() async throws {
        try await runMySQLScenario(port: 13321)
    }

    private func runMySQLScenario(port: Int) async throws {
        try skipIfNoServer(port: port)
        let admin = try await connect(.mysql, port: port, user: "root", password: "rootpw", database: nil)
        defer { Task { try? await admin.disconnect() } }
        let users = try XCTUnwrap(admin as? UserManageable)
        let name = "gx test-user"

        if let existing = try await users.listUsers().first(where: { $0.name == name }) {
            try await users.dropUser(existing)
        }
        // `gxXappXdb` matches `gx_app_db` if `_` is left as a GRANT wildcard.
        for sql in [
            "DROP DATABASE IF EXISTS gx_app_db", "DROP DATABASE IF EXISTS gxXappXdb",
            "CREATE DATABASE gx_app_db", "CREATE DATABASE gxXappXdb",
            "CREATE TABLE gx_app_db.gx_t (id int AUTO_INCREMENT PRIMARY KEY, v text)",
            "CREATE TABLE gxXappXdb.gx_t (id int)",
        ] { _ = try await admin.executeRaw(sql: sql) }

        try await users.createUser(name: name, host: "%", password: password)
        let listed = try await users.listUsers()
        let user = try XCTUnwrap(listed.first { $0.name == name })
        XCTAssertEqual(user.host, "%")
        XCTAssertFalse(user.isSuperuser)
        XCTAssertEqual(listed.first { $0.name == "root" }?.isSuperuser, true)

        func expect(_ allowed: Bool, _ sql: String, _ pw: String? = nil, _ message: String = "", line: UInt = #line) async {
            let ok = await canRun(sql, .mysql, port: port, user: name, password: pw ?? password, database: nil)
            XCTAssertEqual(ok, allowed, "\(sql) \(message)", line: line)
        }

        try await users.setAccess(.none, for: user, database: "gx_app_db")
        await expect(false, "SELECT * FROM gx_app_db.gx_t")

        try await users.setAccess(.readOnly, for: user, database: "gx_app_db")
        await expect(true, "SELECT * FROM gx_app_db.gx_t")
        await expect(false, "SELECT * FROM gxXappXdb.gx_t", nil, "`_` must not act as a wildcard")
        await expect(false, "INSERT INTO gx_app_db.gx_t (v) VALUES ('x')")

        try await users.setAccess(.readWrite, for: user, database: "gx_app_db")
        await expect(true, "INSERT INTO gx_app_db.gx_t (v) VALUES ('x')")
        await expect(false, "CREATE TABLE gx_app_db.gx_t2 (id int)")

        try await users.setAccess(.full, for: user, database: "gx_app_db")
        await expect(true, "CREATE TABLE gx_app_db.gx_t2 (id int)")

        try await users.setAccess(.readOnly, for: user, database: "gx_app_db")
        await expect(false, "INSERT INTO gx_app_db.gx_t (v) VALUES ('x')", nil, "downgrade must revoke write")

        try await users.changePassword(for: user, to: newPassword)
        await expect(false, "SELECT 1")
        await expect(true, "SELECT 1", newPassword)

        try await users.dropUser(user)
        let remaining = try await users.listUsers()
        XCTAssertNil(remaining.first { $0.name == name })
        for sql in ["DROP DATABASE gx_app_db", "DROP DATABASE gxXappXdb"] { _ = try await admin.executeRaw(sql: sql) }
    }

    // MARK: - Helpers

    private func connect(_ type: DatabaseType, port: Int, user: String, password: String, database: String?) async throws -> any DatabaseAdapter {
        let config = ConnectionConfig(
            name: "users-test", databaseType: type, host: "127.0.0.1", port: port,
            database: database, username: user, sslEnabled: false,
            // MySQL adapter + .preferred + IP host trips NIOSSL's SNI check; plain TCP is fine locally.
            sslMode: type == .postgresql ? .preferred : .disabled
        )
        let adapter: any DatabaseAdapter = type == .postgresql ? PostgreSQLAdapter() : MySQLAdapter()
        try await adapter.connect(config: config, password: password)
        return adapter
    }

    /// Logs in as `user` and runs `sql`; false if either step fails.
    private func canRun(_ sql: String, _ type: DatabaseType, port: Int, user: String, password: String, database: String?) async -> Bool {
        guard let adapter = try? await connect(type, port: port, user: user, password: password, database: database) else {
            return false
        }
        defer { Task { try? await adapter.disconnect() } }
        return (try? await adapter.executeRaw(sql: sql)) != nil
    }

    private func skipIfNoServer(port: Int) throws {
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
        try XCTSkipIf(rc != 0, "Server not reachable on 127.0.0.1:\(port) — skipping")
    }
}
