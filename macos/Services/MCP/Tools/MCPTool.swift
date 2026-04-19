// MCPTool.swift
// Gridex
//
// Protocol for MCP tools.

import Foundation

protocol MCPTool: Sendable {
    var name: String { get }
    var description: String { get }
    var tier: MCPPermissionTier { get }
    var inputSchema: [String: Any] { get }

    func execute(params: JSONValue, context: MCPToolContext) async throws -> MCPToolResult
}

struct MCPToolContext: Sendable {
    let connectionManager: ConnectionManager
    let permissionEngine: MCPPermissionEngine
    let auditLogger: MCPAuditLogger
    let rateLimiter: MCPRateLimiter
    let approvalGate: MCPApprovalGate
    let client: MCPAuditClient
    let connectionRepository: any ConnectionRepository

    func getAdapter(for connectionId: UUID) async throws -> (any DatabaseAdapter, ConnectionConfig) {
        guard let connection = await connectionManager.activeConnection(for: connectionId) else {
            throw MCPToolError.connectionNotFound(connectionId)
        }
        return (connection.adapter, connection.config)
    }

    func checkPermission(tier: MCPPermissionTier, connectionId: UUID) async -> MCPPermissionResult {
        await permissionEngine.checkPermission(tier: tier, connectionId: connectionId)
    }

    func checkRateLimit(tier: MCPPermissionTier, connectionId: UUID) async throws {
        let result = await rateLimiter.checkLimit(tier: tier, connectionId: connectionId)
        if let retryAfter = result.retryAfterSeconds {
            throw MCPToolError.rateLimitExceeded(retryAfter: retryAfter)
        }
    }

    func recordUsage(tier: MCPPermissionTier, connectionId: UUID) async {
        await rateLimiter.recordUsage(tier: tier, connectionId: connectionId)
    }

    func requestApproval(
        tool: String,
        description: String,
        details: String,
        connectionId: UUID
    ) async -> Bool {
        await approvalGate.requestApproval(
            tool: tool,
            description: description,
            details: details,
            connectionId: connectionId,
            client: client
        )
    }
}

enum MCPToolError: Error, LocalizedError, Sendable {
    case connectionNotFound(UUID)
    case connectionNotConnected(UUID)
    case tableNotFound(String)
    case invalidParameters(String)
    case permissionDenied(String)
    case queryFailed(String)
    case rateLimitExceeded(retryAfter: Int)

    var errorDescription: String? {
        switch self {
        case .connectionNotFound(let id):
            return "Connection '\(id)' not found. Use list_connections to see available connections."
        case .connectionNotConnected(let id):
            return "Connection '\(id)' is not active. The user needs to connect first."
        case .tableNotFound(let name):
            return "Table '\(name)' not found."
        case .invalidParameters(let msg):
            return "Invalid parameters: \(msg)"
        case .permissionDenied(let msg):
            return msg
        case .queryFailed(let msg):
            return "Query failed: \(msg)"
        case .rateLimitExceeded(let retryAfter):
            return "Rate limit exceeded. Retry after \(retryAfter) seconds."
        }
    }
}

extension MCPTool {
    func definition() -> MCPToolDefinition {
        MCPToolDefinition(name: name, description: description, inputSchema: inputSchema)
    }

    func extractConnectionId(from params: JSONValue) throws -> UUID {
        guard let idString = params["connection_id"]?.stringValue else {
            throw MCPToolError.invalidParameters("connection_id is required")
        }
        guard let uuid = UUID(uuidString: idString) else {
            throw MCPToolError.invalidParameters("connection_id must be a valid UUID")
        }
        return uuid
    }
}
