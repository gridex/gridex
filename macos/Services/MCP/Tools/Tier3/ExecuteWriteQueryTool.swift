// ExecuteWriteQueryTool.swift
// Gridex
//
// MCP Tool: Execute arbitrary write SQL. Requires user approval.

import Foundation

struct ExecuteWriteQueryTool: MCPTool {
    let name = "execute_write_query"
    let description = "Execute one write SQL statement (INSERT/UPDATE/DELETE). Requires user approval. Only available in read-write mode. Multi-statement input is rejected."
    let tier = MCPPermissionTier.write

    let inputSchema: [String: Any] = [
        "type": "object",
        "properties": [
            "connection_id": [
                "type": "string",
                "description": "Connection identifier"
            ],
            "sql": [
                "type": "string",
                "description": "A single SQL statement to execute. Must not contain multiple statements."
            ],
            "params": [
                "type": "array",
                "description": "Parameters for placeholders"
            ]
        ],
        "required": ["connection_id", "sql"]
    ]

    func execute(params: JSONValue, context: MCPToolContext) async throws -> MCPToolResult {
        let connectionId = try extractConnectionId(from: params)

        guard let sql = params["sql"]?.stringValue else {
            throw MCPToolError.invalidParameters("sql is required")
        }

        // Syntactic checks run against a comment/literal-stripped copy so
        // payloads hidden in comments or string literals can't fool them.
        // The ORIGINAL sql is still what gets executed.
        let codeOnly = MCPSQLSanitizer.stripCommentsAndStrings(sql)
        let upperCode = codeOnly.trimmingCharacters(in: .whitespacesAndNewlines).uppercased()

        let withoutTrailingSemi = upperCode.hasSuffix(";")
            ? String(upperCode.dropLast()).trimmingCharacters(in: .whitespacesAndNewlines)
            : upperCode
        if withoutTrailingSemi.contains(";") {
            throw MCPToolError.invalidParameters("Multiple statements are not allowed. Send one statement at a time.")
        }

        let permission = await context.checkPermission(tier: tier, connectionId: connectionId)
        if let error = permission.errorMessage {
            throw MCPToolError.permissionDenied(error)
        }

        if upperCode.hasPrefix("SELECT") || upperCode.hasPrefix("WITH") {
            throw MCPToolError.invalidParameters("Use the 'query' tool for SELECT / WITH statements. This tool is for write operations only.")
        }

        if upperCode.hasPrefix("UPDATE") || upperCode.hasPrefix("DELETE") {
            let range = NSRange(upperCode.startIndex..., in: upperCode)
            if Self.whereRegex.firstMatch(in: upperCode, options: [], range: range) == nil {
                throw MCPToolError.permissionDenied("UPDATE/DELETE without WHERE clause is not allowed.")
            }
            if let whereRange = codeOnly.range(of: "WHERE", options: .caseInsensitive) {
                let whereClause = String(codeOnly[whereRange.upperBound...])
                    .trimmingCharacters(in: .whitespaces)
                if let error = await context.permissionEngine.validateWhereClause(whereClause).errorMessage {
                    throw MCPToolError.permissionDenied(error)
                }
            }
        }

        let (adapter, _) = try await context.getAdapter(for: connectionId)

        if permission.requiresUserApproval {
            let approved = await context.requestApproval(
                tool: name,
                description: "Execute write query",
                details: "SQL:\n\(sql)",
                connectionId: connectionId
            )
            if !approved {
                throw MCPToolError.permissionDenied("User denied the operation.")
            }
        }

        let queryParams = params["params"]?.arrayValue ?? []
        let result: QueryResult
        if queryParams.isEmpty {
            result = try await adapter.executeRaw(sql: sql)
        } else {
            let rowParams = queryParams.map { jsonValueToRowValue($0) }
            result = try await adapter.executeWithRowValues(sql: sql, parameters: rowParams)
        }

        var response = "Query executed successfully."
        if result.rowsAffected > 0 {
            response += " \(result.rowsAffected) row(s) affected."
        }
        return MCPToolResult(text: response)
    }

    private static let whereRegex = try! NSRegularExpression(pattern: "\\bWHERE\\b", options: .caseInsensitive)

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
