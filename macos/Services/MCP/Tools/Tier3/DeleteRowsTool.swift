// DeleteRowsTool.swift
// Gridex
//
// MCP Tool: Delete rows from a table. Requires user approval and WHERE clause.

import Foundation

struct DeleteRowsTool: MCPTool {
    let name = "delete_rows"
    let description = "Delete rows matching WHERE clause. Requires user approval. WHERE clause is MANDATORY."
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
                "description": "Name of the table to delete from (letters, digits, underscore only)"
            ],
            "schema": [
                "type": "string",
                "description": "Optional schema name (letters, digits, underscore only)"
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
        "required": ["connection_id", "table_name", "where"]
    ]

    func execute(params: JSONValue, context: MCPToolContext) async throws -> MCPToolResult {
        let connectionId = try extractConnectionId(from: params)
        let (tableName, schemaName) = try MCPIdentifierValidator.extractTableAndSchema(from: params)

        guard let whereClause = params["where"]?.stringValue else {
            throw MCPToolError.invalidParameters("where clause is required. Bare DELETE without WHERE is not allowed.")
        }

        if let error = await context.permissionEngine.validateWhereClause(whereClause).errorMessage {
            throw MCPToolError.permissionDenied(error)
        }

        let permission = await context.checkPermission(tier: tier, connectionId: connectionId)
        if let error = permission.errorMessage {
            throw MCPToolError.permissionDenied(error)
        }

        let (adapter, config) = try await context.getAdapter(for: connectionId)
        let qualifiedTable = config.databaseType.sqlDialect.qualifiedIdentifier(tableName, schema: schemaName)
        let deleteSQL = "DELETE FROM \(qualifiedTable) WHERE \(whereClause)"

        let estimatedRows = await MCPRowCountEstimator.estimate(
            adapter: adapter,
            qualifiedTable: qualifiedTable,
            whereClause: whereClause,
            config: config
        )

        if permission.requiresUserApproval {
            let details = """
            SQL: \(deleteSQL)

            Estimated rows to delete: \(estimatedRows)

            ⚠️ This operation cannot be undone!
            """

            let approved = await context.requestApproval(
                tool: name,
                description: "Delete rows from '\(tableName)' where \(whereClause)",
                details: details,
                connectionId: connectionId
            )
            if !approved {
                throw MCPToolError.permissionDenied("User denied the operation.")
            }
        }

        let result = try await adapter.executeRaw(sql: deleteSQL)
        return MCPToolResult(text: "Successfully deleted \(result.rowsAffected) row(s) from '\(tableName)'.")
    }
}
