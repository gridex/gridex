// MCPConnectionMode.swift
// Gridex
//
// MCP access mode for database connections.

import Foundation

enum MCPConnectionMode: String, Codable, Sendable, CaseIterable, Identifiable {
    case locked = "locked"
    case readOnly = "read_only"
    case readWrite = "read_write"

    var id: String { rawValue }

    var displayName: String {
        switch self {
        case .locked: return "Locked"
        case .readOnly: return "Read-only"
        case .readWrite: return "Read-write"
        }
    }

    var description: String {
        switch self {
        case .locked:
            return "AI cannot access this connection"
        case .readOnly:
            return "AI can query but not modify (recommended for production)"
        case .readWrite:
            return "AI can modify with your approval (use for dev only)"
        }
    }

    var allowsTier1: Bool { self != .locked }
    var allowsTier2: Bool { self != .locked }
    var allowsTier3: Bool { self == .readWrite }
    var allowsTier4: Bool { self == .readWrite }
    var allowsTier5: Bool { self != .locked }
}
