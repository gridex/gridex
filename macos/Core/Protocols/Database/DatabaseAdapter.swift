// DatabaseAdapter.swift
// Gridex
//
// Core protocol that all database adapters must conform to.
// Each supported database (SQLite, PostgreSQL, MySQL) implements this.

import Foundation

protocol DatabaseAdapter: AnyObject, Sendable {
    var databaseType: DatabaseType { get }
    var isConnected: Bool { get }

    // Connection lifecycle
    func connect(config: ConnectionConfig, password: String?) async throws
    func disconnect() async throws
    func testConnection(config: ConnectionConfig, password: String?) async throws -> Bool

    // Query execution
    func execute(query: String, parameters: [QueryParameter]?) async throws -> QueryResult
    func executeRaw(sql: String) async throws -> QueryResult

    /// Execute a parameterized SQL statement with RowValue parameters.
    /// Uses native driver bindings where available. Placeholders are dialect-specific
    /// ($1, $2... for PostgreSQL; ? for MySQL/SQLite).
    func executeWithRowValues(sql: String, parameters: [RowValue]) async throws -> QueryResult

    // Schema inspection
    func listDatabases() async throws -> [String]
    func listSchemas(database: String?) async throws -> [String]
    func listTables(schema: String?) async throws -> [TableInfo]
    func listViews(schema: String?) async throws -> [ViewInfo]
    func describeTable(name: String, schema: String?) async throws -> TableDescription
    func listIndexes(table: String, schema: String?) async throws -> [IndexInfo]
    func listForeignKeys(table: String, schema: String?) async throws -> [ForeignKeyInfo]
    func listFunctions(schema: String?) async throws -> [String]
    func getFunctionSource(name: String, schema: String?) async throws -> String

    /// List stored procedures. Returns empty by default — only MSSQL implements
    /// native stored procedures; PostgreSQL/MySQL blur the line with listFunctions.
    func listProcedures(schema: String?) async throws -> [String]
    /// Get stored procedure source. Returns empty by default.
    func getProcedureSource(name: String, schema: String?) async throws -> String
    /// Get stored procedure parameter signature (name, type, mode).
    /// Returns empty by default. Each item is a formatted string like "@id INT INPUT".
    func listProcedureParameters(name: String, schema: String?) async throws -> [String]

    // Data manipulation
    func insertRow(table: String, schema: String?, values: [String: RowValue]) async throws -> QueryResult
    func updateRow(table: String, schema: String?, set: [String: RowValue], where: [String: RowValue]) async throws -> QueryResult
    func deleteRow(table: String, schema: String?, where: [String: RowValue]) async throws -> QueryResult

    // Transaction support
    func beginTransaction() async throws
    func commitTransaction() async throws
    func rollbackTransaction() async throws

    /// Execute raw statements without rewriting them in a transaction whose
    /// PostgreSQL search path is scoped to `schema` for the transaction only.
    func executeStatements(_ statements: [String], inSchemaTransaction schema: String) async throws

    // Pagination
    func fetchRows(
        table: String,
        schema: String?,
        columns: [String]?,
        where: FilterExpression?,
        orderBy: [QuerySortDescriptor]?,
        limit: Int,
        offset: Int
    ) async throws -> QueryResult

    // Database management
    func createDatabase(name: String) async throws
    func dropDatabase(name: String) async throws

    // Database-specific
    func serverVersion() async throws -> String
    func currentDatabase() async throws -> String?
}

extension DatabaseAdapter {
    func executeStatements(_ statements: [String], inSchemaTransaction schema: String) async throws {
        try await beginTransaction()
        do {
            let quotedSchema = databaseType.sqlDialect.quoteIdentifier(schema)
            _ = try await executeRaw(sql: "SET LOCAL search_path TO \(quotedSchema)")
            for statement in statements {
                _ = try await executeRaw(sql: statement)
            }
            try await commitTransaction()
        } catch {
            try? await rollbackTransaction()
            throw error
        }
    }

    func createDatabase(name: String) async throws {
        let quoted = databaseType.sqlDialect.quoteIdentifier(name)
        _ = try await executeRaw(sql: "CREATE DATABASE \(quoted)")
    }

    func dropDatabase(name: String) async throws {
        let quoted = databaseType.sqlDialect.quoteIdentifier(name)
        _ = try await executeRaw(sql: "DROP DATABASE \(quoted)")
    }

    // Default: no native stored procedures. Override in MSSQL (and later PG if needed).
    func listProcedures(schema: String?) async throws -> [String] { [] }
    func getProcedureSource(name: String, schema: String?) async throws -> String { "" }
    func listProcedureParameters(name: String, schema: String?) async throws -> [String] { [] }

    /// Default implementation: inlines parameters into the SQL string.
    /// Adapters should override this with native parameter binding.
    func executeWithRowValues(sql: String, parameters: [RowValue]) async throws -> QueryResult {
        var result = sql
        // Replace placeholders with inline values, handling both $N and ? styles
        if sql.contains("$1") {
            // PostgreSQL-style: replace $N in reverse order to avoid $1 matching in $10
            for i in stride(from: parameters.count, through: 1, by: -1) {
                result = result.replacingOccurrences(of: "$\(i)", with: inlineRowValue(parameters[i - 1]))
            }
        } else {
            // MySQL/SQLite-style: replace ? one by one
            for param in parameters {
                if let range = result.range(of: "?") {
                    result = result.replacingCharacters(in: range, with: inlineRowValue(param))
                }
            }
        }
        return try await executeRaw(sql: result)
    }

    private func inlineRowValue(_ value: RowValue) -> String {
        switch value {
        case .null: return "NULL"
        case .string(let v): return "'\(v.replacingOccurrences(of: "'", with: "''"))'"
        case .integer(let v): return "\(v)"
        case .double(let v): return "\(v)"
        case .boolean(let v): return v ? "1" : "0"
        case .date(let v):
            let fmt = DateFormatter()
            fmt.dateFormat = "yyyy-MM-dd HH:mm:ss"
            fmt.timeZone = TimeZone(identifier: "UTC")
            return "'\(fmt.string(from: v))'"
        case .uuid(let v): return "'\(v.uuidString)'"
        case .json(let v): return "'\(v.replacingOccurrences(of: "'", with: "''"))'"
        case .data: return "NULL"
        case .array: return "NULL"
        }
    }
}
