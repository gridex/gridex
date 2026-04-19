// DescribeTableTool.swift
// Gridex
//
// MCP Tool: Get detailed structure of a table.

import Foundation

struct DescribeTableTool: MCPTool {
    let name = "describe_table"
    let description = "Get detailed structure of a table including columns, data types, indexes, primary keys, foreign keys, and constraints."
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
                "description": "Name of the table to describe"
            ],
            "schema": [
                "type": "string",
                "description": "Optional schema name"
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

        let (adapter, config) = try await context.getAdapter(for: connectionId)
        let schemaName = params["schema"]?.stringValue

        let tableDesc = try await adapter.describeTable(name: tableName, schema: schemaName)
        let indexes = try await adapter.listIndexes(table: tableName, schema: schemaName)
        let foreignKeys = try await adapter.listForeignKeys(table: tableName, schema: schemaName)

        var result: [String: Any] = [
            "name": tableDesc.name,
            "database_type": config.databaseType.displayName
        ]

        if let schema = tableDesc.schema {
            result["schema"] = schema
        }

        if let comment = tableDesc.comment {
            result["comment"] = comment
        }

        if let rowCount = tableDesc.estimatedRowCount {
            result["estimated_rows"] = rowCount
        }

        // Columns
        var columns: [[String: Any]] = []
        for col in tableDesc.columns {
            var colInfo: [String: Any] = [
                "name": col.name,
                "type": col.dataType,
                "nullable": col.isNullable
            ]
            if col.isPrimaryKey { colInfo["primary_key"] = true }
            if let defaultVal = col.defaultValue { colInfo["default"] = defaultVal }
            if let comment = col.comment { colInfo["comment"] = comment }
            columns.append(colInfo)
        }
        result["columns"] = columns

        // Primary key
        let pkColumns = tableDesc.columns.filter(\.isPrimaryKey).map(\.name)
        if !pkColumns.isEmpty {
            result["primary_key"] = pkColumns
        }

        // Indexes
        if !indexes.isEmpty {
            var indexList: [[String: Any]] = []
            for idx in indexes {
                indexList.append([
                    "name": idx.name,
                    "columns": idx.columns,
                    "unique": idx.isUnique,
                    "type": idx.type ?? "btree"
                ])
            }
            result["indexes"] = indexList
        }

        // Foreign keys
        if !foreignKeys.isEmpty {
            var fkList: [[String: Any]] = []
            for fk in foreignKeys {
                fkList.append([
                    "name": fk.name ?? "",
                    "columns": fk.columns,
                    "references_table": fk.referencedTable,
                    "references_columns": fk.referencedColumns
                ])
            }
            result["foreign_keys"] = fkList
        }

        // Constraints
        if !tableDesc.constraints.isEmpty {
            var constraintList: [[String: Any]] = []
            for c in tableDesc.constraints {
                constraintList.append([
                    "name": c.name,
                    "type": c.type,
                    "definition": c.definition ?? ""
                ])
            }
            result["constraints"] = constraintList
        }

        // Generate DDL
        let ddl = tableDesc.toDDL(dialect: config.databaseType.sqlDialect)
        result["ddl"] = ddl

        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        let data = try JSONSerialization.data(withJSONObject: result, options: [.prettyPrinted, .sortedKeys])
        let json = String(data: data, encoding: .utf8) ?? "{}"

        return MCPToolResult(text: json)
    }
}
