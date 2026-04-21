// BackupService.swift
// Gridex
//
// Executes database backup and restore via CLI tools:
//   PostgreSQL: pg_dump / pg_restore
//   MySQL:      mysqldump / mysql
//   SQLite:     file copy

import Foundation
import MongoKitten

enum BackupFormat: String, CaseIterable, Sendable {
    case custom = "custom"       // pg_dump -Fc (PostgreSQL only, supports pg_restore)
    case sql = "sql"             // plain SQL dump
    case tar = "tar"             // pg_dump -Ft
    case directory = "directory" // pg_dump -Fd
    case bak = "bak"             // SQL Server native BACKUP DATABASE
    case ndjson = "ndjson"       // MongoDB: NDJSON (one document per line)
    case redisJSON = "redisJSON" // Redis: JSON snapshot via SCAN

    var displayName: String {
        switch self {
        case .custom: return "Custom (compressed)"
        case .sql: return "Plain SQL"
        case .tar: return "Tar archive"
        case .directory: return "Directory"
        case .bak: return "SQL Server Backup (.bak)"
        case .ndjson: return "NDJSON (one doc per line)"
        case .redisJSON: return "JSON snapshot"
        }
    }

    var fileExtension: String {
        switch self {
        case .custom: return "dump"
        case .sql: return "sql"
        case .tar: return "tar"
        case .directory: return ""
        case .bak: return "bak"
        case .ndjson: return "ndjson"
        case .redisJSON: return "json"
        }
    }

    static func available(for dbType: DatabaseType) -> [BackupFormat] {
        switch dbType {
        case .postgresql: return [.custom, .sql, .tar]
        case .mysql: return [.sql]
        case .sqlite: return [.sql]
        case .mssql: return [.bak]
        case .mongodb: return [.ndjson]
        case .redis: return [.redisJSON]
        case .clickhouse: return [.sql]
        }
    }
}

struct BackupOptions: Sendable {
    var format: BackupFormat = .custom
    var compress: Bool = true
    var dataOnly: Bool = false
    var schemaOnly: Bool = false
    var tables: [String] = []      // empty = all tables
    var excludeTables: [String] = []
}

struct BackupResult: Sendable {
    let success: Bool
    let outputPath: String?
    let errorMessage: String?
    let duration: TimeInterval
    let fileSize: Int64?
}

actor BackupService {

    // MARK: - Backup

    func backup(
        config: ConnectionConfig,
        password: String,
        database: String,
        to outputURL: URL,
        options: BackupOptions,
        adapter: (any DatabaseAdapter)? = nil,
        onProgress: (@Sendable (Int64, TimeInterval) -> Void)? = nil
    ) async -> BackupResult {
        let start = CFAbsoluteTimeGetCurrent()

        switch config.databaseType {
        case .postgresql:
            return await pgDump(config: config, password: password, database: database, to: outputURL, options: options, start: start, onProgress: onProgress)
        case .mysql:
            return await mysqlDump(config: config, password: password, database: database, to: outputURL, options: options, start: start, onProgress: onProgress)
        case .sqlite:
            return sqliteCopy(config: config, to: outputURL, start: start)
        case .redis:
            guard let redis = adapter as? RedisAdapter else {
                return BackupResult(success: false, outputPath: nil, errorMessage: "Redis adapter required for backup", duration: 0, fileSize: nil)
            }
            return await redisJSONBackup(adapter: redis, to: outputURL, start: start, onProgress: onProgress)
        case .mongodb:
            guard let mongo = adapter as? MongoDBAdapter else {
                return BackupResult(success: false, outputPath: nil, errorMessage: "MongoDB adapter required for backup", duration: 0, fileSize: nil)
            }
            return await mongoNDJSONBackup(adapter: mongo, to: outputURL, start: start, onProgress: onProgress)
        case .mssql:
            guard let mssql = adapter as? MSSQLAdapter else {
                return BackupResult(success: false, outputPath: nil, errorMessage: "SQL Server adapter required for backup", duration: 0, fileSize: nil)
            }
            return await mssqlBackup(adapter: mssql, database: database, to: outputURL, start: start)
        case .clickhouse:
            guard let ch = adapter as? ClickHouseAdapter else {
                return BackupResult(success: false, outputPath: nil, errorMessage: "ClickHouse adapter required for backup", duration: 0, fileSize: nil)
            }
            return await clickhouseBackup(adapter: ch, database: database, to: outputURL, start: start)
        }
    }

    // MARK: - Restore

    func restore(
        config: ConnectionConfig,
        password: String,
        database: String,
        from inputURL: URL,
        format: BackupFormat,
        adapter: (any DatabaseAdapter)? = nil,
        onProgress: (@Sendable (Double) -> Void)? = nil
    ) async -> BackupResult {
        let start = CFAbsoluteTimeGetCurrent()

        switch config.databaseType {
        case .postgresql:
            return await pgRestore(config: config, password: password, database: database, from: inputURL, format: format, start: start, onProgress: onProgress)
        case .mysql:
            return await mysqlRestore(config: config, password: password, database: database, from: inputURL, start: start, onProgress: onProgress)
        case .sqlite:
            return sqliteRestore(config: config, from: inputURL, start: start)
        case .redis:
            guard let redis = adapter as? RedisAdapter else {
                return BackupResult(success: false, outputPath: nil, errorMessage: "Redis adapter required for restore", duration: 0, fileSize: nil)
            }
            return await redisJSONRestore(adapter: redis, from: inputURL, start: start, onProgress: onProgress)
        case .mongodb:
            guard let mongo = adapter as? MongoDBAdapter else {
                return BackupResult(success: false, outputPath: nil, errorMessage: "MongoDB adapter required for restore", duration: 0, fileSize: nil)
            }
            return await mongoNDJSONRestore(adapter: mongo, from: inputURL, start: start, onProgress: onProgress)
        case .mssql:
            guard let mssql = adapter as? MSSQLAdapter else {
                return BackupResult(success: false, outputPath: nil, errorMessage: "SQL Server adapter required for restore", duration: 0, fileSize: nil)
            }
            return await mssqlRestore(adapter: mssql, database: database, from: inputURL, start: start)
        case .clickhouse:
            guard let ch = adapter as? ClickHouseAdapter else {
                return BackupResult(success: false, outputPath: nil, errorMessage: "ClickHouse adapter required for restore", duration: 0, fileSize: nil)
            }
            return await clickhouseRestore(adapter: ch, database: database, from: inputURL, start: start)
        }
    }

    // MARK: - PostgreSQL

    private func pgDump(
        config: ConnectionConfig,
        password: String,
        database: String,
        to outputURL: URL,
        options: BackupOptions,
        start: CFAbsoluteTime,
        onProgress: (@Sendable (Int64, TimeInterval) -> Void)?
    ) async -> BackupResult {
        guard let pgDumpPath = findTool("pg_dump") else {
            return BackupResult(success: false, outputPath: nil,
                                errorMessage: "pg_dump not found. Install PostgreSQL client tools:\n  brew install libpq", duration: 0, fileSize: nil)
        }

        var args: [String] = []
        args += ["-h", config.host ?? "localhost"]
        args += ["-p", "\(config.port ?? 5432)"]
        args += ["-U", config.username ?? "postgres"]
        args += ["-d", database]
        args += ["-f", outputURL.path]

        switch options.format {
        case .custom: args += ["-Fc"]
        case .sql: args += ["-Fp"]
        case .tar: args += ["-Ft"]
        case .directory: args += ["-Fd"]
        case .bak, .ndjson, .redisJSON: break // non-PostgreSQL formats; handled elsewhere
        }

        if options.compress && options.format == .custom { args += ["-Z", "6"] }
        if options.dataOnly { args.append("--data-only") }
        if options.schemaOnly { args.append("--schema-only") }
        for t in options.tables { args += ["-t", t] }
        for t in options.excludeTables { args += ["-T", t] }

        var env = ProcessInfo.processInfo.environment
        env["PGPASSWORD"] = password

        return await runProcessWithOutputProgress(path: pgDumpPath, args: args, env: env, start: start, outputURL: outputURL, onProgress: onProgress)
    }

    private func pgRestore(
        config: ConnectionConfig,
        password: String,
        database: String,
        from inputURL: URL,
        format: BackupFormat,
        start: CFAbsoluteTime,
        onProgress: (@Sendable (Double) -> Void)?
    ) async -> BackupResult {
        var env = ProcessInfo.processInfo.environment
        env["PGPASSWORD"] = password

        if format == .sql {
            // Plain SQL: pipe through stdin for progress tracking
            guard let psqlPath = findTool("psql") else {
                return BackupResult(success: false, outputPath: nil,
                                    errorMessage: "psql not found. Install PostgreSQL client tools:\n  brew install libpq", duration: 0, fileSize: nil)
            }
            var args: [String] = []
            args += ["-h", config.host ?? "localhost"]
            args += ["-p", "\(config.port ?? 5432)"]
            args += ["-U", config.username ?? "postgres"]
            args += ["-d", database]
            return await runProcessWithStdinProgress(path: psqlPath, args: args, env: env, inputFile: inputURL, start: start, onProgress: onProgress)
        } else {
            // Custom/Tar: use pg_restore
            guard let pgRestorePath = findTool("pg_restore") else {
                return BackupResult(success: false, outputPath: nil,
                                    errorMessage: "pg_restore not found. Install PostgreSQL client tools:\n  brew install libpq", duration: 0, fileSize: nil)
            }
            var args: [String] = []
            args += ["-h", config.host ?? "localhost"]
            args += ["-p", "\(config.port ?? 5432)"]
            args += ["-U", config.username ?? "postgres"]
            args += ["-d", database]
            args.append(inputURL.path)
            return await runProcess(path: pgRestorePath, args: args, env: env, start: start, outputURL: nil)
        }
    }

    // MARK: - MySQL

    private func mysqlDump(
        config: ConnectionConfig,
        password: String,
        database: String,
        to outputURL: URL,
        options: BackupOptions,
        start: CFAbsoluteTime,
        onProgress: (@Sendable (Int64, TimeInterval) -> Void)?
    ) async -> BackupResult {
        guard let mysqldumpPath = findTool("mysqldump") else {
            return BackupResult(success: false, outputPath: nil,
                                errorMessage: "mysqldump not found. Install MySQL client tools:\n  brew install mysql-client", duration: 0, fileSize: nil)
        }

        var args: [String] = []
        args += ["-h", config.host ?? "localhost"]
        args += ["-P", "\(config.port ?? 3306)"]
        args += ["-u", config.username ?? "root"]
        args += ["--password=\(password)"]
        args += ["--result-file=\(outputURL.path)"]
        if options.dataOnly { args.append("--no-create-info") }
        if options.schemaOnly { args.append("--no-data") }
        args.append(database)
        if !options.tables.isEmpty { args += options.tables }

        return await runProcessWithOutputProgress(path: mysqldumpPath, args: args, env: nil, start: start, outputURL: outputURL, onProgress: onProgress)
    }

    private func mysqlRestore(
        config: ConnectionConfig,
        password: String,
        database: String,
        from inputURL: URL,
        start: CFAbsoluteTime,
        onProgress: (@Sendable (Double) -> Void)?
    ) async -> BackupResult {
        guard let mysqlPath = findTool("mysql") else {
            return BackupResult(success: false, outputPath: nil,
                                errorMessage: "mysql not found. Install MySQL client tools:\n  brew install mysql-client", duration: 0, fileSize: nil)
        }

        var args: [String] = []
        args += ["-h", config.host ?? "localhost"]
        args += ["-P", "\(config.port ?? 3306)"]
        args += ["-u", config.username ?? "root"]
        args += ["--password=\(password)"]
        args += [database]

        return await runProcessWithStdinProgress(path: mysqlPath, args: args, env: nil, inputFile: inputURL, start: start, onProgress: onProgress)
    }

    // MARK: - SQLite

    private func sqliteCopy(config: ConnectionConfig, to outputURL: URL, start: CFAbsoluteTime) -> BackupResult {
        guard let filePath = config.filePath ?? config.database else {
            return BackupResult(success: false, outputPath: nil,
                                errorMessage: "No SQLite file path configured", duration: 0, fileSize: nil)
        }
        do {
            let src = URL(fileURLWithPath: filePath)
            if FileManager.default.fileExists(atPath: outputURL.path) {
                try FileManager.default.removeItem(at: outputURL)
            }
            try FileManager.default.copyItem(at: src, to: outputURL)
            let size = try FileManager.default.attributesOfItem(atPath: outputURL.path)[.size] as? Int64
            return BackupResult(success: true, outputPath: outputURL.path, errorMessage: nil,
                                duration: CFAbsoluteTimeGetCurrent() - start, fileSize: size)
        } catch {
            return BackupResult(success: false, outputPath: nil,
                                errorMessage: error.localizedDescription,
                                duration: CFAbsoluteTimeGetCurrent() - start, fileSize: nil)
        }
    }

    private func sqliteRestore(config: ConnectionConfig, from inputURL: URL, start: CFAbsoluteTime) -> BackupResult {
        guard let filePath = config.filePath ?? config.database else {
            return BackupResult(success: false, outputPath: nil,
                                errorMessage: "No SQLite file path configured", duration: 0, fileSize: nil)
        }
        do {
            let dest = URL(fileURLWithPath: filePath)
            if FileManager.default.fileExists(atPath: dest.path) {
                try FileManager.default.removeItem(at: dest)
            }
            try FileManager.default.copyItem(at: inputURL, to: dest)
            return BackupResult(success: true, outputPath: dest.path, errorMessage: nil,
                                duration: CFAbsoluteTimeGetCurrent() - start, fileSize: nil)
        } catch {
            return BackupResult(success: false, outputPath: nil,
                                errorMessage: error.localizedDescription,
                                duration: CFAbsoluteTimeGetCurrent() - start, fileSize: nil)
        }
    }

    // MARK: - SQL Server (native BACKUP/RESTORE DATABASE)

    /// MSSQL backup uses SQL Server's native BACKUP DATABASE command.
    /// IMPORTANT: The path is **server-side** — SQL Server process writes the file.
    /// For Docker, user must volume-mount the path. Not supported on Azure SQL Database.
    private func mssqlBackup(
        adapter: MSSQLAdapter,
        database: String,
        to outputURL: URL,
        start: CFAbsoluteTime
    ) async -> BackupResult {
        let safeDB = database.replacingOccurrences(of: "]", with: "]]")
        let safePath = outputURL.path.replacingOccurrences(of: "'", with: "''")
        let sql = """
            BACKUP DATABASE [\(safeDB)] TO DISK = N'\(safePath)'
            WITH FORMAT, INIT, NAME = 'Gridex Full Backup', COMPRESSION
            """
        do {
            _ = try await adapter.executeRaw(sql: sql)
            let size = try? FileManager.default.attributesOfItem(atPath: outputURL.path)[.size] as? Int64
            return BackupResult(
                success: true,
                outputPath: outputURL.path,
                errorMessage: "Note: Path is server-side. For Docker, ensure the container has access to this path (volume mount).",
                duration: CFAbsoluteTimeGetCurrent() - start,
                fileSize: size
            )
        } catch {
            return BackupResult(
                success: false, outputPath: nil,
                errorMessage: "BACKUP DATABASE failed: \(error.localizedDescription). The path must be accessible to the SQL Server process (server-side, not client).",
                duration: CFAbsoluteTimeGetCurrent() - start, fileSize: nil
            )
        }
    }

    private func mssqlRestore(
        adapter: MSSQLAdapter,
        database: String,
        from inputURL: URL,
        start: CFAbsoluteTime
    ) async -> BackupResult {
        let safeDB = database.replacingOccurrences(of: "]", with: "]]")
        let safePath = inputURL.path.replacingOccurrences(of: "'", with: "''")
        // Force-disconnect users and restore with REPLACE
        let setSingleSQL = "ALTER DATABASE [\(safeDB)] SET SINGLE_USER WITH ROLLBACK IMMEDIATE"
        let restoreSQL = "RESTORE DATABASE [\(safeDB)] FROM DISK = N'\(safePath)' WITH REPLACE"
        let setMultiSQL = "ALTER DATABASE [\(safeDB)] SET MULTI_USER"
        do {
            _ = try? await adapter.executeRaw(sql: setSingleSQL)
            _ = try await adapter.executeRaw(sql: restoreSQL)
            _ = try? await adapter.executeRaw(sql: setMultiSQL)
            return BackupResult(
                success: true, outputPath: nil, errorMessage: nil,
                duration: CFAbsoluteTimeGetCurrent() - start, fileSize: nil
            )
        } catch {
            _ = try? await adapter.executeRaw(sql: setMultiSQL)
            return BackupResult(
                success: false, outputPath: nil,
                errorMessage: "RESTORE DATABASE failed: \(error.localizedDescription). The file must be server-side accessible.",
                duration: CFAbsoluteTimeGetCurrent() - start, fileSize: nil
            )
        }
    }

    // MARK: - MongoDB (NDJSON pure Swift)

    /// MongoDB backup: iterate all collections and write one JSON document per line.
    /// Format: `{"_collection": "name", "_doc": {...}}` per line.
    private func mongoNDJSONBackup(
        adapter: MongoDBAdapter,
        to outputURL: URL,
        start: CFAbsoluteTime,
        onProgress: (@Sendable (Int64, TimeInterval) -> Void)?
    ) async -> BackupResult {
        do {
            // Create file
            FileManager.default.createFile(atPath: outputURL.path, contents: nil)
            guard let handle = try? FileHandle(forWritingTo: outputURL) else {
                return BackupResult(success: false, outputPath: nil,
                                    errorMessage: "Cannot create output file at \(outputURL.path)",
                                    duration: CFAbsoluteTimeGetCurrent() - start, fileSize: nil)
            }
            defer { try? handle.close() }

            try await adapter.backupStream(onBatch: { collection, batch in
                for doc in batch {
                    let json = adapter.documentToJSON(doc)
                    // Wrap each document with its collection name
                    let line = "{\"_collection\":\"\(collection)\",\"_doc\":\(json)}\n"
                    if let data = line.data(using: .utf8) {
                        try handle.write(contentsOf: data)
                    }
                }
            }, onProgress: onProgress)

            let size = try? FileManager.default.attributesOfItem(atPath: outputURL.path)[.size] as? Int64
            return BackupResult(
                success: true, outputPath: outputURL.path, errorMessage: nil,
                duration: CFAbsoluteTimeGetCurrent() - start, fileSize: size
            )
        } catch {
            return BackupResult(
                success: false, outputPath: nil,
                errorMessage: "MongoDB backup failed: \(error.localizedDescription)",
                duration: CFAbsoluteTimeGetCurrent() - start, fileSize: nil
            )
        }
    }

    private func mongoNDJSONRestore(
        adapter: MongoDBAdapter,
        from inputURL: URL,
        start: CFAbsoluteTime,
        onProgress: (@Sendable (Double) -> Void)?
    ) async -> BackupResult {
        do {
            let totalSize = (try? FileManager.default.attributesOfItem(atPath: inputURL.path)[.size] as? Int64) ?? 0
            let data = try Data(contentsOf: inputURL)
            guard let content = String(data: data, encoding: .utf8) else {
                return BackupResult(success: false, outputPath: nil,
                                    errorMessage: "Invalid UTF-8 in backup file",
                                    duration: CFAbsoluteTimeGetCurrent() - start, fileSize: nil)
            }
            let lines = content.components(separatedBy: "\n")
            var bytesProcessed: Int64 = 0
            // Group documents by collection for efficient batch insert
            var batches: [String: [String]] = [:]
            for line in lines {
                let trimmed = line.trimmingCharacters(in: .whitespaces)
                guard !trimmed.isEmpty else { continue }
                bytesProcessed += Int64(line.utf8.count + 1)
                // Parse wrapper {"_collection": "...", "_doc": {...}}
                guard let lineData = trimmed.data(using: .utf8),
                      let json = try? JSONSerialization.jsonObject(with: lineData) as? [String: Any],
                      let coll = json["_collection"] as? String,
                      let docObj = json["_doc"] else { continue }
                // Re-serialize the doc back to JSON string for adapter parsing
                if let docData = try? JSONSerialization.data(withJSONObject: docObj),
                   let docJson = String(data: docData, encoding: .utf8) {
                    batches[coll, default: []].append(docJson)
                }
                if totalSize > 0 {
                    onProgress?(Double(bytesProcessed) / Double(totalSize))
                }
            }
            // Insert batches
            for (collection, jsonDocs) in batches {
                var docs: [Document] = []
                for jsonStr in jsonDocs {
                    if let doc = adapter.jsonLineToDocument(jsonStr) {
                        docs.append(doc)
                    }
                }
                try await adapter.insertBatch(collection: collection, documents: docs)
            }
            return BackupResult(
                success: true, outputPath: nil, errorMessage: nil,
                duration: CFAbsoluteTimeGetCurrent() - start, fileSize: totalSize
            )
        } catch {
            return BackupResult(
                success: false, outputPath: nil,
                errorMessage: "MongoDB restore failed: \(error.localizedDescription)",
                duration: CFAbsoluteTimeGetCurrent() - start, fileSize: nil
            )
        }
    }

    // MARK: - Redis (JSON snapshot via SCAN + DUMP/RESTORE fallback to type-specific commands)

    /// Redis backup: SCAN all keys, fetch value + TTL based on type, write as JSON array.
    /// Format: `[{"key": "...", "type": "...", "ttl": N, "value": ...}, ...]`
    private func redisJSONBackup(
        adapter: RedisAdapter,
        to outputURL: URL,
        start: CFAbsoluteTime,
        onProgress: (@Sendable (Int64, TimeInterval) -> Void)?
    ) async -> BackupResult {
        do {
            FileManager.default.createFile(atPath: outputURL.path, contents: nil)
            guard let handle = try? FileHandle(forWritingTo: outputURL) else {
                return BackupResult(success: false, outputPath: nil,
                                    errorMessage: "Cannot create output file",
                                    duration: CFAbsoluteTimeGetCurrent() - start, fileSize: nil)
            }
            defer { try? handle.close() }

            // Use NDJSON format for streaming (one entry per line)
            try await adapter.backupScanAll(onBatch: { batch in
                for entry in batch {
                    let line = RedisJSONSerializer.serialize(entry) + "\n"
                    if let data = line.data(using: .utf8) {
                        try handle.write(contentsOf: data)
                    }
                }
            }, onProgress: onProgress)

            let size = try? FileManager.default.attributesOfItem(atPath: outputURL.path)[.size] as? Int64
            return BackupResult(
                success: true, outputPath: outputURL.path, errorMessage: nil,
                duration: CFAbsoluteTimeGetCurrent() - start, fileSize: size
            )
        } catch {
            return BackupResult(
                success: false, outputPath: nil,
                errorMessage: "Redis backup failed: \(error.localizedDescription)",
                duration: CFAbsoluteTimeGetCurrent() - start, fileSize: nil
            )
        }
    }

    private func redisJSONRestore(
        adapter: RedisAdapter,
        from inputURL: URL,
        start: CFAbsoluteTime,
        onProgress: (@Sendable (Double) -> Void)?
    ) async -> BackupResult {
        do {
            let totalSize = (try? FileManager.default.attributesOfItem(atPath: inputURL.path)[.size] as? Int64) ?? 0
            let data = try Data(contentsOf: inputURL)
            guard let content = String(data: data, encoding: .utf8) else {
                return BackupResult(success: false, outputPath: nil,
                                    errorMessage: "Invalid UTF-8 in backup file",
                                    duration: CFAbsoluteTimeGetCurrent() - start, fileSize: nil)
            }
            let lines = content.components(separatedBy: "\n")
            var bytesProcessed: Int64 = 0
            for line in lines {
                let trimmed = line.trimmingCharacters(in: .whitespaces)
                bytesProcessed += Int64(line.utf8.count + 1)
                guard !trimmed.isEmpty else { continue }
                if let entry = RedisJSONSerializer.deserialize(trimmed) {
                    try await adapter.restoreKeyEntry(entry)
                }
                if totalSize > 0 {
                    onProgress?(Double(bytesProcessed) / Double(totalSize))
                }
            }
            return BackupResult(
                success: true, outputPath: nil, errorMessage: nil,
                duration: CFAbsoluteTimeGetCurrent() - start, fileSize: totalSize
            )
        } catch {
            return BackupResult(
                success: false, outputPath: nil,
                errorMessage: "Redis restore failed: \(error.localizedDescription)",
                duration: CFAbsoluteTimeGetCurrent() - start, fileSize: nil
            )
        }
    }

    // MARK: - Helpers

    private func findTool(_ name: String) -> String? {
        let searchPaths = [
            "/usr/local/bin/\(name)",
            "/opt/homebrew/bin/\(name)",
            "/usr/bin/\(name)",
            "/opt/homebrew/opt/libpq/bin/\(name)",
            "/usr/local/opt/libpq/bin/\(name)",
            "/opt/homebrew/opt/mysql-client/bin/\(name)",
            "/usr/local/opt/mysql-client/bin/\(name)",
            "/Applications/Postgres.app/Contents/Versions/latest/bin/\(name)",
        ]
        for p in searchPaths {
            if FileManager.default.isExecutableFile(atPath: p) { return p }
        }
        // Try `which`
        let proc = Process()
        proc.executableURL = URL(fileURLWithPath: "/usr/bin/which")
        proc.arguments = [name]
        let pipe = Pipe()
        proc.standardOutput = pipe
        proc.standardError = Pipe()
        try? proc.run()
        proc.waitUntilExit()
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        let path = String(data: data, encoding: .utf8)?.trimmingCharacters(in: .whitespacesAndNewlines)
        if let path, !path.isEmpty, FileManager.default.isExecutableFile(atPath: path) { return path }
        return nil
    }

    /// Run a CLI tool that writes to an output file, polling file size for progress reporting.
    private func runProcessWithOutputProgress(
        path: String,
        args: [String],
        env: [String: String]?,
        start: CFAbsoluteTime,
        outputURL: URL,
        onProgress: (@Sendable (Int64, TimeInterval) -> Void)?
    ) async -> BackupResult {
        await withCheckedContinuation { continuation in
            DispatchQueue.global(qos: .userInitiated).async {
                let proc = Process()
                proc.executableURL = URL(fileURLWithPath: path)
                proc.arguments = args
                if let env { proc.environment = env }

                let stderrPipe = Pipe()
                proc.standardError = stderrPipe
                proc.standardOutput = Pipe()

                do {
                    try proc.run()
                } catch {
                    continuation.resume(returning: BackupResult(
                        success: false, outputPath: nil,
                        errorMessage: "Failed to launch \(path): \(error.localizedDescription)",
                        duration: CFAbsoluteTimeGetCurrent() - start, fileSize: nil))
                    return
                }

                // Poll output file size while process runs
                if onProgress != nil {
                    let pollQueue = DispatchQueue(label: "backup.progress")
                    let timer = DispatchSource.makeTimerSource(queue: pollQueue)
                    timer.schedule(deadline: .now() + 0.5, repeating: 0.5)
                    timer.setEventHandler {
                        let elapsed = CFAbsoluteTimeGetCurrent() - start
                        let size = (try? FileManager.default.attributesOfItem(atPath: outputURL.path)[.size] as? Int64) ?? 0
                        onProgress?(size, elapsed)
                    }
                    timer.resume()
                    proc.waitUntilExit()
                    timer.cancel()
                } else {
                    proc.waitUntilExit()
                }

                let duration = CFAbsoluteTimeGetCurrent() - start
                let stderrData = stderrPipe.fileHandleForReading.readDataToEndOfFile()
                let stderrStr = String(data: stderrData, encoding: .utf8) ?? ""

                var fileSize: Int64?
                fileSize = (try? FileManager.default.attributesOfItem(atPath: outputURL.path)[.size] as? Int64)

                if proc.terminationStatus == 0 {
                    continuation.resume(returning: BackupResult(
                        success: true, outputPath: outputURL.path,
                        errorMessage: stderrStr.isEmpty ? nil : stderrStr,
                        duration: duration, fileSize: fileSize))
                } else {
                    continuation.resume(returning: BackupResult(
                        success: false, outputPath: nil,
                        errorMessage: stderrStr.isEmpty ? "Process exited with code \(proc.terminationStatus)" : stderrStr,
                        duration: duration, fileSize: nil))
                }
            }
        }
    }

    /// Run a CLI tool, piping a file through stdin in chunks, reporting progress as bytes sent / total.
    private func runProcessWithStdinProgress(
        path: String,
        args: [String],
        env: [String: String]?,
        inputFile: URL,
        start: CFAbsoluteTime,
        onProgress: (@Sendable (Double) -> Void)?
    ) async -> BackupResult {
        await withCheckedContinuation { continuation in
            DispatchQueue.global(qos: .userInitiated).async {
                let totalSize: Int64
                do {
                    let attrs = try FileManager.default.attributesOfItem(atPath: inputFile.path)
                    totalSize = (attrs[.size] as? Int64) ?? 0
                } catch {
                    continuation.resume(returning: BackupResult(
                        success: false, outputPath: nil,
                        errorMessage: "Cannot read file: \(error.localizedDescription)",
                        duration: CFAbsoluteTimeGetCurrent() - start, fileSize: nil))
                    return
                }

                guard let inputStream = InputStream(url: inputFile) else {
                    continuation.resume(returning: BackupResult(
                        success: false, outputPath: nil,
                        errorMessage: "Cannot open file for reading",
                        duration: CFAbsoluteTimeGetCurrent() - start, fileSize: nil))
                    return
                }

                let proc = Process()
                proc.executableURL = URL(fileURLWithPath: path)
                proc.arguments = args
                if let env { proc.environment = env }

                let stdinPipe = Pipe()
                let stderrPipe = Pipe()
                proc.standardInput = stdinPipe
                proc.standardError = stderrPipe
                proc.standardOutput = Pipe()

                do {
                    try proc.run()
                } catch {
                    continuation.resume(returning: BackupResult(
                        success: false, outputPath: nil,
                        errorMessage: "Failed to launch \(path): \(error.localizedDescription)",
                        duration: CFAbsoluteTimeGetCurrent() - start, fileSize: nil))
                    return
                }

                // Stream file to stdin in chunks, reporting progress
                inputStream.open()
                let chunkSize = 64 * 1024  // 64KB chunks
                var buffer = [UInt8](repeating: 0, count: chunkSize)
                var bytesSent: Int64 = 0
                let writeHandle = stdinPipe.fileHandleForWriting

                while inputStream.hasBytesAvailable {
                    let bytesRead = inputStream.read(&buffer, maxLength: chunkSize)
                    guard bytesRead > 0 else { break }
                    writeHandle.write(Data(buffer[0..<bytesRead]))
                    bytesSent += Int64(bytesRead)
                    if totalSize > 0 {
                        onProgress?(Double(bytesSent) / Double(totalSize))
                    }
                }
                inputStream.close()
                try? writeHandle.close()

                proc.waitUntilExit()

                let duration = CFAbsoluteTimeGetCurrent() - start
                let stderrData = stderrPipe.fileHandleForReading.readDataToEndOfFile()
                let stderrStr = String(data: stderrData, encoding: .utf8) ?? ""

                if proc.terminationStatus == 0 {
                    onProgress?(1.0)
                    continuation.resume(returning: BackupResult(
                        success: true, outputPath: nil,
                        errorMessage: stderrStr.isEmpty ? nil : stderrStr,
                        duration: duration, fileSize: totalSize))
                } else {
                    continuation.resume(returning: BackupResult(
                        success: false, outputPath: nil,
                        errorMessage: stderrStr.isEmpty ? "Process exited with code \(proc.terminationStatus)" : stderrStr,
                        duration: duration, fileSize: nil))
                }
            }
        }
    }

    private func runProcess(
        path: String,
        args: [String],
        env: [String: String]?,
        start: CFAbsoluteTime,
        outputURL: URL?
    ) async -> BackupResult {
        await withCheckedContinuation { continuation in
            DispatchQueue.global(qos: .userInitiated).async {
                let proc = Process()
                proc.executableURL = URL(fileURLWithPath: path)
                proc.arguments = args
                if let env { proc.environment = env }

                let stderrPipe = Pipe()
                proc.standardError = stderrPipe
                proc.standardOutput = Pipe() // suppress stdout

                do {
                    try proc.run()
                    proc.waitUntilExit()
                } catch {
                    continuation.resume(returning: BackupResult(
                        success: false, outputPath: nil,
                        errorMessage: "Failed to launch \(path): \(error.localizedDescription)",
                        duration: CFAbsoluteTimeGetCurrent() - start, fileSize: nil))
                    return
                }

                let duration = CFAbsoluteTimeGetCurrent() - start
                let stderrData = stderrPipe.fileHandleForReading.readDataToEndOfFile()
                let stderrStr = String(data: stderrData, encoding: .utf8) ?? ""

                if proc.terminationStatus == 0 {
                    var fileSize: Int64?
                    if let outputURL {
                        fileSize = (try? FileManager.default.attributesOfItem(atPath: outputURL.path)[.size] as? Int64)
                    }
                    continuation.resume(returning: BackupResult(
                        success: true, outputPath: outputURL?.path,
                        errorMessage: stderrStr.isEmpty ? nil : stderrStr,
                        duration: duration, fileSize: fileSize))
                } else {
                    continuation.resume(returning: BackupResult(
                        success: false, outputPath: nil,
                        errorMessage: stderrStr.isEmpty ? "Process exited with code \(proc.terminationStatus)" : stderrStr,
                        duration: duration, fileSize: nil))
                }
            }
        }
    }

    // MARK: - ClickHouse (pure Swift SQL dump via HTTP)

    /// ClickHouse backup: emit `CREATE TABLE` + row-level `INSERT ... FORMAT Values`
    /// statements for each table in the database. Views are skipped — they're
    /// derived from their sources and will be recreated when the source tables exist.
    private func clickhouseBackup(
        adapter: ClickHouseAdapter,
        database: String,
        to outputURL: URL,
        start: CFAbsoluteTime
    ) async -> BackupResult {
        do {
            FileManager.default.createFile(atPath: outputURL.path, contents: nil)
            guard let handle = try? FileHandle(forWritingTo: outputURL) else {
                return BackupResult(success: false, outputPath: nil,
                                    errorMessage: "Cannot create output file at \(outputURL.path)",
                                    duration: CFAbsoluteTimeGetCurrent() - start, fileSize: nil)
            }
            defer { try? handle.close() }

            let header = "-- ClickHouse dump for database `\(database)` at \(ISO8601DateFormatter().string(from: Date()))\n\n"
            if let data = header.data(using: .utf8) { try handle.write(contentsOf: data) }

            let tables = try await adapter.listTables(schema: database)
            for t in tables {
                let ddlResult = try await adapter.executeRaw(
                    sql: "SHOW CREATE TABLE `\(database)`.`\(t.name)`"
                )
                let ddl = ddlResult.rows.first?.first?.stringValue ?? ""
                if !ddl.isEmpty {
                    let block = "\(ddl);\n\n"
                    if let data = block.data(using: .utf8) { try handle.write(contentsOf: data) }
                }

                // Row data via `FORMAT Values` → `(v1, v2), (v3, v4)...` for direct INSERT.
                // Loads the whole result into memory — acceptable for v1.
                let rowDump = try await adapter.executeRaw(
                    sql: "SELECT * FROM `\(database)`.`\(t.name)` FORMAT Values"
                )
                if let body = rowDump.rows.first?.first?.stringValue, !body.isEmpty {
                    let stmt = "INSERT INTO `\(database)`.`\(t.name)` VALUES \(body);\n\n"
                    if let data = stmt.data(using: .utf8) { try handle.write(contentsOf: data) }
                }
            }

            let size = try? FileManager.default.attributesOfItem(atPath: outputURL.path)[.size] as? Int64
            return BackupResult(
                success: true, outputPath: outputURL.path, errorMessage: nil,
                duration: CFAbsoluteTimeGetCurrent() - start, fileSize: size
            )
        } catch {
            return BackupResult(
                success: false, outputPath: nil,
                errorMessage: "ClickHouse backup failed: \(error.localizedDescription)",
                duration: CFAbsoluteTimeGetCurrent() - start, fileSize: nil
            )
        }
    }

    /// ClickHouse restore: split the dump on `;` and execute each statement.
    /// Targets the selected database via `USE` first so unqualified identifiers work.
    private func clickhouseRestore(
        adapter: ClickHouseAdapter,
        database: String,
        from inputURL: URL,
        start: CFAbsoluteTime
    ) async -> BackupResult {
        do {
            let data = try Data(contentsOf: inputURL)
            guard let content = String(data: data, encoding: .utf8) else {
                return BackupResult(success: false, outputPath: nil,
                                    errorMessage: "Invalid UTF-8 in backup file",
                                    duration: CFAbsoluteTimeGetCurrent() - start, fileSize: nil)
            }
            _ = try await adapter.executeRaw(sql: "USE `\(database.replacingOccurrences(of: "`", with: "``"))`")

            // Statement splitter: naive — splits on `;` at line ends, skipping lines that
            // start with `--`. ClickHouse's VALUES bodies are emitted on a single line so
            // a trailing `;` is always statement-terminating in the output of this dumper.
            var statement = ""
            for rawLine in content.components(separatedBy: "\n") {
                let line = rawLine.trimmingCharacters(in: .whitespaces)
                if line.hasPrefix("--") || line.isEmpty { continue }
                statement += rawLine + "\n"
                if line.hasSuffix(";") {
                    let trimmed = statement.trimmingCharacters(in: .whitespacesAndNewlines)
                    let sql = trimmed.hasSuffix(";") ? String(trimmed.dropLast()) : trimmed
                    if !sql.isEmpty {
                        _ = try await adapter.executeRaw(sql: sql)
                    }
                    statement = ""
                }
            }

            let size = try? FileManager.default.attributesOfItem(atPath: inputURL.path)[.size] as? Int64
            return BackupResult(
                success: true, outputPath: nil, errorMessage: nil,
                duration: CFAbsoluteTimeGetCurrent() - start, fileSize: size
            )
        } catch {
            return BackupResult(
                success: false, outputPath: nil,
                errorMessage: "ClickHouse restore failed: \(error.localizedDescription)",
                duration: CFAbsoluteTimeGetCurrent() - start, fileSize: nil
            )
        }
    }
}
