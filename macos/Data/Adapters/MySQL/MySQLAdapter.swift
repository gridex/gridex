// MySQLAdapter.swift
// Gridex
//
// MySQL database adapter using MySQLNIO.

import Foundation
import MySQLNIO
import NIOCore
import NIOPosix
import NIOSSL
import Logging

final class MySQLAdapter: DatabaseAdapter, SchemaInspectable, @unchecked Sendable {
    let databaseType: DatabaseType = .mysql
    private(set) var isConnected: Bool = false
    private var connection: MySQLConnection?
    private let eventLoopGroup = MultiThreadedEventLoopGroup(numberOfThreads: 2)
    private let logger = Logger(label: "com.gridex.mysql")
    private var cachedDatabase: String?

    // Stored credentials for auto-reconnect (in-memory only, cleared on disconnect)
    private var storedConfig: ConnectionConfig?
    private var storedPassword: String?

    // Reconnect coordination — prevents concurrent reconnect attempts
    private let stateLock = NSLock()
    private var reconnectTask: Task<Void, Error>?
    private var keepaliveTask: Task<Void, Never>?

    /// Keepalive interval. 30s is well under MySQL's default wait_timeout (28800s)
    /// but low enough to fit under aggressive SSH/firewall idle timeouts (often 60-300s).
    private static let keepaliveInterval: Duration = .seconds(30)

    deinit {
        keepaliveTask?.cancel()
        if let connection, !connection.isClosed {
            _ = connection.close()
        }
        try? eventLoopGroup.syncShutdownGracefully()
    }

    // MARK: - Connection

    func connect(config: ConnectionConfig, password: String?) async throws {
        try await openConnection(config: config, password: password)
        self.storedConfig = config
        self.storedPassword = password
        startKeepalive()
    }

    func disconnect() async throws {
        keepaliveTask?.cancel()
        keepaliveTask = nil
        storedConfig = nil
        storedPassword = nil

        if let connection, !connection.isClosed {
            _ = try await connection.close().get()
        }
        connection = nil
        isConnected = false
        cachedDatabase = nil
    }

    /// Opens a new MySQL connection using the given config. Does not mutate stored
    /// credentials — callers manage those (so reconnect doesn't overwrite them).
    private func openConnection(config: ConnectionConfig, password: String?) async throws {
        let host = config.host ?? "localhost"
        let port = config.port ?? 3306
        let username = config.username ?? "root"
        let database = config.database ?? ""

        let address: SocketAddress
        do {
            address = try SocketAddress.makeAddressResolvingHost(host, port: port)
        } catch {
            throw GridexError.connectionFailed(underlying: error)
        }

        let tlsConfig = try Self.makeTLSConfig(for: config.effectiveSSLMode, config: config)
        // SNI must be a DNS name (RFC 6066) and NIOSSL rejects IP literals outright, which
        // broke TLS for hosts like 127.0.0.1. Without a hostname, NIOSSL still verifies
        // identity (Verify Identity mode) against the connected IP via the cert's IP SANs.
        let isIPAddress = (try? SocketAddress(ipAddress: host, port: 0)) != nil

        do {
            let conn = try await MySQLConnection.connect(
                to: address,
                username: username,
                database: database,
                password: password,
                tlsConfiguration: tlsConfig,
                serverHostname: isIPAddress ? nil : host,
                logger: logger,
                on: eventLoopGroup.next()
            ).get()
            self.connection = conn
            self.isConnected = true
            self.cachedDatabase = nil
        } catch {
            throw GridexError.connectionFailed(underlying: error)
        }
    }

    private static func makeTLSConfig(
        for mode: SSLMode,
        config: ConnectionConfig
    ) throws -> TLSConfiguration? {
        if mode == .disabled { return nil }

        var tls = TLSConfiguration.makeClientConfiguration()

        // mTLS client cert (e.g. Teleport).
        if let certPath = config.sslCertPath, !certPath.isEmpty,
           let keyPath = config.sslKeyPath, !keyPath.isEmpty {
            let cert = try NIOSSLCertificate.fromPEMFile(certPath)
            let key = try NIOSSLPrivateKey(file: keyPath, format: .pem)
            tls.certificateChain = cert.map { .certificate($0) }
            tls.privateKey = .privateKey(key)
        }

        // MySQL CLI semantics: PREFERRED/REQUIRED encrypt without verifying;
        // VERIFY_CA verifies the chain; VERIFY_IDENTITY also verifies hostname.
        switch mode {
        case .verifyCA, .verifyIdentity:
            if let caPath = config.sslCACertPath, !caPath.isEmpty {
                tls.trustRoots = .file(caPath)
            }
            tls.certificateVerification = (mode == .verifyIdentity) ? .fullVerification : .noHostnameVerification
        default:
            tls.certificateVerification = .none
        }

        return tls
    }

    func testConnection(config: ConnectionConfig, password: String?) async throws -> Bool {
        let adapter = MySQLAdapter()
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
        try await ensureLive()
        let startTime = CFAbsoluteTimeGetCurrent()
        let upper = query.trimmingCharacters(in: .whitespacesAndNewlines).uppercased()
        let queryType = detectQueryType(upper)

        guard let connection else { throw GridexError.queryExecutionFailed("Not connected to MySQL") }

        let rows: [MySQLRow]
        do {
            rows = try await connection.simpleQuery(query).get()
        } catch {
            // Connection may have died mid-query (e.g. server idle timeout after our
            // ensureLive check). Try to reconnect once and re-run the query.
            if isConnectionDrop(error) {
                logger.info("MySQL query failed with connection drop; reconnecting and retrying")
                try await forceReconnect()
                guard let live = self.connection else {
                    throw GridexError.queryExecutionFailed("Not connected to MySQL")
                }
                rows = try await live.simpleQuery(query).get()
            } else {
                throw error
            }
        }
        let duration = CFAbsoluteTimeGetCurrent() - startTime

        if queryType == .select || !rows.isEmpty {
            let columns: [ColumnHeader]
            if let first = rows.first {
                columns = first.columnDefinitions.map { col in
                    ColumnHeader(name: col.name, dataType: mysqlDataTypeName(col.columnType))
                }
            } else {
                columns = []
            }

            let resultRows: [[RowValue]] = rows.map { row in
                row.columnDefinitions.enumerated().map { idx, col in
                    let data = MySQLData(
                        type: col.columnType,
                        format: row.format,
                        buffer: row.values[idx],
                        isUnsigned: col.flags.contains(.COLUMN_UNSIGNED)
                    )
                    return decodeData(data, columnType: col.columnType)
                }
            }

            return QueryResult(columns: columns, rows: resultRows, rowsAffected: resultRows.count, executionTime: duration, queryType: queryType)
        } else {
            return QueryResult(columns: [], rows: [], rowsAffected: 0, executionTime: duration, queryType: queryType)
        }
    }

    func executeRaw(sql: String) async throws -> QueryResult {
        try await execute(query: sql, parameters: nil)
    }

    // MARK: - Schema Inspection

    func listDatabases() async throws -> [String] {
        let result = try await executeRaw(sql: "SHOW DATABASES")
        return result.rows.compactMap { $0.first?.stringValue }
    }

    func listSchemas(database: String?) async throws -> [String] {
        try await listDatabases()
    }

    func listTables(schema: String?) async throws -> [TableInfo] {
        let db = try await resolveDB(schema)
        let result = try await executeRaw(sql: """
            SELECT TABLE_NAME, TABLE_ROWS
            FROM information_schema.TABLES
            WHERE TABLE_SCHEMA = '\(escapeSQL(db))' AND TABLE_TYPE = 'BASE TABLE'
            ORDER BY TABLE_NAME
            """)
        return result.rows.compactMap { row -> TableInfo? in
            guard let name = row.first?.stringValue else { return nil }
            let count = row.count > 1 ? row[1].intValue : nil
            return TableInfo(name: name, schema: db, type: .table, estimatedRowCount: count)
        }
    }

    func listViews(schema: String?) async throws -> [ViewInfo] {
        let db = try await resolveDB(schema)
        let result = try await executeRaw(sql: """
            SELECT TABLE_NAME, VIEW_DEFINITION
            FROM information_schema.VIEWS
            WHERE TABLE_SCHEMA = '\(escapeSQL(db))'
            ORDER BY TABLE_NAME
            """)
        return result.rows.compactMap { row -> ViewInfo? in
            guard let name = row.first?.stringValue else { return nil }
            let def = row.count > 1 ? row[1].stringValue : nil
            return ViewInfo(name: name, schema: db, definition: def, isMaterialized: false)
        }
    }

    func describeTable(name: String, schema: String?) async throws -> TableDescription {
        let db = try await resolveDB(schema)

        // Run column/index/FK queries in parallel
        async let columnsTask = describeColumns(table: name, schema: db)
        async let indexesTask = listIndexes(table: name, schema: db)
        async let fksTask = listForeignKeys(table: name, schema: db)

        // Get row count + comment in one query from information_schema.TABLES
        let metaResult = try await executeRaw(sql: """
            SELECT TABLE_ROWS, TABLE_COMMENT FROM information_schema.TABLES
            WHERE TABLE_SCHEMA = '\(escapeSQL(db))' AND TABLE_NAME = '\(escapeSQL(name))'
            """)
        let count = metaResult.rows.first?[0].intValue
        let comment = metaResult.rows.first?[1].stringValue

        let columns = try await columnsTask
        let indexes = try await indexesTask
        let fks = try await fksTask

        return TableDescription(name: name, schema: db, columns: columns, indexes: indexes, foreignKeys: fks, constraints: [], comment: comment, estimatedRowCount: count)
    }

    private func describeColumns(table: String, schema: String) async throws -> [ColumnInfo] {
        let result = try await executeRaw(sql: """
            SELECT COLUMN_NAME, COLUMN_TYPE, IS_NULLABLE, COLUMN_DEFAULT,
                   ORDINAL_POSITION, CHARACTER_MAXIMUM_LENGTH, COLUMN_KEY, EXTRA, COLUMN_COMMENT
            FROM information_schema.COLUMNS
            WHERE TABLE_SCHEMA = '\(escapeSQL(schema))' AND TABLE_NAME = '\(escapeSQL(table))'
            ORDER BY ORDINAL_POSITION
            """)

        return result.rows.map { row in
            let name = row[0].stringValue ?? ""
            let dataType = row[1].stringValue ?? "varchar"
            let nullable = row[2].stringValue == "YES"
            let defVal = row[3].stringValue
            let ordinal = row[4].intValue ?? 0
            let maxLen = row[5].intValue
            let columnKey = row[6].stringValue ?? ""
            let extra = row[7].stringValue ?? ""
            let comment = row[8].stringValue

            return ColumnInfo(
                name: name, dataType: dataType, isNullable: nullable, defaultValue: defVal,
                isPrimaryKey: columnKey == "PRI",
                isAutoIncrement: extra.contains("auto_increment"),
                comment: comment?.isEmpty == true ? nil : comment,
                ordinalPosition: ordinal, characterMaxLength: maxLen
            )
        }
    }

    func listIndexes(table: String, schema: String?) async throws -> [IndexInfo] {
        let db = try await resolveDB(schema)
        let result = try await executeRaw(sql: """
            SELECT INDEX_NAME, GROUP_CONCAT(COLUMN_NAME ORDER BY SEQ_IN_INDEX), NON_UNIQUE, INDEX_TYPE
            FROM information_schema.STATISTICS
            WHERE TABLE_SCHEMA = '\(escapeSQL(db))' AND TABLE_NAME = '\(escapeSQL(table))'
            GROUP BY INDEX_NAME, NON_UNIQUE, INDEX_TYPE
            ORDER BY INDEX_NAME
            """)

        return result.rows.compactMap { row -> IndexInfo? in
            guard let name = row[0].stringValue else { return nil }
            let cols = row[1].stringValue?.split(separator: ",").map(String.init) ?? []
            let nonUnique = row[2].stringValue == "1" || row[2].intValue == 1
            let type = row[3].stringValue
            return IndexInfo(name: name, columns: cols, isUnique: !nonUnique, type: type, tableName: table)
        }
    }

    func listForeignKeys(table: String, schema: String?) async throws -> [ForeignKeyInfo] {
        let db = try await resolveDB(schema)
        let result = try await executeRaw(sql: """
            SELECT kcu.CONSTRAINT_NAME, kcu.COLUMN_NAME, kcu.REFERENCED_TABLE_NAME, kcu.REFERENCED_COLUMN_NAME,
                   rc.DELETE_RULE, rc.UPDATE_RULE
            FROM information_schema.KEY_COLUMN_USAGE kcu
            JOIN information_schema.REFERENTIAL_CONSTRAINTS rc
              ON rc.CONSTRAINT_SCHEMA = kcu.TABLE_SCHEMA AND rc.CONSTRAINT_NAME = kcu.CONSTRAINT_NAME
            WHERE kcu.TABLE_SCHEMA = '\(escapeSQL(db))' AND kcu.TABLE_NAME = '\(escapeSQL(table))'
              AND kcu.REFERENCED_TABLE_NAME IS NOT NULL
            ORDER BY kcu.CONSTRAINT_NAME
            """)

        return result.rows.compactMap { row -> ForeignKeyInfo? in
            let name = row[0].stringValue
            guard let col = row[1].stringValue,
                  let refTable = row[2].stringValue,
                  let refCol = row[3].stringValue else { return nil }
            let onDelete = ForeignKeyAction(rawValue: row[4].stringValue ?? "NO ACTION") ?? .noAction
            let onUpdate = ForeignKeyAction(rawValue: row[5].stringValue ?? "NO ACTION") ?? .noAction
            return ForeignKeyInfo(name: name, columns: [col], referencedTable: refTable, referencedColumns: [refCol], onDelete: onDelete, onUpdate: onUpdate)
        }
    }

    func listFunctions(schema: String?) async throws -> [String] {
        let db = try await resolveDB(schema)
        let result = try await executeRaw(sql: """
            SELECT ROUTINE_NAME FROM information_schema.ROUTINES
            WHERE ROUTINE_SCHEMA = '\(escapeSQL(db))'
            ORDER BY ROUTINE_NAME
            """)
        return result.rows.compactMap { $0[0].stringValue }
    }

    func getFunctionSource(name: String, schema: String?) async throws -> String {
        let result = try await executeRaw(sql: "SHOW CREATE FUNCTION \(quoteIdentifier(name))")
        guard let source = result.rows.first?[2].stringValue else {
            throw GridexError.queryExecutionFailed("Function '\(name)' not found")
        }
        return source
    }

    // MARK: - Data Manipulation

    func insertRow(table: String, schema: String?, values: [String: RowValue]) async throws -> QueryResult {
        let d = SQLDialect.mysql
        let cols = values.keys.map { d.quoteIdentifier($0) }.joined(separator: ", ")
        let vals = values.values.map { inlineValue($0) }.joined(separator: ", ")
        return try await executeRaw(sql: "INSERT INTO \(d.quoteIdentifier(table)) (\(cols)) VALUES (\(vals))")
    }

    func updateRow(table: String, schema: String?, set values: [String: RowValue], where conditions: [String: RowValue]) async throws -> QueryResult {
        let d = SQLDialect.mysql
        let setClauses = values.map { "\(d.quoteIdentifier($0.key)) = \(inlineValue($0.value))" }.joined(separator: ", ")
        let whereClauses = conditions.map { "\(d.quoteIdentifier($0.key)) = \(inlineValue($0.value))" }.joined(separator: " AND ")
        return try await executeRaw(sql: "UPDATE \(d.quoteIdentifier(table)) SET \(setClauses) WHERE \(whereClauses)")
    }

    func deleteRow(table: String, schema: String?, where conditions: [String: RowValue]) async throws -> QueryResult {
        let d = SQLDialect.mysql
        let whereClauses = conditions.map { "\(d.quoteIdentifier($0.key)) = \(inlineValue($0.value))" }.joined(separator: " AND ")
        return try await executeRaw(sql: "DELETE FROM \(d.quoteIdentifier(table)) WHERE \(whereClauses)")
    }

    func beginTransaction() async throws { _ = try await executeRaw(sql: "START TRANSACTION") }
    func commitTransaction() async throws { _ = try await executeRaw(sql: "COMMIT") }
    func rollbackTransaction() async throws { _ = try await executeRaw(sql: "ROLLBACK") }

    // MARK: - Pagination

    func fetchRows(table: String, schema: String?, columns: [String]?, where filter: FilterExpression?, orderBy: [QuerySortDescriptor]?, limit: Int, offset: Int) async throws -> QueryResult {
        let d = SQLDialect.mysql
        let colList = columns?.map { d.quoteIdentifier($0) }.joined(separator: ", ") ?? "*"
        var sql = "SELECT \(colList) FROM \(d.quoteIdentifier(table))"
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
        let r = try await executeRaw(sql: "SELECT VERSION()")
        return r.rows.first?.first?.stringValue ?? "MySQL"
    }

    func currentDatabase() async throws -> String? {
        if let cached = cachedDatabase { return cached }
        let r = try await executeRaw(sql: "SELECT DATABASE()")
        let db = r.rows.first?.first?.stringValue
        cachedDatabase = db
        return db
    }

    /// Resolve the effective database name, using cached value to avoid extra query
    private func resolveDB(_ schema: String?) async throws -> String {
        if let schema, !schema.isEmpty { return schema }
        return try await currentDatabase() ?? ""
    }

    // MARK: - SchemaInspectable

    func fullSchemaSnapshot(database: String?) async throws -> SchemaSnapshot {
        let currentDB = try await currentDatabase()
        let dbName = database ?? currentDB ?? "mysql"
        let tables = try await listTables(schema: dbName)
        let descs: [TableDescription] = try await withThrowingTaskGroup(of: TableDescription.self) { group in
            for t in tables {
                let name = t.name
                group.addTask { try await self.describeTable(name: name, schema: dbName) }
            }
            var results: [TableDescription] = []
            for try await desc in group { results.append(desc) }
            return results
        }
        let views = try await listViews(schema: dbName)
        let schemaInfo = SchemaInfo(name: dbName, tables: descs, views: views, functions: [], enums: [])
        return SchemaSnapshot(databaseName: dbName, databaseType: .mysql, schemas: [schemaInfo], capturedAt: Date())
    }

    func columnStatistics(table: String, schema: String?, sampleSize: Int) async throws -> [ColumnStatistics] {
        let db = try await resolveDB(schema)
        let cols = try await describeColumns(table: table, schema: db)
        var stats: [ColumnStatistics] = []
        let d = SQLDialect.mysql
        for col in cols {
            let q = d.quoteIdentifier(col.name)
            let tbl = d.quoteIdentifier(table)
            let r = try await executeRaw(sql: """
                SELECT COUNT(DISTINCT \(q)),
                       SUM(CASE WHEN \(q) IS NULL THEN 1 ELSE 0 END) / GREATEST(COUNT(*), 1),
                       MIN(\(q)), MAX(\(q))
                FROM (SELECT \(q) FROM \(tbl) LIMIT \(sampleSize)) AS sample
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
        // Use fast estimated count from InnoDB stats instead of slow COUNT(*)
        let db = try await resolveDB(schema)
        let r = try await executeRaw(sql: """
            SELECT TABLE_ROWS FROM information_schema.TABLES
            WHERE TABLE_SCHEMA = '\(escapeSQL(db))' AND TABLE_NAME = '\(escapeSQL(table))'
            """)
        return r.rows.first?.first?.intValue ?? 0
    }

    func tableSizeBytes(table: String, schema: String?) async throws -> Int64? {
        let db = try await resolveDB(schema)
        let r = try await executeRaw(sql: """
            SELECT DATA_LENGTH + INDEX_LENGTH FROM information_schema.TABLES
            WHERE TABLE_SCHEMA = '\(escapeSQL(db))' AND TABLE_NAME = '\(escapeSQL(table))'
            """)
        if let val = r.rows.first?.first?.intValue { return Int64(val) }
        return nil
    }

    func queryStatistics() async throws -> [QueryStatisticsEntry] { [] }

    func primaryKeyColumns(table: String, schema: String?) async throws -> [String] {
        let db = try await resolveDB(schema)
        let r = try await executeRaw(sql: """
            SELECT COLUMN_NAME FROM information_schema.KEY_COLUMN_USAGE
            WHERE TABLE_SCHEMA = '\(escapeSQL(db))' AND TABLE_NAME = '\(escapeSQL(table))'
              AND CONSTRAINT_NAME = 'PRIMARY'
            ORDER BY ORDINAL_POSITION
            """)
        return r.rows.compactMap { $0.first?.stringValue }
    }

    // MARK: - Resilience (keepalive + auto-reconnect)

    /// Ensures the MySQL connection is alive. If closed, attempts to re-open using
    /// stored credentials. Concurrent callers coalesce onto a single reconnect task.
    private func ensureLive() async throws {
        stateLock.lock()
        let needsReconnect = connection == nil || connection?.isClosed == true || !isConnected
        if !needsReconnect {
            stateLock.unlock()
            return
        }

        guard let config = storedConfig else {
            stateLock.unlock()
            throw GridexError.queryExecutionFailed("Not connected to MySQL")
        }

        if let existing = reconnectTask {
            stateLock.unlock()
            try await existing.value
            return
        }

        let password = storedPassword
        let task: Task<Void, Error> = Task { [weak self] in
            guard let self else { return }
            try await self.openConnection(config: config, password: password)
        }
        reconnectTask = task
        stateLock.unlock()

        defer {
            stateLock.lock()
            if self.reconnectTask == task { self.reconnectTask = nil }
            stateLock.unlock()
        }
        try await task.value
    }

    /// Forces a reconnect regardless of current state. Used after a mid-query failure.
    private func forceReconnect() async throws {
        stateLock.lock()
        // Tear down the existing connection so ensureLive sees it as dead
        if let conn = connection, !conn.isClosed {
            _ = try? await conn.close().get()
        }
        connection = nil
        isConnected = false
        stateLock.unlock()
        try await ensureLive()
    }

    private func isConnectionDrop(_ error: Error) -> Bool {
        // MySQLNIO surfaces connection drops as various NIO/Channel errors. Also
        // treat any error as a drop if the connection object is now closed.
        if let conn = connection, conn.isClosed { return true }
        let desc = String(describing: error).lowercased()
        return desc.contains("closed")
            || desc.contains("ioerror")
            || desc.contains("connection")
            || desc.contains("channel")
    }

    private func startKeepalive() {
        keepaliveTask?.cancel()
        keepaliveTask = Task { [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(for: MySQLAdapter.keepaliveInterval)
                guard let self, !Task.isCancelled else { return }
                // If connection died, proactively heal so the next user query is fast.
                if let conn = self.connection, !conn.isClosed {
                    _ = try? await conn.simpleQuery("SELECT 1").get()
                } else if self.storedConfig != nil {
                    try? await self.ensureLive()
                }
            }
        }
    }

    // MARK: - Helpers

    private func detectQueryType(_ upper: String) -> QueryType {
        if upper.hasPrefix("SELECT") || upper.hasPrefix("EXPLAIN") || upper.hasPrefix("SHOW") || upper.hasPrefix("DESCRIBE") || upper.hasPrefix("WITH") { return .select }
        if upper.hasPrefix("INSERT") { return .insert }
        if upper.hasPrefix("UPDATE") { return .update }
        if upper.hasPrefix("DELETE") { return .delete }
        if upper.hasPrefix("CREATE") || upper.hasPrefix("ALTER") || upper.hasPrefix("DROP") { return .ddl }
        return .other
    }

    private func decodeData(_ data: MySQLData, columnType: MySQLProtocol.DataType) -> RowValue {
        if data.buffer == nil { return .null }

        switch columnType {
        case .tiny:
            if let v = data.bool { return .boolean(v) }
            if let v = data.int64 { return .integer(v) }
            return data.string.map { .string($0) } ?? .null
        case .short, .long, .longlong, .int24:
            if let v = data.int64 { return .integer(v) }
            if data.isUnsigned, let v = data.uint64 { return .integer(Int64(bitPattern: v)) }
            return data.string.map { .string($0) } ?? .null
        case .float, .double, .decimal, .newdecimal:
            if let v = data.double { return .double(v) }
            return data.string.map { .string($0) } ?? .null
        case .date, .datetime, .timestamp:
            if let v = data.date { return .date(v) }
            return data.string.map { .string($0) } ?? .null
        case .time, .year:
            return data.string.map { .string($0) } ?? .null
        case .json:
            return data.string.map { .json($0) } ?? .null
        case .blob, .tinyBlob, .mediumBlob, .longBlob:
            if let buf = data.buffer {
                return .data(Data(buf.readableBytesView))
            }
            return .null
        case .varchar, .varString, .string, .enum, .set:
            return data.string.map { .string($0) } ?? .null
        default:
            return data.string.map { .string($0) } ?? .null
        }
    }

    private func mysqlDataTypeName(_ type: MySQLProtocol.DataType) -> String {
        switch type {
        case .tiny: return "TINYINT"
        case .short: return "SMALLINT"
        case .long: return "INT"
        case .longlong: return "BIGINT"
        case .int24: return "MEDIUMINT"
        case .float: return "FLOAT"
        case .double: return "DOUBLE"
        case .decimal, .newdecimal: return "DECIMAL"
        case .varchar, .varString: return "VARCHAR"
        case .string: return "CHAR"
        case .blob: return "BLOB"
        case .tinyBlob: return "TINYBLOB"
        case .mediumBlob: return "MEDIUMBLOB"
        case .longBlob: return "LONGBLOB"
        case .date: return "DATE"
        case .datetime: return "DATETIME"
        case .timestamp: return "TIMESTAMP"
        case .time: return "TIME"
        case .year: return "YEAR"
        case .json: return "JSON"
        case .enum: return "ENUM"
        case .set: return "SET"
        default: return "UNKNOWN"
        }
    }

    private func inlineValue(_ value: RowValue) -> String {
        switch value {
        case .null: return "NULL"
        case .string(let v): return "'\(escapeSQL(v))'"
        case .integer(let v): return "\(v)"
        case .double(let v): return "\(v)"
        case .boolean(let v): return v ? "1" : "0"
        case .date(let v): return "'\(ISO8601DateFormatter().string(from: v))'"
        case .uuid(let v): return "'\(v.uuidString)'"
        case .json(let v): return "'\(escapeSQL(v))'"
        case .data: return "NULL"
        case .array: return "NULL"
        }
    }

    /// Escape a string value for safe inclusion in a single-quoted SQL literal.
    /// Handles all special characters that MySQL interprets within string literals.
    private func escapeSQL(_ str: String) -> String {
        var result = ""
        result.reserveCapacity(str.count)
        for char in str {
            switch char {
            case "\0": result += "\\0"
            case "\n": result += "\\n"
            case "\r": result += "\\r"
            case "\\": result += "\\\\"
            case "'":  result += "''"
            case "\"": result += "\\\""
            case "\u{1a}": result += "\\Z"  // Ctrl+Z / SUB
            default: result.append(char)
            }
        }
        return result
    }

    /// Quote an identifier (table, column, schema name) using backticks with proper escaping.
    private func quoteIdentifier(_ name: String) -> String {
        "`\(name.replacingOccurrences(of: "`", with: "``"))`"
    }
}
