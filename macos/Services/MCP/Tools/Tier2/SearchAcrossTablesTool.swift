// SearchAcrossTablesTool.swift
// Gridex
//
// MCP Tool: Search for a keyword across table names, column names, and column comments.

import Foundation

struct SearchAcrossTablesTool: MCPTool {
    let name = "search_across_tables"
    let description = "Search for a keyword across table names, column names, and column comments. Useful for discovering relevant data."
    let tier = MCPPermissionTier.read

    let inputSchema: [String: Any] = [
        "type": "object",
        "properties": [
            "connection_id": [
                "type": "string",
                "description": "Connection identifier"
            ],
            "keyword": [
                "type": "string",
                "description": "Keyword to search for"
            ]
        ],
        "required": ["connection_id", "keyword"]
    ]

    func execute(params: JSONValue, context: MCPToolContext) async throws -> MCPToolResult {
        let connectionId = try extractConnectionId(from: params)

        guard let keyword = params["keyword"]?.stringValue else {
            throw MCPToolError.invalidParameters("keyword is required")
        }

        let permission = await context.checkPermission(tier: tier, connectionId: connectionId)
        if let error = permission.errorMessage {
            throw MCPToolError.permissionDenied(error)
        }

        let (adapter, _) = try await context.getAdapter(for: connectionId)

        let searchTerm = keyword.lowercased()
        var matches: [[String: Any]] = []

        let tables = try await adapter.listTables(schema: nil)

        for table in tables {
            // Check table name
            if table.name.lowercased().contains(searchTerm) {
                matches.append([
                    "type": "table",
                    "table": table.name,
                    "match": table.name
                ])
            }

            // Check columns
            let tableDesc = try await adapter.describeTable(name: table.name, schema: nil)
            for column in tableDesc.columns {
                if column.name.lowercased().contains(searchTerm) {
                    matches.append([
                        "type": "column",
                        "table": table.name,
                        "column": column.name,
                        "data_type": column.dataType
                    ])
                }

                // Check column comment if available
                if let comment = column.comment, comment.lowercased().contains(searchTerm) {
                    matches.append([
                        "type": "column_comment",
                        "table": table.name,
                        "column": column.name,
                        "comment": comment
                    ])
                }
            }

            // Check table comment if available
            if let comment = tableDesc.comment, comment.lowercased().contains(searchTerm) {
                matches.append([
                    "type": "table_comment",
                    "table": table.name,
                    "comment": comment
                ])
            }
        }

        if matches.isEmpty {
            return MCPToolResult(text: "No matches found for '\(keyword)'.")
        }

        let data = try JSONSerialization.data(withJSONObject: matches, options: [.prettyPrinted, .sortedKeys])
        let json = String(data: data, encoding: .utf8) ?? "[]"

        return MCPToolResult(text: "Found \(matches.count) match(es) for '\(keyword)':\n\(json)")
    }
}
