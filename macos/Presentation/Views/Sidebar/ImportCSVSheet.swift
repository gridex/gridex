// ImportCSVSheet.swift
// Gridex
//
// Dialog to import rows from a CSV file into an existing table.
// Auto-detects columns from the CSV header and maps to table columns by name.

import SwiftUI
import AppKit

struct ImportCSVSheet: View {
    let tableName: String
    let schema: String?
    @EnvironmentObject private var appState: AppState
    @Environment(\.dismiss) private var dismiss

    @State private var fileURL: URL?
    @State private var delimiter: String = ","
    @State private var hasHeader: Bool = true
    @State private var previewRows: [[String]] = []
    @State private var headerRow: [String] = []
    @State private var tableColumns: [String] = []
    @State private var columnMapping: [String: String] = [:]  // CSV header → table column
    @State private var isImporting = false
    @State private var errorMessage: String?
    @State private var importedCount: Int = 0

    private let delimiters = [",", ";", "\\t", "|"]

    var body: some View {
        VStack(spacing: 0) {
            // Header
            Text("Import CSV to '\(tableName)'")
                .font(.system(size: 14, weight: .semibold))
                .padding(.top, 16)
                .padding(.bottom, 12)

            Divider()

            ScrollView {
                VStack(alignment: .leading, spacing: 12) {
                    // File picker
                    HStack {
                        Text("File:")
                            .frame(width: 90, alignment: .trailing)
                            .foregroundStyle(.secondary)
                            .font(.system(size: 12))
                        if let url = fileURL {
                            Text(url.lastPathComponent)
                                .font(.system(size: 12))
                                .lineLimit(1)
                            Spacer()
                            Button("Change…") { pickFile() }
                                .pointerCursor()
                        } else {
                            Button("Choose CSV file…") { pickFile() }
                                .pointerCursor()
                            Spacer()
                        }
                    }

                    // Options
                    optionRow("Delimiter") {
                        Picker("", selection: $delimiter) {
                            ForEach(delimiters, id: \.self) { d in Text(d).tag(d) }
                        }
                        .labelsHidden()
                        .frame(width: 140)
                        .onChange(of: delimiter) { _, _ in loadPreview() }
                    }

                    optionRow("First row is header") {
                        Toggle("", isOn: $hasHeader)
                            .labelsHidden()
                            .onChange(of: hasHeader) { _, _ in loadPreview() }
                    }

                    // Column mapping
                    if !headerRow.isEmpty && !tableColumns.isEmpty {
                        Divider()
                        Text("Column mapping")
                            .font(.system(size: 12, weight: .semibold))
                            .foregroundStyle(.secondary)

                        ForEach(headerRow, id: \.self) { csvCol in
                            HStack {
                                Text(csvCol)
                                    .frame(width: 140, alignment: .trailing)
                                    .font(.system(size: 11, design: .monospaced))
                                    .foregroundStyle(.secondary)
                                Image(systemName: "arrow.right")
                                    .font(.system(size: 9))
                                    .foregroundStyle(.tertiary)
                                Picker("", selection: Binding(
                                    get: { columnMapping[csvCol] ?? "" },
                                    set: { columnMapping[csvCol] = $0 }
                                )) {
                                    Text("— skip —").tag("")
                                    ForEach(tableColumns, id: \.self) { col in
                                        Text(col).tag(col)
                                    }
                                }
                                .labelsHidden()
                            }
                        }
                    }

                    // Preview
                    if !previewRows.isEmpty {
                        Divider()
                        Text("Preview (\(previewRows.count) rows)")
                            .font(.system(size: 12, weight: .semibold))
                            .foregroundStyle(.secondary)

                        VStack(alignment: .leading, spacing: 2) {
                            ForEach(Array(previewRows.prefix(5).enumerated()), id: \.offset) { _, row in
                                Text(row.joined(separator: " | "))
                                    .font(.system(size: 10, design: .monospaced))
                                    .foregroundStyle(.tertiary)
                                    .lineLimit(1)
                            }
                        }
                        .padding(8)
                        .background(Color(nsColor: .controlBackgroundColor))
                        .cornerRadius(6)
                    }
                }
                .padding(16)
            }
            .frame(minHeight: 300)

            Divider()

            // Status + buttons
            HStack {
                if let err = errorMessage {
                    Text(err)
                        .font(.system(size: 11))
                        .foregroundStyle(.red)
                        .lineLimit(2)
                } else if importedCount > 0 {
                    Text("Imported \(importedCount) rows")
                        .font(.system(size: 11))
                        .foregroundStyle(.green)
                }
                Spacer()
                Button("Cancel") { dismiss() }
                    .keyboardShortcut(.cancelAction)
                    .pointerCursor()
                Button("Import") {
                    Task { await performImport() }
                }
                .buttonStyle(.borderedProminent)
                .pointerCursor()
                .disabled(fileURL == nil || isImporting || columnMapping.values.allSatisfy { $0.isEmpty })
            }
            .padding(12)
        }
        .frame(width: 500, height: 540)
        .task { await loadTableColumns() }
    }

    // MARK: - Helpers

    private func optionRow(_ label: String, @ViewBuilder content: () -> some View) -> some View {
        HStack {
            Text(label)
                .frame(width: 140, alignment: .trailing)
                .foregroundStyle(.secondary)
                .font(.system(size: 12))
            content()
            Spacer()
        }
    }

    private func pickFile() {
        let panel = NSOpenPanel()
        panel.title = "Choose CSV file"
        panel.allowsMultipleSelection = false
        panel.canChooseDirectories = false
        panel.allowedContentTypes = [.commaSeparatedText, .plainText, .data]
        if panel.runModal() == .OK, let url = panel.url {
            fileURL = url
            loadPreview()
        }
    }

    private func loadPreview() {
        guard let url = fileURL else { return }
        do {
            let content = try String(contentsOf: url, encoding: .utf8)
            let sep = delimiter == "\\t" ? "\t" : delimiter
            let lines = content.components(separatedBy: .newlines).filter { !$0.isEmpty }
            guard !lines.isEmpty else { return }

            let parsed = lines.map { parseCSVLine($0, separator: sep) }

            if hasHeader, let first = parsed.first {
                headerRow = first
                previewRows = Array(parsed.dropFirst().prefix(10))
            } else {
                headerRow = (0..<(parsed.first?.count ?? 0)).map { "col_\($0 + 1)" }
                previewRows = Array(parsed.prefix(10))
            }

            // Auto-map columns by name match
            for csvCol in headerRow {
                if columnMapping[csvCol] == nil {
                    if tableColumns.contains(csvCol) {
                        columnMapping[csvCol] = csvCol
                    } else {
                        columnMapping[csvCol] = ""
                    }
                }
            }
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    private func parseCSVLine(_ line: String, separator: String) -> [String] {
        // Basic CSV parsing with quoted field support
        var result: [String] = []
        var current = ""
        var inQuotes = false
        var i = line.startIndex
        while i < line.endIndex {
            let c = line[i]
            if c == "\"" {
                if inQuotes, line.index(after: i) < line.endIndex, line[line.index(after: i)] == "\"" {
                    current.append("\"")
                    i = line.index(i, offsetBy: 2)
                    continue
                }
                inQuotes.toggle()
            } else if String(c) == separator && !inQuotes {
                result.append(current)
                current = ""
            } else {
                current.append(c)
            }
            i = line.index(after: i)
        }
        result.append(current)
        return result
    }

    private func loadTableColumns() async {
        guard let adapter = appState.activeAdapter else { return }
        if let desc = try? await adapter.describeTable(name: tableName, schema: schema) {
            tableColumns = desc.columns.map(\.name)
        }
    }

    private func performImport() async {
        guard let adapter = appState.activeAdapter,
              let url = fileURL else { return }
        isImporting = true
        errorMessage = nil
        importedCount = 0
        defer { isImporting = false }

        do {
            let content = try String(contentsOf: url, encoding: .utf8)
            let sep = delimiter == "\\t" ? "\t" : delimiter
            let lines = content.components(separatedBy: .newlines).filter { !$0.isEmpty }
            guard !lines.isEmpty else { return }

            let rows = lines.map { parseCSVLine($0, separator: sep) }
            let dataRows = hasHeader ? Array(rows.dropFirst()) : rows

            // Build INSERT statements
            let mappedCols = headerRow.compactMap { csvCol -> (csvIdx: Int, tableCol: String)? in
                guard let tableCol = columnMapping[csvCol], !tableCol.isEmpty,
                      let idx = headerRow.firstIndex(of: csvCol) else { return nil }
                return (idx, tableCol)
            }
            guard !mappedCols.isEmpty else {
                errorMessage = "No columns mapped"
                return
            }

            let d = adapter.databaseType.sqlDialect
            let colList = mappedCols.map { d.quoteIdentifier($0.tableCol) }.joined(separator: ", ")

            for row in dataRows {
                let values = mappedCols.map { mapping -> String in
                    guard mapping.csvIdx < row.count else { return "NULL" }
                    let raw = row[mapping.csvIdx]
                    if raw.isEmpty { return "NULL" }
                    return "'\(raw.replacingOccurrences(of: "'", with: "''"))'"
                }.joined(separator: ", ")

                let table = d.qualifiedIdentifier(tableName, schema: schema)
                let sql = "INSERT INTO \(table) (\(colList)) VALUES (\(values))"
                _ = try await adapter.executeRaw(sql: sql)
                importedCount += 1
            }
        } catch {
            errorMessage = error.localizedDescription
        }
    }
}
