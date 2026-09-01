// RedisAdapter.swift
// Gridex
//
// Redis adapter — maps the key-value store into Gridex's relational model.
//
// Virtual schema:
//   • "Keys" table with columns: key, type, value, ttl
//   • SCAN-based pagination for fetchRows
//   • executeRaw accepts raw Redis commands (space-separated)

import Foundation
import NIOCore
import NIOPosix
import NIOSSL
import RediStack

final class RedisAdapter: DatabaseAdapter, @unchecked Sendable {

    // MARK: - Properties

    let databaseType: DatabaseType = .redis
    private(set) var isConnected: Bool = false

    private var connection: RedisConnection?
    private let eventLoopGroup = MultiThreadedEventLoopGroup(numberOfThreads: 1)
    private var currentDB: Int = 0

    // MARK: - Connection Lifecycle

    func connect(config: ConnectionConfig, password: String?) async throws {
        let host = config.host ?? "localhost"
        let port = config.port ?? 6379
        let eventLoop = eventLoopGroup.next()

        let redisConfig = try RedisConnection.Configuration(
            hostname: host,
            port: port,
            password: (password?.isEmpty == false) ? password : nil,
            initialDatabase: config.database.flatMap { Int($0) },
            defaultLogger: .init(label: "com.gridex.redis")
        )

        // Build a ClientBootstrap — add TLS handler if sslEnabled (e.g. Upstash, rediss://)
        let bootstrap: ClientBootstrap
        if config.sslEnabled {
            let tlsConfig = TLSConfiguration.makeClientConfiguration()
            let sslContext = try NIOSSLContext(configuration: tlsConfig)
            bootstrap = ClientBootstrap(group: eventLoop)
                .channelOption(ChannelOptions.socket(SocketOptionLevel(SOL_SOCKET), SO_REUSEADDR), value: 1)
                .channelInitializer { channel in
                    do {
                        let sslHandler = try NIOSSLClientHandler(context: sslContext, serverHostname: host)
                        return channel.pipeline.addHandler(sslHandler).flatMap {
                            channel.pipeline.addBaseRedisHandlers()
                        }
                    } catch {
                        return channel.eventLoop.makeFailedFuture(error)
                    }
                }
        } else {
            bootstrap = ClientBootstrap.makeRedisTCPClient(group: eventLoop)
        }

        let conn = try await RedisConnection.make(
            configuration: redisConfig,
            boundEventLoop: eventLoop,
            configuredTCPClient: bootstrap
        ).get()

        self.connection = conn
        self.currentDB = config.database.flatMap { Int($0) } ?? 0
        self.isConnected = true
    }

    func disconnect() async throws {
        connection?.close()
        isConnected = false
        connection = nil
    }

    func testConnection(config: ConnectionConfig, password: String?) async throws -> Bool {
        try await connect(config: config, password: password)
        let pong = try await sendCommand("PING")
        try await disconnect()
        return pong.string == "PONG"
    }

    // MARK: - Command Execution

    @discardableResult
    private func sendCommand(_ command: String, args: [String] = []) async throws -> RESPValue {
        guard let conn = connection else {
            throw GridexError.connectionFailed(underlying: NSError(
                domain: "Redis", code: -1,
                userInfo: [NSLocalizedDescriptionKey: "Not connected to Redis"]))
        }
        let respArgs = args.map { RESPValue(from: $0) }
        return try await conn.send(command: command, with: respArgs).get()
    }

    /// Parse a space-separated Redis command string (respecting quoted strings).
    private func parseCommand(_ raw: String) -> [String] {
        var tokens: [String] = []
        var current = ""
        var inQuote: Character?

        for ch in raw {
            if let q = inQuote {
                if ch == q { inQuote = nil } else { current.append(ch) }
            } else if ch == "\"" || ch == "'" {
                inQuote = ch
            } else if ch == " " {
                if !current.isEmpty { tokens.append(current); current = "" }
            } else {
                current.append(ch)
            }
        }
        if !current.isEmpty { tokens.append(current) }
        return tokens
    }

    // MARK: - DatabaseAdapter — Query Execution

    func execute(query: String, parameters: [QueryParameter]?) async throws -> QueryResult {
        try await executeRaw(sql: query)
    }

    func executeRaw(sql: String) async throws -> QueryResult {
        let start = CFAbsoluteTimeGetCurrent()
        let tokens = parseCommand(sql.trimmingCharacters(in: .whitespacesAndNewlines))
        guard let keyword = tokens.first?.uppercased() else {
            throw GridexError.queryExecutionFailed("Empty command")
        }
        let args = Array(tokens.dropFirst())
        let resp = try await sendCommand(keyword, args: args)
        let elapsed = CFAbsoluteTimeGetCurrent() - start

        let (columns, rows, affected) = formatRESP(resp, keyword: keyword)
        return QueryResult(
            columns: columns,
            rows: rows,
            rowsAffected: affected,
            executionTime: elapsed,
            queryType: redisQueryType(keyword)
        )
    }

    func executeWithRowValues(sql: String, parameters: [RowValue]) async throws -> QueryResult {
        try await executeRaw(sql: sql)
    }

    // MARK: - RESP → QueryResult Formatting

    private func formatRESP(_ value: RESPValue, keyword: String) -> ([ColumnHeader], [[RowValue]], Int) {
        switch value {
        case .simpleString(let buf):
            let str = buf.getString(at: 0, length: buf.readableBytes) ?? ""
            return (
                [ColumnHeader(name: "result", dataType: "string", isNullable: false, tableName: nil)],
                [[.string(str)]],
                0
            )

        case .bulkString(let buf):
            if let buf {
                let str = buf.getString(at: 0, length: buf.readableBytes) ?? ""
                return (
                    [ColumnHeader(name: "value", dataType: "string", isNullable: true, tableName: nil)],
                    [[.string(str)]],
                    0
                )
            }
            return (
                [ColumnHeader(name: "value", dataType: "string", isNullable: true, tableName: nil)],
                [[.null]],
                0
            )

        case .integer(let n):
            return (
                [ColumnHeader(name: "result", dataType: "integer", isNullable: false, tableName: nil)],
                [[.integer(Int64(n))]],
                Int(n)
            )

        case .array(let items):
            return formatArrayResponse(items, keyword: keyword)

        case .error(let err):
            return (
                [ColumnHeader(name: "error", dataType: "string", isNullable: false, tableName: nil)],
                [[.string(err.message)]],
                0
            )

        case .null:
            return (
                [ColumnHeader(name: "value", dataType: "string", isNullable: true, tableName: nil)],
                [[.null]],
                0
            )
        }
    }

    private func formatArrayResponse(_ items: [RESPValue], keyword: String) -> ([ColumnHeader], [[RowValue]], Int) {
        // HGETALL / CONFIG GET: alternating key-value pairs
        if ["HGETALL", "CONFIG"].contains(keyword) && items.count % 2 == 0 && !items.isEmpty {
            let cols = [
                ColumnHeader(name: "field", dataType: "string", isNullable: false, tableName: nil),
                ColumnHeader(name: "value", dataType: "string", isNullable: true, tableName: nil),
            ]
            var rows: [[RowValue]] = []
            for i in stride(from: 0, to: items.count, by: 2) {
                rows.append([respToRowValue(items[i]), respToRowValue(items[i + 1])])
            }
            return (cols, rows, 0)
        }

        // Generic array
        let cols = [
            ColumnHeader(name: "#", dataType: "integer", isNullable: false, tableName: nil),
            ColumnHeader(name: "value", dataType: "string", isNullable: true, tableName: nil),
        ]
        let rows = items.enumerated().map { (i, item) -> [RowValue] in
            [.integer(Int64(i + 1)), respToRowValue(item)]
        }
        return (cols, rows, 0)
    }

    private func respToRowValue(_ value: RESPValue) -> RowValue {
        switch value {
        case .simpleString(let buf):
            return .string(buf.getString(at: 0, length: buf.readableBytes) ?? "")
        case .bulkString(let buf):
            guard let buf else { return .null }
            return .string(buf.getString(at: 0, length: buf.readableBytes) ?? "")
        case .integer(let n):
            return .integer(Int64(n))
        case .array(let arr):
            return .string(arr.map { respToRowValue($0).stringValue ?? "" }.joined(separator: ", "))
        case .error(let err):
            return .string("ERR: \(err.message)")
        case .null:
            return .null
        }
    }

    private func redisQueryType(_ keyword: String) -> QueryType {
        switch keyword {
        case "SET", "MSET", "HSET", "LPUSH", "RPUSH", "SADD", "ZADD":
            return .insert
        case "DEL", "HDEL", "LREM", "SREM", "ZREM", "UNLINK":
            return .delete
        case "GET", "MGET", "HGETALL", "HGET", "LRANGE", "SMEMBERS", "ZRANGE",
             "KEYS", "SCAN", "INFO", "DBSIZE", "TYPE", "TTL", "PTTL":
            return .select
        default:
            return .other
        }
    }

    // MARK: - Schema Inspection (virtual)

    func listDatabases() async throws -> [String] {
        // CONFIG GET is blocked on many cloud providers (Upstash, ElastiCache).
        // Fall back to default 16 databases on error.
        do {
            let resp = try await sendCommand("CONFIG", args: ["GET", "databases"])
            if case .array(let items) = resp, items.count >= 2,
               let str = respToRowValue(items[1]).stringValue, let n = Int(str) {
                return (0..<n).map { "db\($0)" }
            }
        } catch {}
        return (0..<16).map { "db\($0)" }
    }

    func listSchemas(database: String?) async throws -> [String] { [] }

    func listTables(schema: String?) async throws -> [TableInfo] {
        [TableInfo(name: "Keys", schema: nil, type: .table, estimatedRowCount: nil)]
    }

    func listViews(schema: String?) async throws -> [ViewInfo] { [] }

    func describeTable(name: String, schema: String?) async throws -> TableDescription {
        TableDescription(
            name: "Keys",
            schema: nil,
            columns: [
                ColumnInfo(name: "key", dataType: "string", isNullable: false, defaultValue: nil,
                           isPrimaryKey: true, isAutoIncrement: false, comment: nil, ordinalPosition: 1, characterMaxLength: nil),
                ColumnInfo(name: "type", dataType: "string", isNullable: false, defaultValue: nil,
                           isPrimaryKey: false, isAutoIncrement: false, comment: nil, ordinalPosition: 2, characterMaxLength: nil),
                ColumnInfo(name: "value", dataType: "string", isNullable: true, defaultValue: nil,
                           isPrimaryKey: false, isAutoIncrement: false, comment: nil, ordinalPosition: 3, characterMaxLength: nil),
                ColumnInfo(name: "ttl", dataType: "integer", isNullable: true, defaultValue: nil,
                           isPrimaryKey: false, isAutoIncrement: false, comment: nil, ordinalPosition: 4, characterMaxLength: nil),
            ],
            indexes: [],
            foreignKeys: [],
            constraints: [],
            comment: nil,
            estimatedRowCount: nil
        )
    }

    func listIndexes(table: String, schema: String?) async throws -> [IndexInfo] { [] }
    func listForeignKeys(table: String, schema: String?) async throws -> [ForeignKeyInfo] { [] }
    func listFunctions(schema: String?) async throws -> [String] { [] }
    func getFunctionSource(name: String, schema: String?) async throws -> String { "" }

    // MARK: - Data Manipulation

    func insertRow(table: String, schema: String?, values: [String: RowValue]) async throws -> QueryResult {
        guard let key = values["key"]?.stringValue else {
            throw GridexError.queryExecutionFailed("Missing 'key' field")
        }
        let val = values["value"]?.stringValue ?? ""
        let start = CFAbsoluteTimeGetCurrent()
        try await sendCommand("SET", args: [key, val])
        return QueryResult(columns: [], rows: [], rowsAffected: 1,
                           executionTime: CFAbsoluteTimeGetCurrent() - start, queryType: .insert)
    }

    func updateRow(table: String, schema: String?, set: [String: RowValue], where: [String: RowValue]) async throws -> QueryResult {
        guard let key = `where`["key"]?.stringValue else {
            throw GridexError.queryExecutionFailed("Missing 'key' in WHERE clause")
        }
        let start = CFAbsoluteTimeGetCurrent()
        if let newVal = set["value"]?.stringValue {
            try await sendCommand("SET", args: [key, newVal])
        } else if let ttl = set["ttl"]?.intValue {
            if ttl < 0 {
                try await sendCommand("PERSIST", args: [key])
            } else {
                try await sendCommand("EXPIRE", args: [key, String(ttl)])
            }
        } else {
            throw GridexError.queryExecutionFailed("Nothing to update")
        }
        return QueryResult(columns: [], rows: [], rowsAffected: 1,
                           executionTime: CFAbsoluteTimeGetCurrent() - start, queryType: .update)
    }

    func deleteRow(table: String, schema: String?, where: [String: RowValue]) async throws -> QueryResult {
        guard let key = `where`["key"]?.stringValue else {
            throw GridexError.queryExecutionFailed("Missing 'key' in WHERE clause")
        }
        let start = CFAbsoluteTimeGetCurrent()
        try await sendCommand("DEL", args: [key])
        return QueryResult(columns: [], rows: [], rowsAffected: 1,
                           executionTime: CFAbsoluteTimeGetCurrent() - start, queryType: .delete)
    }

    // MARK: - Transactions

    func beginTransaction() async throws {
        try await sendCommand("MULTI")
    }

    func commitTransaction() async throws {
        try await sendCommand("EXEC")
    }

    func rollbackTransaction() async throws {
        try await sendCommand("DISCARD")
    }

    // MARK: - Pagination (SCAN-based)

    func scanKeyNames(
        matching pattern: String = "*",
        maximumCount: Int = 10_000
    ) async throws -> RedisKeyScanResult {
        var cursor = 0
        var accumulator = RedisKeyScanAccumulator(maximumCount: maximumCount)

        repeat {
            guard connection?.isConnected == true else {
                throw GridexError.queryExecutionFailed("Connection lost during SCAN")
            }

            let response: RESPValue
            do {
                response = try await sendCommand(
                    "SCAN",
                    args: [String(cursor), "MATCH", pattern, "COUNT", "500"]
                )
            } catch {
                guard connection?.isConnected == true else {
                    throw GridexError.queryExecutionFailed("Connection lost during SCAN")
                }
                throw error
            }

            guard case .array(let parts) = response, parts.count == 2 else { break }
            cursor = Int(respToRowValue(parts[0]).stringValue ?? "0") ?? 0

            if case .array(let keys) = parts[1] {
                accumulator.append(keys.compactMap { respToRowValue($0).stringValue })
            }
        } while cursor != 0 && !accumulator.isFull

        return accumulator.result(hasMoreCursor: cursor != 0)
    }

    func fetchRows(
        table: String,
        schema: String?,
        columns: [String]?,
        where filter: FilterExpression?,
        orderBy: [QuerySortDescriptor]?,
        limit: Int,
        offset: Int
    ) async throws -> QueryResult {
        let start = CFAbsoluteTimeGetCurrent()
        let pattern = extractPattern(from: filter) ?? "*"

        // Collect keys using SCAN (capped at 100k keys for safety)
        var cursor = 0
        var allKeys: [String] = []
        var scanIterations = 0
        repeat {
            guard connection?.isConnected == true else {
                throw GridexError.queryExecutionFailed("Connection lost during SCAN")
            }
            let resp = try await sendCommand("SCAN", args: [String(cursor), "MATCH", pattern, "COUNT", "500"])
            if case .array(let parts) = resp, parts.count == 2 {
                cursor = Int(respToRowValue(parts[0]).stringValue ?? "0") ?? 0
                if case .array(let keys) = parts[1] {
                    allKeys.append(contentsOf: keys.compactMap { respToRowValue($0).stringValue })
                }
            } else { break }
            scanIterations += 1
            if allKeys.count >= 100_000 || scanIterations >= 500 { break }
        } while cursor != 0

        allKeys.sort()
        let paged = Array(allKeys.dropFirst(offset).prefix(limit))

        // Fire TYPE and TTL for all keys concurrently via NIO futures (single round-trip batch)
        guard let conn = connection else {
            throw GridexError.connectionFailed(underlying: NSError(
                domain: "Redis", code: -1, userInfo: [NSLocalizedDescriptionKey: "Not connected"]))
        }

        // Phase 1: TYPE + TTL for all keys (2 commands per key, all pipelined)
        let typeFutures = paged.map { conn.send(command: "TYPE", with: [RESPValue(from: $0)]) }
        let ttlFutures = paged.map { conn.send(command: "TTL", with: [RESPValue(from: $0)]) }

        let types = try await withCheckedThrowingContinuation { (cont: CheckedContinuation<[RESPValue], Error>) in
            EventLoopFuture.whenAllSucceed(typeFutures, on: conn.eventLoop).whenComplete {
                switch $0 {
                case .success(let v): cont.resume(returning: v)
                case .failure(let e): cont.resume(throwing: e)
                }
            }
        }
        let ttls = try await withCheckedThrowingContinuation { (cont: CheckedContinuation<[RESPValue], Error>) in
            EventLoopFuture.whenAllSucceed(ttlFutures, on: conn.eventLoop).whenComplete {
                switch $0 {
                case .success(let v): cont.resume(returning: v)
                case .failure(let e): cont.resume(throwing: e)
                }
            }
        }

        // Phase 2: fetch values based on type (pipelined)
        var valueFutures: [EventLoopFuture<RESPValue>] = []
        for (i, key) in paged.enumerated() {
            let typeStr = respToRowValue(types[i]).stringValue ?? "unknown"
            let keyResp = RESPValue(from: key)
            switch typeStr {
            case "string":
                valueFutures.append(conn.send(command: "GET", with: [keyResp]))
            case "list":
                valueFutures.append(conn.send(command: "LRANGE", with: [keyResp, RESPValue(from: "0"), RESPValue(from: "99")]))
            case "set":
                valueFutures.append(conn.send(command: "SMEMBERS", with: [keyResp]))
            case "zset":
                valueFutures.append(conn.send(command: "ZRANGE", with: [keyResp, RESPValue(from: "0"), RESPValue(from: "99"), RESPValue(from: "WITHSCORES")]))
            case "hash":
                valueFutures.append(conn.send(command: "HGETALL", with: [keyResp]))
            default:
                valueFutures.append(conn.eventLoop.makeSucceededFuture(.simpleString(ByteBuffer(string: "(\(typeStr))"))))
            }
        }

        let values = try await withCheckedThrowingContinuation { (cont: CheckedContinuation<[RESPValue], Error>) in
            EventLoopFuture.whenAllSucceed(valueFutures, on: conn.eventLoop).whenComplete {
                switch $0 {
                case .success(let v): cont.resume(returning: v)
                case .failure(let e): cont.resume(throwing: e)
                }
            }
        }

        // Assemble rows
        var rows: [[RowValue]] = []
        for (i, key) in paged.enumerated() {
            let typeStr = respToRowValue(types[i]).stringValue ?? "unknown"
            let ttlVal: RowValue = {
                if case .integer(let n) = ttls[i] { return n < 0 ? .null : .integer(Int64(n)) }
                return .null
            }()

            let value: String
            switch typeStr {
            case "string":
                value = respToRowValue(values[i]).stringValue ?? ""
            case "list", "set", "zset":
                value = formatListValue(values[i])
            case "hash":
                value = formatHashValue(values[i])
            default:
                value = respToRowValue(values[i]).stringValue ?? "(unknown)"
            }

            rows.append([.string(key), .string(typeStr), .string(value), ttlVal])
        }

        let elapsed = CFAbsoluteTimeGetCurrent() - start
        return QueryResult(
            columns: [
                ColumnHeader(name: "key", dataType: "string", isNullable: false, tableName: "Keys"),
                ColumnHeader(name: "type", dataType: "string", isNullable: false, tableName: "Keys"),
                ColumnHeader(name: "value", dataType: "string", isNullable: true, tableName: "Keys"),
                ColumnHeader(name: "ttl", dataType: "integer", isNullable: true, tableName: "Keys"),
            ],
            rows: rows,
            rowsAffected: 0,
            executionTime: elapsed,
            queryType: .select
        )
    }

    // MARK: - Database Info

    func serverVersion() async throws -> String {
        let resp = try await sendCommand("INFO", args: ["server"])
        if let info = respToRowValue(resp).stringValue,
           let line = info.split(separator: "\n").first(where: { $0.hasPrefix("redis_version:") }) {
            return String(line.split(separator: ":").last ?? "unknown")
        }
        return "unknown"
    }

    func currentDatabase() async throws -> String? {
        "db\(currentDB)"
    }

    // MARK: - Helpers

    private func extractPattern(from filter: FilterExpression?) -> String? {
        guard let filter else { return nil }
        for cond in filter.conditions {
            if cond.column == "key" {
                switch cond.op {
                case .equal:
                    return cond.value.stringValue
                case .like:
                    return cond.value.stringValue?
                        .replacingOccurrences(of: "%", with: "*")
                        .replacingOccurrences(of: "_", with: "?")
                default:
                    continue
                }
            }
        }
        return nil
    }

    private func formatListValue(_ resp: RESPValue) -> String {
        if case .array(let items) = resp {
            let values = items.map { respToRowValue($0).stringValue ?? "" }
            return "[\(values.joined(separator: ", "))]"
        }
        return "[]"
    }

    private func formatHashValue(_ resp: RESPValue) -> String {
        if case .array(let items) = resp, items.count % 2 == 0 {
            var pairs: [String] = []
            for i in stride(from: 0, to: items.count, by: 2) {
                let k = respToRowValue(items[i]).stringValue ?? ""
                let v = respToRowValue(items[i + 1]).stringValue ?? ""
                pairs.append("\(k): \(v)")
            }
            return "{\(pairs.joined(separator: ", "))}"
        }
        return "{}"
    }

    // MARK: - Redis-Specific Operations

    /// Create a key with a specific Redis data type.
    func redisInsertTyped(key: String, type: RedisKeyType, data: RedisKeyData, ttl: Int? = nil) async throws {
        switch data {
        case .string(let value):
            try await sendCommand("SET", args: [key, value])
        case .hash(let fields):
            var args = [key]
            for f in fields { args.append(contentsOf: [f.field, f.value]) }
            try await sendCommand("HSET", args: args)
        case .list(let items):
            try await sendCommand("RPUSH", args: [key] + items)
        case .set(let members):
            try await sendCommand("SADD", args: [key] + members)
        case .zset(let members):
            var args = [key]
            for m in members { args.append(contentsOf: [String(m.score), m.member]) }
            try await sendCommand("ZADD", args: args)
        }
        if let ttl, ttl > 0 {
            try await sendCommand("EXPIRE", args: [key, String(ttl)])
        }
    }

    /// Fetch detailed data for a single key.
    func fetchKeyDetail(key: String) async throws -> RedisKeyDetail {
        let typeStr = try await respToRowValue(sendCommand("TYPE", args: [key])).stringValue ?? "none"
        let ttlVal = try await sendCommand("TTL", args: [key])
        let ttl: Int? = {
            if case .integer(let n) = ttlVal { return n < 0 ? nil : Int(n) }
            return nil
        }()

        let memoryBytes: Int?
        do {
            let mem = try await sendCommand("MEMORY", args: ["USAGE", key])
            if case .integer(let n) = mem { memoryBytes = Int(n) } else { memoryBytes = nil }
        } catch { memoryBytes = nil }

        let keyType = RedisKeyType(rawValue: typeStr) ?? .string
        let data: RedisKeyData
        switch keyType {
        case .string:
            let v = try await respToRowValue(sendCommand("GET", args: [key])).stringValue ?? ""
            data = .string(value: v)
        case .hash:
            let resp = try await sendCommand("HGETALL", args: [key])
            var fields: [(field: String, value: String)] = []
            if case .array(let items) = resp, items.count >= 2 {
                for i in stride(from: 0, to: items.count, by: 2) {
                    fields.append((
                        field: respToRowValue(items[i]).stringValue ?? "",
                        value: respToRowValue(items[i + 1]).stringValue ?? ""
                    ))
                }
            }
            data = .hash(fields: fields)
        case .list:
            // Cap at 10,000 items to prevent OOM
            let resp = try await sendCommand("LRANGE", args: [key, "0", "9999"])
            let items: [String] = {
                if case .array(let arr) = resp { return arr.map { respToRowValue($0).stringValue ?? "" } }
                return []
            }()
            data = .list(items: items)
        case .set:
            // SSCAN with limit to prevent OOM on huge sets
            var members: [String] = []
            var setCursor = 0
            repeat {
                let resp = try await sendCommand("SSCAN", args: [key, String(setCursor), "COUNT", "1000"])
                if case .array(let parts) = resp, parts.count == 2 {
                    setCursor = Int(respToRowValue(parts[0]).stringValue ?? "0") ?? 0
                    if case .array(let items) = parts[1] {
                        members.append(contentsOf: items.compactMap { respToRowValue($0).stringValue })
                    }
                } else { break }
                if members.count >= 10000 { break }
            } while setCursor != 0
            data = .set(members: members.sorted())
        case .zset:
            let resp = try await sendCommand("ZRANGE", args: [key, "0", "9999", "WITHSCORES"])
            var members: [(member: String, score: Double)] = []
            if case .array(let arr) = resp, arr.count >= 2 {
                for i in stride(from: 0, to: arr.count, by: 2) {
                    let m = respToRowValue(arr[i]).stringValue ?? ""
                    let s = Double(respToRowValue(arr[i + 1]).stringValue ?? "0") ?? 0
                    members.append((member: m, score: s))
                }
            }
            data = .zset(members: members)
        }

        return RedisKeyDetail(key: key, type: keyType, ttl: ttl, data: data, memoryBytes: memoryBytes)
    }

    // Hash field operations
    func updateHashField(key: String, field: String, value: String) async throws {
        try await sendCommand("HSET", args: [key, field, value])
    }
    func deleteHashField(key: String, field: String) async throws {
        try await sendCommand("HDEL", args: [key, field])
    }

    // List operations
    func updateListItem(key: String, index: Int, value: String) async throws {
        try await sendCommand("LSET", args: [key, String(index), value])
    }

    // Set operations
    func addSetMember(key: String, member: String) async throws {
        try await sendCommand("SADD", args: [key, member])
    }
    func removeSetMember(key: String, member: String) async throws {
        try await sendCommand("SREM", args: [key, member])
    }

    // Sorted set operations
    func addZSetMember(key: String, member: String, score: Double) async throws {
        try await sendCommand("ZADD", args: [key, String(score), member])
    }
    func removeZSetMember(key: String, member: String) async throws {
        try await sendCommand("ZREM", args: [key, member])
    }

    // TTL management
    func setTTL(key: String, seconds: Int) async throws {
        try await sendCommand("EXPIRE", args: [key, String(seconds)])
    }
    func removeTTL(key: String) async throws {
        try await sendCommand("PERSIST", args: [key])
    }

    // Key management
    func renameKey(oldName: String, newName: String) async throws {
        try await sendCommand("RENAME", args: [oldName, newName])
    }
    func duplicateKey(source: String, destination: String) async throws {
        // Try COPY first (Redis 6.2+), fall back to type-aware copy
        do {
            try await sendCommand("COPY", args: [source, destination])
        } catch {
            let detail = try await fetchKeyDetail(key: source)
            try await redisInsertTyped(key: destination, type: detail.type, data: detail.data, ttl: detail.ttl)
        }
    }

    // Server info
    func serverInfoSections() async throws -> [RedisInfoSection] {
        let resp = try await sendCommand("INFO")
        guard let raw = respToRowValue(resp).stringValue else { return [] }
        var sections: [RedisInfoSection] = []
        var currentName = "Server"
        var currentEntries: [(key: String, value: String)] = []
        // Redis INFO uses \r\n line endings
        for line in raw.components(separatedBy: .newlines) {
            let trimmed = line.trimmingCharacters(in: .whitespacesAndNewlines)
            if trimmed.isEmpty { continue }
            if trimmed.hasPrefix("# ") {
                if !currentEntries.isEmpty {
                    sections.append(RedisInfoSection(name: currentName, entries: currentEntries))
                    currentEntries = []
                }
                currentName = String(trimmed.dropFirst(2))
            } else if let colonIdx = trimmed.firstIndex(of: ":") {
                let k = String(trimmed[trimmed.startIndex..<colonIdx])
                let v = String(trimmed[trimmed.index(after: colonIdx)...])
                currentEntries.append((key: k, value: v))
            }
        }
        if !currentEntries.isEmpty {
            sections.append(RedisInfoSection(name: currentName, entries: currentEntries))
        }
        return sections
    }

    // DBSIZE
    func dbSize() async throws -> Int {
        let resp = try await sendCommand("DBSIZE")
        if case .integer(let n) = resp { return Int(n) }
        return 0
    }

    // Flush
    func flushDB() async throws {
        try await sendCommand("FLUSHDB")
    }

    // Memory usage
    func memoryUsage(key: String) async throws -> Int? {
        do {
            let resp = try await sendCommand("MEMORY", args: ["USAGE", key])
            if case .integer(let n) = resp { return Int(n) }
        } catch {}
        return nil
    }

    // Slow log
    func slowLog(count: Int = 50) async throws -> [RedisSlowLogEntry] {
        let resp = try await sendCommand("SLOWLOG", args: ["GET", String(count)])
        guard case .array(let entries) = resp else { return [] }
        return entries.compactMap { entry -> RedisSlowLogEntry? in
            guard case .array(let parts) = entry, parts.count >= 4 else { return nil }
            let id = { if case .integer(let n) = parts[0] { return Int(n) }; return 0 }()
            let ts = { if case .integer(let n) = parts[1] { return Date(timeIntervalSince1970: Double(n)) }; return Date() }()
            let dur = { if case .integer(let n) = parts[2] { return Int(n) }; return 0 }()
            let cmd: String = {
                if case .array(let args) = parts[3] {
                    return args.map { respToRowValue($0).stringValue ?? "" }.joined(separator: " ")
                }
                return ""
            }()
            let client = parts.count > 4 ? (respToRowValue(parts[4]).stringValue ?? "") : ""
            return RedisSlowLogEntry(id: id, timestamp: ts, durationMicros: dur, command: cmd, clientInfo: client)
        }
    }

    // MARK: - Backup / Restore

    /// A single Redis key entry for backup. Value is type-specific:
    /// - string: `.string(String)`
    /// - list: `.list([String])`
    /// - set: `.set([String])`
    /// - zset: `.zset([(String, Double)])` — member + score
    /// - hash: `.hash([String: String])`
    struct RedisKeyEntry {
        let key: String
        let type: String
        let ttl: Int?
        let value: Value

        enum Value {
            case string(String)
            case list([String])
            case set([String])
            case zset([(String, Double)])
            case hash([String: String])
        }
    }

    /// Iterate all keys using SCAN and yield batches for backup.
    func backupScanAll(batchSize: Int = 100, onBatch: @escaping ([RedisKeyEntry]) async throws -> Void, onProgress: ((Int64, TimeInterval) -> Void)? = nil) async throws {
        let start = CFAbsoluteTimeGetCurrent()
        var cursor = "0"
        var totalKeys: Int64 = 0
        var batch: [RedisKeyEntry] = []

        repeat {
            let scanResp = try await sendCommand("SCAN", args: [cursor, "COUNT", String(batchSize)])
            guard case .array(let scanArr) = scanResp, scanArr.count >= 2,
                  let newCursor = respToRowValue(scanArr[0]).stringValue else {
                break
            }
            cursor = newCursor
            guard case .array(let keys) = scanArr[1] else { continue }

            for keyResp in keys {
                guard let key = respToRowValue(keyResp).stringValue else { continue }
                if let entry = try? await fetchKeyEntry(key: key) {
                    batch.append(entry)
                }
                totalKeys += 1
                if batch.count >= batchSize {
                    try await onBatch(batch)
                    batch = []
                }
                onProgress?(totalKeys, CFAbsoluteTimeGetCurrent() - start)
            }
        } while cursor != "0"

        if !batch.isEmpty {
            try await onBatch(batch)
        }
    }

    /// Fetch a single key with its type-specific value + TTL.
    private func fetchKeyEntry(key: String) async throws -> RedisKeyEntry? {
        let typeResp = try await sendCommand("TYPE", args: [key])
        guard let type = typeResp.string else { return nil }

        let ttlResp = try await sendCommand("TTL", args: [key])
        let ttl: Int? = {
            if case .integer(let n) = ttlResp, n > 0 { return Int(n) }
            return nil
        }()

        let value: RedisKeyEntry.Value
        switch type {
        case "string":
            let resp = try await sendCommand("GET", args: [key])
            value = .string(respToRowValue(resp).stringValue ?? "")
        case "list":
            let resp = try await sendCommand("LRANGE", args: [key, "0", "-1"])
            if case .array(let items) = resp {
                value = .list(items.compactMap { respToRowValue($0).stringValue })
            } else { value = .list([]) }
        case "set":
            let resp = try await sendCommand("SMEMBERS", args: [key])
            if case .array(let items) = resp {
                value = .set(items.compactMap { respToRowValue($0).stringValue })
            } else { value = .set([]) }
        case "zset":
            let resp = try await sendCommand("ZRANGE", args: [key, "0", "-1", "WITHSCORES"])
            if case .array(let items) = resp {
                var pairs: [(String, Double)] = []
                var i = 0
                while i + 1 < items.count {
                    let member = respToRowValue(items[i]).stringValue ?? ""
                    let score = Double(respToRowValue(items[i + 1]).stringValue ?? "0") ?? 0
                    pairs.append((member, score))
                    i += 2
                }
                value = .zset(pairs)
            } else { value = .zset([]) }
        case "hash":
            let resp = try await sendCommand("HGETALL", args: [key])
            if case .array(let items) = resp {
                var dict: [String: String] = [:]
                var i = 0
                while i + 1 < items.count {
                    let k = respToRowValue(items[i]).stringValue ?? ""
                    let v = respToRowValue(items[i + 1]).stringValue ?? ""
                    dict[k] = v
                    i += 2
                }
                value = .hash(dict)
            } else { value = .hash([:]) }
        default:
            return nil
        }

        return RedisKeyEntry(key: key, type: type, ttl: ttl, value: value)
    }

    /// Restore a single key entry.
    func restoreKeyEntry(_ entry: RedisKeyEntry) async throws {
        // Delete any existing key first to ensure clean restore
        _ = try? await sendCommand("DEL", args: [entry.key])

        switch entry.value {
        case .string(let s):
            _ = try await sendCommand("SET", args: [entry.key, s])
        case .list(let items):
            if !items.isEmpty {
                _ = try await sendCommand("RPUSH", args: [entry.key] + items)
            }
        case .set(let members):
            if !members.isEmpty {
                _ = try await sendCommand("SADD", args: [entry.key] + members)
            }
        case .zset(let pairs):
            var args = [entry.key]
            for (member, score) in pairs {
                args.append(String(score))
                args.append(member)
            }
            if pairs.count > 0 {
                _ = try await sendCommand("ZADD", args: args)
            }
        case .hash(let dict):
            var args = [entry.key]
            for (k, v) in dict {
                args.append(k)
                args.append(v)
            }
            if !dict.isEmpty {
                _ = try await sendCommand("HSET", args: args)
            }
        }

        if let ttl = entry.ttl {
            _ = try await sendCommand("EXPIRE", args: [entry.key, String(ttl)])
        }
    }

    deinit {
        connection?.close()
        try? eventLoopGroup.syncShutdownGracefully()
    }
}
