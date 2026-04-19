// MongoDBAdapter.swift
// Gridex
//
// MongoDB adapter — maps document collections into Gridex's relational model.
//
// Mapping:
//   • Each MongoDB collection appears as a "table" in the sidebar
//   • Documents are flattened: each top-level field becomes a column
//   • Nested objects/arrays are rendered as JSON strings
//   • executeRaw accepts a JSON command body that gets sent via runCommand()

import Foundation
import MongoKitten
import MongoClient
import NIOCore

final class MongoDBAdapter: DatabaseAdapter, @unchecked Sendable {

    // MARK: - Properties

    let databaseType: DatabaseType = .mongodb
    private(set) var isConnected: Bool = false

    private var database: MongoDatabase?
    private var connectionConfig: ConnectionConfig?

    // MARK: - Connection Lifecycle

    func connect(config: ConnectionConfig, password: String?) async throws {
        let uri = buildURI(config: config, password: password)

        do {
            self.database = try await MongoDatabase.connect(to: uri)
            self.connectionConfig = config
            self.isConnected = true
        } catch {
            throw GridexError.connectionFailed(underlying: Self.wrap(mongoError: error))
        }
    }

    /// Build a MongoDB URI from a ConnectionConfig.
    /// If the host field already contains a full mongodb:// or mongodb+srv:// URI,
    /// it is used as-is, but the database segment is overridden when config.database is set.
    private func buildURI(config: ConnectionConfig, password: String?) -> String {
        let hostField = config.host ?? "localhost"

        // If user pasted a raw URI in host field, use it (with database override if set)
        if hostField.hasPrefix("mongodb://") || hostField.hasPrefix("mongodb+srv://") {
            return overrideDatabase(in: hostField, with: config.database)
        }

        let port = config.port ?? 27017
        let dbName = config.database ?? "admin"
        let hasCredentials = !(config.username?.isEmpty ?? true)

        var uri = "mongodb://"
        if let user = config.username, !user.isEmpty {
            uri += user.addingPercentEncoding(withAllowedCharacters: .urlUserAllowed) ?? user
            if let pw = password, !pw.isEmpty {
                uri += ":" + (pw.addingPercentEncoding(withAllowedCharacters: .urlPasswordAllowed) ?? pw)
            }
            uri += "@"
        }
        uri += "\(hostField):\(port)/\(dbName)"

        // Most MongoDB deployments store users in the `admin` database. Without
        // an explicit authSource, MongoKitten authenticates against the URI's
        // database and the server returns a generic auth failure.
        var params: [String] = []
        if hasCredentials {
            params.append("authSource=admin")
        }
        if config.sslEnabled {
            params.append("tls=true")
        }
        if !params.isEmpty {
            uri += "?" + params.joined(separator: "&")
        }
        return uri
    }

    /// Replace the /database segment of a mongodb URI with a new database name.
    /// Preserves user:pass@host[:port] and any ?query string after the database.
    private func overrideDatabase(in uri: String, with newDB: String?) -> String {
        guard let newDB = newDB, !newDB.isEmpty else { return uri }

        let prefix: String
        let rest: String
        if uri.hasPrefix("mongodb+srv://") {
            prefix = "mongodb+srv://"
            rest = String(uri.dropFirst(prefix.count))
        } else if uri.hasPrefix("mongodb://") {
            prefix = "mongodb://"
            rest = String(uri.dropFirst(prefix.count))
        } else {
            return uri
        }

        // Split off query string
        var pathPart = rest
        var query = ""
        if let qIdx = rest.firstIndex(of: "?") {
            pathPart = String(rest[..<qIdx])
            query = String(rest[qIdx...])
        }

        // Find the / that separates host from database (skip past credentials @)
        let searchStart: String.Index
        if let atIdx = pathPart.lastIndex(of: "@") {
            searchStart = pathPart.index(after: atIdx)
        } else {
            searchStart = pathPart.startIndex
        }

        if let slashIdx = pathPart[searchStart...].firstIndex(of: "/") {
            // Replace existing database segment
            let hostPart = String(pathPart[..<slashIdx])
            return prefix + hostPart + "/" + newDB + query
        } else {
            // No database in URI — append it
            return prefix + pathPart + "/" + newDB + query
        }
    }

    /// MongoKitten error types lack NSError bridging, so the default
    /// `localizedDescription` is the unhelpful "operation couldn't be completed
    /// (… error 1.)". Extract `errmsg`/`codeName` from the server reply when
    /// available so the user sees the real failure reason.
    private static func wrap(mongoError error: Error) -> NSError {
        let message: String
        if let serverError = error as? MongoServerError {
            let doc = serverError.document
            let msg = (doc["errmsg"] as? String) ?? "MongoDB server returned an error"
            let codeName = (doc["codeName"] as? String).map { " (\($0))" } ?? ""
            message = msg + codeName
        } else if let reply = error as? MongoGenericErrorReply {
            let msg = reply.errorMessage ?? "MongoDB server returned an error"
            let codeName = reply.codeName.map { " (\($0))" } ?? ""
            message = msg + codeName
        } else if let described = error as? CustomStringConvertible {
            message = described.description
        } else {
            message = error.localizedDescription
        }
        return NSError(
            domain: "MongoDB",
            code: -1,
            userInfo: [NSLocalizedDescriptionKey: message]
        )
    }

    func disconnect() async throws {
        // MongoKitten cleans up via deinit; just clear references
        database = nil
        connectionConfig = nil
        isConnected = false
    }

    func testConnection(config: ConnectionConfig, password: String?) async throws -> Bool {
        try await connect(config: config, password: password)
        // Run a ping command to verify
        do {
            let _ = try await listCollectionNames()
            try await disconnect()
            return true
        } catch {
            try? await disconnect()
            throw error
        }
    }

    // MARK: - Helpers

    private func requireDatabase() throws -> MongoDatabase {
        guard let db = database else {
            throw GridexError.connectionFailed(underlying: NSError(
                domain: "MongoDB", code: -1,
                userInfo: [NSLocalizedDescriptionKey: "Not connected to MongoDB"]))
        }
        return db
    }

    private func listCollectionNames() async throws -> [String] {
        let db = try requireDatabase()
        let collections = try await db.listCollections()
        return collections.map(\.name).sorted()
    }

    // Convert a BSON Primitive into Gridex's RowValue
    private func primitiveToRowValue(_ value: Primitive?) -> RowValue {
        guard let value = value else { return .null }
        if value is Null { return .null }
        if let s = value as? String { return .string(s) }
        if let i = value as? Int32 { return .integer(Int64(i)) }
        if let i = value as? Int64 { return .integer(i) }
        if let i = value as? Int { return .integer(Int64(i)) }
        if let d = value as? Double { return .double(d) }
        if let b = value as? Bool { return .boolean(b) }
        if let date = value as? Date { return .date(date) }
        if let oid = value as? ObjectId { return .string(oid.hexString) }
        if let doc = value as? Document {
            // Render nested document as JSON
            return .json(documentToJSONString(doc))
        }
        // Arrays are also Documents in BSON; fall back to string
        return .string(String(describing: value))
    }

    private func documentToJSONString(_ doc: Document) -> String {
        // Best-effort JSON representation. BSON has more types than JSON,
        // so this is lossy but readable.
        // Use keys/subscript instead of for-in to sidestep any iterator quirks
        // when sub-documents have unusual encodings.
        if doc.isArray {
            var items: [String] = []
            for key in doc.keys {
                items.append(primitiveToJSON(doc[key]))
            }
            return "[" + items.joined(separator: ",") + "]"
        }
        var pairs: [String] = []
        for key in doc.keys {
            let escapedKey = "\"\(key)\""
            let val = primitiveToJSON(doc[key])
            pairs.append("\(escapedKey):\(val)")
        }
        return "{" + pairs.joined(separator: ",") + "}"
    }

    private func primitiveToJSON(_ value: Primitive?) -> String {
        guard let value = value else { return "null" }
        if value is Null { return "null" }
        if let s = value as? String { return "\"\(s.replacingOccurrences(of: "\"", with: "\\\""))\"" }
        if let i = value as? Int32 { return String(i) }
        if let i = value as? Int64 { return String(i) }
        if let i = value as? Int { return String(i) }
        if let d = value as? Double { return String(d) }
        if let b = value as? Bool { return b ? "true" : "false" }
        if let oid = value as? ObjectId { return "\"\(oid.hexString)\"" }
        if let date = value as? Date {
            let fmt = ISO8601DateFormatter()
            return "\"\(fmt.string(from: date))\""
        }
        if let doc = value as? Document { return documentToJSONString(doc) }
        return "\"\(String(describing: value).replacingOccurrences(of: "\"", with: "\\\""))\""
    }

    // MARK: - DatabaseAdapter — Query Execution

    func execute(query: String, parameters: [QueryParameter]?) async throws -> QueryResult {
        try await executeRaw(sql: query)
    }

    func executeRaw(sql: String) async throws -> QueryResult {
        // Accept raw shell-like syntax: "collection.find()" or JSON command body
        // For MVP: parse "collectionName.find()" / "collectionName.count()"
        let start = CFAbsoluteTimeGetCurrent()
        let trimmed = sql.trimmingCharacters(in: .whitespacesAndNewlines)

        // Match patterns like: collectionName.find() or collectionName.count()
        if let match = trimmed.range(of: #"^(\w+)\.(\w+)\(\s*\)$"#, options: .regularExpression) {
            let parts = trimmed[match].split(separator: ".")
            guard parts.count >= 2 else {
                throw GridexError.queryExecutionFailed("Invalid query syntax. Use: collectionName.find() or collectionName.count()")
            }
            let coll = String(parts[0])
            let op = String(parts[1].dropLast(2)) // strip "()"

            switch op {
            case "find":
                let result = try await fetchRows(table: coll, schema: nil, columns: nil, where: nil, orderBy: nil, limit: 100, offset: 0)
                return QueryResult(
                    columns: result.columns,
                    rows: result.rows,
                    rowsAffected: 0,
                    executionTime: CFAbsoluteTimeGetCurrent() - start,
                    queryType: .select
                )
            case "count":
                let db = try requireDatabase()
                let count = try await db[coll].count()
                return QueryResult(
                    columns: [ColumnHeader(name: "count", dataType: "integer", isNullable: false)],
                    rows: [[.integer(Int64(count))]],
                    rowsAffected: 0,
                    executionTime: CFAbsoluteTimeGetCurrent() - start,
                    queryType: .select
                )
            default:
                throw GridexError.queryExecutionFailed("Unsupported operation: \(op). Try collectionName.find() or .count()")
            }
        }

        throw GridexError.queryExecutionFailed("Use the syntax: collectionName.find() or collectionName.count()")
    }

    func executeWithRowValues(sql: String, parameters: [RowValue]) async throws -> QueryResult {
        try await executeRaw(sql: sql)
    }

    // MARK: - Schema Inspection

    func listDatabases() async throws -> [String] {
        let db = try requireDatabase()
        let dbs = try await db.pool.listDatabases()
        return dbs.map { $0.name }.sorted()
    }

    func listSchemas(database: String?) async throws -> [String] { [] }

    func listTables(schema: String?) async throws -> [TableInfo] {
        let names = try await listCollectionNames()
        var infos: [TableInfo] = []
        let db = try requireDatabase()
        for name in names {
            let count = try? await db[name].count()
            infos.append(TableInfo(name: name, schema: nil, type: .table, estimatedRowCount: count))
        }
        return infos
    }

    func listViews(schema: String?) async throws -> [ViewInfo] { [] }

    func describeTable(name: String, schema: String?) async throws -> TableDescription {
        let db = try requireDatabase()
        let collection = db[name]

        // Sample up to 10 documents to infer the schema (including nested fields).
        let samples = try await collection.find().limit(10).drain()

        var fieldOrder: [String] = []
        var fieldTypes: [String: String] = [:]
        for doc in samples {
            collectFieldPaths(doc, prefix: "", order: &fieldOrder, types: &fieldTypes)
        }

        // Always ensure _id appears first
        if let idx = fieldOrder.firstIndex(of: "_id"), idx != 0 {
            fieldOrder.remove(at: idx)
            fieldOrder.insert("_id", at: 0)
        } else if !fieldOrder.contains("_id") {
            fieldOrder.insert("_id", at: 0)
            fieldTypes["_id"] = "objectId"
        }

        let columns = fieldOrder.enumerated().map { (idx, name) in
            ColumnInfo(
                name: name,
                dataType: fieldTypes[name] ?? "any",
                isNullable: name != "_id",
                defaultValue: nil,
                isPrimaryKey: name == "_id",
                isAutoIncrement: false,
                comment: nil,
                ordinalPosition: idx + 1,
                characterMaxLength: nil
            )
        }

        let count = try? await collection.count()

        return TableDescription(
            name: name,
            schema: nil,
            columns: columns,
            indexes: [],
            foreignKeys: [],
            constraints: [],
            comment: nil,
            estimatedRowCount: count
        )
    }

    /// Recursively collect field paths from a BSON Document, flattening nested
    /// objects into dot-notation paths (e.g. "address.city"). Arrays and primitive
    /// fields are recorded at their parent level. The top-level field is also
    /// recorded so users can filter on the parent (e.g. `address` for object equality
    /// or `tags` for array contains).
    /// Limited recursion depth to avoid pathological docs and stack overflow.
    private func collectFieldPaths(_ doc: Document, prefix: String, order: inout [String], types: inout [String: String], depth: Int = 0) {
        guard depth < 10 else { return } // safety: cap nesting depth

        // Use keys/subscript instead of for-in to avoid any iterator quirks
        // with deeply nested or unusual BSON encodings.
        for key in doc.keys {
            let value = doc[key]
            let path = prefix.isEmpty ? key : "\(prefix).\(key)"

            if !types.keys.contains(path) {
                order.append(path)
                types[path] = inferType(value)
            }

            // Recurse only into nested object documents (NOT arrays — they have
            // numeric string keys "0", "1", "2" which would create paths like
            // "tags.0" that aren't useful for filtering most cases).
            if let nested = value as? Document, !nested.isArray {
                collectFieldPaths(nested, prefix: path, order: &order, types: &types, depth: depth + 1)
            }
        }
    }

    /// Look up a value in a Document by dot-notation path (e.g. "address.city").
    /// Returns nil if any segment is missing or hits a non-document.
    /// Skips array documents to avoid weird numeric-key traversal.
    private func valueAtPath(_ doc: Document, path: String) -> Primitive? {
        let segments = path.split(separator: ".").map(String.init)
        guard !segments.isEmpty else { return nil }
        var current: Primitive? = doc
        for seg in segments {
            guard let cur = current else { return nil }
            guard let curDoc = cur as? Document else { return nil }
            // Don't traverse into arrays via dot path
            if curDoc.isArray { return nil }
            current = curDoc[seg]
        }
        return current
    }

    private func inferType(_ value: Primitive?) -> String {
        guard let value = value else { return "null" }
        if value is Null { return "null" }
        if value is String { return "string" }
        if value is Int32 || value is Int64 || value is Int { return "integer" }
        if value is Double { return "double" }
        if value is Bool { return "boolean" }
        if value is Date { return "date" }
        if value is ObjectId { return "objectId" }
        if let doc = value as? Document {
            return doc.isArray ? "array" : "document"
        }
        return "any"
    }

    func listIndexes(table: String, schema: String?) async throws -> [IndexInfo] { [] }
    func listForeignKeys(table: String, schema: String?) async throws -> [ForeignKeyInfo] { [] }
    func listFunctions(schema: String?) async throws -> [String] { [] }
    func getFunctionSource(name: String, schema: String?) async throws -> String { "" }

    // MARK: - Data Manipulation

    func insertRow(table: String, schema: String?, values: [String: RowValue]) async throws -> QueryResult {
        let db = try requireDatabase()
        let collection = db[table]
        var doc = Document()
        for (key, value) in values {
            doc[key] = rowValueToPrimitive(value)
        }
        let reply = try await collection.insert(doc)
        return QueryResult(
            columns: [],
            rows: [],
            rowsAffected: reply.insertCount,
            executionTime: 0,
            queryType: .insert
        )
    }

    func updateRow(table: String, schema: String?, set: [String: RowValue], where whereClause: [String: RowValue]) async throws -> QueryResult {
        let db = try requireDatabase()
        let collection = db[table]

        var filter = Document()
        for (key, value) in whereClause {
            filter[key] = rowValueToPrimitive(value)
        }
        var setDoc = Document()
        for (key, value) in set {
            setDoc[key] = rowValueToPrimitive(value)
        }
        let reply = try await collection.updateOne(where: filter, to: ["$set": setDoc])
        return QueryResult(
            columns: [],
            rows: [],
            rowsAffected: reply.updatedCount ?? 0,
            executionTime: 0,
            queryType: .update
        )
    }

    func deleteRow(table: String, schema: String?, where whereClause: [String: RowValue]) async throws -> QueryResult {
        let db = try requireDatabase()
        let collection = db[table]
        var filter = Document()
        for (key, value) in whereClause {
            filter[key] = rowValueToPrimitive(value)
        }
        let reply = try await collection.deleteOne(where: filter)
        return QueryResult(
            columns: [],
            rows: [],
            rowsAffected: reply.deletes,
            executionTime: 0,
            queryType: .delete
        )
    }

    /// Convert a Gridex FilterExpression into a BSON Document for MongoDB find().
    /// Returns an empty Document (matches everything) when filter is nil or empty.
    private func buildMongoFilter(from expr: FilterExpression?) -> Document {
        guard let expr = expr, !expr.conditions.isEmpty else { return Document() }

        let conditionDocs: [Document] = expr.conditions.compactMap { condition in
            buildConditionDoc(condition)
        }

        if conditionDocs.isEmpty { return Document() }
        if conditionDocs.count == 1 { return conditionDocs[0] }

        // Combine multiple conditions with $and / $or
        var combined = Document()
        let combinator = expr.combinator == .and ? "$and" : "$or"
        var array = Document(isArray: true)
        for (idx, doc) in conditionDocs.enumerated() {
            array[String(idx)] = doc
        }
        combined[combinator] = array
        return combined
    }

    private func buildConditionDoc(_ condition: FilterCondition) -> Document? {
        let column = condition.column
        let value = mongoValueFromRowValue(condition.value, fieldName: column)
        var doc = Document()

        switch condition.op {
        case .equal:
            doc[column] = value
        case .notEqual:
            var inner = Document()
            inner["$ne"] = value
            doc[column] = inner
        case .greaterThan:
            var inner = Document()
            inner["$gt"] = value
            doc[column] = inner
        case .lessThan:
            var inner = Document()
            inner["$lt"] = value
            doc[column] = inner
        case .greaterOrEqual:
            var inner = Document()
            inner["$gte"] = value
            doc[column] = inner
        case .lessOrEqual:
            var inner = Document()
            inner["$lte"] = value
            doc[column] = inner
        case .like, .notLike:
            // Convert SQL LIKE pattern to MongoDB regex
            // % → .*  _ → .
            let pattern: String = {
                if let s = condition.value.stringValue {
                    let escaped = NSRegularExpression.escapedPattern(for: s)
                        .replacingOccurrences(of: "%", with: ".*")
                        .replacingOccurrences(of: "_", with: ".")
                    return "^" + escaped + "$"
                }
                return ".*"
            }()
            var inner = Document()
            inner["$regex"] = pattern
            inner["$options"] = "i"
            if condition.op == .notLike {
                var notDoc = Document()
                notDoc["$not"] = inner
                doc[column] = notDoc
            } else {
                doc[column] = inner
            }
        case .isNull:
            var inner = Document()
            inner["$eq"] = Null()
            doc[column] = inner
        case .isNotNull:
            var inner = Document()
            inner["$ne"] = Null()
            doc[column] = inner
        case .in_:
            // Parse comma-separated values from string
            if let s = condition.value.stringValue {
                let parts = s.split(separator: ",").map { $0.trimmingCharacters(in: .whitespaces) }
                var array = Document(isArray: true)
                for (idx, part) in parts.enumerated() {
                    array[String(idx)] = parseScalar(part)
                }
                var inner = Document()
                inner["$in"] = array
                doc[column] = inner
            }
        }
        return doc
    }

    /// Convert a Gridex RowValue to a BSON primitive for use in queries.
    /// Special-cased for the _id field: 24-char hex strings auto-convert to ObjectId.
    private func mongoValueFromRowValue(_ value: RowValue, fieldName: String) -> Primitive {
        if fieldName == "_id", case .string(let s) = value, s.count == 24,
           s.allSatisfy({ $0.isHexDigit }), let oid = try? ObjectId(s) {
            return oid
        }
        return rowValueToPrimitive(value)
    }

    /// Best-effort parse of a string into a typed BSON primitive (used for IN clauses).
    private func parseScalar(_ s: String) -> Primitive {
        let trimmed = s.trimmingCharacters(in: CharacterSet(charactersIn: "'\""))
        if let i = Int(trimmed) { return i }
        if let d = Double(trimmed) { return d }
        if trimmed.lowercased() == "true" { return true }
        if trimmed.lowercased() == "false" { return false }
        if trimmed.lowercased() == "null" { return Null() }
        if trimmed.count == 24, trimmed.allSatisfy({ $0.isHexDigit }), let oid = try? ObjectId(trimmed) {
            return oid
        }
        return trimmed
    }

    private func rowValueToPrimitive(_ value: RowValue) -> Primitive {
        switch value {
        case .null: return Null()
        case .string(let s):
            // Auto-detect ObjectId hex strings (24 hex chars)
            if s.count == 24, s.allSatisfy({ $0.isHexDigit }), let oid = try? ObjectId(s) {
                return oid
            }
            return s
        case .integer(let i): return Int(i)
        case .double(let d): return d
        case .boolean(let b): return b
        case .date(let d): return d
        case .data(let d): return d.base64EncodedString()
        case .json(let s): return s
        case .uuid(let u): return u.uuidString
        case .array(let arr): return arr.map { String(describing: $0) }.joined(separator: ", ")
        }
    }

    // MARK: - Transactions

    func beginTransaction() async throws {
        // MongoKitten supports startTransaction on the database instance.
        // For MVP we no-op since most operations are atomic at document level.
    }

    func commitTransaction() async throws {}
    func rollbackTransaction() async throws {}

    // MARK: - Pagination

    func fetchRows(
        table: String,
        schema: String?,
        columns: [String]?,
        where whereExpr: FilterExpression?,
        orderBy: [QuerySortDescriptor]?,
        limit: Int,
        offset: Int
    ) async throws -> QueryResult {
        let start = CFAbsoluteTimeGetCurrent()
        let db = try requireDatabase()
        let collection = db[table]

        // Build BSON filter from FilterExpression
        let filter = buildMongoFilter(from: whereExpr)
        var query = collection.find(filter)

        // Apply sort if provided — _id ASC is the natural default
        if let orderBy, !orderBy.isEmpty {
            var sortDoc = Document()
            for desc in orderBy {
                sortDoc[desc.column] = desc.direction == .ascending ? Int32(1) : Int32(-1)
            }
            query = query.sort(sortDoc)
        }

        query = query.skip(offset).limit(limit)

        let documents = try await query.drain()

        // Build flat field paths from the returned documents (includes nested
        // dot-notation paths like "address.city").
        var fieldOrder: [String] = []
        var fieldTypes: [String: String] = [:]
        // Always show _id first
        if !documents.isEmpty {
            fieldOrder.append("_id")
            fieldTypes["_id"] = "objectId"
            for doc in documents {
                collectFieldPaths(doc, prefix: "", order: &fieldOrder, types: &fieldTypes)
            }
        }

        let cols = fieldOrder.map { name in
            ColumnHeader(
                name: name,
                dataType: fieldTypes[name] ?? "any",
                isNullable: name != "_id",
                tableName: table
            )
        }

        let rows: [[RowValue]] = documents.map { doc in
            fieldOrder.map { path in
                if path.contains(".") {
                    return primitiveToRowValue(valueAtPath(doc, path: path))
                } else {
                    return primitiveToRowValue(doc[path])
                }
            }
        }

        return QueryResult(
            columns: cols,
            rows: rows,
            rowsAffected: 0,
            executionTime: CFAbsoluteTimeGetCurrent() - start,
            queryType: .select
        )
    }

    // MARK: - Database Management

    func createDatabase(name: String) async throws {
        // MongoDB creates databases lazily when first collection is created.
        // Use the existing connection's pool to access the new database namespace,
        // then materialize it by inserting + dropping a placeholder document.
        let db = try requireDatabase()
        let newDb = db.pool[name]
        _ = try await newDb["__gridex_init__"].insert(["_id": ObjectId()])
        try await newDb["__gridex_init__"].drop()
    }

    /// Create an empty collection in the current database. MongoDB collections are
    /// schemaless, so no column definitions are needed.
    func createCollection(name: String) async throws {
        let db = try requireDatabase()
        let collection = db[name]
        // Materialize the collection by inserting a sentinel doc, then deleting it
        let sentinelId = ObjectId()
        _ = try await collection.insert(["_id": sentinelId])
        _ = try await collection.deleteOne(where: ["_id": sentinelId])
    }

    // MARK: - Backup / Restore

    /// Stream all collections and documents for backup. Calls `onBatch` for each
    /// batch of documents with the collection name. Progress callback receives
    /// cumulative document count + elapsed time.
    func backupStream(
        onBatch: @escaping (String, [Document]) async throws -> Void,
        onProgress: ((Int64, TimeInterval) -> Void)? = nil
    ) async throws {
        let db = try requireDatabase()
        let collectionNames = try await listCollectionNames()
        let start = CFAbsoluteTimeGetCurrent()
        var totalDocs: Int64 = 0
        let batchSize = 500

        for name in collectionNames {
            let collection = db[name]
            var skip = 0
            while true {
                let batch = try await collection.find().skip(skip).limit(batchSize).drain()
                if batch.isEmpty { break }
                try await onBatch(name, batch)
                totalDocs += Int64(batch.count)
                onProgress?(totalDocs, CFAbsoluteTimeGetCurrent() - start)
                if batch.count < batchSize { break }
                skip += batchSize
            }
        }
    }

    /// Insert a batch of documents into a collection (used by restore).
    func insertBatch(collection: String, documents: [Document]) async throws {
        guard !documents.isEmpty else { return }
        let db = try requireDatabase()
        let coll = db[collection]
        for doc in documents {
            _ = try? await coll.insert(doc)
        }
    }

    /// Convert a BSON Document to a JSON string for NDJSON export.
    func documentToJSON(_ doc: Document) -> String {
        documentToJSONString(doc)
    }

    /// Parse a single JSON line into a BSON Document for NDJSON restore.
    func jsonLineToDocument(_ line: String) -> Document? {
        guard let data = line.data(using: .utf8),
              let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            return nil
        }
        return jsonObjectToDocument(json)
    }

    /// Insert a single document from a raw JSON string into a collection.
    /// Supports MongoDB extended JSON shortcuts: {"$oid": "..."} for ObjectId,
    /// {"$date": "..."} for Date.
    func insertJSONDocument(into collection: String, json: String) async throws {
        let db = try requireDatabase()
        guard let data = json.data(using: .utf8) else {
            throw GridexError.queryExecutionFailed("Invalid UTF-8 in JSON")
        }
        let jsonObject: Any
        do {
            jsonObject = try JSONSerialization.jsonObject(with: data, options: [.allowFragments])
        } catch {
            throw GridexError.queryExecutionFailed("Invalid JSON: \(error.localizedDescription)")
        }
        guard let dict = jsonObject as? [String: Any] else {
            throw GridexError.queryExecutionFailed("JSON must be a single document object")
        }
        let doc = jsonObjectToDocument(dict)
        _ = try await db[collection].insert(doc)
    }

    /// Convert a JSON dictionary into a BSON Document, handling extended JSON shortcuts.
    private func jsonObjectToDocument(_ dict: [String: Any]) -> Document {
        var doc = Document()
        for (key, value) in dict {
            doc[key] = jsonValueToBSON(value)
        }
        return doc
    }

    private func jsonValueToBSON(_ value: Any) -> Primitive? {
        if value is NSNull { return Null() }
        if let str = value as? String {
            // Auto-detect ObjectId hex strings
            if str.count == 24, str.allSatisfy({ $0.isHexDigit }), let oid = try? ObjectId(str) {
                return oid
            }
            return str
        }
        if let num = value as? NSNumber {
            // Distinguish bool / int / double
            let typeChar = String(cString: num.objCType)
            if typeChar == "c" || typeChar == "B" { return num.boolValue }
            if typeChar == "d" || typeChar == "f" { return num.doubleValue }
            return Int(truncating: num)
        }
        if let arr = value as? [Any] {
            // Convert to BSON array (Document with isArray)
            var doc = Document(isArray: true)
            for (idx, item) in arr.enumerated() {
                doc[String(idx)] = jsonValueToBSON(item)
            }
            return doc
        }
        if let dict = value as? [String: Any] {
            // Extended JSON shortcuts
            if let oidStr = dict["$oid"] as? String, let oid = try? ObjectId(oidStr) {
                return oid
            }
            if let dateStr = dict["$date"] as? String {
                let fmt = ISO8601DateFormatter()
                fmt.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
                if let date = fmt.date(from: dateStr) {
                    return date
                }
                fmt.formatOptions = [.withInternetDateTime]
                if let date = fmt.date(from: dateStr) {
                    return date
                }
            }
            return jsonObjectToDocument(dict)
        }
        return nil
    }

    func dropDatabase(name: String) async throws {
        let db = try requireDatabase()
        let target = db.pool[name]
        try await target.drop()
    }

    // MARK: - Database Info

    func serverVersion() async throws -> String {
        // MongoKitten doesn't expose server version directly without runCommand;
        // return generic label for now.
        return "MongoDB"
    }

    func currentDatabase() async throws -> String? {
        connectionConfig?.database
    }
}

// MARK: - Character helper

private extension Character {
    var isHexDigit: Bool {
        return ("0"..."9").contains(self) || ("a"..."f").contains(self) || ("A"..."F").contains(self)
    }
}
