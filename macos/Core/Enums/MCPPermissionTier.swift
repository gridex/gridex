// MCPPermissionTier.swift
// Gridex
//
// Permission tiers for MCP tools.

import Foundation

enum MCPPermissionTier: Int, Codable, Sendable, Comparable {
    case schema = 1      // Tier 1: Schema introspection (read-only, no approval)
    case read = 2        // Tier 2: Query execution (read, no approval by default)
    case write = 3       // Tier 3: Data modification (write, REQUIRES approval)
    case ddl = 4         // Tier 4: DDL (schema change, CRITICAL approval)
    case advanced = 5    // Tier 5: Advanced features

    static func < (lhs: MCPPermissionTier, rhs: MCPPermissionTier) -> Bool {
        lhs.rawValue < rhs.rawValue
    }

    var displayName: String {
        switch self {
        case .schema: return "Schema"
        case .read: return "Read"
        case .write: return "Write"
        case .ddl: return "DDL"
        case .advanced: return "Advanced"
        }
    }

    var requiresApproval: Bool {
        switch self {
        case .schema, .read: return false
        case .write, .ddl, .advanced: return true
        }
    }

    var isReadOnly: Bool {
        self == .schema || self == .read
    }
}
