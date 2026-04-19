// ListSchemasTool.swift
// Gridex
//
// MCP Tool: List schemas/databases available in a connection.

import Foundation

struct ListSchemasTool: MCPTool {
    let name = "list_schemas"
    let description = "List schemas/databases available in a connection."
    let tier = MCPPermissionTier.schema

    let inputSchema: [String: Any] = [
        "type": "object",
        "properties": [
            "connection_id": [
                "type": "string",
                "description": "Connection identifier"
            ]
        ],
        "required": ["connection_id"]
    ]

    func execute(params: JSONValue, context: MCPToolContext) async throws -> MCPToolResult {
        let connectionId = try extractConnectionId(from: params)

        let permission = await context.checkPermission(tier: tier, connectionId: connectionId)
        if let error = permission.errorMessage {
            throw MCPToolError.permissionDenied(error)
        }

        let (adapter, config) = try await context.getAdapter(for: connectionId)

        // For databases that support schemas (PostgreSQL, MSSQL)
        if config.databaseType.supportsSchemas {
            let schemas = try await adapter.listSchemas(database: nil)
            if schemas.isEmpty {
                return MCPToolResult(text: "No schemas found.")
            }
            let json = try JSONEncoder().encode(schemas)
            return MCPToolResult(text: "Schemas: " + (String(data: json, encoding: .utf8) ?? "[]"))
        }

        // For MySQL and others, list databases
        let databases = try await adapter.listDatabases()
        if databases.isEmpty {
            return MCPToolResult(text: "No databases found.")
        }
        let json = try JSONEncoder().encode(databases)
        return MCPToolResult(text: "Databases: " + (String(data: json, encoding: .utf8) ?? "[]"))
    }
}
