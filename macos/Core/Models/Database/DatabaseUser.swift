// DatabaseUser.swift
// Gridex
//
// Server-level user account (PostgreSQL role, MySQL 'user'@'host').

import Foundation

struct DatabaseUser: Sendable, Hashable, Identifiable {
    let name: String
    /// MySQL account host ('%', 'localhost', …). nil for PostgreSQL roles.
    let host: String?
    /// false for PostgreSQL group roles (NOLOGIN).
    let canLogin: Bool
    let isSuperuser: Bool

    var id: String { host.map { "\(name)@\($0)" } ?? name }
}

/// Preset privilege bundles applied to one database.
enum DatabaseAccessLevel: String, CaseIterable, Sendable {
    case none
    case readOnly
    case readWrite
    case full

    var displayName: String {
        switch self {
        case .none: return "No access"
        case .readOnly: return "Read only"
        case .readWrite: return "Read & write"
        case .full: return "Full access"
        }
    }
}
