// UserManageable.swift
// Gridex
//
// Adapters whose engine has server-level user accounts Gridex can manage.
// Conformed by PostgreSQL and MySQL/MariaDB. Callers check `adapter as? UserManageable`.

import Foundation

protocol UserManageable {
    func listUsers() async throws -> [DatabaseUser]
    /// `host` is the MySQL account host (e.g. "%"); ignored by PostgreSQL.
    func createUser(name: String, host: String?, password: String) async throws
    func changePassword(for user: DatabaseUser, to password: String) async throws
    func dropUser(_ user: DatabaseUser) async throws
    /// Replaces the user's privileges on `database` with the preset (`.none` revokes them).
    /// PostgreSQL can only grant inside the connected database, so pass the current one.
    func setAccess(_ level: DatabaseAccessLevel, for user: DatabaseUser, database: String) async throws
}
