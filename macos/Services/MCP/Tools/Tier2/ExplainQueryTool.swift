// ExplainQueryTool.swift
// Gridex
//
// MCP Tool: Get EXPLAIN plan for a query without executing it.

import Foundation

struct ExplainQueryTool: MCPTool {
    let name = "explain_query"
    let description = "Get EXPLAIN plan for a query without executing it. Helps AI understand query performance."
    let tier = MCPPermissionTier.read

    let inputSchema: [String: Any] = [
        "type": "object",
        "properties": [
            "connection_id": [
                "type": "string",
                "description": "Connection identifier"
            ],
            "sql": [
                "type": "string",
                "description": "SQL query to explain"
            ]
        ],
        "required": ["connection_id", "sql"]
    ]

    func execute(params: JSONValue, context: MCPToolContext) async throws -> MCPToolResult {
        let connectionId = try extractConnectionId(from: params)

        guard let sql = params["sql"]?.stringValue else {
            throw MCPToolError.invalidParameters("sql is required")
        }

        let permission = await context.checkPermission(tier: tier, connectionId: connectionId)
        if let error = permission.errorMessage {
            throw MCPToolError.permissionDenied(error)
        }

        let (adapter, config) = try await context.getAdapter(for: connectionId)

        // Build EXPLAIN query based on database type
        let explainSQL: String
        switch config.databaseType {
        case .postgresql:
            explainSQL = "EXPLAIN (ANALYZE false, COSTS true, FORMAT TEXT) \(sql)"
        case .mysql:
            explainSQL = "EXPLAIN \(sql)"
        case .sqlite:
            explainSQL = "EXPLAIN QUERY PLAN \(sql)"
        case .mssql:
            // SQL Server uses SET SHOWPLAN_TEXT or estimated plan
            explainSQL = "SET SHOWPLAN_TEXT ON; \(sql); SET SHOWPLAN_TEXT OFF"
        case .mongodb, .redis:
            return MCPToolResult(text: "EXPLAIN is not supported for \(config.databaseType.displayName) connections.", isError: true)
        }

        let result = try await adapter.executeRaw(sql: explainSQL)

        // Format the explain output
        var output = "Query Plan for: \(sql)\n\n"

        if result.rows.isEmpty {
            output += "No plan information available."
        } else {
            for row in result.rows {
                let line = row.map(\.displayString).joined(separator: " | ")
                output += line + "\n"
            }
        }

        return MCPToolResult(text: output)
    }
}
