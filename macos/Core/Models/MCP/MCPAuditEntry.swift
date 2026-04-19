// MCPAuditEntry.swift
// Gridex
//
// Audit log entry for MCP tool invocations.

import Foundation

struct MCPAuditEntry: Codable, Sendable, Identifiable {
    var id: UUID { eventId }
    let timestamp: Date
    let eventId: UUID
    let client: MCPAuditClient
    let tool: String
    let tier: Int
    let connectionId: UUID?
    let connectionType: String?
    let input: MCPAuditInput
    let result: MCPAuditResult
    let security: MCPAuditSecurity
    let error: String?

    init(
        tool: String,
        tier: MCPPermissionTier,
        connectionId: UUID?,
        connectionType: DatabaseType?,
        client: MCPAuditClient,
        input: MCPAuditInput,
        result: MCPAuditResult,
        security: MCPAuditSecurity,
        error: String? = nil
    ) {
        self.timestamp = Date()
        self.eventId = UUID()
        self.client = client
        self.tool = tool
        self.tier = tier.rawValue
        self.connectionId = connectionId
        self.connectionType = connectionType?.rawValue
        self.input = input
        self.result = result
        self.security = security
        self.error = error
    }
}

struct MCPAuditClient: Codable, Sendable {
    let name: String
    let version: String
    let transport: String

    static let unknown = MCPAuditClient(name: "unknown", version: "0.0.0", transport: "stdio")
}

struct MCPAuditInput: Codable, Sendable {
    let sqlPreview: String?
    let paramsCount: Int?
    let inputHash: String?

    init(sql: String? = nil, paramsCount: Int? = nil) {
        if let sql {
            let truncated = sql.count > 200 ? String(sql.prefix(200)) + "..." : sql
            self.sqlPreview = truncated
            self.inputHash = "sha256:\(sql.hashValue)"
        } else {
            self.sqlPreview = nil
            self.inputHash = nil
        }
        self.paramsCount = paramsCount
    }

    static let empty = MCPAuditInput()
}

struct MCPAuditResult: Codable, Sendable {
    let status: String
    let rowsAffected: Int?
    let rowsReturned: Int?
    let durationMs: Int
    let bytesReturned: Int?

    init(status: MCPAuditStatus, rowsAffected: Int? = nil, rowsReturned: Int? = nil, durationMs: Int, bytesReturned: Int? = nil) {
        self.status = status.rawValue
        self.rowsAffected = rowsAffected
        self.rowsReturned = rowsReturned
        self.durationMs = durationMs
        self.bytesReturned = bytesReturned
    }
}

enum MCPAuditStatus: String, Codable, Sendable {
    case success
    case error
    case denied
    case timeout
}

struct MCPAuditSecurity: Codable, Sendable {
    let permissionMode: String
    let userApproved: Bool?
    let approvalSessionId: UUID?
    let scopesApplied: [String]?

    init(mode: MCPConnectionMode, userApproved: Bool? = nil, approvalSessionId: UUID? = nil, scopesApplied: [String]? = nil) {
        self.permissionMode = mode.rawValue
        self.userApproved = userApproved
        self.approvalSessionId = approvalSessionId
        self.scopesApplied = scopesApplied
    }
}
