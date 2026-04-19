// PostgreSQLAdapter.swift
// Gridex
//
// PostgreSQL database adapter using PostgresNIO.

import Foundation
import PostgresNIO
import NIOCore
import NIOPosix
import NIOSSL

final class PostgreSQLAdapter: DatabaseAdapter, SchemaInspectable, @unchecked Sendable {
    let databaseType: DatabaseType = .postgresql
    private(set) var isConnected: Bool = false
    private var client: PostgresClient?
    private var clientTask: Task<Void, Never>?
    private let eventLoopGroup = MultiThreadedEventLoopGroup(numberOfThreads: 2)

    deinit {
        clientTask?.cancel()
    }

    // MARK: - Connection

    func connect(config: ConnectionConfig, password: String?) async throws {
        let host = config.host ?? "localhost"
        let port = config.port ?? 5432
        let username = config.username ?? "postgres"
        let database = config.database ?? username

        let tlsConfig: PostgresClient.Configuration.TLS
        if config.sslEnabled {
            var tls = TLSConfiguration.makeClientConfiguration()

            // Load client certificate and key for mTLS (e.g., Teleport)
            if let certPath = config.sslCertPath, !certPath.isEmpty,
               let keyPath = config.sslKeyPath, !keyPath.isEmpty {
                let cert = try NIOSSLCertificate.fromPEMFile(certPath)
                let key = try NIOSSLPrivateKey(file: keyPath, format: .pem)
                tls.certificateChain = cert.map { .certificate($0) }
                tls.privateKey = .privateKey(key)
            }

            // Load CA certificate for server verification
            if let caPath = config.sslCACertPath, !caPath.isEmpty {
                tls.trustRoots = .file(caPath)
                tls.certificateVerification = .fullVerification
            } else {
                tls.certificateVerification = .none
            }

            tlsConfig = .prefer(tls)
        } else {
            tlsConfig = .disable
        }

        let pgConfig = PostgresClient.Configuration(
            host: host,
            port: port,
            username: username,
            password: password,
            database: database,
            tls: tlsConfig
        )

        let newClient = PostgresClient(configuration: pgConfig)
        self.client = newClient

        // PostgresClient requires run() in a background task
        self.clientTask = Task { await newClient.run() }

        // Verify the connection works
        _ = try await newClient.query("SELECT 1")
        isConnected = true
    }

    func disconnect() async throws {
        clientTask?.cancel()
        clientTask = nil
        client = nil
        isConnected = false
    }

    func testConnection(config: ConnectionConfig, password: String?) async throws -> Bool {
        let adapter = PostgreSQLAdapter()
        do {
            try await adapter.connect(config: config, password: password)
            try await adapter.disconnect()
            return true
        } catch {
            try? await adapter.disconnect()
            throw error
        }
    }

    // MARK: - Query Execution

    func execute(query: String, parameters: [QueryParameter]?) async throws -> QueryResult {
        try ensureConnected()
        let startTime = CFAbsoluteTimeGetCurrent()
        let upper = query.trimmingCharacters(in: .whitespacesAndNewlines).uppercased()
        let queryType = detectQueryType(upper)
        let isSelect = queryType == .select || upper.hasPrefix("SHOW") || upper.hasPrefix("EXPLAIN")

        guard let client else { throw GridexError.queryExecutionFailed("No connection") }

        do {
            let pgQuery = PostgresQuery(unsafeSQL: query)
            let rows = try await client.query(pgQuery)
            let columns = rows.columns.map { col in
                ColumnHeader(name: col.name, dataType: col.dataType.description)
            }

            if isSelect {
                var resultRows: [[RowValue]] = []
                for try await row in rows {
                    var rowValues: [RowValue] = []
                    for (cell, meta) in zip(row, rows.columns) {
                        rowValues.append(decodeCell(cell, dataType: meta.dataType))
                    }
                    resultRows.append(rowValues)
                }
                let duration = CFAbsoluteTimeGetCurrent() - startTime
                return QueryResult(columns: columns, rows: resultRows, rowsAffected: resultRows.count, executionTime: duration, queryType: queryType)
            } else {
                var count = 0
                for try await _ in rows { count += 1 }
                let duration = CFAbsoluteTimeGetCurrent() - startTime
                return QueryResult(columns: columns, rows: [], rowsAffected: count, executionTime: duration, queryType: queryType)
            }
        } catch {
            throw GridexError.queryExecutionFailed(Self.formatPostgresError(error))
        }
    }

    /// Extract a human-readable message from a PSQLError (which doesn't implement LocalizedError well).
    static func formatPostgresError(_ error: Error) -> String {
        if let pgError = error as? PSQLError {
            var parts: [String] = []
            if let msg = pgError.serverInfo?[.message] {
                parts.append(msg)
            }
            if let detail = pgError.serverInfo?[.detail] {
                parts.append("Detail: \(detail)")
            }
            if let hint = pgError.serverInfo?[.hint] {
                parts.append("Hint: \(hint)")
            }
            if let position = pgError.serverInfo?[.position] {
                parts.append("Position: \(position)")
            }
            if let sqlState = pgError.serverInfo?[.sqlState] {
                parts.append("SQLSTATE: \(sqlState)")
            }
            if !parts.isEmpty {
                return parts.joined(separator: "\n")
            }
            return String(describing: pgError)
        }
        return error.localizedDescription
    }

    func executeRaw(sql: String) async throws -> QueryResult {
        try await execute(query: sql, parameters: nil)
    }

    func executeWithRowValues(sql: String, parameters: [RowValue]) async throws -> QueryResult {
        try ensureConnected()
        guard let client else { throw GridexError.queryExecutionFailed("No connection") }

        let startTime = CFAbsoluteTimeGetCurrent()
        let upper = sql.trimmingCharacters(in: .whitespacesAndNewlines).uppercased()
        let queryType = detectQueryType(upper)

        var bindings = PostgresBindings(capacity: parameters.count)
        for param in parameters {
            appendBinding(param, to: &bindings)
        }

        let pgQuery = PostgresQuery(unsafeSQL: sql, binds: bindings)
        let rows = try await client.query(pgQuery)
        let columns = rows.columns.map { col in
            ColumnHeader(name: col.name, dataType: col.dataType.description)
        }

        var resultRows: [[RowValue]] = []
        for try await row in rows {
            var rowValues: [RowValue] = []
            for (cell, meta) in zip(row, rows.columns) {
                rowValues.append(decodeCell(cell, dataType: meta.dataType))
            }
            resultRows.append(rowValues)
        }

        let duration = CFAbsoluteTimeGetCurrent() - startTime
        return QueryResult(columns: columns, rows: resultRows, rowsAffected: resultRows.count, executionTime: duration, queryType: queryType)
    }

    private func appendBinding(_ value: RowValue, to bindings: inout PostgresBindings) {
        switch value {
        case .null:
            bindings.appendNull()
        case .string(let v):
            bindings.append(v)
        case .integer(let v):
            bindings.append(v)
        case .double(let v):
            bindings.append(v)
        case .boolean(let v):
            bindings.append(v)
        case .date(let v):
            bindings.append(v)
        case .uuid(let v):
            bindings.append(v)
        case .json(let v):
            bindings.append(v)
        case .data(let v):
            var buf = ByteBuffer()
            buf.writeBytes(v)
            bindings.append(buf)
        case .array(let v):
            bindings.append(v.map(\.description).joined(separator: ","))
        }
    }

    // MARK: - Schema Inspection

    func listDatabases() async throws -> [String] {
        let result = try await executeRaw(sql: "SELECT datname FROM pg_database WHERE datistemplate = false ORDER BY datname")
        return result.rows.compactMap { $0.first?.stringValue }
    }

    func listSchemas(database: String?) async throws -> [String] {
        let result = try await executeRaw(sql: """
            SELECT schema_name FROM information_schema.schemata
            WHERE schema_name NOT IN ('pg_catalog', 'information_schema', 'pg_toast')
            ORDER BY schema_name
            """)
        return result.rows.compactMap { $0.first?.stringValue }
    }

    func listTables(schema: String?) async throws -> [TableInfo] {
        let schemaFilter = schema ?? "public"
        let result = try await executeParameterized(sql: """
            SELECT t.table_name,
                   (SELECT reltuples::bigint FROM pg_class c
                    JOIN pg_namespace n ON n.oid = c.relnamespace
                    WHERE c.relname = t.table_name AND n.nspname = $1)
            FROM information_schema.tables t
            WHERE t.table_schema = $1 AND t.table_type = 'BASE TABLE'
            ORDER BY t.table_name
            """, params: [schemaFilter])
        return result.rows.compactMap { row -> TableInfo? in
            guard let name = row.first?.stringValue else { return nil }
            let count = row.count > 1 ? row[1].intValue : nil
            return TableInfo(name: name, schema: schemaFilter, type: .table, estimatedRowCount: count)
        }
    }

    func listViews(schema: String?) async throws -> [ViewInfo] {
        let schemaFilter = schema ?? "public"
        let result = try await executeParameterized(sql: """
            SELECT table_name, view_definition, false AS is_materialized
            FROM information_schema.views
            WHERE table_schema = $1
            UNION ALL
            SELECT matviewname, definition, true
            FROM pg_matviews
            WHERE schemaname = $1
            ORDER BY table_name
            """, params: [schemaFilter])

        return result.rows.compactMap { row -> ViewInfo? in
            guard let name = row[0].stringValue else { return nil }
            let def = row[1].stringValue
            let isMat = row[2].stringValue == "true" || row[2].stringValue == "t"
            return ViewInfo(name: name, schema: schemaFilter, definition: def, isMaterialized: isMat)
        }
    }

    func describeTable(name: String, schema: String?) async throws -> TableDescription {
        let schemaFilter = schema ?? "public"

        async let colsTask = describeColumns(table: name, schema: schemaFilter)
        async let constraintsTask = listAllConstraints(table: name, schema: schemaFilter)
        async let indexesTask = listIndexes(table: name, schema: schemaFilter)

        let cols = try await colsTask
        let constraints = try await constraintsTask
        let indexes = try await indexesTask

        // Merge check constraints into columns
        var columns = cols.columns
        for (colName, checkExpr) in constraints.checks {
            if let idx = columns.firstIndex(where: { $0.name == colName }) {
                columns[idx].checkConstraint = checkExpr
            }
        }

        return TableDescription(
            name: name, schema: schemaFilter, columns: columns,
            indexes: indexes, foreignKeys: constraints.foreignKeys, constraints: [],
            comment: cols.tableComment, estimatedRowCount: cols.estimatedRowCount
        )
    }

    static func shortenPgType(_ raw: String) -> String {
        switch raw {
        case "timestamp without time zone": return "timestamp"
        case "timestamp with time zone": return "timestamptz"
        case "time without time zone": return "time"
        case "time with time zone": return "timetz"
        case "integer": return "int4"
        case "smallint": return "int2"
        case "bigint": return "int8"
        case "real": return "float4"
        case "double precision": return "float8"
        case "boolean": return "bool"
        default: break
        }
        if raw.hasPrefix("character varying") {
            return raw.replacingOccurrences(of: "character varying", with: "varchar")
        }
        if raw.hasPrefix("character") {
            return raw.replacingOccurrences(of: "character", with: "char")
        }
        return raw
    }

    // MARK: - Query 1: Columns + PK + defaults + comments + meta (no FK/check joins)

    private func describeColumns(table: String, schema: String) async throws -> (columns: [ColumnInfo], tableComment: String?, estimatedRowCount: Int?) {
        let result = try await executeParameterized(sql: """
            SELECT a.attname,
                   format_type(a.atttypid, a.atttypmod),
                   NOT a.attnotnull,
                   pg_get_expr(d.adbin, d.adrelid),
                   a.attnum,
                   CASE WHEN a.atttypid IN (1042, 1043) AND a.atttypmod > 0 THEN a.atttypmod - 4 END,
                   CASE WHEN pk.conkey @> ARRAY[a.attnum] THEN true ELSE false END,
                   col_description(cls.oid, a.attnum),
                   cls.reltuples::bigint,
                   obj_description(cls.oid)
            FROM pg_attribute a
            JOIN pg_class cls ON cls.oid = a.attrelid
            JOIN pg_namespace ns ON ns.oid = cls.relnamespace
            LEFT JOIN pg_attrdef d ON d.adrelid = a.attrelid AND d.adnum = a.attnum
            LEFT JOIN pg_constraint pk ON pk.conrelid = cls.oid AND pk.contype = 'p'
            WHERE cls.relname = $1 AND ns.nspname = $2
              AND a.attnum > 0 AND NOT a.attisdropped
            ORDER BY a.attnum
            """, params: [table, schema])

        var columns: [ColumnInfo] = []
        var rowCount: Int?
        var tableComment: String?

        for row in result.rows {
            let colName = row[0].stringValue ?? ""
            if columns.contains(where: { $0.name == colName }) { continue }

            let dataType = Self.shortenPgType(row[1].stringValue ?? "text")
            let nullable = row[2].stringValue == "t" || row[2].stringValue == "true"
            let defVal = row[3].stringValue
            let ordinal = row[4].intValue ?? 0
            let maxLen = row[5].intValue
            let isPK = row[6].stringValue == "t" || row[6].stringValue == "true"
            let comment = row[7].stringValue

            columns.append(ColumnInfo(
                name: colName, dataType: dataType, isNullable: nullable, defaultValue: defVal,
                isPrimaryKey: isPK, isAutoIncrement: defVal?.hasPrefix("nextval(") == true,
                comment: comment, ordinalPosition: ordinal, characterMaxLength: maxLen
            ))

            if rowCount == nil { rowCount = row[8].intValue }
            if tableComment == nil { tableComment = row[9].stringValue }
        }

        return (columns, tableComment, rowCount)
    }

    // MARK: - Query 2: All constraints (FK + check) in one simple query

    private func listAllConstraints(table: String, schema: String) async throws -> (foreignKeys: [ForeignKeyInfo], checks: [(String, String)]) {
        let result = try await executeParameterized(sql: """
            SELECT con.contype,
                   con.conname,
                   con.conkey,
                   pg_get_constraintdef(con.oid),
                   ref_cls.relname,
                   (SELECT string_agg(a.attname, ',' ORDER BY array_position(con.confkey, a.attnum))
                    FROM pg_attribute a WHERE a.attrelid = con.confrelid AND a.attnum = ANY(con.confkey)),
                   (SELECT string_agg(a.attname, ',' ORDER BY array_position(con.conkey, a.attnum))
                    FROM pg_attribute a WHERE a.attrelid = con.conrelid AND a.attnum = ANY(con.conkey)),
                   con.confdeltype,
                   con.confupdtype
            FROM pg_constraint con
            JOIN pg_class cls ON cls.oid = con.conrelid
            JOIN pg_namespace ns ON ns.oid = cls.relnamespace
            LEFT JOIN pg_class ref_cls ON ref_cls.oid = con.confrelid
            WHERE cls.relname = $1 AND ns.nspname = $2
              AND con.contype IN ('f', 'c')
            ORDER BY con.conname
            """, params: [table, schema])

        var fks: [ForeignKeyInfo] = []
        var checks: [(String, String)] = []

        for row in result.rows {
            let contype = row[0].stringValue ?? ""
            let conname = row[1].stringValue ?? ""

            if contype == "f" {
                let refTable = row[4].stringValue ?? ""
                let refCols = splitCols(row[5].stringValue)
                let localCols = splitCols(row[6].stringValue)
                let onDelete = pgFKAction(row[7].stringValue)
                let onUpdate = pgFKAction(row[8].stringValue)
                fks.append(ForeignKeyInfo(name: conname, columns: localCols, referencedTable: refTable, referencedColumns: refCols, onDelete: onDelete, onUpdate: onUpdate))
            } else if contype == "c" {
                let conDef = row[3].stringValue ?? ""
                let localCols = splitCols(row[6].stringValue)
                // Strip "CHECK (...)" wrapper
                let expr: String
                if conDef.hasPrefix("CHECK (") && conDef.hasSuffix(")") {
                    expr = String(conDef.dropFirst(7).dropLast(1))
                } else {
                    expr = conDef
                }
                for col in localCols {
                    checks.append((col, expr))
                }
            }
        }

        return (fks, checks)
    }

    /// Parse PostgreSQL array string like "{a,b,c}" into [String]
    private func parseArrayString(_ raw: String?) -> [String] {
        guard let raw, raw.hasPrefix("{") && raw.hasSuffix("}") else {
            return raw?.split(separator: ",").map(String.init) ?? []
        }
        return String(raw.dropFirst().dropLast()).split(separator: ",").map { String($0) }
    }

    /// Split comma-separated column names from string_agg result
    private func splitCols(_ raw: String?) -> [String] {
        guard let raw, !raw.isEmpty else { return [] }
        return raw.split(separator: ",").map { String($0).trimmingCharacters(in: .whitespaces) }
    }

    func listIndexes(table: String, schema: String?) async throws -> [IndexInfo] {
        let schemaFilter = schema ?? "public"
        // Use pg_get_indexdef + regex parsing (faster, no subqueries)
        let result = try await executeParameterized(sql: """
            SELECT ix.relname,
                   upper(am.amname),
                   i.indisunique,
                   pg_get_indexdef(i.indexrelid),
                   obj_description(ix.oid)
            FROM pg_index i
            JOIN pg_class t ON t.oid = i.indrelid
            JOIN pg_class ix ON ix.oid = i.indexrelid
            JOIN pg_namespace n ON t.relnamespace = n.oid
            JOIN pg_am am ON ix.relam = am.oid
            WHERE t.relname = $1 AND n.nspname = $2
            ORDER BY ix.relname
            """, params: [table, schemaFilter])

        return result.rows.compactMap { row -> IndexInfo? in
            guard let name = row[0].stringValue else { return nil }
            let type = row[1].stringValue
            let unique = row[2].stringValue == "t" || row[2].stringValue == "true"
            let indexDef = row[3].stringValue ?? ""
            let comment = row[4].stringValue

            // Parse columns from indexdef: "CREATE INDEX ... ON ... USING btree (col1, col2)"
            let cols: [String]
            if let openParen = indexDef.range(of: "("),
               let closeParen = indexDef.range(of: ")", options: .backwards) {
                let colStr = indexDef[openParen.upperBound..<closeParen.lowerBound]
                cols = colStr.split(separator: ",").map { String($0.trimmingCharacters(in: .whitespaces)) }
            } else {
                cols = []
            }

            // Parse WHERE condition
            let condition: String?
            if let whereRange = indexDef.range(of: " WHERE ", options: .caseInsensitive) {
                condition = String(indexDef[whereRange.upperBound...])
            } else {
                condition = nil
            }

            // Parse INCLUDE
            let include: String?
            if let includeRange = indexDef.range(of: " INCLUDE ", options: .caseInsensitive) {
                let afterInclude = indexDef[includeRange.upperBound...]
                if let iParen = afterInclude.range(of: "("),
                   let iClose = afterInclude.range(of: ")") {
                    include = String(afterInclude[iParen.upperBound..<iClose.lowerBound])
                } else {
                    include = nil
                }
            } else {
                include = nil
            }

            return IndexInfo(name: name, columns: cols, isUnique: unique, type: type, tableName: table, condition: condition, include: include, comment: comment)
        }
    }

    func listForeignKeys(table: String, schema: String?) async throws -> [ForeignKeyInfo] {
        let schemaFilter = schema ?? "public"
        let result = try await executeParameterized(sql: """
            SELECT con.conname,
                   att.attname AS column_name,
                   ref_cls.relname AS foreign_table,
                   ref_att.attname AS foreign_column,
                   con.confdeltype,
                   con.confupdtype
            FROM pg_constraint con
            JOIN pg_class cls ON cls.oid = con.conrelid
            JOIN pg_namespace ns ON ns.oid = cls.relnamespace
            JOIN pg_attribute att ON att.attrelid = con.conrelid AND att.attnum = ANY(con.conkey)
            JOIN pg_class ref_cls ON ref_cls.oid = con.confrelid
            JOIN pg_attribute ref_att ON ref_att.attrelid = con.confrelid AND ref_att.attnum = ANY(con.confkey)
            WHERE con.contype = 'f'
              AND ns.nspname = $1 AND cls.relname = $2
            ORDER BY con.conname
            """, params: [schemaFilter, table])

        return result.rows.compactMap { row -> ForeignKeyInfo? in
            let name = row[0].stringValue
            guard let col = row[1].stringValue,
                  let refTable = row[2].stringValue,
                  let refCol = row[3].stringValue else { return nil }
            let onDelete = pgFKAction(row[4].stringValue)
            let onUpdate = pgFKAction(row[5].stringValue)
            return ForeignKeyInfo(name: name, columns: [col], referencedTable: refTable, referencedColumns: [refCol], onDelete: onDelete, onUpdate: onUpdate)
        }
    }

    private func pgFKAction(_ code: String?) -> ForeignKeyAction {
        switch code {
        case "c": return .cascade
        case "n": return .setNull
        case "d": return .setDefault
        case "r": return .restrict
        default: return .noAction
        }
    }

    func listFunctions(schema: String?) async throws -> [String] {
        let schemaFilter = schema ?? "public"
        let result = try await executeParameterized(sql: """
            SELECT p.proname || '(' || pg_get_function_identity_arguments(p.oid) || ')'
            FROM pg_proc p
            JOIN pg_namespace n ON p.pronamespace = n.oid
            WHERE n.nspname = $1
              AND p.prokind IN ('f', 'p')
            ORDER BY p.proname
            """, params: [schemaFilter])
        return result.rows.compactMap { $0[0].stringValue }
    }

    func getFunctionSource(name: String, schema: String?) async throws -> String {
        let schemaFilter = schema ?? "public"
        // name may be "funcname(arg_types)" or just "funcname"
        let result: QueryResult
        if name.contains("(") {
            // Match exact overload by full signature
            result = try await executeParameterized(sql: """
                SELECT pg_get_functiondef(p.oid)
                FROM pg_proc p
                JOIN pg_namespace n ON p.pronamespace = n.oid
                WHERE n.nspname = $1
                  AND p.proname || '(' || pg_get_function_identity_arguments(p.oid) || ')' = $2
                LIMIT 1
                """, params: [schemaFilter, name])
        } else {
            result = try await executeParameterized(sql: """
                SELECT pg_get_functiondef(p.oid)
                FROM pg_proc p
                JOIN pg_namespace n ON p.pronamespace = n.oid
                WHERE n.nspname = $1 AND p.proname = $2
                LIMIT 1
                """, params: [schemaFilter, name])
        }
        guard let source = result.rows.first?[0].stringValue else {
            throw GridexError.queryExecutionFailed("Function '\(name)' not found")
        }
        return source
    }

    // MARK: - Data Manipulation

    func insertRow(table: String, schema: String?, values: [String: RowValue]) async throws -> QueryResult {
        let d = SQLDialect.postgresql
        let schemaPrefix = schema.map { d.quoteIdentifier($0) + "." } ?? ""
        let cols = values.keys.map { d.quoteIdentifier($0) }.joined(separator: ", ")
        let placeholders = (1...values.count).map { "$\($0)" }.joined(separator: ", ")
        let sql = "INSERT INTO \(schemaPrefix)\(d.quoteIdentifier(table)) (\(cols)) VALUES (\(placeholders))"
        return try await executeRaw(sql: buildInlineSQL(sql: sql, values: Array(values.values)))
    }

    func updateRow(table: String, schema: String?, set values: [String: RowValue], where conditions: [String: RowValue]) async throws -> QueryResult {
        let d = SQLDialect.postgresql
        let schemaPrefix = schema.map { d.quoteIdentifier($0) + "." } ?? ""
        let setClauses = values.compactMap { k, v in "\(d.quoteIdentifier(k)) = \(inlineValue(k, v))" }.joined(separator: ", ")
        let whereClauses = conditions.compactMap { k, v in "\(d.quoteIdentifier(k)) = \(inlineValue(k, v))" }.joined(separator: " AND ")
        return try await executeRaw(sql: "UPDATE \(schemaPrefix)\(d.quoteIdentifier(table)) SET \(setClauses) WHERE \(whereClauses)")
    }

    func deleteRow(table: String, schema: String?, where conditions: [String: RowValue]) async throws -> QueryResult {
        let d = SQLDialect.postgresql
        let schemaPrefix = schema.map { d.quoteIdentifier($0) + "." } ?? ""
        let whereClauses = conditions.compactMap { k, v in "\(d.quoteIdentifier(k)) = \(inlineValue(k, v))" }.joined(separator: " AND ")
        return try await executeRaw(sql: "DELETE FROM \(schemaPrefix)\(d.quoteIdentifier(table)) WHERE \(whereClauses)")
    }

    func beginTransaction() async throws { _ = try await executeRaw(sql: "BEGIN") }
    func commitTransaction() async throws { _ = try await executeRaw(sql: "COMMIT") }
    func rollbackTransaction() async throws { _ = try await executeRaw(sql: "ROLLBACK") }

    // MARK: - Pagination

    func fetchRows(table: String, schema: String?, columns: [String]?, where filter: FilterExpression?, orderBy: [QuerySortDescriptor]?, limit: Int, offset: Int) async throws -> QueryResult {
        let d = SQLDialect.postgresql
        let schemaPrefix = schema.map { d.quoteIdentifier($0) + "." } ?? ""
        let colList = columns?.map { d.quoteIdentifier($0) }.joined(separator: ", ") ?? "*"
        var sql = "SELECT \(colList) FROM \(schemaPrefix)\(d.quoteIdentifier(table))"
        if let filter, !filter.conditions.isEmpty {
            sql += " WHERE \(filter.toSQL(dialect: d))"
        }
        if let orderBy, !orderBy.isEmpty {
            sql += " ORDER BY " + orderBy.map { $0.toSQL(dialect: d) }.joined(separator: ", ")
        }
        sql += " LIMIT \(limit) OFFSET \(offset)"
        return try await executeRaw(sql: sql)
    }

    func serverVersion() async throws -> String {
        let r = try await executeRaw(sql: "SELECT version()")
        return r.rows.first?.first?.stringValue ?? "PostgreSQL"
    }

    func currentDatabase() async throws -> String? {
        let r = try await executeRaw(sql: "SELECT current_database()")
        return r.rows.first?.first?.stringValue
    }

    // MARK: - SchemaInspectable

    func fullSchemaSnapshot(database: String?) async throws -> SchemaSnapshot {
        let schemas = try await listSchemas(database: database)
        var schemaInfos: [SchemaInfo] = []
        for schemaName in schemas {
            let tables = try await listTables(schema: schemaName)
            let descs: [TableDescription] = try await withThrowingTaskGroup(of: TableDescription.self) { group in
                for t in tables {
                    let name = t.name
                    group.addTask { try await self.describeTable(name: name, schema: schemaName) }
                }
                var results: [TableDescription] = []
                for try await desc in group { results.append(desc) }
                return results
            }
            let views = try await listViews(schema: schemaName)
            schemaInfos.append(SchemaInfo(name: schemaName, tables: descs, views: views, functions: [], enums: []))
        }
        let dbName = try await currentDatabase() ?? "postgres"
        return SchemaSnapshot(databaseName: dbName, databaseType: .postgresql, schemas: schemaInfos, capturedAt: Date())
    }

    func columnStatistics(table: String, schema: String?, sampleSize: Int) async throws -> [ColumnStatistics] {
        let schemaFilter = schema ?? "public"
        let colResult = try await describeColumns(table: table, schema: schemaFilter)
        let cols = colResult.columns
        var stats: [ColumnStatistics] = []
        for col in cols {
            let q = SQLDialect.postgresql.quoteIdentifier(col.name)
            let tbl = "\(SQLDialect.postgresql.quoteIdentifier(schemaFilter)).\(SQLDialect.postgresql.quoteIdentifier(table))"
            let r = try await executeRaw(sql: """
                SELECT COUNT(DISTINCT \(q)),
                       CAST(SUM(CASE WHEN \(q) IS NULL THEN 1 ELSE 0 END) AS DOUBLE PRECISION) / GREATEST(COUNT(*), 1),
                       MIN(\(q)::text), MAX(\(q)::text)
                FROM \(tbl)
                """)
            if let row = r.rows.first {
                stats.append(ColumnStatistics(
                    columnName: col.name,
                    distinctCount: row[0].intValue,
                    nullRatio: row[1].doubleValue,
                    topValues: nil,
                    minValue: row[2].stringValue,
                    maxValue: row[3].stringValue
                ))
            }
        }
        return stats
    }

    func tableRowCount(table: String, schema: String?) async throws -> Int {
        let schemaFilter = schema ?? "public"
        // Use fast estimated count from pg_class instead of slow COUNT(*)
        let r = try await executeParameterized(sql: """
            SELECT reltuples::bigint FROM pg_class c
            JOIN pg_namespace n ON n.oid = c.relnamespace
            WHERE c.relname = $1 AND n.nspname = $2
            """, params: [table, schemaFilter])
        return r.rows.first?.first?.intValue ?? 0
    }

    func tableSizeBytes(table: String, schema: String?) async throws -> Int64? {
        let schemaFilter = schema ?? "public"
        let d = SQLDialect.postgresql
        let qualifiedName = "\(d.quoteIdentifier(schemaFilter)).\(d.quoteIdentifier(table))"
        let r = try await executeParameterized(sql: "SELECT pg_total_relation_size($1)", params: [qualifiedName])
        if let val = r.rows.first?.first?.intValue { return Int64(val) }
        return nil
    }

    func queryStatistics() async throws -> [QueryStatisticsEntry] { [] }

    func primaryKeyColumns(table: String, schema: String?) async throws -> [String] {
        let schemaFilter = schema ?? "public"
        let result = try await executeParameterized(sql: """
            SELECT kcu.column_name
            FROM information_schema.table_constraints tc
            JOIN information_schema.key_column_usage kcu
              ON tc.constraint_name = kcu.constraint_name AND tc.table_schema = kcu.table_schema
            WHERE tc.constraint_type = 'PRIMARY KEY'
              AND tc.table_schema = $1 AND tc.table_name = $2
            ORDER BY kcu.ordinal_position
            """, params: [schemaFilter, table])
        return result.rows.compactMap { $0.first?.stringValue }
    }

    // MARK: - Helpers

    /// Execute a parameterized query with `$1, $2, ...` placeholders and String bindings.
    /// Use this for metadata queries where values come from user input.
    private func executeParameterized(sql: String, params: [String]) async throws -> QueryResult {
        try ensureConnected()
        guard let client else { throw GridexError.queryExecutionFailed("No connection") }

        let startTime = CFAbsoluteTimeGetCurrent()
        var bindings = PostgresBindings(capacity: params.count)
        for param in params {
            bindings.append(param)
        }
        let pgQuery = PostgresQuery(unsafeSQL: sql, binds: bindings)
        let rows = try await client.query(pgQuery)
        let columns = rows.columns.map { col in
            ColumnHeader(name: col.name, dataType: col.dataType.description)
        }
        var resultRows: [[RowValue]] = []
        for try await row in rows {
            var rowValues: [RowValue] = []
            for (cell, meta) in zip(row, rows.columns) {
                rowValues.append(decodeCell(cell, dataType: meta.dataType))
            }
            resultRows.append(rowValues)
        }
        let duration = CFAbsoluteTimeGetCurrent() - startTime
        return QueryResult(columns: columns, rows: resultRows, rowsAffected: resultRows.count, executionTime: duration, queryType: .select)
    }

    private func ensureConnected() throws {
        guard isConnected, client != nil else {
            throw GridexError.queryExecutionFailed("Not connected to PostgreSQL")
        }
    }

    private func detectQueryType(_ upper: String) -> QueryType {
        if upper.hasPrefix("SELECT") || upper.hasPrefix("EXPLAIN") || upper.hasPrefix("WITH") { return .select }
        if upper.hasPrefix("INSERT") { return .insert }
        if upper.hasPrefix("UPDATE") { return .update }
        if upper.hasPrefix("DELETE") { return .delete }
        if upper.hasPrefix("CREATE") || upper.hasPrefix("ALTER") || upper.hasPrefix("DROP") { return .ddl }
        return .other
    }

    private func decodeCell(_ cell: PostgresCell, dataType: PostgresDataType) -> RowValue {
        if cell.bytes == nil { return .null }
        do {
            switch dataType {
            case .bool:
                return .boolean(try cell.decode(Bool.self))
            case .int2:
                return .integer(Int64(try cell.decode(Int16.self)))
            case .int4:
                return .integer(Int64(try cell.decode(Int32.self)))
            case .int8:
                return .integer(try cell.decode(Int64.self))
            case .float4:
                return .double(Double(try cell.decode(Float.self)))
            case .float8:
                return .double(try cell.decode(Double.self))
            case .numeric:
                // numeric binary format requires Decimal decoder, not String
                let decimal = try cell.decode(Decimal.self)
                let str = "\(decimal)"
                if let d = Double(str) { return .double(d) }
                return .string(str)
            case .money:
                // money is stored as Int64 (cents) in binary protocol
                if var bytes = cell.bytes, bytes.readableBytes == 8,
                   let cents = bytes.readInteger(as: Int64.self) {
                    return .double(Double(cents) / 100.0)
                }
                return .null
            case .text, .varchar, .bpchar, .name:
                return .string(try cell.decode(String.self))
            case .bytea:
                var buf = try cell.decode(ByteBuffer.self)
                let data = buf.readData(length: buf.readableBytes) ?? Data()
                return .data(data)
            case .timestamp, .timestamptz, .date:
                return .date(try cell.decode(Date.self))
            case .time:
                // time/timetz: decode as String via text representation
                if let s = try? cell.decode(String.self) { return .string(s) }
                return .null
            case .interval:
                if let s = try? cell.decode(String.self) { return .string(s) }
                return .null
            case .uuid:
                return .uuid(try cell.decode(UUID.self))
            case .json, .jsonb:
                return .json(try cell.decode(String.self))
            default:
                // Try String decoding, but validate it's not binary garbage
                if let s = try? cell.decode(String.self),
                   !s.unicodeScalars.contains(where: { $0.value == 0xFFFD }) {
                    return .string(s)
                }
                return .null
            }
        } catch {
            if let s = try? cell.decode(String.self),
               !s.unicodeScalars.contains(where: { $0.value == 0xFFFD }) {
                return .string(s)
            }
            return .null
        }
    }

    private func inlineValue(_ key: String, _ value: RowValue) -> String {
        switch value {
        case .null: return "NULL"
        case .string(let v): return "'\(v.replacingOccurrences(of: "'", with: "''"))'"
        case .integer(let v): return "\(v)"
        case .double(let v): return "\(v)"
        case .boolean(let v): return v ? "TRUE" : "FALSE"
        case .date(let v):
            let fmt = DateFormatter()
            fmt.dateFormat = "yyyy-MM-dd HH:mm:ss"
            fmt.timeZone = TimeZone(identifier: "UTC")
            return "'\(fmt.string(from: v))'"
        case .uuid(let v): return "'\(v.uuidString)'"
        case .json(let v): return "'\(v.replacingOccurrences(of: "'", with: "''"))'::jsonb"
        case .data: return "NULL"
        case .array: return "NULL"
        }
    }

    private func buildInlineSQL(sql: String, values: [RowValue]) -> String {
        var result = sql
        for (i, value) in values.enumerated() {
            result = result.replacingOccurrences(of: "$\(i + 1)", with: inlineValue("", value))
        }
        return result
    }
}
