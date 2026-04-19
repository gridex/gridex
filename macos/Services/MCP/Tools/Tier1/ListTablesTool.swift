// ListTablesTool.swift
// Gridex
//
// MCP Tool: List all tables in a database connection.

import Foundation

struct ListTablesTool: MCPTool {
    let name = "list_tables"
    let description = "List all tables in a database connection. Returns table names, schemas, and approximate row counts."
    let tier = MCPPermissionTier.schema

    let inputSchema: [String: Any] = [
        "type": "object",
        "properties": [
            "connection_id": [
                "type": "string",
                "description": "Connection identifier"
            ],
            "schema": [
                "type": "string",
                "description": "Optional schema/database filter"
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

        let (adapter, _) = try await context.getAdapter(for: connectionId)
        let schemaFilter = params["schema"]?.stringValue

        let tables = try await adapter.listTables(schema: schemaFilter)

        var result: [[String: Any]] = []
        for table in tables {
            var entry: [String: Any] = [
                "name": table.name,
                "type": table.type.rawValue
            ]
            if let schema = table.schema {
                entry["schema"] = schema
            }
            if let rowCount = table.estimatedRowCount {
                entry["estimated_rows"] = rowCount
            }
            result.append(entry)
        }

        if result.isEmpty {
            return MCPToolResult(text: "No tables found" + (schemaFilter.map { " in schema '\($0)'" } ?? "") + ".")
        }

        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        let data = try encoder.encode(result.map { dict -> [String: String] in
            dict.mapValues { "\($0)" }
        })
        let json = String(data: data, encoding: .utf8) ?? "[]"

        return MCPToolResult(text: "Found \(result.count) table(s):\n\(json)")
    }
}
