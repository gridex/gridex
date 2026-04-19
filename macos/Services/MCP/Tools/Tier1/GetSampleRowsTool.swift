// GetSampleRowsTool.swift
// Gridex
//
// MCP Tool: Get sample rows from a table.

import Foundation

struct GetSampleRowsTool: MCPTool {
    let name = "get_sample_rows"
    let description = "Get sample rows from a table to help understand data shape. Default limit 10, max 100."
    let tier = MCPPermissionTier.schema

    let inputSchema: [String: Any] = [
        "type": "object",
        "properties": [
            "connection_id": [
                "type": "string",
                "description": "Connection identifier"
            ],
            "table_name": [
                "type": "string",
                "description": "Name of the table"
            ],
            "limit": [
                "type": "integer",
                "description": "Number of rows to return (default 10, max 100)",
                "default": 10,
                "minimum": 1,
                "maximum": 100
            ]
        ],
        "required": ["connection_id", "table_name"]
    ]

    func execute(params: JSONValue, context: MCPToolContext) async throws -> MCPToolResult {
        let connectionId = try extractConnectionId(from: params)

        guard let tableName = params["table_name"]?.stringValue else {
            throw MCPToolError.invalidParameters("table_name is required")
        }

        let limit = min(100, max(1, params["limit"]?.intValue ?? 10))

        let permission = await context.checkPermission(tier: tier, connectionId: connectionId)
        if let error = permission.errorMessage {
            throw MCPToolError.permissionDenied(error)
        }

        let (adapter, _) = try await context.getAdapter(for: connectionId)

        let result = try await adapter.fetchRows(
            table: tableName,
            schema: nil,
            columns: nil,
            where: nil,
            orderBy: nil,
            limit: limit,
            offset: 0
        )

        if result.rows.isEmpty {
            return MCPToolResult(text: "Table '\(tableName)' is empty.")
        }

        // Format as JSON array of objects
        var rows: [[String: String]] = []
        for row in result.rows {
            var rowDict: [String: String] = [:]
            for (idx, col) in result.columns.enumerated() {
                if idx < row.count {
                    rowDict[col.name] = row[idx].displayString
                }
            }
            rows.append(rowDict)
        }

        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        let data = try encoder.encode(rows)
        let json = String(data: data, encoding: .utf8) ?? "[]"

        let header = "Sample \(rows.count) row(s) from '\(tableName)':\n"
        let columns = "Columns: " + result.columns.map { "\($0.name) (\($0.dataType))" }.joined(separator: ", ") + "\n\n"

        return MCPToolResult(text: header + columns + json)
    }
}
