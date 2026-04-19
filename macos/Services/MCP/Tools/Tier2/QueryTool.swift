// QueryTool.swift
// Gridex
//
// MCP Tool: Execute a SQL query with read-only enforcement.

import Foundation

struct QueryTool: MCPTool {
    let name = "query"
    let description = "Execute a SQL query. In read-only mode, only SELECT statements are allowed. Returns rows with metadata (column types, row count, execution time)."
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
                "description": "SQL query. Use parameterized placeholders."
            ],
            "params": [
                "type": "array",
                "description": "Parameters for placeholders"
            ],
            "row_limit": [
                "type": "integer",
                "description": "Maximum rows to return (default 1000, max 10000)",
                "default": 1000,
                "maximum": 10000
            ]
        ],
        "required": ["connection_id", "sql"]
    ]

    func execute(params: JSONValue, context: MCPToolContext) async throws -> MCPToolResult {
        let connectionId = try extractConnectionId(from: params)

        guard let sql = params["sql"]?.stringValue else {
            throw MCPToolError.invalidParameters("sql is required")
        }

        let rowLimit = min(10000, max(1, params["row_limit"]?.intValue ?? 1000))

        // Check basic permission
        let permission = await context.checkPermission(tier: tier, connectionId: connectionId)
        if let error = permission.errorMessage {
            throw MCPToolError.permissionDenied(error)
        }

        // Check connection mode for read-only enforcement
        let mode = await context.permissionEngine.getMode(for: connectionId)
        if mode == .readOnly {
            let readOnlyCheck = await context.permissionEngine.validateReadOnlyQuery(sql)
            if let error = readOnlyCheck.errorMessage {
                throw MCPToolError.permissionDenied(error)
            }
        }

        let (adapter, config) = try await context.getAdapter(for: connectionId)

        // Apply row limit if not already present
        var limitedSQL = sql
        let upperSQL = sql.uppercased()
        if !upperSQL.contains("LIMIT") && config.databaseType.isSQL {
            limitedSQL = "\(sql.trimmingCharacters(in: .whitespacesAndNewlines).trimmingCharacters(in: CharacterSet(charactersIn: ";"))) LIMIT \(rowLimit)"
        }

        // Extract parameters
        var queryParams: [RowValue] = []
        if let paramsArray = params["params"]?.arrayValue {
            for p in paramsArray {
                queryParams.append(jsonValueToRowValue(p))
            }
        }

        let startTime = Date()
        let result: QueryResult

        if queryParams.isEmpty {
            result = try await adapter.executeRaw(sql: limitedSQL)
        } else {
            result = try await adapter.executeWithRowValues(sql: limitedSQL, parameters: queryParams)
        }

        let durationMs = Int(Date().timeIntervalSince(startTime) * 1000)

        // Format response
        var response: [String: Any] = [
            "success": true,
            "row_count": result.rowCount,
            "execution_time_ms": durationMs,
            "query_type": result.queryType.rawValue
        ]

        if result.rowsAffected > 0 {
            response["rows_affected"] = result.rowsAffected
        }

        // Include columns metadata
        response["columns"] = result.columns.map { col -> [String: Any] in
            var c: [String: Any] = [
                "name": col.name,
                "type": col.dataType
            ]
            if col.isNullable { c["nullable"] = true }
            return c
        }

        // Include rows (limited)
        var rows: [[String: String]] = []
        for row in result.rows.prefix(rowLimit) {
            var rowDict: [String: String] = [:]
            for (idx, col) in result.columns.enumerated() {
                if idx < row.count {
                    rowDict[col.name] = row[idx].displayString
                }
            }
            rows.append(rowDict)
        }
        response["rows"] = rows

        if result.rowCount > rowLimit {
            response["truncated"] = true
            response["message"] = "Results truncated to \(rowLimit) rows"
        }

        let data = try JSONSerialization.data(withJSONObject: response, options: [.prettyPrinted, .sortedKeys])
        let json = String(data: data, encoding: .utf8) ?? "{}"

        return MCPToolResult(text: json)
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
