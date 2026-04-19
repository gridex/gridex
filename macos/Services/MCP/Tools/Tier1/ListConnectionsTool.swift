// ListConnectionsTool.swift
// Gridex
//
// MCP Tool: List all configured database connections.

import Foundation

struct ListConnectionsTool: MCPTool {
    let name = "list_connections"
    let description = "List all configured database connections. Returns connection IDs, names, and types (postgres/mysql/sqlite/redis/mongodb/mssql)."
    let tier = MCPPermissionTier.schema

    let inputSchema: [String: Any] = [
        "type": "object",
        "properties": [:] as [String: Any]
    ]

    func execute(params: JSONValue, context: MCPToolContext) async throws -> MCPToolResult {
        let configs = try await context.connectionRepository.fetchAll()
        let activeConnections = await context.connectionManager.allActiveConnections

        var connections: [[String: Any]] = []
        for config in configs {
            let mode = await context.permissionEngine.getMode(for: config.id)

            // Skip locked connections from MCP view
            if mode == .locked { continue }

            let isConnected = activeConnections.contains { $0.id == config.id }

            connections.append([
                "id": config.id.uuidString,
                "name": config.name,
                "type": config.databaseType.rawValue,
                "host": config.displayHost,
                "database": config.database ?? "",
                "is_connected": isConnected,
                "mcp_mode": mode.rawValue
            ])
        }

        if connections.isEmpty {
            return MCPToolResult(text: "No connections available for MCP access. All connections are either locked or none exist.")
        }

        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        let data = try encoder.encode(connections.map { dict -> [String: String] in
            dict.mapValues { "\($0)" }
        })
        let json = String(data: data, encoding: .utf8) ?? "[]"

        return MCPToolResult(text: json)
    }
}
