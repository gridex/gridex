// InsertRowsTool.swift
// Gridex
//
// MCP Tool: Insert rows into a table. Requires user approval.

import Foundation

struct InsertRowsTool: MCPTool {
    let name = "insert_rows"
    let description = "Insert one or more rows into a table. Requires user approval. Returns affected count."
    let tier = MCPPermissionTier.write

    let inputSchema: [String: Any] = [
        "type": "object",
        "properties": [
            "connection_id": [
                "type": "string",
                "description": "Connection identifier"
            ],
            "table_name": [
                "type": "string",
                "description": "Name of the table to insert into"
            ],
            "schema": [
                "type": "string",
                "description": "Optional schema name"
            ],
            "rows": [
                "type": "array",
                "description": "Array of row objects to insert",
                "items": ["type": "object"]
            ]
        ],
        "required": ["connection_id", "table_name", "rows"]
    ]

    func execute(params: JSONValue, context: MCPToolContext) async throws -> MCPToolResult {
        let connectionId = try extractConnectionId(from: params)

        guard let tableName = params["table_name"]?.stringValue else {
            throw MCPToolError.invalidParameters("table_name is required")
        }

        guard let rowsArray = params["rows"]?.arrayValue, !rowsArray.isEmpty else {
            throw MCPToolError.invalidParameters("rows must be a non-empty array")
        }

        // Check permission
        let permission = await context.checkPermission(tier: tier, connectionId: connectionId)
        if let error = permission.errorMessage {
            throw MCPToolError.permissionDenied(error)
        }

        // Requires approval
        if permission.requiresUserApproval {
            let approved = await context.requestApproval(
                tool: name,
                description: "Insert \(rowsArray.count) row(s) into '\(tableName)'",
                details: formatRowsPreview(rowsArray),
                connectionId: connectionId
            )
            if !approved {
                throw MCPToolError.permissionDenied("User denied the operation.")
            }
        }

        let (adapter, _) = try await context.getAdapter(for: connectionId)
        let schemaName = params["schema"]?.stringValue

        var insertedCount = 0

        for rowValue in rowsArray {
            guard let rowObj = rowValue.objectValue else { continue }

            var values: [String: RowValue] = [:]
            for (key, val) in rowObj {
                values[key] = jsonValueToRowValue(val)
            }

            _ = try await adapter.insertRow(table: tableName, schema: schemaName, values: values)
            insertedCount += 1
        }

        return MCPToolResult(text: "Successfully inserted \(insertedCount) row(s) into '\(tableName)'.")
    }

    private func formatRowsPreview(_ rows: [JSONValue]) -> String {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        let previewRows = Array(rows.prefix(5))
        if let data = try? encoder.encode(previewRows),
           let str = String(data: data, encoding: .utf8) {
            if rows.count > 5 {
                return str + "\n... and \(rows.count - 5) more rows"
            }
            return str
        }
        return "\(rows.count) rows"
    }

    private func jsonValueToRowValue(_ value: JSONValue) -> RowValue {
        switch value {
        case .null: return .null
        case .bool(let b): return .boolean(b)
        case .int(let i): return .integer(Int64(i))
        case .double(let d): return .double(d)
        case .string(let s): return .string(s)
        case .array, .object:
            let encoder = JSONEncoder()
            if let data = try? encoder.encode(value),
               let str = String(data: data, encoding: .utf8) {
                return .json(str)
            }
            return .null
        }
    }
}
