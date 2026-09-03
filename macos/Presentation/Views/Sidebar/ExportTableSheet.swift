import SwiftUI
import AppKit

// MARK: - Export Format

enum ExportFormat: String, CaseIterable {
    case csv = "CSV"
    case json = "JSON"
    case sql = "SQL"
}

// MARK: - Export Table Sheet

struct ExportTableSheet: View {
    let tableName: String
    let schema: String?
    @EnvironmentObject private var appState: AppState
    @Environment(\.dismiss) private var dismiss

    // Table selection
    @State private var allTables: [String] = []
    @State private var selectedTables: Set<String> = []
    @State private var tableSearchText = ""

    // Format
    @State private var format: ExportFormat = .csv

    // CSV options
    @State private var convertNullToEmpty = true
    @State private var convertLineBreakToSpace = false
    @State private var includeFieldNames = true
    @State private var delimiter: String = ","
    @State private var quoteMode: String = "Quote if needed"
    @State private var lineBreak: String = "\\n"
    @State private var decimal: String = "."

    // State
    @State private var isExporting = false
    @State private var errorMessage: String?

    private let delimiters = [",", ";", "\\t", "|"]
    private let quoteModes = ["Quote if needed", "Always quote", "Never quote"]
    private let lineBreaks = ["\\n", "\\r\\n", "\\r"]
    private let decimals = [".", ","]

    private var filteredTables: [String] {
        if tableSearchText.isEmpty { return allTables }
        return allTables.filter { $0.localizedCaseInsensitiveContains(tableSearchText) }
    }

    var body: some View {
        VStack(spacing: 0) {
            // Header tabs
            HStack {
                Text("Items")
                    .font(.system(size: 12, weight: .medium))
                    .foregroundStyle(.secondary)
                    .frame(width: 200, alignment: .leading)
                    .padding(.leading, 16)

                Spacer()

                Text("Data")
                    .font(.system(size: 12, weight: .medium))
                    .foregroundStyle(.secondary)
            }
            .padding(.top, 10)
            .padding(.trailing, 16)
            .padding(.bottom, 6)

            Divider()

            // Main content: table list | options
            HStack(spacing: 0) {
                // Left: table list with checkboxes
                tableListPanel
                    .frame(width: 220)

                Divider()

                // Right: format tabs + options
                optionsPanel
            }
            .frame(minHeight: 380)

            Divider()

            // File name
            HStack {
                Text("File name:")
                    .foregroundStyle(.secondary)
                    .font(.system(size: 12))
                Text(exportFileName)
                    .font(.system(size: 12, weight: .medium))
                Spacer()
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 8)

            Divider()

            // Bottom buttons
            HStack {
                if let error = errorMessage {
                    Text(error)
                        .font(.system(size: 11))
                        .foregroundStyle(.red)
                        .lineLimit(2)
                }
                Spacer()
                Button("Cancel") { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Button("Export...") {
                    Task { await performExport() }
                }
                .buttonStyle(.borderedProminent)
                .keyboardShortcut(.defaultAction)
                .disabled(isExporting || selectedTables.isEmpty)
            }
            .padding(12)
        }
        .frame(width: 620, height: 520)
        .onAppear {
            loadTables()
            selectedTables = [tableName]
        }
    }

    // MARK: - Table List Panel

    private var tableListPanel: some View {
        VStack(spacing: 0) {
            List {
                // Schema group
                ForEach(filteredTables, id: \.self) { name in
                    HStack(spacing: 8) {
                        Toggle("", isOn: Binding(
                            get: { selectedTables.contains(name) },
                            set: { isOn in
                                if isOn { selectedTables.insert(name) }
                                else { selectedTables.remove(name) }
                            }
                        ))
                        .toggleStyle(.checkbox)
                        .labelsHidden()

                        Image(systemName: "tablecells")
                            .font(.system(size: 10))
                            .foregroundStyle(Color(red: 0.8, green: 0.5, blue: 0.2))

                        Text(name)
                            .font(.system(size: 12))
                            .lineLimit(1)
                    }
                    .padding(.vertical, 1)
                }
            }
            .listStyle(.plain)
            .environment(\.defaultMinListRowHeight, 24)
        }
    }

    // MARK: - Options Panel

    private var optionsPanel: some View {
        VStack(spacing: 0) {
            // Format tabs
            HStack(spacing: 0) {
                ForEach(ExportFormat.allCases, id: \.self) { fmt in
                    Button(action: { format = fmt }) {
                        Text(fmt.rawValue)
                            .font(.system(size: 12, weight: format == fmt ? .semibold : .regular))
                            .foregroundStyle(format == fmt ? .primary : .secondary)
                            .padding(.horizontal, 20)
                            .padding(.vertical, 6)
                            .background(format == fmt ? Color(nsColor: .selectedContentBackgroundColor).opacity(0.3) : .clear)
                            .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .pointerCursor()
                }
            }
            .background(Color(nsColor: .controlBackgroundColor))
            .clipShape(RoundedRectangle(cornerRadius: 6))
            .overlay(RoundedRectangle(cornerRadius: 6).stroke(Color(nsColor: .separatorColor), lineWidth: 0.5))
            .padding(.top, 16)
            .padding(.bottom, 12)

            Divider()

            // Options
            ScrollView {
                VStack(spacing: 12) {
                    switch format {
                    case .csv: csvOptions
                    case .json: jsonOptions
                    case .sql: sqlOptions
                    }
                }
                .padding(16)
            }

            Spacer()
        }
    }

    // MARK: - CSV Options

    private var csvOptions: some View {
        VStack(alignment: .leading, spacing: 10) {
            Toggle("Convert NULL to EMPTY", isOn: $convertNullToEmpty)
            Toggle("Convert line break to space", isOn: $convertLineBreakToSpace)
            Toggle("Put field names in the first row", isOn: $includeFieldNames)

            optionRow("Delimiter") {
                Picker("", selection: $delimiter) {
                    ForEach(delimiters, id: \.self) { d in Text(d).tag(d) }
                }.labelsHidden().frame(width: 160)
            }
            optionRow("Swap") {
                Picker("", selection: $quoteMode) {
                    ForEach(quoteModes, id: \.self) { m in Text(m).tag(m) }
                }.labelsHidden().frame(width: 160)
            }
            optionRow("Line break") {
                Picker("", selection: $lineBreak) {
                    ForEach(lineBreaks, id: \.self) { l in Text(l).tag(l) }
                }.labelsHidden().frame(width: 160)
            }
            optionRow("Decimal") {
                Picker("", selection: $decimal) {
                    ForEach(decimals, id: \.self) { d in Text(d).tag(d) }
                }.labelsHidden().frame(width: 160)
            }
        }
        .font(.system(size: 12))
    }

    // MARK: - JSON Options

    private var jsonOptions: some View {
        VStack(alignment: .leading, spacing: 10) {
            Toggle("Convert NULL to EMPTY", isOn: $convertNullToEmpty)
            Toggle("Pretty print", isOn: .constant(true))
        }
        .font(.system(size: 12))
    }

    // MARK: - SQL Options

    private var sqlOptions: some View {
        VStack(alignment: .leading, spacing: 10) {
            Toggle("Convert NULL to EMPTY", isOn: $convertNullToEmpty)
            Text("Generates INSERT INTO statements for all rows.")
                .font(.system(size: 11))
                .foregroundStyle(.secondary)
        }
        .font(.system(size: 12))
    }

    // MARK: - Helpers

    private func optionRow(_ label: String, @ViewBuilder content: () -> some View) -> some View {
        HStack {
            Text(label)
                .frame(width: 100, alignment: .trailing)
                .foregroundStyle(.secondary)
            content()
        }
    }

    private var exportFileName: String {
        if selectedTables.count == 1, let name = selectedTables.first {
            return "\(name).\(format.rawValue.lowercased())"
        }
        return "export.\(format.rawValue.lowercased())"
    }

    private func loadTables() {
        // Extract table names from sidebar items
        func extractTables(_ items: [SidebarItem]) -> [String] {
            var result: [String] = []
            for item in items {
                if case .table(let name) = item.type {
                    result.append(name)
                }
                result.append(contentsOf: extractTables(item.children))
            }
            return result
        }
        allTables = extractTables(appState.sidebarItems).sorted()
    }

    // MARK: - Export Action

    private func performExport() async {
        guard let adapter = appState.activeAdapter else {
            errorMessage = "No active connection"
            return
        }
        guard !selectedTables.isEmpty else { return }

        isExporting = true
        errorMessage = nil

        do {
            if selectedTables.count == 1, let table = selectedTables.first {
                // Single table export
                let url = showSavePanel(defaultName: "\(table).\(format.rawValue.lowercased())")
                guard let url else { isExporting = false; return }
                try await exportTable(table, to: url, adapter: adapter)
            } else {
                // Multiple tables — pick a directory
                let dir = showDirectoryPanel()
                guard let dir else { isExporting = false; return }
                for table in selectedTables.sorted() {
                    let url = dir.appendingPathComponent("\(table).\(format.rawValue.lowercased())")
                    try await exportTable(table, to: url, adapter: adapter)
                }
            }
            dismiss()
        } catch {
            errorMessage = error.localizedDescription
        }

        isExporting = false
    }

    private func exportTable(_ table: String, to url: URL, adapter: any DatabaseAdapter) async throws {
        let d = adapter.databaseType.sqlDialect
        let sql = "SELECT * FROM \(d.qualifiedIdentifier(table, schema: schema))"
        var result = try await adapter.executeRaw(sql: sql)

        if convertNullToEmpty {
            result = convertNulls(in: result)
        }

        switch format {
        case .csv:
            try await exportCSV(data: result, to: url)
        case .json:
            try await ExportService().exportJSON(data: result, to: url)
        case .sql:
            // Full-fidelity SQL export — fetch structure first so we can emit
            // DROP/CREATE/sequences/indices alongside the INSERT rows.
            let description = try await adapter.describeTable(name: table, schema: schema)
            try await ExportService().exportTableSQL(
                description: description,
                rows: result.rows,
                databaseType: adapter.databaseType,
                databaseName: appState.currentDatabaseName ?? "",
                to: url
            )
        }
    }

    private func exportCSV(data: QueryResult, to url: URL) async throws {
        let sep = delimiter == "\\t" ? "\t" : delimiter
        let lb: String
        switch lineBreak {
        case "\\r\\n": lb = "\r\n"
        case "\\r": lb = "\r"
        default: lb = "\n"
        }

        var csv = ""
        if includeFieldNames {
            csv += data.columns.map(\.name).joined(separator: sep) + lb
        }

        for row in data.rows {
            csv += row.map { value in
                var str = value.description
                if convertLineBreakToSpace {
                    str = str.replacingOccurrences(of: "\n", with: " ")
                        .replacingOccurrences(of: "\r", with: " ")
                }
                if decimal != "." {
                    if Double(str) != nil {
                        str = str.replacingOccurrences(of: ".", with: decimal)
                    }
                }
                switch quoteMode {
                case "Always quote":
                    return "\"\(str.replacingOccurrences(of: "\"", with: "\"\""))\""
                case "Never quote":
                    return str
                default:
                    if str.contains(sep) || str.contains("\"") || str.contains("\n") || str.contains("\r") {
                        return "\"\(str.replacingOccurrences(of: "\"", with: "\"\""))\""
                    }
                    return str
                }
            }.joined(separator: sep) + lb
        }

        try csv.write(to: url, atomically: true, encoding: .utf8)
    }

    private func convertNulls(in result: QueryResult) -> QueryResult {
        let rows = result.rows.map { row in
            row.map { value in
                value.isNull ? RowValue.string("") : value
            }
        }
        return QueryResult(
            columns: result.columns,
            rows: rows,
            rowsAffected: result.rowsAffected,
            executionTime: result.executionTime,
            queryType: result.queryType
        )
    }

    @MainActor
    private func showSavePanel(defaultName: String) -> URL? {
        let panel = NSSavePanel()
        panel.title = "Export"
        panel.nameFieldStringValue = defaultName
        panel.allowedContentTypes = [.data]
        panel.canCreateDirectories = true
        guard panel.runModal() == .OK else { return nil }
        return panel.url
    }

    @MainActor
    private func showDirectoryPanel() -> URL? {
        let panel = NSOpenPanel()
        panel.title = "Choose export folder"
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.canCreateDirectories = true
        guard panel.runModal() == .OK else { return nil }
        return panel.url
    }
}
