// MCPAuditLogger.swift
// Gridex
//
// Async audit logger for MCP tool invocations.
// Writes JSONL to ~/Library/Application Support/Gridex/mcp-audit.jsonl

import Foundation

actor MCPAuditLogger {
    private let fileURL: URL
    private let encoder: JSONEncoder
    private let maxFileSize: Int64 = 100 * 1024 * 1024 // 100MB
    private var fileHandle: FileHandle?

    init() {
        let appSupport = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first!
        let gridexDir = appSupport.appendingPathComponent("Gridex", isDirectory: true)
        try? FileManager.default.createDirectory(at: gridexDir, withIntermediateDirectories: true)
        self.fileURL = gridexDir.appendingPathComponent("mcp-audit.jsonl")

        self.encoder = JSONEncoder()
        self.encoder.dateEncodingStrategy = .iso8601
        self.encoder.outputFormatting = .sortedKeys
    }

    func log(_ entry: MCPAuditEntry) async {
        do {
            try await rotateIfNeeded()
            let data = try encoder.encode(entry)
            guard var line = String(data: data, encoding: .utf8) else { return }
            line.append("\n")
            try await appendToFile(line)
        } catch {
            // Audit logging should never crash the app
            print("[MCP Audit] Failed to log entry: \(error)")
        }
    }

    private func appendToFile(_ line: String) async throws {
        let handle = try getOrCreateHandle()
        if let data = line.data(using: .utf8) {
            try handle.write(contentsOf: data)
        }
    }

    private func getOrCreateHandle() throws -> FileHandle {
        if let handle = fileHandle { return handle }

        if !FileManager.default.fileExists(atPath: fileURL.path) {
            FileManager.default.createFile(atPath: fileURL.path, contents: nil)
        }

        let handle = try FileHandle(forWritingTo: fileURL)
        try handle.seekToEnd()
        fileHandle = handle
        return handle
    }

    private func rotateIfNeeded() async throws {
        guard FileManager.default.fileExists(atPath: fileURL.path) else { return }
        let attrs = try FileManager.default.attributesOfItem(atPath: fileURL.path)
        guard let size = attrs[.size] as? Int64, size >= maxFileSize else { return }

        // Close current handle
        try fileHandle?.close()
        fileHandle = nil

        // Rotate: rename current to timestamped backup
        let timestamp = ISO8601DateFormatter().string(from: Date())
            .replacingOccurrences(of: ":", with: "-")
        let backupURL = fileURL.deletingLastPathComponent()
            .appendingPathComponent("mcp-audit-\(timestamp).jsonl")
        try FileManager.default.moveItem(at: fileURL, to: backupURL)
    }

    func close() async {
        try? fileHandle?.close()
        fileHandle = nil
    }

    // MARK: - Query Methods

    func recentEntries(limit: Int = 100) async throws -> [MCPAuditEntry] {
        guard FileManager.default.fileExists(atPath: fileURL.path) else { return [] }

        let data = try Data(contentsOf: fileURL)
        let lines = String(data: data, encoding: .utf8)?.components(separatedBy: .newlines) ?? []
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601

        var entries: [MCPAuditEntry] = []
        for line in lines.suffix(limit).reversed() {
            guard !line.isEmpty, let lineData = line.data(using: .utf8) else { continue }
            if let entry = try? decoder.decode(MCPAuditEntry.self, from: lineData) {
                entries.append(entry)
            }
        }
        return entries
    }

    func clearAll() async throws {
        try fileHandle?.close()
        fileHandle = nil
        try FileManager.default.removeItem(at: fileURL)
    }

    var logFileURL: URL { fileURL }
}
