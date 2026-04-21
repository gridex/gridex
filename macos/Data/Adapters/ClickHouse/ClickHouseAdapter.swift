// ClickHouseAdapter.swift
// Gridex
//
// ClickHouse adapter using the HTTP interface (ports 8123/8443). Speaks
// JSONCompact for SELECTs and plain statements for DDL/DML. No native TCP.

import Foundation

final class ClickHouseAdapter: DatabaseAdapter, SchemaInspectable, @unchecked Sendable {

    let databaseType: DatabaseType = .clickhouse
    private(set) var isConnected: Bool = false

    private let stateLock = NSLock()
    private var client: ClickHouseHTTPClient?
    private var storedConfig: ConnectionConfig?
    private var storedPassword: String?
    private var currentDB: String?
    private var serverVersionCache: String?

    // MARK: - Connection lifecycle

    func connect(config: ConnectionConfig, password: String?) async throws {
        let host = config.host ?? "localhost"
        let port = config.port ?? (config.sslEnabled ? 8443 : 8123)
        let username = config.username ?? "default"
        let database = config.database

        let newClient = ClickHouseHTTPClient(
            host: host,
            port: port,
            username: username,
            password: password ?? "",
            defaultDatabase: database,
            useTLS: config.sslEnabled,
            sslCACertPath: config.sslCACertPath,
            sslClientBundlePath: config.sslCertPath
        )

        do {
            _ = try await newClient.send(sql: "SELECT 1 FORMAT JSONCompact", readOnly: true)
        } catch {
            if case GridexError.connectionFailed = error { throw error }
            throw GridexError.connectionFailed(underlying: error)
        }

        stateLock.lock()
        self.client = newClient
        self.storedConfig = config
        self.storedPassword = password
        self.currentDB = (database?.isEmpty == false) ? database : nil
        self.isConnected = true
        stateLock.unlock()

        // Cache server version for introspection fallbacks (21.3+ has is_in_primary_key).
        self.serverVersionCache = try? await fetchServerVersion()
    }

    func disconnect() async throws {
        stateLock.lock()
        client = nil
        storedConfig = nil
        storedPassword = nil
        currentDB = nil
        serverVersionCache = nil
        isConnected = false
        stateLock.unlock()
    }

    func testConnection(config: ConnectionConfig, password: String?) async throws -> Bool {
        let adapter = ClickHouseAdapter()
        do {
            try await adapter.connect(config: config, password: password)
            _ = try? await adapter.serverVersion()
            try await adapter.disconnect()
            return true
        } catch {
            try? await adapter.disconnect()
            throw error
        }
    }

    private func requireClient() throws -> ClickHouseHTTPClient {
        stateLock.lock()
        defer { stateLock.unlock() }
        guard let client else {
            throw GridexError.queryExecutionFailed("Not connected to ClickHouse")
        }
        return client
    }

    // MARK: - Query Execution

    func execute(query: String, parameters: [QueryParameter]?) async throws -> QueryResult {
        try await executeRaw(sql: query)
    }

    func executeRaw(sql: String) async throws -> QueryResult {
        let start = CFAbsoluteTimeGetCurrent()
        let trimmed = sql.trimmingCharacters(in: .whitespacesAndNewlines)
        let upper = trimmed.uppercased()

        // ClickHouse HTTP is stateless — intercept USE to update in-memory default DB.
        if upper.hasPrefix("USE ") {
            let name = parseUseStatement(trimmed)
            if !name.isEmpty {
                stateLock.lock()
                currentDB = name
                // Rebuild client so future requests include the new default database.
                if let cfg = storedConfig {
                    let host = cfg.host ?? "localhost"
                    let port = cfg.port ?? (cfg.sslEnabled ? 8443 : 8123)
                    let username = cfg.username ?? "default"
                    client = ClickHouseHTTPClient(
                        host: host,
                        port: port,
                        username: username,
                        password: storedPassword ?? "",
                        defaultDatabase: name,
                        useTLS: cfg.sslEnabled,
                        sslCACertPath: cfg.sslCACertPath,
                        sslClientBundlePath: cfg.sslCertPath
                    )
                }
                stateLock.unlock()
            }
            let elapsed = CFAbsoluteTimeGetCurrent() - start
            return QueryResult(columns: [], rows: [], rowsAffected: 0, executionTime: elapsed, queryType: .other)
        }

        let qType = detectQueryType(upper)
        let isSelectShaped = qType == .select
            || upper.hasPrefix("EXPLAIN")
            || upper.hasPrefix("WITH")
            || upper.hasPrefix("SHOW")
            || upper.hasPrefix("DESCRIBE")
            || upper.hasPrefix("DESC ")

        let client = try requireClient()

        if isSelectShaped {
            let wireSQL = trimmed + " FORMAT JSONCompact"
            let response = try await client.send(sql: wireSQL, readOnly: false)
            let elapsed = CFAbsoluteTimeGetCurrent() - start
            return try parseJSONCompact(data: response.body, elapsed: elapsed)
        } else {
            let response = try await client.send(sql: trimmed, readOnly: false)
            let elapsed = CFAbsoluteTimeGetCurrent() - start
            return QueryResult(columns: [], rows: [], rowsAffected: response.writtenRows, executionTime: elapsed, queryType: qType)
        }
    }

    // MARK: - JSONCompact parsing

    private func parseJSONCompact(data: Data, elapsed: TimeInterval) throws -> QueryResult {
        guard !data.isEmpty else {
            return QueryResult(columns: [], rows: [], rowsAffected: 0, executionTime: elapsed, queryType: .select)
        }

        let root: Any
        do {
            root = try JSONSerialization.jsonObject(with: data, options: [.fragmentsAllowed])
        } catch {
            let snippet = String(data: data.prefix(400), encoding: .utf8) ?? ""
            throw GridexError.queryExecutionFailed("Unexpected ClickHouse response: \(snippet)")
        }
        guard let dict = root as? [String: Any],
              let metaArr = dict["meta"] as? [[String: Any]],
              let dataArr = dict["data"] as? [[Any]]
        else {
            return QueryResult(columns: [], rows: [], rowsAffected: 0, executionTime: elapsed, queryType: .select)
        }

        struct Meta { let name: String; let type: String }
        let meta: [Meta] = metaArr.compactMap { entry in
            guard let name = entry["name"] as? String,
                  let type = entry["type"] as? String else { return nil }
            return Meta(name: name, type: type)
        }
        let columns = meta.map {
            ColumnHeader(name: $0.name, dataType: $0.type, isNullable: $0.type.hasPrefix("Nullable"))
        }
        let rows: [[RowValue]] = dataArr.map { row in
            row.enumerated().map { idx, raw in
                let type = idx < meta.count ? meta[idx].type : "String"
                return decodeRow(raw, chType: type)
            }
        }
        return QueryResult(columns: columns, rows: rows, rowsAffected: 0, executionTime: elapsed, queryType: .select)
    }

    private func decodeRow(_ raw: Any, chType: String) -> RowValue {
        // Strip Nullable(T) wrapping
        let unwrapped: String = {
            if chType.hasPrefix("Nullable(") && chType.hasSuffix(")") {
                return String(chType.dropFirst("Nullable(".count).dropLast())
            }
            return chType
        }()

        if raw is NSNull { return .null }

        if unwrapped.hasPrefix("Array(") {
            if let items = raw as? [Any] {
                let inner = String(unwrapped.dropFirst("Array(".count).dropLast())
                return .array(items.map { decodeRow($0, chType: inner) })
            }
            return .null
        }

        if unwrapped == "Boolean" || unwrapped == "Bool" {
            if let b = raw as? Bool { return .boolean(b) }
            if let n = raw as? NSNumber { return .boolean(n.intValue != 0) }
            if let s = raw as? String { return .boolean(s == "true" || s == "1") }
            return .null
        }

        if unwrapped.hasPrefix("UInt") || unwrapped.hasPrefix("Int") {
            if let s = raw as? String {
                if let i = Int64(s) { return .integer(i) }
                if let u = UInt64(s) { return .integer(Int64(bitPattern: u)) }
                return .string(s)
            }
            if let n = raw as? NSNumber { return .integer(n.int64Value) }
            return .null
        }

        if unwrapped.hasPrefix("Float") {
            if let n = raw as? NSNumber { return .double(n.doubleValue) }
            if let s = raw as? String, let d = Double(s) { return .double(d) }
            return .null
        }

        if unwrapped.hasPrefix("Decimal") {
            if let s = raw as? String { return .string(s) }
            if let n = raw as? NSNumber { return .string(n.stringValue) }
            return .null
        }

        if unwrapped == "UUID" {
            if let s = raw as? String, let uuid = UUID(uuidString: s) { return .uuid(uuid) }
            if let s = raw as? String { return .string(s) }
            return .null
        }

        if unwrapped.hasPrefix("Date") {
            if let s = raw as? String {
                let fmts = ["yyyy-MM-dd HH:mm:ss.SSS", "yyyy-MM-dd HH:mm:ss", "yyyy-MM-dd"]
                let fmt = DateFormatter()
                fmt.timeZone = TimeZone(identifier: "UTC")
                for pattern in fmts {
                    fmt.dateFormat = pattern
                    if let d = fmt.date(from: s) { return .date(d) }
                }
                return .string(s)
            }
            return .null
        }

        if unwrapped.hasPrefix("Map(") || unwrapped.hasPrefix("Tuple(") || unwrapped == "JSON" || unwrapped == "Object('json')" {
            if let s = raw as? String { return .json(s) }
            if let data = try? JSONSerialization.data(withJSONObject: raw),
               let text = String(data: data, encoding: .utf8) {
                return .json(text)
            }
            return .null
        }

        // String / FixedString / IPv4 / IPv6 / Enum* / fallback
        if let s = raw as? String { return .string(s) }
        if let n = raw as? NSNumber { return .integer(n.int64Value) }
        if let b = raw as? Bool { return .boolean(b) }
        return .null
    }

    // MARK: - Schema Inspection

    func listDatabases() async throws -> [String] {
        let r = try await executeRaw(sql: "SELECT name FROM system.databases ORDER BY name")
        return r.rows.compactMap { $0.first?.stringValue }
    }

    func listSchemas(database: String?) async throws -> [String] {
        // ClickHouse is flat — databases are the schema boundary.
        []
    }

    func listTables(schema: String?) async throws -> [TableInfo] {
        let db = try resolveDB(schema)
        let sql = """
            SELECT name, total_rows FROM system.tables
            WHERE database = '\(escape(db))' AND engine NOT LIKE '%View'
            ORDER BY name
            """
        let r = try await executeRaw(sql: sql)
        return r.rows.compactMap { row -> TableInfo? in
            guard let name = row.first?.stringValue else { return nil }
            let count = row.count > 1 ? row[1].intValue : nil
            return TableInfo(name: name, schema: db, type: .table, estimatedRowCount: count)
        }
    }

    func listViews(schema: String?) async throws -> [ViewInfo] {
        let db = try resolveDB(schema)
        let sql = """
            SELECT name, as_select, engine FROM system.tables
            WHERE database = '\(escape(db))' AND engine LIKE '%View'
            ORDER BY name
            """
        let r = try await executeRaw(sql: sql)
        return r.rows.compactMap { row -> ViewInfo? in
            guard let name = row.first?.stringValue else { return nil }
            let def = row.count > 1 ? row[1].stringValue : nil
            let engine = row.count > 2 ? (row[2].stringValue ?? "") : ""
            return ViewInfo(name: name, schema: db, definition: def, isMaterialized: engine == "MaterializedView")
        }
    }

    func describeTable(name: String, schema: String?) async throws -> TableDescription {
        let db = try resolveDB(schema)
        let columns = try await describeColumns(table: name, schema: db)
        let indexes = try await listIndexes(table: name, schema: db)

        // Row count + comment from system.tables
        let meta = try? await executeRaw(sql: """
            SELECT total_rows, comment FROM system.tables
            WHERE database = '\(escape(db))' AND name = '\(escape(name))'
            """)
        let estRows = meta?.rows.first?[0].intValue
        let comment = meta?.rows.first?[1].stringValue

        return TableDescription(
            name: name,
            schema: db,
            columns: columns,
            indexes: indexes,
            foreignKeys: [], // ClickHouse has no foreign keys
            constraints: [],
            comment: (comment?.isEmpty == true) ? nil : comment,
            estimatedRowCount: estRows
        )
    }

    private func describeColumns(table: String, schema db: String) async throws -> [ColumnInfo] {
        let hasPKFlag = supportsIsInPrimaryKey()

        let selectList = hasPKFlag
            ? "name, type, default_kind, default_expression, comment, is_in_primary_key, position"
            : "name, type, default_kind, default_expression, comment, 0 AS is_in_primary_key, position"

        let r = try await executeRaw(sql: """
            SELECT \(selectList) FROM system.columns
            WHERE database = '\(escape(db))' AND table = '\(escape(table))'
            ORDER BY position
            """)

        // Fallback PK set from system.tables.primary_key when is_in_primary_key is unavailable.
        var pkSet: Set<String> = []
        if !hasPKFlag {
            let pkResult = try? await executeRaw(sql: """
                SELECT primary_key FROM system.tables
                WHERE database = '\(escape(db))' AND name = '\(escape(table))'
                """)
            if let raw = pkResult?.rows.first?.first?.stringValue {
                pkSet = Set(raw.split(separator: ",").map {
                    $0.trimmingCharacters(in: .whitespaces)
                })
            }
        }

        return r.rows.enumerated().compactMap { idx, row -> ColumnInfo? in
            guard let colName = row[0].stringValue,
                  let dataType = row[1].stringValue else { return nil }
            let defKind = row[2].stringValue ?? ""
            let defExpr = row[3].stringValue ?? ""
            let comment = row[4].stringValue
            let isPK = hasPKFlag ? (row[5].intValue == 1) : pkSet.contains(colName)

            let isNullable = dataType.hasPrefix("Nullable(")
            let defaultValue: String?
            if defExpr.isEmpty {
                defaultValue = nil
            } else if defKind.isEmpty {
                defaultValue = defExpr
            } else {
                defaultValue = "\(defKind) \(defExpr)"
            }

            return ColumnInfo(
                name: colName,
                dataType: dataType,
                isNullable: isNullable,
                defaultValue: defaultValue,
                isPrimaryKey: isPK,
                isAutoIncrement: false,
                comment: (comment?.isEmpty == true) ? nil : comment,
                ordinalPosition: idx + 1,
                characterMaxLength: nil
            )
        }
    }

    /// `is_in_primary_key` was added in 21.3. Cache the check based on serverVersionCache.
    private func supportsIsInPrimaryKey() -> Bool {
        guard let v = serverVersionCache else { return true } // optimistic for unknown
        let parts = v.split(separator: ".").prefix(2).compactMap { Int($0) }
        guard parts.count == 2 else { return true }
        let (major, minor) = (parts[0], parts[1])
        if major > 21 { return true }
        if major == 21 && minor >= 3 { return true }
        return false
    }

    func listIndexes(table: String, schema: String?) async throws -> [IndexInfo] {
        let db = try resolveDB(schema)
        let r = try? await executeRaw(sql: """
            SELECT name, expr, type FROM system.data_skipping_indices
            WHERE database = '\(escape(db))' AND table = '\(escape(table))'
            ORDER BY name
            """)
        return (r?.rows ?? []).compactMap { row -> IndexInfo? in
            guard let name = row[0].stringValue else { return nil }
            let expr = row.count > 1 ? (row[1].stringValue ?? "") : ""
            let type = row.count > 2 ? row[2].stringValue : nil
            return IndexInfo(
                name: name,
                columns: [expr],
                isUnique: false,
                type: type,
                tableName: table
            )
        }
    }

    func listForeignKeys(table: String, schema: String?) async throws -> [ForeignKeyInfo] {
        [] // ClickHouse has no foreign keys.
    }

    func listFunctions(schema: String?) async throws -> [String] {
        let r = try? await executeRaw(sql: """
            SELECT name FROM system.functions
            WHERE is_aggregate = 0 AND origin = 'SQLUserDefined'
            ORDER BY name
            """)
        return (r?.rows ?? []).compactMap { $0.first?.stringValue }
    }

    func getFunctionSource(name: String, schema: String?) async throws -> String {
        let r = try await executeRaw(sql: """
            SELECT create_query FROM system.functions WHERE name = '\(escape(name))'
            """)
        return r.rows.first?.first?.stringValue ?? ""
    }

    // MARK: - Data Manipulation

    func insertRow(table: String, schema: String?, values: [String: RowValue]) async throws -> QueryResult {
        let d = SQLDialect.clickhouse
        let db = try resolveDB(schema)
        let qualified = qualifiedName(db: db, table: table, dialect: d)
        let keys = values.keys.sorted()
        let cols = keys.map { d.quoteIdentifier($0) }.joined(separator: ", ")
        let vals = keys.map { inlineValue(values[$0] ?? .null) }.joined(separator: ", ")
        return try await executeRaw(sql: "INSERT INTO \(qualified) (\(cols)) VALUES (\(vals))")
    }

    func updateRow(table: String, schema: String?, set: [String: RowValue], where whereClause: [String: RowValue]) async throws -> QueryResult {
        // ClickHouse uses ALTER TABLE ... UPDATE (async mutation).
        let d = SQLDialect.clickhouse
        let db = try resolveDB(schema)
        let qualified = qualifiedName(db: db, table: table, dialect: d)
        let setClauses = set.map { "\(d.quoteIdentifier($0.key)) = \(inlineValue($0.value))" }.joined(separator: ", ")
        let whereClauses = whereClause.map { "\(d.quoteIdentifier($0.key)) = \(inlineValue($0.value))" }.joined(separator: " AND ")
        return try await executeRaw(sql: "ALTER TABLE \(qualified) UPDATE \(setClauses) WHERE \(whereClauses)")
    }

    func deleteRow(table: String, schema: String?, where whereClause: [String: RowValue]) async throws -> QueryResult {
        let d = SQLDialect.clickhouse
        let db = try resolveDB(schema)
        let qualified = qualifiedName(db: db, table: table, dialect: d)
        let clauses = whereClause.map { "\(d.quoteIdentifier($0.key)) = \(inlineValue($0.value))" }.joined(separator: " AND ")
        return try await executeRaw(sql: "ALTER TABLE \(qualified) DELETE WHERE \(clauses)")
    }

    // MARK: - Transactions (no-op — ClickHouse has no traditional transactions)

    func beginTransaction() async throws {}
    func commitTransaction() async throws {}
    func rollbackTransaction() async throws {}

    // MARK: - Pagination

    func fetchRows(
        table: String,
        schema: String?,
        columns: [String]?,
        where filter: FilterExpression?,
        orderBy: [QuerySortDescriptor]?,
        limit: Int,
        offset: Int
    ) async throws -> QueryResult {
        let d = SQLDialect.clickhouse
        let db = try resolveDB(schema)
        let qualified = qualifiedName(db: db, table: table, dialect: d)
        let colList = columns?.map { d.quoteIdentifier($0) }.joined(separator: ", ") ?? "*"

        var sql = "SELECT \(colList) FROM \(qualified)"
        if let filter, !filter.conditions.isEmpty {
            sql += " WHERE \(filter.toSQL(dialect: d))"
        }
        if let orderBy, !orderBy.isEmpty {
            sql += " ORDER BY " + orderBy.map { $0.toSQL(dialect: d) }.joined(separator: ", ")
        }
        sql += " LIMIT \(limit) OFFSET \(offset)"
        return try await executeRaw(sql: sql)
    }

    // MARK: - Database management

    func createDatabase(name: String) async throws {
        let quoted = SQLDialect.clickhouse.quoteIdentifier(name)
        _ = try await executeRaw(sql: "CREATE DATABASE IF NOT EXISTS \(quoted)")
    }

    func dropDatabase(name: String) async throws {
        let quoted = SQLDialect.clickhouse.quoteIdentifier(name)
        _ = try await executeRaw(sql: "DROP DATABASE \(quoted)")
    }

    // MARK: - Info

    func serverVersion() async throws -> String {
        if let cached = serverVersionCache { return cached }
        return try await fetchServerVersion()
    }

    func currentDatabase() async throws -> String? {
        stateLock.lock()
        let cached = currentDB
        stateLock.unlock()
        if let cached, !cached.isEmpty { return cached }
        let r = try await executeRaw(sql: "SELECT currentDatabase()")
        let name = r.rows.first?.first?.stringValue
        if let name, !name.isEmpty {
            stateLock.lock()
            currentDB = name
            stateLock.unlock()
        }
        return name
    }

    private func fetchServerVersion() async throws -> String {
        let r = try await executeRaw(sql: "SELECT version()")
        let v = r.rows.first?.first?.stringValue ?? "ClickHouse"
        serverVersionCache = v
        return v
    }

    // MARK: - SchemaInspectable

    func fullSchemaSnapshot(database: String?) async throws -> SchemaSnapshot {
        let db: String
        if let explicit = database, !explicit.isEmpty {
            db = explicit
        } else {
            db = (try await currentDatabase()) ?? "default"
        }
        let tables = try await listTables(schema: db)
        let descs: [TableDescription] = try await withThrowingTaskGroup(of: TableDescription.self) { group in
            for t in tables {
                let name = t.name
                group.addTask { try await self.describeTable(name: name, schema: db) }
            }
            var results: [TableDescription] = []
            for try await desc in group { results.append(desc) }
            return results
        }
        let views = try await listViews(schema: db)
        let schemaInfo = SchemaInfo(name: db, tables: descs, views: views, functions: [], enums: [])
        return SchemaSnapshot(databaseName: db, databaseType: .clickhouse, schemas: [schemaInfo], capturedAt: Date())
    }

    func columnStatistics(table: String, schema: String?, sampleSize: Int) async throws -> [ColumnStatistics] {
        let db = try resolveDB(schema)
        let cols = try await describeColumns(table: table, schema: db)
        let d = SQLDialect.clickhouse
        let qualified = qualifiedName(db: db, table: table, dialect: d)
        var stats: [ColumnStatistics] = []
        for col in cols {
            let q = d.quoteIdentifier(col.name)
            let r = try? await executeRaw(sql: """
                SELECT count(DISTINCT \(q)),
                       countIf(\(q) IS NULL) / greatest(count(), 1),
                       toString(min(\(q))),
                       toString(max(\(q)))
                FROM (SELECT \(q) FROM \(qualified) LIMIT \(sampleSize)) AS sample
                """)
            if let row = r?.rows.first {
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
        let db = try resolveDB(schema)
        let r = try await executeRaw(sql: """
            SELECT total_rows FROM system.tables
            WHERE database = '\(escape(db))' AND name = '\(escape(table))'
            """)
        return r.rows.first?.first?.intValue ?? 0
    }

    func tableSizeBytes(table: String, schema: String?) async throws -> Int64? {
        let db = try resolveDB(schema)
        let r = try await executeRaw(sql: """
            SELECT total_bytes FROM system.tables
            WHERE database = '\(escape(db))' AND name = '\(escape(table))'
            """)
        if let v = r.rows.first?.first?.intValue { return Int64(v) }
        return nil
    }

    func queryStatistics() async throws -> [QueryStatisticsEntry] { [] }

    func primaryKeyColumns(table: String, schema: String?) async throws -> [String] {
        let db = try resolveDB(schema)
        if supportsIsInPrimaryKey() {
            let r = try await executeRaw(sql: """
                SELECT name FROM system.columns
                WHERE database = '\(escape(db))' AND table = '\(escape(table))' AND is_in_primary_key = 1
                ORDER BY position
                """)
            return r.rows.compactMap { $0.first?.stringValue }
        } else {
            let r = try await executeRaw(sql: """
                SELECT primary_key FROM system.tables
                WHERE database = '\(escape(db))' AND name = '\(escape(table))'
                """)
            guard let raw = r.rows.first?.first?.stringValue, !raw.isEmpty else { return [] }
            return raw.split(separator: ",").map { $0.trimmingCharacters(in: .whitespaces) }
        }
    }

    // MARK: - Helpers

    private func resolveDB(_ schema: String?) throws -> String {
        if let schema, !schema.isEmpty { return schema }
        stateLock.lock()
        let cur = currentDB
        stateLock.unlock()
        return cur ?? "default"
    }

    private func qualifiedName(db: String, table: String, dialect: SQLDialect) -> String {
        "\(dialect.quoteIdentifier(db)).\(dialect.quoteIdentifier(table))"
    }

    private func parseUseStatement(_ sql: String) -> String {
        // Handles: USE dbname, USE `dbname`, USE "dbname"; optional trailing ';'
        let after = sql.dropFirst(4).trimmingCharacters(in: .whitespacesAndNewlines)
        let stripped = after.trimmingCharacters(in: CharacterSet(charactersIn: "`\"'; "))
        return stripped
    }

    private func detectQueryType(_ upper: String) -> QueryType {
        if upper.hasPrefix("SELECT") || upper.hasPrefix("WITH") || upper.hasPrefix("EXPLAIN") || upper.hasPrefix("SHOW") || upper.hasPrefix("DESCRIBE") || upper.hasPrefix("DESC ") { return .select }
        if upper.hasPrefix("INSERT") { return .insert }
        if upper.hasPrefix("UPDATE") || upper.hasPrefix("ALTER TABLE") && upper.contains(" UPDATE ") { return .update }
        if upper.hasPrefix("DELETE") || upper.hasPrefix("ALTER TABLE") && upper.contains(" DELETE ") { return .delete }
        if upper.hasPrefix("CREATE") || upper.hasPrefix("ALTER") || upper.hasPrefix("DROP") || upper.hasPrefix("TRUNCATE") || upper.hasPrefix("RENAME") { return .ddl }
        return .other
    }

    private func escape(_ s: String) -> String {
        s.replacingOccurrences(of: "\\", with: "\\\\")
            .replacingOccurrences(of: "'", with: "\\'")
    }

    private func inlineValue(_ value: RowValue) -> String {
        switch value {
        case .null: return "NULL"
        case .string(let v): return "'\(escape(v))'"
        case .integer(let v): return "\(v)"
        case .double(let v): return "\(v)"
        case .boolean(let v): return v ? "1" : "0"
        case .date(let v):
            let fmt = DateFormatter()
            fmt.dateFormat = "yyyy-MM-dd HH:mm:ss"
            fmt.timeZone = TimeZone(identifier: "UTC")
            return "'\(fmt.string(from: v))'"
        case .uuid(let v): return "'\(v.uuidString)'"
        case .json(let v): return "'\(escape(v))'"
        case .data: return "NULL"
        case .array(let items):
            let parts = items.map { inlineValue($0) }.joined(separator: ", ")
            return "[\(parts)]"
        }
    }
}
