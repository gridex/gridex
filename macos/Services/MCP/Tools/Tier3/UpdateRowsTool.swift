// UpdateRowsTool.swift
// Gridex
//
// MCP Tool: Update rows in a table. Requires user approval and WHERE clause.

import Foundation

struct UpdateRowsTool: MCPTool {
    let name = "update_rows"
    let description = "Update rows matching WHERE clause. Requires user approval. WHERE clause is MANDATORY (no bare UPDATE)."
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
                "description": "Name of the table to update (letters, digits, underscore only)"
            ],
            "schema": [
                "type": "string",
                "description": "Optional schema name (letters, digits, underscore only)"
            ],
            "set": [
                "type": "object",
                "description": "Column-value pairs to update. Column names must be letters, digits, underscore only."
            ],
            "where": [
                "type": "string",
                "description": "WHERE clause (required). Must not contain ';', '--', '/*', or '*/'."
            ],
            "where_params": [
                "type": "array",
                "description": "Parameters for WHERE clause placeholders"
            ]
        ],
        "required": ["connection_id", "table_name", "set", "where"]
    ]

    func execute(params: JSONValue, context: MCPToolContext) async throws -> MCPToolResult {
        let connectionId = try extractConnectionId(from: params)
        let (tableName, schemaName) = try MCPIdentifierValidator.extractTableAndSchema(from: params)

        guard let setObj = params["set"]?.objectValue, !setObj.isEmpty else {
            throw MCPToolError.invalidParameters("set must be a non-empty object with column-value pairs")
        }
        for key in setObj.keys {
            try MCPIdentifierValidator.validate(key, as: "column name")
        }

        guard let whereClause = params["where"]?.stringValue else {
            throw MCPToolError.invalidParameters("where clause is required. Bare UPDATE without WHERE is not allowed.")
        }

        if let error = await context.permissionEngine.validateWhereClause(whereClause).errorMessage {
            throw MCPToolError.permissionDenied(error)
        }

        let permission = await context.checkPermission(tier: tier, connectionId: connectionId)
        if let error = permission.errorMessage {
            throw MCPToolError.permissionDenied(error)
        }

        let (adapter, config) = try await context.getAdapter(for: connectionId)
        let dialect = config.databaseType.sqlDialect
        let qualifiedTable = dialect.qualifiedIdentifier(tableName, schema: schemaName)

        // Values are still inlined here because executeRaw takes a single string.
        // A future refactor should bind parameters via executeWithRowValues.
        let setClause = setObj.map { key, val in
            "\(dialect.quoteIdentifier(key)) = \(formatValueForSQL(val, dialect: dialect))"
        }.joined(separator: ", ")

        let updateSQL = "UPDATE \(qualifiedTable) SET \(setClause) WHERE \(whereClause)"

        let estimatedRows = await MCPRowCountEstimator.estimate(
            adapter: adapter,
            qualifiedTable: qualifiedTable,
            whereClause: whereClause,
            config: config
        )

        if permission.requiresUserApproval {
            let details = """
            SQL: \(updateSQL)

            Estimated rows affected: \(estimatedRows)
            """

            let approved = await context.requestApproval(
                tool: name,
                description: "Update rows in '\(tableName)' where \(whereClause)",
                details: details,
                connectionId: connectionId
            )
            if !approved {
                throw MCPToolError.permissionDenied("User denied the operation.")
            }
        }

        let result = try await adapter.executeRaw(sql: updateSQL)
        return MCPToolResult(text: "Successfully updated \(result.rowsAffected) row(s) in '\(tableName)'.")
    }

    private func formatValueForSQL(_ value: JSONValue, dialect: SQLDialect) -> String {
        switch value {
        case .null: return "NULL"
        case .bool(let b):
            return dialect == .postgresql ? (b ? "true" : "false") : (b ? "1" : "0")
        case .int(let i): return "\(i)"
        case .double(let d): return "\(d)"
        case .string(let s):
            let escaped = s.replacingOccurrences(of: "'", with: "''")
            return "'\(escaped)'"
        case .array, .object:
            let encoder = JSONEncoder()
            if let data = try? encoder.encode(value),
               let str = String(data: data, encoding: .utf8) {
                let escaped = str.replacingOccurrences(of: "'", with: "''")
                return "'\(escaped)'"
            }
            return "NULL"
        }
    }
}
