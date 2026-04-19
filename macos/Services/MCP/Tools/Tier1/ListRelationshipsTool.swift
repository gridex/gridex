// ListRelationshipsTool.swift
// Gridex
//
// MCP Tool: List foreign key relationships for a table.

import Foundation

struct ListRelationshipsTool: MCPTool {
    let name = "list_relationships"
    let description = "List foreign key relationships for a table. Returns both incoming and outgoing references."
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
            ]
        ],
        "required": ["connection_id", "table_name"]
    ]

    func execute(params: JSONValue, context: MCPToolContext) async throws -> MCPToolResult {
        let connectionId = try extractConnectionId(from: params)

        guard let tableName = params["table_name"]?.stringValue else {
            throw MCPToolError.invalidParameters("table_name is required")
        }

        let permission = await context.checkPermission(tier: tier, connectionId: connectionId)
        if let error = permission.errorMessage {
            throw MCPToolError.permissionDenied(error)
        }

        let (adapter, _) = try await context.getAdapter(for: connectionId)

        // Get outgoing foreign keys (this table references other tables)
        let outgoing = try await adapter.listForeignKeys(table: tableName, schema: nil)

        // For incoming relationships, we need to scan all tables
        // This is expensive, so we'll do it if possible
        var incoming: [ForeignKeyInfo] = []
        let allTables = try await adapter.listTables(schema: nil)

        for table in allTables where table.name != tableName {
            let fks = try await adapter.listForeignKeys(table: table.name, schema: nil)
            for fk in fks where fk.referencedTable == tableName {
                incoming.append(ForeignKeyInfo(
                    name: fk.name,
                    columns: fk.columns,
                    referencedTable: table.name, // The table that references us
                    referencedColumns: fk.referencedColumns,
                    onDelete: fk.onDelete,
                    onUpdate: fk.onUpdate
                ))
            }
        }

        var result: [String: Any] = [
            "table": tableName
        ]

        // Outgoing relationships
        if !outgoing.isEmpty {
            var outList: [[String: Any]] = []
            for fk in outgoing {
                outList.append([
                    "name": fk.name ?? "",
                    "columns": fk.columns,
                    "references_table": fk.referencedTable,
                    "references_columns": fk.referencedColumns
                ])
            }
            result["outgoing"] = outList
        } else {
            result["outgoing"] = "None"
        }

        // Incoming relationships
        if !incoming.isEmpty {
            var inList: [[String: Any]] = []
            for fk in incoming {
                inList.append([
                    "from_table": fk.referencedTable,
                    "from_columns": fk.columns,
                    "to_columns": fk.referencedColumns
                ])
            }
            result["incoming"] = inList
        } else {
            result["incoming"] = "None"
        }

        let data = try JSONSerialization.data(withJSONObject: result, options: [.prettyPrinted, .sortedKeys])
        let json = String(data: data, encoding: .utf8) ?? "{}"

        return MCPToolResult(text: json)
    }
}
