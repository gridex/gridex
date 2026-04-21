// DataGridView.swift
// Gridex
//
// SwiftUI data grid with pagination, inline editing, column resize, and visual diff.
// Styled after professional database GUIs (professional database GUI).

import SwiftUI
import AppKit

enum DataGridViewMode: String {
    case data
    case structure
}

struct DataGridView: View {
    let tableName: String
    let schema: String?
    let tabId: UUID
    var initialViewMode: String?

    @EnvironmentObject private var appState: AppState
    @State private var showDiscardWarning = false
    @State private var viewMode: DataGridViewMode = .data

    var body: some View {
        let viewModel = appState.cachedDataGridState(for: tabId)
        DataGridContentView(
            tableName: tableName,
            schema: schema,
            viewModel: viewModel,
            showDiscardWarning: $showDiscardWarning,
            viewMode: $viewMode
        )
        .onAppear {
            if initialViewMode == "structure" {
                viewMode = .structure
            }
        }
        .onReceive(NotificationCenter.default.publisher(for: .init("switchToStructure"))) { notif in
            if let name = notif.object as? String, name == tableName {
                viewMode = .structure
            }
        }
    }
}

private struct DataGridContentView: View {
    let tableName: String
    let schema: String?
    @ObservedObject var viewModel: DataGridViewState
    @Binding var showDiscardWarning: Bool
    @Binding var viewMode: DataGridViewMode
    @State private var showAddIndex = false
    @EnvironmentObject private var appState: AppState

    var body: some View {
        VStack(spacing: 0) {
            if viewMode == .data {
                // Toolbar
                DataGridToolbar(viewModel: viewModel)

                // Filter bar — Redis uses pattern-based filter, SQL uses column filter
                if viewModel.showFilterBar {
                    if appState.activeConfig?.databaseType == .redis {
                        RedisFilterBar(
                            initialFilter: viewModel.activeFilter,
                            onApply: { filter in
                                viewModel.activeFilter = filter
                                Task { await viewModel.applyFilter() }
                            },
                            onClear: {
                                viewModel.activeFilter = nil
                                Task { await viewModel.applyFilter() }
                            },
                            onDismiss: {
                                viewModel.showFilterBar = false
                            }
                        )
                    } else {
                        FilterBarSwiftUIView(
                            columns: viewModel.columns,
                            initialFilter: viewModel.activeFilter,
                            onApply: { filter in
                                viewModel.activeFilter = filter
                                Task { await viewModel.applyFilter() }
                            },
                            onClear: {
                                viewModel.activeFilter = nil
                                Task { await viewModel.applyFilter() }
                            },
                            onDismiss: {
                                viewModel.showFilterBar = false
                            }
                        )
                    }
                }

                // Data grid
                if viewModel.isLoading && viewModel.rows.isEmpty {
                    ProgressView("Loading...")
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                } else if viewModel.columns.isEmpty {
                    Text("No data")
                        .foregroundStyle(.secondary)
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                } else {
                    AppKitDataGrid(
                        viewModel: viewModel,
                        onSelectRows: { rows in
                            viewModel.editingCell = nil
                            viewModel.selectedRows = rows
                            updateSelectedRowDetails()
                        },
                        onFKClick: { refTable, refColumn, value in
                            appState.openTableWithFilter(
                                name: refTable,
                                schema: schema,
                                filterColumn: refColumn,
                                filterValue: value
                            )
                        }
                    )
                }
            } else {
                // Structure view
                InlineStructureView(
                    tableDescription: viewModel.tableDescription,
                    isLoading: viewModel.tableDescription == nil && viewModel.isLoading,
                    tableName: tableName,
                    schema: schema,
                    adapter: appState.activeAdapter,
                    onStructureChanged: {
                        await viewModel.reloadAfterStructureChange()
                        return viewModel.tableDescription
                    },
                    onSelectColumn: { details in
                        appState.selectedRowDetails = details
                        appState.onDetailFieldEdit = nil
                    },
                    showAddIndex: $showAddIndex
                )
            }

            // Bottom bar with Data/Structure tabs + pagination
            BottomTabBar(
                viewMode: $viewMode,
                viewModel: viewModel,
                onAddColumn: { NotificationCenter.default.post(name: .init("structureAddColumn"), object: nil) },
                showAddIndex: $showAddIndex
            )

            // Query log panel
            if appState.showQueryLog {
                QueryLogPanel(appState: appState)
            }
        }
        .task {
            viewModel.appState = appState
            // Skip reload if already loaded (cached from previous tab switch)
            guard viewModel.columns.isEmpty else { return }
            await viewModel.load(
                tableName: tableName,
                schema: schema,
                adapter: appState.activeAdapter
            )
        }
        .onChange(of: viewModel.statusRowCount) { _, count in
            appState.statusRowCount = count
        }
        .onChange(of: viewModel.executionTime) { _, time in
            appState.statusQueryTime = time
        }
        .onChange(of: viewModel.sortColumn) { _, _ in
            Task { await viewModel.loadPage(0) }
        }
        .onChange(of: viewModel.sortAscending) { _, _ in
            Task { await viewModel.loadPage(0) }
        }
        .sheet(isPresented: $viewModel.showCommitPreview) {
            CommitPreviewSheet(viewModel: viewModel)
        }
        .alert("Warning", isPresented: $showDiscardWarning) {
            Button("Cancel", role: .cancel) {}
            Button("Discard", role: .destructive) {
                viewModel.discardAllChanges()
                Task { await viewModel.loadPage(viewModel.currentPage) }
            }
        } message: {
            Text("Discard all changes?\nTips: You can commit the changes by\n1. Command + S.\n2. Use the top left segment control.")
        }
        .alert("Error", isPresented: .constant(viewModel.errorMessage != nil)) {
            Button("OK") { viewModel.dismissError() }
        } message: {
            Text(viewModel.errorMessage ?? "")
        }
        // Cmd+S: commit changes
        .onReceive(NotificationCenter.default.publisher(for: .commitChanges)) { _ in
            if viewModel.hasPendingChanges {
                viewModel.prepareCommit()
            }
        }
        // Delete selected rows
        .onReceive(NotificationCenter.default.publisher(for: .deleteSelectedRows)) { _ in
            viewModel.deleteSelectedRows()
        }
        // Cmd+R: reload (with warning if pending changes)
        .onReceive(NotificationCenter.default.publisher(for: .reloadData)) { _ in
            if viewModel.hasPendingChanges {
                showDiscardWarning = true
            } else {
                Task { await viewModel.loadPage(viewModel.currentPage) }
            }
        }
        // Cmd+F: toggle filter bar
        .onReceive(NotificationCenter.default.publisher(for: .toggleFilterBar)) { _ in
            viewModel.showFilterBar.toggle()
        }
    }

    private func updateSelectedRowDetails() {
        guard viewModel.selectedRows.count == 1,
              let rowIndex = viewModel.selectedRows.first,
              rowIndex < viewModel.rows.count else {
            appState.selectedRowDetails = nil
            appState.onDetailFieldEdit = nil
            return
        }
        let row = viewModel.rows[rowIndex]
        appState.selectedRowDetails = viewModel.columns.enumerated().map { idx, col in
            let value = idx < row.count ? row[idx].displayString : ""
            return (column: col.name, value: value)
        }
        appState.onDetailFieldEdit = { [weak viewModel] colIdx, newValue in
            guard let viewModel else { return }
            viewModel.commitCellEdit(rowIndex: rowIndex, colIdx: colIdx, newText: newValue)
            // Update the details panel to reflect the edit
            if colIdx < viewModel.columns.count {
                self.appState.selectedRowDetails?[colIdx].value = newValue
            }
        }
    }

}

// MARK: - DataGrid View State

// MARK: - Query Log Entry

struct QueryLogEntry: Identifiable {
    let id = UUID()
    let sql: String
    let timestamp: Date
    let duration: TimeInterval?
    /// Pre-computed highlighted SQL (with syntax coloring)
    let highlightedColored: AttributedString
    /// Pre-computed highlighted SQL (plain, no coloring)
    let highlightedPlain: AttributedString

    init(sql: String, timestamp: Date, duration: TimeInterval?) {
        self.sql = sql
        self.timestamp = timestamp
        self.duration = duration
        self.highlightedColored = Self.buildHighlightedSQL(sql, colored: true)
        self.highlightedPlain = Self.buildHighlightedSQL(sql, colored: false)
    }

    static func buildHighlightedSQL(_ sql: String, colored: Bool) -> AttributedString {
        let fullStr = sql + ";"
        var result = AttributedString(fullStr)
        result.font = .system(size: 12, design: .monospaced)
        result.foregroundColor = .primary

        guard colored else { return result }

        let keywords = [
            "SELECT", "FROM", "WHERE", "ORDER BY", "LIMIT", "OFFSET",
            "INSERT", "UPDATE", "DELETE", "SET", "INTO", "VALUES",
            "JOIN", "LEFT", "RIGHT", "INNER", "OUTER", "ON",
            "GROUP BY", "HAVING", "AS", "AND", "OR", "NOT", "IN",
            "COUNT", "SUM", "AVG", "MIN", "MAX", "DISTINCT",
            "ASC", "DESC", "NULL", "IS", "LIKE", "BETWEEN",
            "CREATE", "ALTER", "DROP", "TABLE", "INDEX",
            "BEGIN", "COMMIT", "ROLLBACK"
        ]

        let charCount = fullStr.count
        var coloredFlags = [Bool](repeating: false, count: charCount)
        let chars = Array(fullStr.unicodeScalars)
        let upperChars = Array(fullStr.uppercased().unicodeScalars)

        func applyColor(_ color: Color, startOffset: Int, length: Int) {
            let startIdx = result.index(result.startIndex, offsetByCharacters: startOffset)
            let endIdx = result.index(startIdx, offsetByCharacters: length)
            result[startIdx..<endIdx].foregroundColor = color
            for i in startOffset..<min(startOffset + length, charCount) {
                coloredFlags[i] = true
            }
        }

        for keyword in keywords {
            let kwChars = Array(keyword.unicodeScalars)
            let kwLen = kwChars.count
            var i = 0
            while i <= charCount - kwLen {
                var match = true
                for j in 0..<kwLen {
                    if upperChars[i + j] != kwChars[j] {
                        match = false
                        break
                    }
                }
                if match {
                    let before = i == 0 || !CharacterSet.letters.contains(chars[i - 1])
                    let after = (i + kwLen) >= charCount || !CharacterSet.letters.contains(chars[i + kwLen])
                    if before && after {
                        applyColor(Color(nsColor: .systemBlue), startOffset: i, length: kwLen)
                        i += kwLen
                        continue
                    }
                }
                i += 1
            }
        }

        var i = 0
        while i < charCount {
            if chars[i] == "\"" {
                if let closeIdx = (i + 1..<charCount).first(where: { chars[$0] == "\"" }) {
                    let len = closeIdx - i + 1
                    applyColor(Color(nsColor: .systemGreen), startOffset: i, length: len)
                    i = closeIdx + 1
                    continue
                }
            }
            i += 1
        }

        i = 0
        while i < charCount {
            if !coloredFlags[i] && chars[i].properties.numericType != nil {
                var end = i + 1
                while end < charCount && chars[end].properties.numericType != nil { end += 1 }
                let before = i == 0 || !CharacterSet.letters.contains(chars[i - 1])
                let after = end >= charCount || !CharacterSet.letters.contains(chars[end])
                if before && after {
                    applyColor(Color(nsColor: .systemOrange), startOffset: i, length: end - i)
                }
                i = end
            } else {
                i += 1
            }
        }

        return result
    }
}

@MainActor
final class DataGridViewState: ObservableObject {
    @Published var columns: [ColumnHeader] = []
    @Published var rows: [[RowValue]] = []
    @Published var totalRows: Int = 0
    @Published var currentPage: Int = 0
    @Published var isLoading = false
    @Published var executionTime: TimeInterval = 0
    @Published var sortColumn: String?
    @Published var sortAscending = true
    @Published var showFilterBar = false
    @Published var activeFilter: FilterExpression?
    @Published var primaryKeyColumns: [String] = []
    @Published var showCommitPreview = false
    @Published var commitSQL: [(sql: String, parameters: [RowValue])] = []
    @Published var commitError: String?
    @Published var isCommitting = false
    @Published var errorMessage: String?
    @Published var tableDescription: TableDescription?

    // Non-published: high-frequency changes that only the coordinator observes via Combine.
    // Removing @Published avoids SwiftUI body re-evaluation on every click/resize/edit.
    var columnWidths: [String: CGFloat] = [:]
    var editingCell: CellID?
    var selectedRows: Set<Int> = []
    var insertedRowIndices: Set<Int> = []

    // Column metadata for UI indicators
    var foreignKeyColumns: [String: String] = [:]     // column -> "referenced_table"
    var foreignKeyRefColumns: [String: String] = [:]  // column -> "referenced_column"
    var columnDefaults: [String: String] = [:]     // column -> default expression
    var columnEnumValues: [String: [String]] = [:] // column -> enum labels (for USER-DEFINED enum types)

    let changeTracker = DefaultChangeTracker()
    let pageSize = 300

    /// Pre-computed display strings for the current page — avoids repeated
    /// `displayString` computation during cell rendering and scrolling.
    var displayCache: [[String]] = []

    func rebuildDisplayCache() {
        displayCache = rows.map { row in
            row.map { $0.displayString }
        }
    }

    func updateDisplayCache(row: Int, col: Int) {
        guard row < displayCache.count, col < displayCache[row].count, row < rows.count, col < rows[row].count else { return }
        displayCache[row][col] = rows[row][col].displayString
    }

    var statusRowCount: Int? { totalRows > 0 ? totalRows : nil }

    private(set) var adapter: (any DatabaseAdapter)?
    private(set) var tableName: String = ""
    private(set) var schema: String?
    weak var appState: AppState?

    // MARK: - Schema metadata cache (survives tab switches)

    private struct TableMetadataCache {
        var primaryKeyColumns: [String]
        var foreignKeyColumns: [String: String]
        var foreignKeyRefColumns: [String: String]
        var columnDefaults: [String: String]
        var columnEnumValues: [String: [String]]
        var estimatedRowCount: Int?
        var tableDescription: TableDescription?
    }

    private static var metadataCache: [String: TableMetadataCache] = [:]

    private var cacheKey: String {
        "\(schema ?? "public").\(tableName)"
    }

    static func clearMetadataCache() {
        metadataCache.removeAll()
    }

    struct CellID: Equatable {
        let row: Int
        let col: Int
    }

    func load(tableName: String, schema: String?, adapter: (any DatabaseAdapter)?) async {
        guard let adapter else { return }
        self.adapter = adapter
        self.tableName = tableName
        self.schema = schema

        isLoading = true

        // Apply cache immediately if available (before any query)
        let hasCachedMeta: Bool
        if let cached = Self.metadataCache[cacheKey] {
            self.primaryKeyColumns = cached.primaryKeyColumns
            self.foreignKeyColumns = cached.foreignKeyColumns
            self.foreignKeyRefColumns = cached.foreignKeyRefColumns
            self.columnDefaults = cached.columnDefaults
            self.columnEnumValues = cached.columnEnumValues
            self.tableDescription = cached.tableDescription
            if let count = cached.estimatedRowCount, count > 0 {
                self.totalRows = count
            }
            hasCachedMeta = true
        } else {
            hasCachedMeta = false
        }

        let schemaFilter = schema ?? "public"
        let d = adapter.databaseType.sqlDialect
        let qualifiedTable = "\(d.quoteIdentifier(schemaFilter)).\(d.quoteIdentifier(tableName))"

        // Fetch rows first, load metadata in background (non-blocking)
        let fetchSQL = buildFetchSQL(orderBy: nil, limit: pageSize, offset: 0)

        // 1. Fetch rows — show data ASAP
        do {
            let start = Date()
            let result = try await adapter.fetchRows(table: tableName, schema: schema, columns: nil, where: activeFilter, orderBy: nil, limit: pageSize, offset: 0)
            let duration = Date().timeIntervalSince(start)
            logQuery(sql: fetchSQL, duration: duration)

            self.columns = result.columns
            self.rows = result.rows
            rebuildDisplayCache()
            if self.totalRows == 0 { self.totalRows = result.rowCount }
            self.executionTime = result.executionTime
            computeColumnWidths(columns: result.columns, rows: result.rows)
        } catch {
            errorMessage = Self.detailedErrorMessage(error)
            isLoading = false
            return
        }

        isLoading = false

        // 2. Load metadata in background (FK icons, structure) — doesn't block row display
        if hasCachedMeta {
            Task {
                if let desc = try? await adapter.describeTable(name: tableName, schema: schema) {
                    self.tableDescription = desc
                    Self.metadataCache[self.cacheKey]?.tableDescription = desc
                    self.objectWillChange.send()
                }
            }
            return
        }

        let start = Date()
        async let descTask = adapter.describeTable(name: tableName, schema: schema)
        async let enumTask: [String: [String]] = {
            guard adapter.databaseType == .postgresql else { return [:] }
            let sql = """
                SELECT a.attname, e.enumlabel
                FROM pg_attribute a
                JOIN pg_class cls ON cls.oid = a.attrelid
                JOIN pg_namespace ns ON ns.oid = cls.relnamespace
                JOIN pg_type t ON t.oid = a.atttypid
                JOIN pg_enum e ON e.enumtypid = t.oid
                WHERE cls.relname = $1 AND ns.nspname = $2
                  AND a.attnum > 0 AND NOT a.attisdropped
                ORDER BY a.attname, e.enumsortorder
                """
            guard let result = try? await adapter.executeWithRowValues(sql: sql, parameters: [.string(tableName), .string(schemaFilter)]) else { return [:] }
            var enums: [String: [String]] = [:]
            for row in result.rows {
                if let colName = row[0].stringValue, let label = row[1].stringValue {
                    enums[colName, default: []].append(label)
                }
            }
            return enums
        }()

        if let desc = try? await descTask {
            let dur = Date().timeIntervalSince(start)
            self.primaryKeyColumns = desc.columns.filter { $0.isPrimaryKey }.map { $0.name }
            for col in desc.columns {
                if let def = col.defaultValue, !col.isAutoIncrement {
                    self.columnDefaults[col.name] = def
                }
            }
            for fk in desc.foreignKeys {
                for (i, col) in fk.columns.enumerated() {
                    self.foreignKeyColumns[col] = fk.referencedTable
                    if i < fk.referencedColumns.count {
                        self.foreignKeyRefColumns[col] = fk.referencedColumns[i]
                    }
                }
            }
            if let count = desc.estimatedRowCount, count > 0 {
                self.totalRows = count
            }
            self.tableDescription = desc
            self.logQuery(sql: "-- describeTable(\(qualifiedTable)) → \(desc.columns.count) cols, \(desc.indexes.count) idx, \(desc.foreignKeys.count) fks", duration: dur)
        }

        self.columnEnumValues = await enumTask

        // Save to cache
        Self.metadataCache[self.cacheKey] = TableMetadataCache(
            primaryKeyColumns: self.primaryKeyColumns,
            foreignKeyColumns: self.foreignKeyColumns,
            foreignKeyRefColumns: self.foreignKeyRefColumns,
            columnDefaults: self.columnDefaults,
            columnEnumValues: self.columnEnumValues,
            estimatedRowCount: self.totalRows,
            tableDescription: self.tableDescription
        )
        self.objectWillChange.send()
    }

    func reloadStructure() async {
        guard let adapter else { return }
        do {
            tableDescription = try await adapter.describeTable(name: tableName, schema: schema)
        } catch {
            // Structure reload error — silently ignore
        }
    }

    /// Invalidate cache and reload everything (after structure changes like ADD COLUMN)
    func reloadAfterStructureChange() async {
        Self.metadataCache.removeValue(forKey: cacheKey)
        guard let adapter else { return }
        await load(tableName: tableName, schema: schema, adapter: adapter)
    }

    func loadPage(_ page: Int) async {
        guard let adapter else { return }
        isLoading = true
        defer { isLoading = false }

        do {
            let offset = page * pageSize
            // Use explicit sort if set, otherwise fall back to primary key for stable ordering
            let sort: [QuerySortDescriptor]? = if let col = sortColumn {
                [QuerySortDescriptor(column: col, direction: sortAscending ? .ascending : .descending)]
            } else if let pk = primaryKeyColumns.first {
                [QuerySortDescriptor(column: pk, direction: .ascending)]
            } else {
                nil
            }
            let sql = buildFetchSQL(orderBy: sort, limit: pageSize, offset: offset)
            let start = Date()
            let result = try await adapter.fetchRows(table: tableName, schema: schema, columns: nil, where: activeFilter, orderBy: sort, limit: pageSize, offset: offset)
            let duration = Date().timeIntervalSince(start)
            logQuery(sql: sql, duration: duration)
            self.rows = result.rows
            rebuildDisplayCache()
            self.currentPage = page
            self.executionTime = result.executionTime
            // Update row count when filter is active via a proper COUNT query
            if activeFilter != nil {
                let countSQL = buildCountSQL()
                if let countResult = try? await adapter.executeRaw(sql: countSQL),
                   let firstRow = countResult.rows.first,
                   let firstVal = firstRow.first {
                    switch firstVal {
                    case .integer(let n): self.totalRows = Int(n)
                    case .string(let s): self.totalRows = Int(s) ?? (result.rows.count + offset)
                    default: self.totalRows = result.rows.count + offset
                    }
                }
            }
        } catch {
            errorMessage = Self.detailedErrorMessage(error)
        }
    }

    func applyFilter() async {
        await loadPage(0)
    }

    func nextPage() async {
        await loadPage(currentPage + 1)
    }

    func previousPage() async {
        guard currentPage > 0 else { return }
        await loadPage(currentPage - 1)
    }

    func dismissError() {
        errorMessage = nil
    }

    // MARK: - Error Formatting

    static func detailedErrorMessage(_ error: Error) -> String {
        // PSQLError intentionally hides details in String(describing:).
        // String(reflecting:) includes the full debug info (server message, detail, hint).
        let debug = String(reflecting: error)
        let describing = String(describing: error)

        // If reflecting gives us substantially more info than describing, prefer it
        if debug.count > describing.count + 20 {
            return debug
        }
        // If describing is informative enough, use it
        if describing.count > 40, !describing.contains("prevent accidental leakage") {
            return describing
        }
        // Fall back to localizedDescription
        let localized = error.localizedDescription
        if localized.count > 20, !localized.contains("error 1") {
            return localized
        }
        return debug
    }

    // MARK: - Query Log

    private func logQuery(sql: String, duration: TimeInterval?) {
        appState?.logQuery(sql: sql, duration: duration)
    }

    private func buildFetchSQL(orderBy: [QuerySortDescriptor]?, limit: Int, offset: Int) -> String {
        guard let adapter else { return "" }
        let d = adapter.databaseType.sqlDialect
        let schemaPrefix = schema.map { d.quoteIdentifier($0) + "." } ?? ""
        var sql = "SELECT * FROM \(schemaPrefix)\(d.quoteIdentifier(tableName))"
        if let filter = activeFilter {
            sql += " WHERE " + filter.toSQL(dialect: d)
        }
        if let orderBy, !orderBy.isEmpty {
            sql += " ORDER BY " + orderBy.map { $0.toSQL(dialect: d) }.joined(separator: ", ")
        }
        sql += " LIMIT \(limit) OFFSET \(offset)"
        return sql
    }

    private func buildCountSQL() -> String {
        guard let adapter else { return "" }
        let d = adapter.databaseType.sqlDialect
        let schemaPrefix = schema.map { d.quoteIdentifier($0) + "." } ?? ""
        var sql = "SELECT COUNT(*) FROM \(schemaPrefix)\(d.quoteIdentifier(tableName))"
        if let filter = activeFilter {
            sql += " WHERE " + filter.toSQL(dialect: d)
        }
        return sql
    }

    // MARK: - Column Auto-sizing

    private func computeColumnWidths(columns: [ColumnHeader], rows: [[RowValue]]) {
        let font = NSFont.monospacedSystemFont(ofSize: 12, weight: .regular)
        let attrs: [NSAttributedString.Key: Any] = [.font: font]
        let headerFont = NSFont.systemFont(ofSize: 12, weight: .medium)
        let headerAttrs: [NSAttributedString.Key: Any] = [.font: headerFont]

        for (colIdx, col) in columns.enumerated() {
            // Already manually resized? Keep it.
            if columnWidths[col.name] != nil { continue }

            // Measure header
            let headerWidth = (col.name as NSString).size(withAttributes: headerAttrs).width

            // Measure sample of row data (first 50 rows for performance)
            var maxDataWidth: CGFloat = 0
            let sampleCount = min(rows.count, 50)
            for rowIdx in 0..<sampleCount {
                if colIdx < rows[rowIdx].count {
                    let raw: String
                    if rowIdx < displayCache.count, colIdx < displayCache[rowIdx].count {
                        raw = displayCache[rowIdx][colIdx]
                    } else {
                        raw = rows[rowIdx][colIdx].displayString
                    }
                    let text = raw.count > 100 ? String(raw.prefix(100)) : raw
                    let w = (text as NSString).size(withAttributes: attrs).width
                    maxDataWidth = max(maxDataWidth, w)
                }
            }

            let contentWidth = max(headerWidth, maxDataWidth)
            // Add padding (12px each side) and clamp
            let width = min(max(contentWidth + 24, 70), 500)
            columnWidths[col.name] = ceil(width)
        }
    }

    // MARK: - Inline Editing

    func commitCellEdit(rowIndex: Int, colIdx: Int, newText: String) {
        guard colIdx < columns.count, rowIndex < rows.count else { return }
        let col = columns[colIdx]
        let oldValue = rows[rowIndex][colIdx]
        let newValue = parseRowValue(from: newText, oldValue: oldValue)

        guard oldValue != newValue else {
            editingCell = nil
            return
        }

        let pk = buildPrimaryKey(forRow: rowIndex)
        changeTracker.trackEdit(row: rowIndex, column: col.name, oldValue: oldValue, newValue: newValue, primaryKey: pk)
        rows[rowIndex][colIdx] = newValue
        updateDisplayCache(row: rowIndex, col: colIdx)
        editingCell = nil
    }

    func commitDateEdit(rowIndex: Int, colIdx: Int, newValue: RowValue) {
        guard colIdx < columns.count, rowIndex < rows.count else { return }
        let col = columns[colIdx]
        let oldValue = rows[rowIndex][colIdx]
        guard oldValue != newValue else { return }

        if insertedRowIndices.contains(rowIndex) {
            rows[rowIndex][colIdx] = newValue
        } else {
            let pk = buildPrimaryKey(forRow: rowIndex)
            changeTracker.trackEdit(row: rowIndex, column: col.name, oldValue: oldValue, newValue: newValue, primaryKey: pk)
            rows[rowIndex][colIdx] = newValue
        }
        updateDisplayCache(row: rowIndex, col: colIdx)
        editingCell = nil
    }

    private func buildPrimaryKey(forRow rowIndex: Int) -> RowDictionary? {
        guard rowIndex < rows.count else { return nil }

        // Use primary key columns if available
        if !primaryKeyColumns.isEmpty {
            var pk: RowDictionary = [:]
            for pkCol in primaryKeyColumns {
                if let colIdx = columns.firstIndex(where: { $0.name == pkCol }) {
                    pk[pkCol] = rows[rowIndex][colIdx]
                }
            }
            if !pk.isEmpty { return pk }
        }

        // Fallback: use only safe-to-compare columns (integer, string, boolean, uuid)
        var safeCols: RowDictionary = [:]
        for (colIdx, col) in columns.enumerated() {
            guard colIdx < rows[rowIndex].count else { continue }
            let value = rows[rowIndex][colIdx]
            switch value {
            case .integer, .string, .boolean, .uuid:
                safeCols[col.name] = value
            case .null, .double, .date, .data, .json, .array:
                continue // Skip types that can cause comparison issues
            }
        }
        return safeCols.isEmpty ? nil : safeCols
    }

    private func parseRowValue(from text: String, oldValue: RowValue) -> RowValue {
        let trimmed = text.trimmingCharacters(in: .whitespaces)
        if trimmed.uppercased() == "NULL" { return .null }

        switch oldValue {
        case .integer:
            if let v = Int64(trimmed) { return .integer(v) }
        case .double:
            if let v = Double(trimmed) { return .double(v) }
        case .boolean:
            let lower = trimmed.lowercased()
            if lower == "true" || lower == "1" { return .boolean(true) }
            if lower == "false" || lower == "0" { return .boolean(false) }
        case .date:
            if let d = Self.parseDate(trimmed) { return .date(d) }
        case .uuid:
            if let u = UUID(uuidString: trimmed) { return .uuid(u) }
        default:
            break
        }
        return .string(trimmed)
    }

    private static let dateFormatters: [DateFormatter] = {
        let formats = [
            "yyyy-MM-dd HH:mm:ss",
            "yyyy-MM-dd",
            "yyyy-MM-dd'T'HH:mm:ss",
            "yyyy-MM-dd'T'HH:mm:ssZ",
            "yyyy-MM-dd HH:mm:ss.SSS",
            "dd/MM/yyyy",
            "MM/dd/yyyy"
        ]
        return formats.map { fmt in
            let f = DateFormatter()
            f.dateFormat = fmt
            f.timeZone = TimeZone(identifier: "UTC")
            f.locale = Locale(identifier: "en_US_POSIX")
            return f
        }
    }()

    private static func parseDate(_ text: String) -> Date? {
        for formatter in dateFormatters {
            if let date = formatter.date(from: text) { return date }
        }
        return nil
    }

    private func parseRowValue(from text: String, columnType: String) -> RowValue {
        let trimmed = text.trimmingCharacters(in: .whitespaces)
        if trimmed.uppercased() == "NULL" { return .null }

        let lower = columnType.lowercased()

        if lower.contains("int") || lower.contains("serial") {
            if let v = Int64(trimmed) { return .integer(v) }
        }
        if lower.contains("float") || lower.contains("double") || lower.contains("real") || lower.contains("numeric") || lower.contains("decimal") {
            if let v = Double(trimmed) { return .double(v) }
        }
        if lower.contains("bool") {
            let l = trimmed.lowercased()
            if l == "true" || l == "1" { return .boolean(true) }
            if l == "false" || l == "0" { return .boolean(false) }
        }
        if lower.contains("date") || lower.contains("time") {
            if let d = Self.parseDate(trimmed) { return .date(d) }
        }
        if lower.contains("uuid") {
            if let u = UUID(uuidString: trimmed) { return .uuid(u) }
        }
        if lower.contains("json") {
            return .json(trimmed)
        }
        return .string(trimmed)
    }

    var hasPendingChanges: Bool {
        changeTracker.hasChanges || !insertedRowIndices.isEmpty
    }

    func discardAllChanges() {
        // Remove inserted rows from end (reverse order to preserve indices)
        for rowIndex in insertedRowIndices.sorted().reversed() {
            if rowIndex < rows.count {
                rows.remove(at: rowIndex)
                if rowIndex < displayCache.count { displayCache.remove(at: rowIndex) }
                totalRows -= 1
            }
        }
        insertedRowIndices.removeAll()
        changeTracker.discardAll()
        rebuildDisplayCache()
        objectWillChange.send()
    }

    // MARK: - Add New Row

    func addNewRow() {
        let newRow: [RowValue] = columns.map { col in
            if let def = defaultValueFor(column: col) {
                return def
            }
            return .null
        }
        let rowIndex = rows.count
        rows.append(newRow)
        displayCache.append(newRow.map { $0.displayString })
        insertedRowIndices.insert(rowIndex)
        totalRows += 1
        objectWillChange.send()
    }

    private func defaultValueFor(column: ColumnHeader) -> RowValue? {
        // Auto-increment / serial columns get NULL (server-generated)
        nil
    }

    func commitNewRowEdit(rowIndex: Int, colIdx: Int, newText: String) {
        guard colIdx < columns.count, rowIndex < rows.count else { return }
        // Always use column dataType for inserted rows to ensure correct binding types
        let newValue = parseRowValue(from: newText, columnType: columns[colIdx].dataType)
        rows[rowIndex][colIdx] = newValue
        updateDisplayCache(row: rowIndex, col: colIdx)
        editingCell = nil
    }

    func buildInsertValues(forRow rowIndex: Int) -> RowDictionary {
        var values: RowDictionary = [:]
        for (colIdx, col) in columns.enumerated() {
            guard colIdx < rows[rowIndex].count else { continue }
            let value = rows[rowIndex][colIdx]
            if case .null = value { continue } // Skip NULL — let DB handle defaults
            values[col.name] = value
        }
        return values
    }

    // MARK: - Delete Rows

    func deleteSelectedRows() {
        guard !selectedRows.isEmpty else { return }
        for rowIndex in selectedRows.sorted().reversed() {
            guard rowIndex < rows.count else { continue }
            // Skip if already marked for deletion
            if changeTracker.pendingChanges.contains(where: { $0.row == rowIndex && $0.editType == .delete }) { continue }
            guard let pk = buildPrimaryKey(forRow: rowIndex) else { continue }
            changeTracker.trackDelete(row: rowIndex, primaryKey: pk)
        }
        objectWillChange.send()
    }

    func undoDeleteRow(_ rowIndex: Int) {
        let changes = changeTracker.pendingChanges
        if let idx = changes.firstIndex(where: { $0.row == rowIndex && $0.editType == .delete }) {
            changeTracker.discardChange(at: idx)
            objectWillChange.send()
        }
    }

    // MARK: - Commit Flow

    func prepareCommit() {
        guard let adapter else { return }

        // MongoDB: skip SQL generation and preview, commit directly
        if adapter.databaseType == .mongodb {
            commitError = nil
            Task { await executeCommit() }
            return
        }

        let dialect = adapter.databaseType.sqlDialect

        // Remove any previously tracked inserts (in case prepareCommit is called again after cancel)
        changeTracker.removeInserts()

        // Track inserts from new rows before generating SQL
        for rowIndex in insertedRowIndices.sorted() {
            let values = buildInsertValues(forRow: rowIndex)
            if !values.isEmpty {
                changeTracker.trackInsert(values: values)
            }
        }

        commitSQL = changeTracker.generateSQL(table: tableName, schema: schema, dialect: dialect)
        commitError = nil
        showCommitPreview = true
    }

    func executeCommit() async {
        guard let adapter else { return }
        isCommitting = true
        commitError = nil

        // MongoDB: use adapter CRUD methods directly (no SQL)
        if adapter.databaseType == .mongodb {
            await executeCommitMongo(adapter: adapter)
            isCommitting = false
            return
        }

        do {
            logQuery(sql: "BEGIN", duration: nil)
            try await adapter.beginTransaction()
            for statement in commitSQL {
                let start = Date()
                _ = try await adapter.executeWithRowValues(sql: statement.sql, parameters: statement.parameters)
                let duration = Date().timeIntervalSince(start)
                let paramDesc = statement.parameters.isEmpty ? "" : " -- params: \(statement.parameters.map(\.description).joined(separator: ", "))"
                logQuery(sql: statement.sql + paramDesc, duration: duration)
            }
            try await adapter.commitTransaction()
            logQuery(sql: "COMMIT", duration: nil)

            changeTracker.discardAll()
            insertedRowIndices.removeAll()
            showCommitPreview = false
            // Reload to get server-generated values (serials, defaults, triggers)
            await loadPage(currentPage)
        } catch {
            try? await adapter.rollbackTransaction()
            logQuery(sql: "ROLLBACK", duration: nil)
            commitError = Self.detailedErrorMessage(error)
        }

        isCommitting = false
    }

    /// MongoDB-specific commit: applies pending changes via insertRow/updateRow/deleteRow
    /// instead of generating SQL.
    private func executeCommitMongo(adapter: any DatabaseAdapter) async {
        do {
            // Inserts (new rows)
            for rowIndex in insertedRowIndices.sorted() {
                let values = buildInsertValues(forRow: rowIndex)
                guard !values.isEmpty else { continue }
                _ = try await adapter.insertRow(table: tableName, schema: nil, values: values)
            }

            // Updates and deletes from change tracker
            // Group edits by row to issue one updateOne per modified document
            var updatesByRow: [Int: [String: RowValue]] = [:]
            var deletes: Set<Int> = []
            for change in changeTracker.pendingChanges {
                switch change.editType {
                case .update:
                    if let col = change.column {
                        var rowSet = updatesByRow[change.row] ?? [:]
                        rowSet[col] = change.newValue
                        updatesByRow[change.row] = rowSet
                    }
                case .delete:
                    deletes.insert(change.row)
                case .insert:
                    break  // handled above via insertedRowIndices
                }
            }

            for (rowIndex, setValues) in updatesByRow {
                guard let pk = buildPrimaryKey(forRow: rowIndex) else { continue }
                _ = try await adapter.updateRow(table: tableName, schema: nil, set: setValues, where: pk)
            }

            for rowIndex in deletes {
                guard let pk = buildPrimaryKey(forRow: rowIndex) else { continue }
                _ = try await adapter.deleteRow(table: tableName, schema: nil, where: pk)
            }

            changeTracker.discardAll()
            insertedRowIndices.removeAll()
            showCommitPreview = false
            await loadPage(currentPage)
        } catch {
            commitError = Self.detailedErrorMessage(error)
        }
    }
}

// MARK: - Header

struct DataGridHeader: View {
    let columns: [ColumnHeader]
    @Binding var columnWidths: [String: CGFloat]
    @Binding var sortColumn: String?
    @Binding var sortAscending: Bool

    var body: some View {
        HStack(spacing: 0) {
            ForEach(Array(columns.enumerated()), id: \.1.name) { idx, col in
                let colWidth = columnWidths[col.name] ?? 120

                ZStack(alignment: .trailing) {
                    // Column label — entire cell is clickable for sort
                    HStack(spacing: 4) {
                        Text(col.name)
                            .font(.system(size: 12, weight: .medium, design: .monospaced))
                            .lineLimit(1)
                            .foregroundStyle(.secondary)
                        if sortColumn == col.name {
                            Image(systemName: sortAscending ? "chevron.up" : "chevron.down")
                                .font(.system(size: 8, weight: .semibold))
                                .foregroundStyle(.secondary)
                        }
                        Spacer(minLength: 0)
                    }
                    .padding(.horizontal, 8)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .contentShape(Rectangle())
                    .onTapGesture {
                        if sortColumn == col.name {
                            sortAscending.toggle()
                        } else {
                            sortColumn = col.name
                            sortAscending = true
                        }
                    }

                    // Resize handle + separator on right edge
                    Rectangle()
                        .fill(Color(nsColor: .separatorColor).opacity(0.5))
                        .frame(width: 1)
                        .padding(.vertical, 4)
                        .overlay {
                            Color.clear
                                .frame(width: 8)
                                .contentShape(Rectangle())
                                .onHover { hovering in
                                    if hovering { NSCursor.resizeLeftRight.push() }
                                    else { NSCursor.pop() }
                                }
                                .gesture(
                                    DragGesture(minimumDistance: 1)
                                        .onChanged { value in
                                            let newWidth = max(50, colWidth + value.translation.width)
                                            columnWidths[col.name] = round(newWidth)
                                        }
                                )
                        }
                }
                .frame(width: colWidth)
            }

            Spacer(minLength: 0)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .frame(height: 28)
        .background(Color(nsColor: .windowBackgroundColor))
        .overlay(alignment: .bottom) {
            Rectangle().fill(Color(nsColor: .separatorColor)).frame(height: 1)
        }
    }
}

// MARK: - Row

struct DataGridRow: View {
    let rowIndex: Int
    let row: [RowValue]
    let columns: [ColumnHeader]
    let columnWidths: [String: CGFloat]
    let pageOffset: Int
    let isDeleted: Bool
    let modifiedColumns: Set<String>
    let primaryKeyColumns: [String]
    let isSelected: Bool
    @Binding var editingCell: DataGridViewState.CellID?
    let onSelectRow: (_ extend: Bool) -> Void
    let onCellEdit: (Int, String) -> Void
    @State private var lastClickedCol: Int = -1
    @State private var lastClickTime: Date = .distantPast

    private static let cellFont = NSFont.monospacedSystemFont(ofSize: 12, weight: .regular)
    private static let separatorColor = Color(nsColor: .separatorColor)
    private static let altRowColor = Color(nsColor: .alternatingContentBackgroundColors.last ?? .controlBackgroundColor)
    private static let bgColor = Color(nsColor: .textBackgroundColor)

    var body: some View {
        let editingRow = editingCell?.row == rowIndex
        HStack(spacing: 0) {
            ForEach(0..<columns.count, id: \.self) { colIdx in
                let col = columns[colIdx]
                let value = colIdx < row.count ? row[colIdx] : .null
                let colWidth = columnWidths[col.name] ?? 120

                if editingRow && editingCell?.col == colIdx {
                    CellTextField(
                        initialValue: value.isNull ? "" : value.description,
                        onCommit: { newText in onCellEdit(colIdx, newText) },
                        onCancel: { editingCell = nil }
                    )
                    .frame(width: colWidth, height: 26)
                } else {
                    cellText(value: value, isModified: modifiedColumns.contains(col.name))
                        .frame(width: colWidth, height: 26)
                        .overlay(alignment: .trailing) {
                            Self.separatorColor.opacity(0.3).frame(width: 1)
                        }
                }
            }

            Spacer(minLength: 0)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(rowBackground)
        .contentShape(Rectangle())
        .onTapGesture(count: 1) {
            handleClick(colIdx: nil, shiftKey: NSEvent.modifierFlags.contains(.shift))
        }
        .overlay(alignment: .bottom) {
            Self.separatorColor.opacity(0.15).frame(height: 1)
        }
        .contextMenu {
            Button("Copy Row") {
                let text = row.map { $0.isNull ? "NULL" : $0.description }.joined(separator: "\t")
                NSPasteboard.general.clearContents()
                NSPasteboard.general.setString(text, forType: .string)
            }
        }
    }

    private func cellText(value: RowValue, isModified: Bool) -> some View {
        Text(value.isNull ? "NULL" : value.displayString)
            .font(.system(size: 12, design: .monospaced))
            .foregroundStyle(value.isNull ? Color.Gridex.cellNull : .primary)
            .italic(value.isNull)
            .lineLimit(1)
            .padding(.horizontal, 8)
            .frame(maxWidth: .infinity, alignment: value.isNumeric ? .trailing : .leading)
            .background(isModified ? Color.Gridex.cellModified : .clear)
    }

    private func handleClick(colIdx: Int?, shiftKey: Bool) {
        let now = Date()
        if let col = colIdx, lastClickedCol == col && now.timeIntervalSince(lastClickTime) < 0.3 {
            editingCell = DataGridViewState.CellID(row: rowIndex, col: col)
            lastClickTime = .distantPast
        } else {
            onSelectRow(shiftKey)
            lastClickedCol = colIdx ?? -1
            lastClickTime = now
        }
    }

    private var rowBackground: Color {
        if isDeleted { return Color.Gridex.cellDeleted }
        if isSelected { return Color.accentColor.opacity(0.18) }
        return rowIndex.isMultiple(of: 2) ? Self.bgColor : Self.altRowColor
    }
}

// MARK: - Cell TextField (for inline editing)

struct CellTextField: NSViewRepresentable {
    let initialValue: String
    let onCommit: (String) -> Void
    let onCancel: () -> Void

    func makeNSView(context: Context) -> NSTextField {
        let field = NSTextField(string: initialValue)
        field.font = NSFont.monospacedSystemFont(ofSize: 12, weight: .regular)
        field.isBordered = true
        field.bezelStyle = .squareBezel
        field.focusRingType = .none
        field.delegate = context.coordinator
        DispatchQueue.main.async { field.selectText(nil) }
        return field
    }

    func updateNSView(_ nsView: NSTextField, context: Context) {}

    func makeCoordinator() -> Coordinator {
        Coordinator(onCommit: onCommit, onCancel: onCancel)
    }

    class Coordinator: NSObject, NSTextFieldDelegate {
        let onCommit: (String) -> Void
        let onCancel: () -> Void

        init(onCommit: @escaping (String) -> Void, onCancel: @escaping () -> Void) {
            self.onCommit = onCommit
            self.onCancel = onCancel
        }

        func control(_ control: NSControl, textView: NSTextView, doCommandBy commandSelector: Selector) -> Bool {
            if commandSelector == #selector(NSResponder.insertNewline(_:)) {
                onCommit(control.stringValue)
                return true
            } else if commandSelector == #selector(NSResponder.insertTab(_:)) {
                onCommit(control.stringValue)
                return true
            } else if commandSelector == #selector(NSResponder.cancelOperation(_:)) {
                onCancel()
                return true
            }
            return false
        }

        func controlTextDidEndEditing(_ obj: Notification) {
            guard let field = obj.object as? NSTextField else { return }
            onCommit(field.stringValue)
        }
    }
}

// MARK: - Toolbar

struct DataGridToolbar: View {
    @ObservedObject var viewModel: DataGridViewState
    @EnvironmentObject private var appState: AppState
    @State private var showInsertDocSheet = false

    private var isMongoDB: Bool {
        appState.activeAdapter?.databaseType == .mongodb
    }

    var body: some View {
        HStack(spacing: 12) {
            // MongoDB: Insert document button
            if isMongoDB {
                Button {
                    showInsertDocSheet = true
                } label: {
                    HStack(spacing: 4) {
                        Image(systemName: "plus.circle")
                            .font(.system(size: 11))
                        Text("Insert Document")
                            .font(.system(size: 11, weight: .medium))
                    }
                }
                .buttonStyle(.plain)
                .foregroundStyle(.green)
                .help("Insert a JSON document into this collection")
                .sheet(isPresented: $showInsertDocSheet) {
                    insertDocumentSheet
                }
            }

            // Pending changes indicator
            if viewModel.hasPendingChanges {
                HStack(spacing: 6) {
                    Circle()
                        .fill(.orange)
                        .frame(width: 6, height: 6)
                    Text("\(viewModel.changeTracker.pendingChanges.count + viewModel.insertedRowIndices.count) pending")
                        .font(.system(size: 11))
                        .foregroundStyle(.orange)
                }

                Button {
                    viewModel.discardAllChanges()
                } label: {
                    HStack(spacing: 3) {
                        Image(systemName: "arrow.uturn.backward")
                            .font(.system(size: 10))
                        Text("Discard")
                            .font(.system(size: 11))
                    }
                }
                .buttonStyle(.plain)
                .foregroundStyle(.secondary)

                Button {
                    viewModel.prepareCommit()
                } label: {
                    HStack(spacing: 3) {
                        Image(systemName: "checkmark.circle.fill")
                            .font(.system(size: 10))
                        Text("Commit")
                            .font(.system(size: 11, weight: .medium))
                    }
                }
                .buttonStyle(.plain)
                .foregroundStyle(Color.accentColor)
            }

            Spacer()

            // Reload button
            Button {
                NotificationCenter.default.post(name: .reloadData, object: nil)
            } label: {
                Image(systemName: "arrow.clockwise")
                    .font(.system(size: 11))
            }
            .buttonStyle(.plain)
            .foregroundStyle(.secondary)
            .help("Reload (⌘R)")
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 5)
        .frame(height: 28)
        .background(Color(nsColor: .windowBackgroundColor))
        .overlay(alignment: .bottom) { Divider() }
    }

    @ViewBuilder
    private var insertDocumentSheet: some View {
        let fields: [(name: String, type: String)] = viewModel.columns.map { col in
            (name: col.name, type: col.dataType)
        }
        MongoDocumentEditorSheet(
            collectionName: viewModel.tableName,
            detectedFields: fields,
            onInsert: { jsonText in
                guard let mongo = appState.activeAdapter as? MongoDBAdapter else {
                    return .failure(GridexError.queryExecutionFailed("Not connected to MongoDB"))
                }
                do {
                    try await mongo.insertJSONDocument(into: viewModel.tableName, json: jsonText)
                    await viewModel.loadPage(viewModel.currentPage)
                    return .success(())
                } catch {
                    return .failure(error)
                }
            }
        )
    }
}

// MARK: - Bottom Tab Bar (Data | Structure + Pagination)

struct BottomTabBar: View {
    @Binding var viewMode: DataGridViewMode
    @ObservedObject var viewModel: DataGridViewState
    var onAddColumn: (() -> Void)?
    @Binding var showAddIndex: Bool
    @EnvironmentObject private var appState: AppState

    var body: some View {
        HStack(spacing: 0) {
            // Left: Data / Structure tabs (Structure hidden for Redis)
            HStack(spacing: 0) {
                tabButton("Data", mode: .data)
                if appState.activeConfig?.databaseType != .redis {
                    tabButton("Structure", mode: .structure)
                }
            }
            .background(Color(nsColor: .controlBackgroundColor))
            .clipShape(RoundedRectangle(cornerRadius: 5))
            .overlay(RoundedRectangle(cornerRadius: 5).stroke(Color(nsColor: .separatorColor), lineWidth: 0.5))

            Spacer().frame(width: 8)

            if viewMode == .data {
                // + Row / + Key button
                if appState.activeConfig?.databaseType == .redis {
                    Button {
                        appState.showRedisAddKey = true
                    } label: {
                        HStack(spacing: 3) {
                            Image(systemName: "plus")
                                .font(.system(size: 10, weight: .medium))
                            Text("Key")
                                .font(.system(size: 12))
                        }
                        .padding(.horizontal, 10)
                        .padding(.vertical, 4)
                        .background(Color(nsColor: .controlBackgroundColor))
                        .clipShape(RoundedRectangle(cornerRadius: 5))
                        .overlay(RoundedRectangle(cornerRadius: 5).stroke(Color(nsColor: .separatorColor), lineWidth: 0.5))
                    }
                    .buttonStyle(.plain)
                    .pointerCursor()
                    .help("Add new Redis key")
                } else {
                    Button {
                        viewModel.addNewRow()
                    } label: {
                        HStack(spacing: 3) {
                            Image(systemName: "plus")
                                .font(.system(size: 10, weight: .medium))
                            Text("Row")
                                .font(.system(size: 12))
                        }
                        .padding(.horizontal, 10)
                        .padding(.vertical, 4)
                        .background(Color(nsColor: .controlBackgroundColor))
                        .clipShape(RoundedRectangle(cornerRadius: 5))
                        .overlay(RoundedRectangle(cornerRadius: 5).stroke(Color(nsColor: .separatorColor), lineWidth: 0.5))
                    }
                    .buttonStyle(.plain)
                    .pointerCursor()
                    .help("Add new row")
                }
            } else if viewMode == .structure {
                // + Column / + Index buttons
                Button { onAddColumn?() } label: {
                    HStack(spacing: 3) {
                        Image(systemName: "plus").font(.system(size: 10, weight: .medium))
                        Text("Column").font(.system(size: 12))
                    }
                    .padding(.horizontal, 10)
                    .padding(.vertical, 4)
                    .background(Color(nsColor: .controlBackgroundColor))
                    .clipShape(RoundedRectangle(cornerRadius: 5))
                    .overlay(RoundedRectangle(cornerRadius: 5).stroke(Color(nsColor: .separatorColor), lineWidth: 0.5))
                }
                .buttonStyle(.plain)
                .pointerCursor()
                .help("Add column")

                Spacer().frame(width: 6)

                Button { showAddIndex = true } label: {
                    HStack(spacing: 3) {
                        Image(systemName: "plus").font(.system(size: 10, weight: .medium))
                        Text("Index").font(.system(size: 12))
                    }
                    .padding(.horizontal, 10)
                    .padding(.vertical, 4)
                    .background(Color(nsColor: .controlBackgroundColor))
                    .clipShape(RoundedRectangle(cornerRadius: 5))
                    .overlay(RoundedRectangle(cornerRadius: 5).stroke(Color(nsColor: .separatorColor), lineWidth: 0.5))
                }
                .buttonStyle(.plain)
                .pointerCursor()
                .help("Add index")
            }

            Spacer().frame(width: 16)

            // Row count
            if viewMode == .data {
                Text(pageText)
                    .font(.system(size: 12))
                    .foregroundStyle(.secondary)
            }

            Spacer()

            if viewMode == .data {
                HStack(spacing: 8) {
                    // Pending changes
                    if viewModel.hasPendingChanges {
                        Text("\(viewModel.changeTracker.pendingChanges.count + viewModel.insertedRowIndices.count) pending")
                            .font(.system(size: 12))
                            .foregroundStyle(.orange)

                        Button("Discard") {
                            viewModel.discardAllChanges()
                        }
                        .controlSize(.small)

                        Button("Commit") {
                            viewModel.prepareCommit()
                        }
                        .controlSize(.small)
                        .buttonStyle(.borderedProminent)
                    }

                    // Filters toggle
                    Button {
                        viewModel.showFilterBar.toggle()
                    } label: {
                        HStack(spacing: 3) {
                            Image(systemName: "line.3.horizontal.decrease")
                                .font(.system(size: 10, weight: .medium))
                            Text("Filters")
                                .font(.system(size: 12))
                        }
                        .padding(.horizontal, 10)
                        .padding(.vertical, 4)
                        .background(viewModel.showFilterBar
                            ? Color.accentColor.opacity(0.2)
                            : Color(nsColor: .controlBackgroundColor))
                        .clipShape(RoundedRectangle(cornerRadius: 5))
                        .overlay(
                            RoundedRectangle(cornerRadius: 5)
                                .stroke(viewModel.showFilterBar
                                    ? Color.accentColor
                                    : Color(nsColor: .separatorColor), lineWidth: 0.5)
                        )
                    }
                    .buttonStyle(.plain)
                    .pointerCursor()
                    .foregroundStyle(viewModel.showFilterBar ? Color.accentColor : .secondary)
                    .help("Toggle filters (⌘F)")

                    // Pagination
                    Button {
                        Task { await viewModel.previousPage() }
                    } label: {
                        Image(systemName: "chevron.left")
                            .font(.system(size: 11))
                    }
                    .buttonStyle(.plain)
                    .pointerCursor()
                    .foregroundStyle(.secondary)
                    .disabled(viewModel.currentPage == 0)

                    Button {
                        Task { await viewModel.nextPage() }
                    } label: {
                        Image(systemName: "chevron.right")
                            .font(.system(size: 11))
                    }
                    .buttonStyle(.plain)
                    .pointerCursor()
                    .foregroundStyle(.secondary)
                    .disabled(viewModel.rows.count < viewModel.pageSize)
                }
            }

            // SQL Log toggle
            Spacer().frame(width: 8)
            Button {
                withAnimation(.easeInOut(duration: 0.15)) {
                    appState.showQueryLog.toggle()
                }
            } label: {
                Image(systemName: "rectangle.bottomhalf.filled")
                    .font(.system(size: 13))
                    .padding(4)
                    .background(
                        appState.showQueryLog
                            ? Color.accentColor.opacity(0.2)
                            : Color.clear
                    )
                    .clipShape(RoundedRectangle(cornerRadius: 4))
                    .overlay(
                        RoundedRectangle(cornerRadius: 4)
                            .stroke(appState.showQueryLog ? Color.accentColor : Color.clear, lineWidth: 0.5)
                    )
            }
            .buttonStyle(.plain)
            .pointerCursor()
            .foregroundStyle(appState.showQueryLog ? Color.accentColor : .secondary)
            .help("Toggle SQL Log")
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 5)
        .frame(height: 34)
        .background(Color(nsColor: .windowBackgroundColor))
        .overlay(alignment: .top) { Divider() }
    }

    private func tabButton(_ title: String, mode: DataGridViewMode) -> some View {
        Button(action: { viewMode = mode }) {
            Text(title)
                .font(.system(size: 12, weight: viewMode == mode ? .medium : .regular))
                .foregroundStyle(viewMode == mode ? .primary : .secondary)
                .padding(.horizontal, 12)
                .padding(.vertical, 4)
                .background(viewMode == mode ? Color(nsColor: .selectedContentBackgroundColor).opacity(0.3) : .clear)
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .pointerCursor()
    }

    private var pageText: String {
        guard !viewModel.rows.isEmpty else { return "No rows" }
        let start = viewModel.currentPage * viewModel.pageSize + 1
        let end = min(start + viewModel.rows.count - 1, viewModel.totalRows)
        return "\(start)–\(end) of \(viewModel.totalRows) rows"
    }
}

// MARK: - ComboBox (NSComboBox wrapper — editable text + dropdown)

private struct ComboBoxView: NSViewRepresentable {
    @Binding var text: String
    var items: [String]
    var onChange: ((String) -> Void)?

    func makeNSView(context: Context) -> NSComboBox {
        let combo = NSComboBox()
        combo.isEditable = true
        combo.completes = true
        combo.hasVerticalScroller = true
        combo.numberOfVisibleItems = 12
        combo.usesDataSource = false
        combo.font = .systemFont(ofSize: 12)
        combo.isBordered = true
        combo.isBezeled = true
        combo.bezelStyle = .roundedBezel
        combo.delegate = context.coordinator
        combo.target = context.coordinator
        combo.action = #selector(Coordinator.comboBoxAction(_:))
        return combo
    }

    func updateNSView(_ combo: NSComboBox, context: Context) {
        combo.removeAllItems()
        combo.addItems(withObjectValues: items)
        if combo.stringValue != text {
            combo.stringValue = text
        }
        context.coordinator.parent = self
    }

    func makeCoordinator() -> Coordinator { Coordinator(parent: self) }

    class Coordinator: NSObject, NSComboBoxDelegate {
        var parent: ComboBoxView
        init(parent: ComboBoxView) { self.parent = parent }

        @objc func comboBoxAction(_ sender: NSComboBox) {
            let newVal = sender.stringValue
            if parent.text != newVal {
                parent.text = newVal
                parent.onChange?(newVal)
            }
        }

        func comboBoxSelectionDidChange(_ notification: Notification) {
            guard let combo = notification.object as? NSComboBox,
                  combo.indexOfSelectedItem >= 0,
                  let val = combo.objectValueOfSelectedItem as? String else { return }
            DispatchQueue.main.async {
                if self.parent.text != val {
                    self.parent.text = val
                    self.parent.onChange?(val)
                }
            }
        }

        func controlTextDidEndEditing(_ obj: Notification) {
            guard let combo = obj.object as? NSComboBox else { return }
            let newVal = combo.stringValue
            if parent.text != newVal {
                parent.text = newVal
                parent.onChange?(newVal)
            }
        }
    }
}

// MARK: - Inline Structure View

// MARK: - Common Data Types (canonical names from format_type())

let postgresDataTypes = [
    "bool", "bytea", "char", "varchar",
    "int2", "int4", "int8", "serial", "bigserial", "smallserial",
    "float4", "float8", "numeric", "date",
    "time", "timetz", "timestamp", "timestamptz",
    "interval", "text", "json", "jsonb", "uuid", "xml"
]

let mysqlDataTypes = [
    "tinyint", "smallint", "int", "bigint", "float", "double",
    "decimal", "char", "varchar", "text", "mediumtext", "longtext",
    "date", "datetime", "timestamp", "time", "year",
    "blob", "json", "enum", "set", "bool"
]

let sqliteDataTypes = [
    "INTEGER", "REAL", "TEXT", "BLOB", "NUMERIC"
]

struct InlineStructureView: View {
    let tableDescription: TableDescription?
    let isLoading: Bool
    let tableName: String
    let schema: String?
    let adapter: (any DatabaseAdapter)?
    var onStructureChanged: (() async -> TableDescription?)?
    var onSelectColumn: ((_ details: [(column: String, value: String)]?) -> Void)?
    @Binding var showAddIndex: Bool

    enum ColumnField: Hashable {
        case name(UUID)
        case dataType(UUID)
        case comment(UUID)
    }

    @State private var editableColumns: [EditableColumn] = []
    @State private var columnIndexMap: [UUID: Int] = [:]
    @State private var selectedColumnId: EditableColumn.ID?
    @FocusState private var focusedColumnField: ColumnField?
    @State private var pendingChanges: [StructureChange] = []
    @State private var isApplying = false
    @State private var tableGeneration: Int = 0  // Force table recreation after apply
    @State private var cachedDataTypes: [String] = []
    @State private var errorMessage: String?
    @State private var showDeleteConfirm = false
    @State private var columnToDelete: EditableColumn?
    @State private var searchText: String = ""
    @State private var editableIndexes: [EditableIndex] = []
    @State private var selectedIndexId: EditableIndex.ID?
    @State private var fkEditColumnId: UUID? = nil
    @State private var fkRefTable: String = ""
    @State private var fkRefColumn: String = ""
    @State private var fkOnUpdate: ForeignKeyAction = .noAction
    @State private var fkOnDelete: ForeignKeyAction = .noAction
    @State private var availableTables: [String] = []
    @State private var newIndexName: String = ""
    @State private var newIndexColumns: String = ""
    @State private var newIndexAlgorithm: String = "BTREE"
    @State private var newIndexIsUnique: Bool = false
    @State private var newIndexCondition: String = ""

    // Default value editor
    @State private var defaultEditColumnId: UUID? = nil
    @State private var defaultTab: DefaultValueTab = .string
    @State private var defaultStringValue: String = ""
    @State private var defaultExpressionValue: String = ""
    @State private var defaultSequenceName: String = ""
    @State private var defaultCreateSequence: Bool = false

    enum DefaultValueTab: String, CaseIterable {
        case string = "String"
        case expression = "Expression"
        case sequence = "Sequence"
    }

    struct EditableColumn: Identifiable {
        let id = UUID()
        var originalName: String?  // nil = new column
        var name: String
        var dataType: String
        var isNullable: Bool
        var defaultValue: String
        var comment: String
        var isPrimaryKey: Bool
        var foreignKeyDisplay: String  // read-only display
        var checkConstraint: String    // read-only display
        var isMarkedForDeletion: Bool = false
        // FK details
        var fkConstraintName: String = ""
        var fkReferencedTable: String = ""
        var fkReferencedColumn: String = ""
        var fkOnUpdate: ForeignKeyAction = .noAction
        var fkOnDelete: ForeignKeyAction = .noAction
        var hasForeignKey: Bool { !fkReferencedTable.isEmpty }
        var isNew: Bool { originalName == nil }
    }

    struct EditableIndex: Identifiable {
        let id = UUID()
        var originalName: String?  // nil = new index
        var name: String
        var algorithm: String
        var isUnique: Bool
        var columns: String       // comma-separated
        var condition: String
        var include: String
        var comment: String
        var isMarkedForDeletion: Bool = false
        var isNew: Bool { originalName == nil }
    }

    enum StructureChange {
        case addColumn(name: String, dataType: String, nullable: Bool, defaultValue: String?)
        case renameColumn(oldName: String, newName: String)
        case changeType(column: String, newType: String)
        case setNullable(column: String, nullable: Bool)
        case setDefault(column: String, value: String?)
        case setComment(column: String, comment: String?)
        case dropColumn(name: String)
        case addForeignKey(column: String, refTable: String, refColumn: String, onUpdate: ForeignKeyAction, onDelete: ForeignKeyAction)
        case dropForeignKey(constraintName: String)
        case addIndex(name: String, columns: [String], algorithm: String, isUnique: Bool, condition: String?)
        case dropIndex(name: String)
        case modifyIndex(oldName: String, newName: String, columns: [String], algorithm: String, isUnique: Bool, condition: String?)
        case setIndexComment(name: String, comment: String?)
        case createSequence(name: String)  // raw SQL for CREATE SEQUENCE
    }

    private var primaryKeyName: String {
        editableColumns.first(where: { $0.isPrimaryKey })?.name ?? ""
    }

    private var filteredColumns: [EditableColumn] {
        guard !searchText.isEmpty else { return editableColumns }
        return editableColumns.filter { $0.name.localizedCaseInsensitiveContains(searchText) }
    }

    /// Full-bleed row background — extends beyond cell bounds with negative padding
    /// NSViewRepresentable that walks up to find NSTableRowView and sets its background color
    struct RowBackgroundSetter: NSViewRepresentable {
        let color: NSColor?

        final class RowBGView: NSView {
            var targetColor: NSColor?

            override func layout() {
                super.layout()
                applyColor()
            }

            override func viewDidMoveToSuperview() {
                super.viewDidMoveToSuperview()
                DispatchQueue.main.async { [weak self] in
                    self?.applyColor()
                }
            }

            func applyColor() {
                var current: NSView? = superview
                while let view = current {
                    if let rowView = view as? NSTableRowView {
                        if let targetColor {
                            rowView.backgroundColor = targetColor
                        } else {
                            let row = rowIndex(of: rowView)
                            let colors = NSColor.alternatingContentBackgroundColors
                            rowView.backgroundColor = colors[row % colors.count]
                        }
                        return
                    }
                    current = view.superview
                }
            }

            private func rowIndex(of rowView: NSTableRowView) -> Int {
                // Walk up to find NSTableView and get the row index
                var current: NSView? = rowView.superview
                while let view = current {
                    if let tableView = view as? NSTableView {
                        return tableView.row(for: rowView)
                    }
                    current = view.superview
                }
                return 0
            }
        }

        func makeNSView(context: Context) -> RowBGView {
            let view = RowBGView()
            view.targetColor = color
            return view
        }

        func updateNSView(_ nsView: RowBGView, context: Context) {
            nsView.targetColor = color
            nsView.applyColor()
            DispatchQueue.main.async {
                nsView.applyColor()
            }
        }
    }

    enum RowChangeState {
        case none, new, deleted, edited
        var color: Color {
            switch self {
            case .none: return .primary
            case .new: return .green
            case .deleted: return .red
            case .edited: return .orange
            }
        }
        var nsColor: NSColor? {
            switch self {
            case .none: return nil
            case .new: return NSColor.systemGreen.withAlphaComponent(0.25)
            case .deleted: return NSColor.systemRed.withAlphaComponent(0.3)
            case .edited: return NSColor.systemOrange.withAlphaComponent(0.25)
            }
        }
    }

    private func columnChangeState(_ col: EditableColumn) -> RowChangeState {
        if col.isMarkedForDeletion { return .deleted }
        if col.isNew { return .new }
        guard let origName = col.originalName else { return .none }
        let hasChange = pendingChanges.contains { change in
            switch change {
            case .renameColumn(let old, _): return old == origName
            case .changeType(let c, _): return c == origName
            case .setNullable(let c, _): return c == origName
            case .setDefault(let c, _): return c == origName
            case .setComment(let c, _): return c == origName
            case .addForeignKey(let c, _, _, _, _): return c == origName
            case .dropForeignKey(let n): return col.fkConstraintName == n
            default: return false
            }
        }
        return hasChange ? .edited : .none
    }

    private func indexChangeState(_ idx: EditableIndex) -> RowChangeState {
        if idx.isMarkedForDeletion { return .deleted }
        if idx.isNew { return .new }
        guard let origName = idx.originalName else { return .none }
        let hasChange = pendingChanges.contains { change in
            switch change {
            case .modifyIndex(let old, _, _, _, _, _): return old == origName
            case .setIndexComment(let n, _): return n == origName
            default: return false
            }
        }
        return hasChange ? .edited : .none
    }

    private var isMongoDB: Bool {
        adapter?.databaseType == .mongodb
    }

    var body: some View {
        if isLoading {
            ProgressView("Loading structure...")
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        } else if isMongoDB, let desc = tableDescription {
            mongoSchemalessNotice(desc: desc)
        } else if let desc = tableDescription {
            VStack(spacing: 0) {
                // Top info bar (Name, Primary, Search)
                structureInfoBar

                // Toolbar for pending changes + Column
                structureToolbar

                VSplitView {
                    columnsSection(desc)
                    indexesSection(desc)
                }
            }
            .onAppear {
                syncFromDescription(desc)
            }
            .alert("Drop Column", isPresented: $showDeleteConfirm) {
                Button("Cancel", role: .cancel) {}
                Button("Drop", role: .destructive) {
                    if let col = columnToDelete {
                        dropColumn(col)
                    }
                }
            } message: {
                Text("Drop column \"\(columnToDelete?.name ?? "")\"? This cannot be undone.")
            }
            .alert("Error", isPresented: .constant(errorMessage != nil)) {
                Button("OK") { errorMessage = nil }
            } message: {
                Text(errorMessage ?? "")
            }
            .onReceive(NotificationCenter.default.publisher(for: .init("structureAddColumn"))) { _ in
                addNewColumn()
            }
            .sheet(isPresented: $showAddIndex) {
                addIndexPopover
            }
        } else {
            Text("Failed to load table structure")
                .foregroundStyle(.secondary)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
    }

    // MARK: - MongoDB Schemaless Notice

    @ViewBuilder
    private func mongoSchemalessNotice(desc: TableDescription) -> some View {
        VStack(spacing: 0) {
            // Compact header with collection name + count
            HStack(spacing: 12) {
                Image(systemName: "doc.text")
                    .foregroundStyle(.secondary)
                Text(desc.name)
                    .font(.system(size: 13, weight: .semibold))
                if let count = desc.estimatedRowCount {
                    Text("\(count) documents")
                        .font(.system(size: 11))
                        .foregroundStyle(.secondary)
                }
                Spacer()
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 8)
            Divider()

            VStack(spacing: 12) {
                Spacer()
                Image(systemName: "doc.text.below.ecg")
                    .font(.system(size: 40))
                    .foregroundStyle(.secondary)
                Text("Schemaless Collection")
                    .font(.system(size: 14, weight: .semibold))
                Text("MongoDB collections don't have a fixed schema.\nEach document can have its own structure.")
                    .font(.system(size: 12))
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
                if !desc.columns.isEmpty {
                    Divider().padding(.vertical, 8).frame(width: 200)
                    Text("Detected fields (from sampled documents):")
                        .font(.system(size: 11))
                        .foregroundStyle(.secondary)
                    VStack(alignment: .leading, spacing: 4) {
                        ForEach(desc.columns, id: \.name) { col in
                            HStack(spacing: 6) {
                                if col.isPrimaryKey {
                                    Image(systemName: "key.fill")
                                        .font(.system(size: 9))
                                        .foregroundStyle(.yellow)
                                }
                                Text(col.name)
                                    .font(.system(size: 12, weight: .medium, design: .monospaced))
                                Text(col.dataType)
                                    .font(.system(size: 11))
                                    .foregroundStyle(.secondary)
                            }
                        }
                    }
                    .padding(.horizontal, 16)
                    .padding(.vertical, 8)
                    .background(Color.primary.opacity(0.05))
                    .cornerRadius(6)
                }
                Spacer()
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
    }

    // MARK: - Structure Toolbar

    // MARK: - Info Bar (Name, Primary, Search)

    private var structureInfoBar: some View {
        HStack(spacing: 16) {
            HStack(spacing: 6) {
                Text("Name").foregroundStyle(.secondary).font(.system(size: 11))
                TextField("", text: .constant(tableName))
                    .textFieldStyle(.roundedBorder)
                    .font(.system(size: 12))
                    .frame(width: 160)
                    .disabled(true)
            }

            HStack(spacing: 6) {
                Text("Primary").foregroundStyle(.secondary).font(.system(size: 11))
                Text(primaryKeyName)
                    .font(.system(size: 12, weight: .medium))
                    .padding(.horizontal, 8)
                    .padding(.vertical, 2)
                    .background(Color.accentColor.opacity(0.2))
                    .cornerRadius(4)
            }

            Spacer()

            HStack(spacing: 4) {
                Image(systemName: "magnifyingglass").foregroundStyle(.tertiary).font(.system(size: 11))
                TextField("Search for column...", text: $searchText)
                    .textFieldStyle(.plain)
                    .font(.system(size: 12))
                    .frame(width: 150)
            }
            .padding(.horizontal, 8)
            .padding(.vertical, 4)
            .background(Color(nsColor: .controlBackgroundColor))
            .cornerRadius(6)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
        .background(Color(nsColor: .windowBackgroundColor))
        .overlay(alignment: .bottom) { Divider() }
    }

    // MARK: - Structure Toolbar

    private var structureToolbar: some View {
        HStack(spacing: 12) {
            if !pendingChanges.isEmpty {
                HStack(spacing: 6) {
                    Circle().fill(.orange).frame(width: 6, height: 6)
                    Text("\(pendingChanges.count) pending")
                        .font(.system(size: 11))
                        .foregroundStyle(.orange)
                }

                Button {
                    pendingChanges.removeAll()
                    if let desc = tableDescription { syncFromDescription(desc) }
                } label: {
                    HStack(spacing: 3) {
                        Image(systemName: "arrow.uturn.backward").font(.system(size: 10))
                        Text("Discard").font(.system(size: 11))
                    }
                }
                .buttonStyle(.plain)
                .pointerCursor()
                .foregroundStyle(.secondary)

                Button {
                    Task { await applyChanges() }
                } label: {
                    HStack(spacing: 3) {
                        Image(systemName: "checkmark.circle.fill").font(.system(size: 10))
                        Text("Apply").font(.system(size: 11, weight: .medium))
                    }
                }
                .buttonStyle(.plain)
                .pointerCursor()
                .foregroundStyle(Color.accentColor)
                .disabled(isApplying)
            }

            Spacer()
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 5)
        .frame(height: 28)
        .background(Color(nsColor: .windowBackgroundColor))
        .overlay(alignment: .bottom) { Divider() }
    }

    // MARK: - Columns Section (Editable on select)

    @ViewBuilder
    private func columnsSection(_ desc: TableDescription) -> some View {
        VStack(spacing: 0) {
            Table($editableColumns, selection: $selectedColumnId) {
                TableColumn("#") { $col in
                    let state = columnChangeState(col)
                    Text("\((columnIndexMap[col.id] ?? 0) + 1)")
                        .foregroundStyle(.secondary)
                        .background { RowBackgroundSetter(color: state.nsColor) }
                }
                .width(30)

                TableColumn("column_name") { $col in
                    columnNameCell(col: $col)
                }

                TableColumn("data_type") { $col in
                    dataTypeCell(col: $col)
                }

                TableColumn("is_nullable") { $col in
                    nullableCell(col: $col)
                }
                .width(80)

                TableColumn("check") { $col in
                    Text(col.checkConstraint.isEmpty ? "" : col.checkConstraint)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                        .help(col.checkConstraint)
                }

                TableColumn("foreign_key") { $col in
                    HStack(spacing: 4) {
                        Text(col.hasForeignKey ? col.foreignKeyDisplay : "EMPTY")
                            .foregroundStyle(col.hasForeignKey ? .primary : .tertiary)
                            .lineLimit(1)
                        Spacer()
                        Image(systemName: "arrow.right")
                            .font(.system(size: 9))
                            .foregroundStyle(.tertiary)
                    }
                    .contentShape(Rectangle())
                    .pointerCursor()
                    .onTapGesture { openFKEditor(col) }
                    .popover(isPresented: Binding(
                        get: { fkEditColumnId == col.id },
                        set: { if !$0 { fkEditColumnId = nil } }
                    )) {
                        foreignKeyPopover(col)
                    }
                }

                TableColumn("column_default") { $col in
                    defaultCell(col: $col)
                }

                TableColumn("comment") { $col in
                    commentCell(col: $col)
                }
            }
            .contextMenu(forSelectionType: EditableColumn.ID.self) { ids in
                if let id = ids.first, let col = editableColumns.first(where: { $0.id == id }) {
                    if col.isMarkedForDeletion {
                        Button("Undo Drop Column \"\(col.name)\"") {
                            dropColumn(col) // toggles deletion
                        }
                    } else {
                        Button("Drop Column \"\(col.name)\"", role: .destructive) {
                            columnToDelete = col
                            showDeleteConfirm = true
                        }
                        .disabled(col.isPrimaryKey)
                    }
                }
            } primaryAction: { _ in }
            .onChange(of: selectedColumnId) { oldId, newId in
                // Track any uncommitted edits from the previously selected column
                if let oldId, let col = editableColumns.first(where: { $0.id == oldId }) {
                    trackAllChanges(col)
                }
                focusedColumnField = nil  // Clear focus when row changes to prevent scroll jump
                updateColumnDetails(newId)
            }
            .environment(\.defaultMinListRowHeight, 34)
            .id(tableGeneration)

            // Add new column button
            HStack {
                Button { addNewColumn() } label: {
                    HStack(spacing: 4) {
                        Image(systemName: "plus").font(.system(size: 9))
                        Text("New column").font(.system(size: 11))
                    }
                    .padding(.horizontal, 10)
                    .padding(.vertical, 6)
                }
                .buttonStyle(.plain)
                .pointerCursor()
                .foregroundStyle(.tertiary)
                Spacer()
            }
            .padding(.leading, 8)
            .padding(.top, 4)
        }
        .frame(minHeight: 120)
    }

    // MARK: - Column Cell Views (extracted to help Swift type-checker)

    @ViewBuilder
    private func columnNameCell(col: Binding<EditableColumn>) -> some View {
        let c = col.wrappedValue
        let isEditable = selectedColumnId == c.id && !c.isMarkedForDeletion
        HStack(spacing: 4) {
            if c.isPrimaryKey {
                Image(systemName: "key.fill").font(.system(size: 9)).foregroundStyle(.yellow)
            }
            if isEditable {
                TextField("name", text: col.name)
                    .textFieldStyle(.squareBorder)
                    .fontWeight(c.isPrimaryKey ? .medium : .regular)
                    .focused($focusedColumnField, equals: .name(c.id))
                    .onSubmit { trackRename(c) }
            } else {
                Text(c.name)
                    .fontWeight(c.isPrimaryKey ? .medium : .regular)
                    .strikethrough(c.isMarkedForDeletion)
                    .foregroundStyle(c.isMarkedForDeletion ? .secondary : .primary)
                    .lineLimit(1)
            }
        }
    }

    @ViewBuilder
    private func dataTypeCell(col: Binding<EditableColumn>) -> some View {
        let c = col.wrappedValue
        let isEditable = selectedColumnId == c.id && !c.isMarkedForDeletion
        if isEditable {
            HStack(spacing: 2) {
                TextField("type", text: col.dataType)
                    .textFieldStyle(.squareBorder)
                    .focused($focusedColumnField, equals: .dataType(c.id))
                    .onSubmit { trackTypeChange(c) }
                Menu {
                    ForEach(cachedDataTypes, id: \.self) { t in
                        Button(t) { col.wrappedValue.dataType = t; trackTypeChange(col.wrappedValue) }
                    }
                } label: {
                    Image(systemName: "chevron.down")
                        .font(.system(size: 8, weight: .semibold))
                        .foregroundStyle(.secondary)
                }
                .menuStyle(.borderlessButton)
                .menuIndicator(.hidden)
                .fixedSize()
            }
        } else {
            Text(c.dataType).lineLimit(1)
        }
    }

    @ViewBuilder
    private func nullableCell(col: Binding<EditableColumn>) -> some View {
        let c = col.wrappedValue
        let isEditable = selectedColumnId == c.id && !c.isMarkedForDeletion
        if isEditable {
            Picker("", selection: Binding(
                get: { c.isNullable },
                set: { col.wrappedValue.isNullable = $0; trackNullable(col.wrappedValue) }
            )) {
                Text("NO").tag(false)
                Text("YES").tag(true)
            }
            .pickerStyle(.menu)
            .labelsHidden()
            .focusable(false)
        } else {
            Text(c.isNullable ? "YES" : "NO")
                .foregroundStyle(c.isNullable ? .primary : .secondary)
                .lineLimit(1)
        }
    }

    @ViewBuilder
    private func defaultCell(col: Binding<EditableColumn>) -> some View {
        let c = col.wrappedValue
        HStack(spacing: 4) {
            Text(c.defaultValue.isEmpty ? "NULL" : c.defaultValue)
                .foregroundStyle(c.defaultValue.isEmpty ? .tertiary : .primary)
                .lineLimit(1)
            Image(systemName: "chevron.down")
                .font(.system(size: 8))
                .foregroundStyle(selectedColumnId == c.id ? Color.gray.opacity(0.4) : Color.clear)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .contentShape(Rectangle())
        .pointerCursor()
        .onTapGesture { openDefaultEditor(c) }
        .popover(isPresented: Binding(
            get: { defaultEditColumnId == c.id },
            set: { if !$0 { defaultEditColumnId = nil } }
        )) {
            defaultValuePopover(c)
        }
    }

    @ViewBuilder
    private func commentCell(col: Binding<EditableColumn>) -> some View {
        let c = col.wrappedValue
        let isEditable = selectedColumnId == c.id && !c.isMarkedForDeletion
        if isEditable {
            TextField("NULL", text: col.comment)
                .textFieldStyle(.squareBorder)
                .focused($focusedColumnField, equals: .comment(c.id))
                .onSubmit { trackComment(c) }
        } else {
            Text(c.comment.isEmpty ? "NULL" : c.comment)
                .foregroundStyle(c.comment.isEmpty ? .tertiary : .primary)
                .lineLimit(1)
        }
    }

    private func updateColumnDetails(_ colId: UUID?) {
        guard let colId, let col = editableColumns.first(where: { $0.id == colId }) else {
            onSelectColumn?(nil)
            return
        }
        let details: [(column: String, value: String)] = [
            (column: "column_name", value: col.name),
            (column: "data_type", value: col.dataType),
            (column: "is_nullable", value: col.isNullable ? "YES" : "NO"),
            (column: "is_primary_key", value: col.isPrimaryKey ? "YES" : "NO"),
            (column: "column_default", value: col.defaultValue.isEmpty ? "NULL" : col.defaultValue),
            (column: "foreign_key", value: col.hasForeignKey ? col.foreignKeyDisplay : "EMPTY"),
            (column: "fk_referenced_table", value: col.fkReferencedTable.isEmpty ? "NULL" : col.fkReferencedTable),
            (column: "fk_referenced_column", value: col.fkReferencedColumn.isEmpty ? "NULL" : col.fkReferencedColumn),
            (column: "fk_on_update", value: col.fkOnUpdate.rawValue),
            (column: "fk_on_delete", value: col.fkOnDelete.rawValue),
            (column: "check_constraint", value: col.checkConstraint.isEmpty ? "EMPTY" : col.checkConstraint),
            (column: "comment", value: col.comment.isEmpty ? "NULL" : col.comment),
        ]
        onSelectColumn?(details)
    }

    // MARK: - Indexes Section (Editable)

    @ViewBuilder
    private func indexesSection(_ desc: TableDescription) -> some View {
        VStack(spacing: 0) {
            Rectangle()
                .fill(Color.accentColor.opacity(0.6))
                .frame(height: 2)

            if editableIndexes.isEmpty {
                Text("No indexes")
                    .foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                Table($editableIndexes, selection: $selectedIndexId) {
                    TableColumn("index_name") { $idx in
                        let state = indexChangeState(idx)
                        HStack(spacing: 4) {
                            if idx.isMarkedForDeletion {
                                Text(idx.name).strikethrough().foregroundStyle(.secondary).lineLimit(1)
                            } else if selectedIndexId == idx.id {
                                TextField("name", text: $idx.name)
                                    .textFieldStyle(.squareBorder)
                                    .onSubmit { trackIndexChange(idx) }
                            } else {
                                Text(idx.name).lineLimit(1)
                            }
                        }
                        .background { RowBackgroundSetter(color: state.nsColor) }
                    }

                    TableColumn("index_algorithm") { $idx in
                        if selectedIndexId == idx.id && !idx.isMarkedForDeletion {
                            Picker("", selection: Binding(
                                get: { idx.algorithm },
                                set: { idx.algorithm = $0; trackIndexChange(idx) }
                            )) {
                                Text("BTREE").tag("BTREE")
                                Text("HASH").tag("HASH")
                                Text("GIST").tag("GIST")
                                Text("SPGIST").tag("SPGIST")
                                Text("GIN").tag("GIN")
                                Text("BRIN").tag("BRIN")
                            }
                            .pickerStyle(.menu)
                            .labelsHidden()
                        } else {
                            Text(idx.algorithm).lineLimit(1)
                        }
                    }.width(120)

                    TableColumn("is_unique") { $idx in
                        if selectedIndexId == idx.id && !idx.isMarkedForDeletion {
                            Picker("", selection: Binding(
                                get: { idx.isUnique },
                                set: { idx.isUnique = $0; trackIndexChange(idx) }
                            )) {
                                Text("FALSE").tag(false)
                                Text("TRUE").tag(true)
                            }
                            .pickerStyle(.menu)
                            .labelsHidden()
                        } else {
                            Text(idx.isUnique ? "TRUE" : "FALSE")
                                .foregroundStyle(idx.isUnique ? .primary : .secondary)
                        }
                    }.width(80)

                    TableColumn("column_name") { $idx in
                        if selectedIndexId == idx.id && !idx.isMarkedForDeletion {
                            TextField("col1, col2", text: $idx.columns)
                                .textFieldStyle(.squareBorder)
                                .onSubmit { trackIndexChange(idx) }
                        } else {
                            Text(idx.columns).lineLimit(1)
                        }
                    }

                    TableColumn("condition") { $idx in
                        if selectedIndexId == idx.id && !idx.isMarkedForDeletion {
                            TextField("EMPTY", text: $idx.condition)
                                .textFieldStyle(.squareBorder)
                                .onSubmit { trackIndexChange(idx) }
                        } else {
                            Text(idx.condition.isEmpty ? "EMPTY" : idx.condition)
                                .foregroundStyle(idx.condition.isEmpty ? .tertiary : .secondary)
                                .lineLimit(1)
                        }
                    }

                    TableColumn("include") { $idx in
                        if selectedIndexId == idx.id && !idx.isMarkedForDeletion {
                            TextField("EMPTY", text: $idx.include)
                                .textFieldStyle(.squareBorder)
                        } else {
                            Text(idx.include.isEmpty ? "EMPTY" : idx.include)
                                .foregroundStyle(idx.include.isEmpty ? .tertiary : .secondary)
                                .lineLimit(1)
                        }
                    }

                    TableColumn("comment") { $idx in
                        if selectedIndexId == idx.id && !idx.isMarkedForDeletion {
                            TextField("NULL", text: $idx.comment)
                                .textFieldStyle(.squareBorder)
                                .onSubmit { trackIndexComment(idx) }
                        } else {
                            Text(idx.comment.isEmpty ? "NULL" : idx.comment)
                                .foregroundStyle(idx.comment.isEmpty ? .tertiary : .secondary)
                                .lineLimit(1)
                        }
                    }
                }
                .contextMenu(forSelectionType: EditableIndex.ID.self) { ids in
                    if let id = ids.first, let idx = editableIndexes.first(where: { $0.id == id }) {
                        if idx.isMarkedForDeletion {
                            Button("Undo Drop Index \"\(idx.name)\"") {
                                dropIndex(idx) // toggles deletion
                            }
                        } else {
                            Button("Drop Index \"\(idx.name)\"", role: .destructive) {
                                dropIndex(idx)
                            }
                        }
                    }
                } primaryAction: { _ in }
                .onChange(of: selectedIndexId) { oldId, _ in
                    if let oldId, let idx = editableIndexes.first(where: { $0.id == oldId }) {
                        trackIndexChangeOnDeselect(idx)
                    }
                }
                .environment(\.defaultMinListRowHeight, 34)
                .id(tableGeneration)
            }

            // Add new index button
            HStack {
                Button { showAddIndex = true } label: {
                    HStack(spacing: 4) {
                        Image(systemName: "plus").font(.system(size: 9))
                        Text("New index").font(.system(size: 11))
                    }
                    .padding(.horizontal, 10)
                    .padding(.vertical, 6)
                }
                .buttonStyle(.plain)
                .pointerCursor()
                .foregroundStyle(.tertiary)
                Spacer()
            }
            .padding(.leading, 8)
            .padding(.top, 4)
        }
        .frame(minHeight: 80)
    }

    /// Track all field changes for an index (called when deselecting a row)
    private func trackIndexChangeOnDeselect(_ idx: EditableIndex) {
        guard !idx.isNew, let orig = idx.originalName else { return }
        guard let desc = tableDescription,
              let origIdx = desc.indexes.first(where: { $0.name == orig }) else { return }

        let currentCols = idx.columns.split(separator: ",").map { String($0).trimmingCharacters(in: .whitespaces) }
        let origCols = origIdx.columns
        let origAlgo = origIdx.type?.uppercased() ?? "BTREE"
        let origCond = origIdx.condition ?? ""
        let origComment = origIdx.comment ?? ""

        if idx.name != orig || currentCols != origCols || idx.algorithm != origAlgo
            || idx.isUnique != origIdx.isUnique || idx.condition != origCond {
            trackIndexChange(idx)
        }
        if idx.comment != origComment {
            trackIndexComment(idx)
        }
    }

    private func trackIndexChange(_ idx: EditableIndex) {
        guard let orig = idx.originalName else { return }
        let cols = idx.columns.split(separator: ",").map { String($0).trimmingCharacters(in: .whitespaces) }
        guard !cols.isEmpty else { return }
        pendingChanges.removeAll { if case .modifyIndex(let o, _, _, _, _, _) = $0, o == orig { return true }; return false }
        let cond = idx.condition.isEmpty ? nil : idx.condition
        pendingChanges.append(.modifyIndex(oldName: orig, newName: idx.name, columns: cols, algorithm: idx.algorithm, isUnique: idx.isUnique, condition: cond))
    }

    private func trackIndexComment(_ idx: EditableIndex) {
        let name = idx.originalName ?? idx.name
        guard !idx.isNew else { return }
        pendingChanges.removeAll { if case .setIndexComment(let n, _) = $0, n == name { return true }; return false }
        let val = idx.comment.isEmpty ? nil : idx.comment
        pendingChanges.append(.setIndexComment(name: name, comment: val))
    }

    private func dropIndex(_ idx: EditableIndex) {
        if idx.isNew {
            editableIndexes.removeAll { $0.id == idx.id }
            pendingChanges.removeAll { if case .addIndex(let n, _, _, _, _) = $0, n == idx.name { return true }; return false }
        } else if let orig = idx.originalName {
            if let i = editableIndexes.firstIndex(where: { $0.id == idx.id }) {
                if idx.isMarkedForDeletion {
                    // Undo delete
                    editableIndexes[i].isMarkedForDeletion = false
                    pendingChanges.removeAll { if case .dropIndex(let n) = $0, n == orig { return true }; return false }
                } else {
                    // Mark for deletion
                    editableIndexes[i].isMarkedForDeletion = true
                    pendingChanges.append(.dropIndex(name: orig))
                }
            }
        }
    }

    // MARK: - Sync

    private func syncFromDescription(_ desc: TableDescription) {
        editableColumns = desc.columns.map { col in
            let fk = desc.foreignKeys.first { $0.columns.contains(col.name) }
            let fkDisplay = fk.map { "\($0.referencedTable)(\($0.referencedColumns.joined(separator: ", ")))" } ?? ""
            let fkRefCol = fk.flatMap { fkInfo -> String? in
                guard let idx = fkInfo.columns.firstIndex(of: col.name), idx < fkInfo.referencedColumns.count else { return nil }
                return fkInfo.referencedColumns[idx]
            } ?? ""
            return EditableColumn(
                originalName: col.name,
                name: col.name,
                dataType: col.dataType,
                isNullable: col.isNullable,
                defaultValue: col.defaultValue ?? "",
                comment: col.comment ?? "",
                isPrimaryKey: col.isPrimaryKey,
                foreignKeyDisplay: fkDisplay,
                checkConstraint: col.checkConstraint ?? "",
                fkConstraintName: fk?.name ?? "",
                fkReferencedTable: fk?.referencedTable ?? "",
                fkReferencedColumn: fkRefCol,
                fkOnUpdate: fk?.onUpdate ?? .noAction,
                fkOnDelete: fk?.onDelete ?? .noAction
            )
        }
        columnIndexMap = Dictionary(uniqueKeysWithValues: editableColumns.enumerated().map { ($1.id, $0) })
        editableIndexes = desc.indexes.map { idx in
            EditableIndex(
                originalName: idx.name,
                name: idx.name,
                algorithm: idx.type?.uppercased() ?? "BTREE",
                isUnique: idx.isUnique,
                columns: idx.columns.joined(separator: ", "),
                condition: idx.condition ?? "",
                include: idx.include ?? "",
                comment: idx.comment ?? ""
            )
        }
        rebuildDataTypeCache()
    }

    private func rebuildDataTypeCache() {
        let base: [String]
        if let adapter {
            switch adapter.databaseType {
            case .postgresql: base = postgresDataTypes
            case .mysql: base = mysqlDataTypes
            case .sqlite: base = sqliteDataTypes
            case .mssql: base = [
                // Exact numerics
                "INT", "BIGINT", "SMALLINT", "TINYINT", "BIT",
                "DECIMAL", "NUMERIC", "MONEY", "SMALLMONEY",
                // Approximate numerics
                "FLOAT", "REAL",
                // Date and time
                "DATE", "TIME", "DATETIME", "DATETIME2", "DATETIMEOFFSET", "SMALLDATETIME",
                // Character strings
                "CHAR", "VARCHAR", "VARCHAR(MAX)", "TEXT",
                // Unicode character strings
                "NCHAR", "NVARCHAR", "NVARCHAR(MAX)", "NTEXT",
                // Binary strings
                "BINARY", "VARBINARY", "VARBINARY(MAX)", "IMAGE",
                // Other
                "UNIQUEIDENTIFIER", "XML", "JSON", "ROWVERSION", "HIERARCHYID",
                "GEOGRAPHY", "GEOMETRY", "SQL_VARIANT",
            ]
            case .clickhouse: base = [
                "UInt8", "UInt16", "UInt32", "UInt64", "UInt128", "UInt256",
                "Int8", "Int16", "Int32", "Int64", "Int128", "Int256",
                "Float32", "Float64", "Decimal(18, 4)", "Decimal(38, 4)", "Decimal(76, 4)",
                "String", "FixedString(32)", "UUID",
                "Date", "Date32", "DateTime", "DateTime64(3)", "DateTime64(6)",
                "Boolean", "IPv4", "IPv6", "JSON",
                "Nullable(String)", "Array(String)", "Array(UInt64)", "Map(String, String)",
            ]
            case .redis: base = ["string", "list", "set", "zset", "hash", "stream"]
            case .mongodb: base = ["string", "integer", "double", "boolean", "date", "objectId", "document", "array"]
            }
        } else {
            base = postgresDataTypes
        }
        let baseSet = Set(base.map { $0.lowercased() })
        let missing = Array(Set(
            editableColumns.map { $0.dataType }
                .filter { !$0.isEmpty && !baseSet.contains($0.lowercased()) }
        )).sorted()
        cachedDataTypes = missing + base
    }

    // MARK: - Change Tracking

    /// Track all field changes for a column (called when deselecting a row)
    private func trackAllChanges(_ col: EditableColumn) {
        guard !col.isNew, let orig = col.originalName else { return }
        // Find the original description to compare
        guard let desc = tableDescription,
              let origCol = desc.columns.first(where: { $0.name == orig }) else { return }

        if col.name != orig { trackRename(col) }
        if col.dataType != origCol.dataType { trackTypeChange(col) }
        if col.isNullable != origCol.isNullable { trackNullable(col) }
        let origDefault = origCol.defaultValue ?? ""
        if col.defaultValue != origDefault { trackDefault(col) }
        let origComment = origCol.comment ?? ""
        if col.comment != origComment { trackComment(col) }
    }

    private func trackRename(_ col: EditableColumn) {
        guard let orig = col.originalName, orig != col.name, !col.name.isEmpty else { return }
        pendingChanges.removeAll { if case .renameColumn(let o, _) = $0, o == orig { return true }; return false }
        pendingChanges.append(.renameColumn(oldName: orig, newName: col.name))
    }

    private func trackTypeChange(_ col: EditableColumn) {
        let name = col.originalName ?? col.name
        guard !col.isNew else { return }
        pendingChanges.removeAll { if case .changeType(let c, _) = $0, c == name { return true }; return false }
        pendingChanges.append(.changeType(column: name, newType: col.dataType))
    }

    private func trackNullable(_ col: EditableColumn) {
        let name = col.originalName ?? col.name
        guard !col.isNew else { return }
        pendingChanges.removeAll { if case .setNullable(let c, _) = $0, c == name { return true }; return false }
        pendingChanges.append(.setNullable(column: name, nullable: col.isNullable))
    }

    private func trackDefault(_ col: EditableColumn) {
        let name = col.originalName ?? col.name
        guard !col.isNew else { return }
        pendingChanges.removeAll { if case .setDefault(let c, _) = $0, c == name { return true }; return false }
        let val = col.defaultValue.isEmpty ? nil : col.defaultValue
        pendingChanges.append(.setDefault(column: name, value: val))
    }

    // MARK: - Default Value Editor

    private func openDefaultEditor(_ col: EditableColumn) {
        let current = col.defaultValue
        // Detect current default type
        if current.hasPrefix("nextval(") || current.contains("_seq") {
            defaultTab = .sequence
            // Extract sequence name from nextval('seq_name'::regclass)
            let seqName = current
                .replacingOccurrences(of: "nextval('", with: "")
                .replacingOccurrences(of: "'::regclass)", with: "")
                .replacingOccurrences(of: "nextval('", with: "")
                .replacingOccurrences(of: "')", with: "")
            defaultSequenceName = seqName
            defaultStringValue = ""
            defaultExpressionValue = ""
            defaultCreateSequence = false
        } else if current.contains("(") || current == "CURRENT_TIMESTAMP" || current == "NOW()" ||
                    current == "true" || current == "false" || current == "gen_random_uuid()" ||
                    current.uppercased().hasPrefix("CURRENT_") {
            defaultTab = .expression
            defaultExpressionValue = current
            defaultStringValue = ""
            defaultSequenceName = ""
            defaultCreateSequence = false
        } else {
            defaultTab = .string
            // Strip surrounding quotes if present
            var str = current
            if str.hasPrefix("'") && str.hasSuffix("'") && str.count >= 2 {
                str = String(str.dropFirst().dropLast())
            }
            defaultStringValue = str
            defaultExpressionValue = ""
            defaultSequenceName = ""
            defaultCreateSequence = false
        }
        defaultEditColumnId = col.id
    }

    private func saveDefaultValue(_ col: EditableColumn) {
        guard let idx = editableColumns.firstIndex(where: { $0.id == col.id }) else { return }
        let value: String
        switch defaultTab {
        case .string:
            value = defaultStringValue.isEmpty ? "" : "'\(defaultStringValue.replacingOccurrences(of: "'", with: "''"))'"
        case .expression:
            value = defaultExpressionValue
        case .sequence:
            if defaultSequenceName.isEmpty {
                value = ""
            } else {
                value = "nextval('\(defaultSequenceName)')"
            }
        }
        editableColumns[idx].defaultValue = value

        // Create sequence if needed
        if defaultTab == .sequence && defaultCreateSequence && !defaultSequenceName.isEmpty {
            let seqSQL = "CREATE SEQUENCE IF NOT EXISTS \(defaultSequenceName)"
            // Add as a pending change — we'll prepend it before the ALTER
            pendingChanges.removeAll { if case .createSequence(let n) = $0, n == defaultSequenceName { return true }; return false }
            pendingChanges.append(.createSequence(name: seqSQL))
        }

        trackDefault(editableColumns[idx])
        defaultEditColumnId = nil
    }

    private func defaultValuePopover(_ col: EditableColumn) -> some View {
        VStack(spacing: 12) {
            // Tab picker
            Picker("", selection: $defaultTab) {
                ForEach(DefaultValueTab.allCases, id: \.self) { tab in
                    Text(tab.rawValue).tag(tab)
                }
            }
            .pickerStyle(.segmented)
            .labelsHidden()

            // Tab content
            switch defaultTab {
            case .string:
                VStack(alignment: .leading, spacing: 6) {
                    Text("String value").font(.caption).foregroundStyle(.secondary)
                    TextField("Enter string value...", text: $defaultStringValue)
                        .textFieldStyle(.roundedBorder)
                }

            case .expression:
                VStack(alignment: .leading, spacing: 6) {
                    Text("SQL expression").font(.caption).foregroundStyle(.secondary)
                    TextField("e.g. CURRENT_TIMESTAMP, gen_random_uuid()", text: $defaultExpressionValue)
                        .textFieldStyle(.roundedBorder)
                    // Quick-pick buttons
                    HStack(spacing: 4) {
                        ForEach(["CURRENT_TIMESTAMP", "NOW()", "gen_random_uuid()", "true", "false"], id: \.self) { expr in
                            Button(expr) { defaultExpressionValue = expr }
                                .buttonStyle(.plain)
                                .font(.system(size: 10, design: .monospaced))
                                .padding(.horizontal, 6)
                                .padding(.vertical, 2)
                                .background(Color(nsColor: .controlBackgroundColor))
                                .cornerRadius(4)
                        }
                    }
                }

            case .sequence:
                VStack(alignment: .leading, spacing: 6) {
                    Text("Sequence name").font(.caption).foregroundStyle(.secondary)
                    TextField("e.g. table_id_seq", text: $defaultSequenceName)
                        .textFieldStyle(.roundedBorder)
                    Toggle("Create sequence if not exists", isOn: $defaultCreateSequence)
                        .font(.caption)
                }
            }

            Divider()

            // Buttons
            HStack {
                if !col.defaultValue.isEmpty {
                    Button("Clear", role: .destructive) {
                        guard let idx = editableColumns.firstIndex(where: { $0.id == col.id }) else { return }
                        editableColumns[idx].defaultValue = ""
                        trackDefault(editableColumns[idx])
                        defaultEditColumnId = nil
                    }
                }
                Spacer()
                Button("Cancel") { defaultEditColumnId = nil }
                Button("OK") { saveDefaultValue(col) }
                    .buttonStyle(.borderedProminent)
            }
        }
        .padding(16)
        .frame(width: 360)
    }

    private func trackComment(_ col: EditableColumn) {
        let name = col.originalName ?? col.name
        guard !col.isNew else { return }
        pendingChanges.removeAll { if case .setComment(let c, _) = $0, c == name { return true }; return false }
        let val = col.comment.isEmpty ? nil : col.comment
        pendingChanges.append(.setComment(column: name, comment: val))
    }

    // MARK: - Foreign Key Editor

    private func openFKEditor(_ col: EditableColumn) {
        fkRefTable = col.fkReferencedTable
        fkRefColumn = col.fkReferencedColumn
        fkOnUpdate = col.fkOnUpdate
        fkOnDelete = col.fkOnDelete
        fkEditColumnId = col.id
        // Load available tables if needed
        if availableTables.isEmpty {
            Task {
                if let tables = try? await adapter?.listTables(schema: schema) {
                    availableTables = tables
                        .filter { $0.type == .table }
                        .map { $0.name }
                        .sorted()
                }
            }
        }
    }

    @ViewBuilder
    private func foreignKeyPopover(_ col: EditableColumn) -> some View {
        VStack(spacing: 12) {
            // Table (current, read-only)
            HStack {
                Text("Table").frame(width: 130, alignment: .trailing).foregroundStyle(.secondary)
                Text(tableName)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.horizontal, 8).padding(.vertical, 4)
                    .background(Color(nsColor: .controlBackgroundColor))
                    .cornerRadius(6)
            }

            // Column (read-only)
            HStack {
                Text("Columns").frame(width: 130, alignment: .trailing).foregroundStyle(.secondary)
                Text(col.name)
                    .padding(.horizontal, 8).padding(.vertical, 4)
                    .background(Color.accentColor.opacity(0.2))
                    .cornerRadius(4)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }

            // Referenced Table
            HStack {
                Text("Referenced Table").frame(width: 130, alignment: .trailing).foregroundStyle(.secondary)
                Picker("", selection: $fkRefTable) {
                    Text("Select a table...").tag("")
                    ForEach(availableTables, id: \.self) { t in
                        Text(t).tag(t)
                    }
                }
                .labelsHidden()
            }

            // Referenced Column
            HStack {
                Text("Referenced Columns").frame(width: 130, alignment: .trailing).foregroundStyle(.secondary)
                TextField("Column name...", text: $fkRefColumn)
                    .textFieldStyle(.roundedBorder)
            }

            // On Update
            HStack {
                Text("On Update").frame(width: 130, alignment: .trailing).foregroundStyle(.secondary)
                Picker("", selection: $fkOnUpdate) {
                    ForEach([ForeignKeyAction.noAction, .cascade, .setNull, .setDefault, .restrict], id: \.self) { a in
                        Text(a.rawValue).tag(a)
                    }
                }
                .labelsHidden()
            }

            // On Delete
            HStack {
                Text("On Delete").frame(width: 130, alignment: .trailing).foregroundStyle(.secondary)
                Picker("", selection: $fkOnDelete) {
                    ForEach([ForeignKeyAction.noAction, .cascade, .setNull, .setDefault, .restrict], id: \.self) { a in
                        Text(a.rawValue).tag(a)
                    }
                }
                .labelsHidden()
            }

            Divider()

            // Buttons
            HStack {
                if col.hasForeignKey {
                    Button("Delete", role: .destructive) {
                        deleteForeignKey(col)
                        fkEditColumnId = nil
                    }
                }
                Spacer()
                Button("Cancel") { fkEditColumnId = nil }
                Button("OK") {
                    saveForeignKey(col)
                    fkEditColumnId = nil
                }
                .disabled(fkRefTable.isEmpty || fkRefColumn.isEmpty)
                .buttonStyle(.borderedProminent)
            }
        }
        .padding(16)
        .frame(width: 400)
    }

    private func saveForeignKey(_ col: EditableColumn) {
        guard let idx = editableColumns.firstIndex(where: { $0.id == col.id }) else { return }
        let colName = col.originalName ?? col.name

        // Drop old FK if exists
        if !col.fkConstraintName.isEmpty {
            pendingChanges.append(.dropForeignKey(constraintName: col.fkConstraintName))
        }

        // Add new FK
        pendingChanges.append(.addForeignKey(
            column: colName, refTable: fkRefTable, refColumn: fkRefColumn,
            onUpdate: fkOnUpdate, onDelete: fkOnDelete
        ))

        // Update local state
        editableColumns[idx].fkReferencedTable = fkRefTable
        editableColumns[idx].fkReferencedColumn = fkRefColumn
        editableColumns[idx].fkOnUpdate = fkOnUpdate
        editableColumns[idx].fkOnDelete = fkOnDelete
        editableColumns[idx].foreignKeyDisplay = "\(fkRefTable)(\(fkRefColumn))"
    }

    private func deleteForeignKey(_ col: EditableColumn) {
        guard let idx = editableColumns.firstIndex(where: { $0.id == col.id }) else { return }
        if !col.fkConstraintName.isEmpty {
            pendingChanges.append(.dropForeignKey(constraintName: col.fkConstraintName))
        }
        editableColumns[idx].fkReferencedTable = ""
        editableColumns[idx].fkReferencedColumn = ""
        editableColumns[idx].fkOnUpdate = .noAction
        editableColumns[idx].fkOnDelete = .noAction
        editableColumns[idx].foreignKeyDisplay = ""
    }

    // MARK: - Add Index Popover

    private var addIndexPopover: some View {
        VStack(spacing: 12) {
            HStack {
                Text("Index Name").frame(width: 110, alignment: .trailing).foregroundStyle(.secondary)
                TextField("idx_\(tableName)_...", text: $newIndexName)
                    .textFieldStyle(.roundedBorder)
            }

            HStack {
                Text("Columns").frame(width: 110, alignment: .trailing).foregroundStyle(.secondary)
                TextField("col1, col2", text: $newIndexColumns)
                    .textFieldStyle(.roundedBorder)
            }

            HStack {
                Text("Algorithm").frame(width: 110, alignment: .trailing).foregroundStyle(.secondary)
                Picker("", selection: $newIndexAlgorithm) {
                    Text("BTREE").tag("BTREE")
                    Text("HASH").tag("HASH")
                    Text("GIN").tag("GIN")
                    Text("GIST").tag("GIST")
                    Text("BRIN").tag("BRIN")
                }
                .labelsHidden()
            }

            HStack {
                Text("Unique").frame(width: 110, alignment: .trailing).foregroundStyle(.secondary)
                Toggle("", isOn: $newIndexIsUnique).labelsHidden()
                Spacer()
            }

            HStack {
                Text("Condition").frame(width: 110, alignment: .trailing).foregroundStyle(.secondary)
                TextField("WHERE ...", text: $newIndexCondition)
                    .textFieldStyle(.roundedBorder)
            }

            Divider()

            HStack {
                Spacer()
                Button("Cancel") { showAddIndex = false }
                Button("OK") {
                    addNewIndex()
                    showAddIndex = false
                }
                .disabled(newIndexColumns.isEmpty)
                .buttonStyle(.borderedProminent)
            }
        }
        .padding(16)
        .frame(width: 380)
    }

    private func addNewIndex() {
        let cols = newIndexColumns.split(separator: ",").map { String($0).trimmingCharacters(in: .whitespaces) }
        guard !cols.isEmpty else { return }
        let name = newIndexName.isEmpty ? "idx_\(tableName)_\(cols.joined(separator: "_"))" : newIndexName
        let cond = newIndexCondition.isEmpty ? nil : newIndexCondition
        pendingChanges.append(.addIndex(name: name, columns: cols, algorithm: newIndexAlgorithm, isUnique: newIndexIsUnique, condition: cond))
        // Add to editable list
        let newIdx = EditableIndex(
            originalName: nil,
            name: name,
            algorithm: newIndexAlgorithm,
            isUnique: newIndexIsUnique,
            columns: newIndexColumns,
            condition: newIndexCondition,
            include: "",
            comment: ""
        )
        editableIndexes.append(newIdx)
        selectedIndexId = newIdx.id
        // Reset form
        newIndexName = ""
        newIndexColumns = ""
        newIndexAlgorithm = "BTREE"
        newIndexIsUnique = false
        newIndexCondition = ""
    }

    // MARK: - Add / Drop

    private func addNewColumn() {
        let newCol = EditableColumn(
            originalName: nil,
            name: "new_column",
            dataType: "text",
            isNullable: true,
            defaultValue: "",
            comment: "",
            isPrimaryKey: false,
            foreignKeyDisplay: "",
            checkConstraint: ""
        )
        editableColumns.append(newCol)
        columnIndexMap = Dictionary(uniqueKeysWithValues: editableColumns.enumerated().map { ($1.id, $0) })
        selectedColumnId = newCol.id
        // Placeholder — will be replaced with actual values at apply time
        pendingChanges.append(.addColumn(name: "__new__", dataType: "text", nullable: true, defaultValue: nil))
    }

    private func dropColumn(_ col: EditableColumn) {
        if col.isNew {
            editableColumns.removeAll { $0.id == col.id }
            columnIndexMap = Dictionary(uniqueKeysWithValues: editableColumns.enumerated().map { ($1.id, $0) })
            pendingChanges.removeAll {
                if case .addColumn(let n, _, _, _) = $0, n == col.name || n == "__new__" { return true }; return false
            }
        } else if let name = col.originalName {
            if let idx = editableColumns.firstIndex(where: { $0.id == col.id }) {
                if col.isMarkedForDeletion {
                    // Undo delete
                    editableColumns[idx].isMarkedForDeletion = false
                    pendingChanges.removeAll { if case .dropColumn(let n) = $0, n == name { return true }; return false }
                } else {
                    // Mark for deletion (keep row visible with red highlight)
                    editableColumns[idx].isMarkedForDeletion = true
                    pendingChanges.append(.dropColumn(name: name))
                }
            }
        }
    }

    // MARK: - Apply Changes

    private func applyChanges() async {
        guard let adapter else { return }

        // Remove placeholder addColumn entries and replace with actual user-edited values
        pendingChanges.removeAll { if case .addColumn(let n, _, _, _) = $0, n == "__new__" { return true }; return false }
        let newColumns = editableColumns.filter { $0.isNew && !$0.name.isEmpty }
        for col in newColumns {
            let def = col.defaultValue.isEmpty ? nil : col.defaultValue
            pendingChanges.append(.addColumn(name: col.name, dataType: col.dataType, nullable: col.isNullable, defaultValue: def))
        }

        guard !pendingChanges.isEmpty else { return }
        isApplying = true
        defer { isApplying = false }

        let d = adapter.databaseType.sqlDialect
        let schemaPrefix = schema.map { d.quoteIdentifier($0) + "." } ?? ""
        let qualifiedTable = "\(schemaPrefix)\(d.quoteIdentifier(tableName))"

        var sqls: [String] = []

        let isMSSQL = adapter.databaseType == .mssql
        for change in pendingChanges {
            switch change {
            case .addColumn(let name, let dataType, let nullable, let defaultValue):
                // MSSQL uses "ALTER TABLE ... ADD col type" (no COLUMN keyword, no IF NOT EXISTS)
                var sql: String
                if isMSSQL {
                    sql = "ALTER TABLE \(qualifiedTable) ADD \(d.quoteIdentifier(name)) \(dataType)"
                } else {
                    sql = "ALTER TABLE \(qualifiedTable) ADD COLUMN IF NOT EXISTS \(d.quoteIdentifier(name)) \(dataType)"
                }
                if let def = defaultValue, !def.isEmpty {
                    sql += " DEFAULT \(def)"
                } else if !nullable {
                    // NOT NULL column on existing table needs a default for existing rows
                    let typeLower = dataType.lowercased()
                    let autoDefault: String
                    if typeLower.contains("int") || typeLower == "serial" || typeLower == "bigserial" || typeLower == "smallserial" {
                        autoDefault = "0"
                    } else if typeLower.contains("float") || typeLower.contains("double") || typeLower.contains("numeric") || typeLower.contains("decimal") || typeLower == "real" {
                        autoDefault = "0"
                    } else if typeLower == "bool" || typeLower == "boolean" {
                        autoDefault = "false"
                    } else if typeLower.contains("timestamp") || typeLower == "date" {
                        autoDefault = "CURRENT_TIMESTAMP"
                    } else if typeLower == "uuid" {
                        autoDefault = "gen_random_uuid()"
                    } else if typeLower == "jsonb" || typeLower == "json" {
                        autoDefault = "'{}'"
                    } else {
                        autoDefault = "''"
                    }
                    sql += " DEFAULT \(autoDefault)"
                }
                if !nullable { sql += " NOT NULL" }
                sqls.append(sql)

            case .renameColumn(let oldName, let newName):
                if isMSSQL {
                    // SQL Server: sp_rename 'schema.table.oldCol', 'newCol', 'COLUMN'
                    let tableRef = "\(schemaPrefix.replacingOccurrences(of: "[", with: "").replacingOccurrences(of: "]", with: "").replacingOccurrences(of: ".", with: ""))\(tableName)"
                    sqls.append("EXEC sp_rename '\(tableRef).\(oldName)', '\(newName)', 'COLUMN'")
                } else {
                    sqls.append("ALTER TABLE \(qualifiedTable) RENAME COLUMN \(d.quoteIdentifier(oldName)) TO \(d.quoteIdentifier(newName))")
                }

            case .changeType(let column, let newType):
                if adapter.databaseType == .postgresql {
                    // Try without USING first — PG handles implicit casts for most type changes.
                    sqls.append("ALTER TABLE \(qualifiedTable) ALTER COLUMN \(d.quoteIdentifier(column)) TYPE \(newType)")
                } else if isMSSQL {
                    // MSSQL: ALTER COLUMN col type (no TYPE keyword, no MODIFY)
                    sqls.append("ALTER TABLE \(qualifiedTable) ALTER COLUMN \(d.quoteIdentifier(column)) \(newType)")
                } else {
                    sqls.append("ALTER TABLE \(qualifiedTable) MODIFY COLUMN \(d.quoteIdentifier(column)) \(newType)")
                }

            case .setNullable(let column, let nullable):
                if isMSSQL {
                    // MSSQL requires re-declaring the column type to toggle nullability;
                    // skip for MVP (user can manually re-apply type with the right nullability)
                    break
                }
                if nullable {
                    sqls.append("ALTER TABLE \(qualifiedTable) ALTER COLUMN \(d.quoteIdentifier(column)) DROP NOT NULL")
                } else {
                    sqls.append("ALTER TABLE \(qualifiedTable) ALTER COLUMN \(d.quoteIdentifier(column)) SET NOT NULL")
                }

            case .setDefault(let column, let value):
                if isMSSQL {
                    // MSSQL uses constraint-based defaults — more complex, skip for MVP
                    break
                }
                if let val = value {
                    sqls.append("ALTER TABLE \(qualifiedTable) ALTER COLUMN \(d.quoteIdentifier(column)) SET DEFAULT \(val)")
                } else {
                    sqls.append("ALTER TABLE \(qualifiedTable) ALTER COLUMN \(d.quoteIdentifier(column)) DROP DEFAULT")
                }

            case .setComment(let column, let comment):
                if adapter.databaseType == .postgresql {
                    let val = comment.map { "'\($0.replacingOccurrences(of: "'", with: "''"))'" } ?? "NULL"
                    sqls.append("COMMENT ON COLUMN \(qualifiedTable).\(d.quoteIdentifier(column)) IS \(val)")
                }

            case .dropColumn(let name):
                sqls.append("ALTER TABLE \(qualifiedTable) DROP COLUMN \(d.quoteIdentifier(name))")

            case .dropForeignKey(let constraintName):
                sqls.append("ALTER TABLE \(qualifiedTable) DROP CONSTRAINT \(d.quoteIdentifier(constraintName))")

            case .addForeignKey(let column, let refTable, let refColumn, let onUpdate, let onDelete):
                let refQualified = "\(schemaPrefix)\(d.quoteIdentifier(refTable))"
                var sql = "ALTER TABLE \(qualifiedTable) ADD FOREIGN KEY (\(d.quoteIdentifier(column))) REFERENCES \(refQualified)(\(d.quoteIdentifier(refColumn)))"
                if onUpdate != .noAction { sql += " ON UPDATE \(onUpdate.rawValue)" }
                if onDelete != .noAction { sql += " ON DELETE \(onDelete.rawValue)" }
                sqls.append(sql)

            case .addIndex(let name, let columns, let algorithm, let isUnique, let condition):
                let colList = columns.map { d.quoteIdentifier($0) }.joined(separator: ", ")
                var sql = "CREATE"
                if isUnique { sql += " UNIQUE" }
                sql += " INDEX \(d.quoteIdentifier(name)) ON \(qualifiedTable)"
                if algorithm != "BTREE" { sql += " USING \(algorithm)" }
                sql += " (\(colList))"
                if let cond = condition { sql += " WHERE \(cond)" }
                sqls.append(sql)

            case .dropIndex(let name):
                sqls.append("DROP INDEX \(schemaPrefix)\(d.quoteIdentifier(name))")

            case .modifyIndex(let oldName, let newName, let columns, let algorithm, let isUnique, let condition):
                // Drop old, create new (PG doesn't support ALTER INDEX for structure)
                sqls.append("DROP INDEX \(schemaPrefix)\(d.quoteIdentifier(oldName))")
                let colList = columns.map { d.quoteIdentifier($0) }.joined(separator: ", ")
                var sql = "CREATE"
                if isUnique { sql += " UNIQUE" }
                sql += " INDEX \(d.quoteIdentifier(newName)) ON \(qualifiedTable)"
                if algorithm != "BTREE" { sql += " USING \(algorithm)" }
                sql += " (\(colList))"
                if let cond = condition { sql += " WHERE \(cond)" }
                sqls.append(sql)

            case .setIndexComment(let name, let comment):
                if adapter.databaseType == .postgresql {
                    let val = comment.map { "'\($0.replacingOccurrences(of: "'", with: "''"))'" } ?? "NULL"
                    sqls.append("COMMENT ON INDEX \(schemaPrefix)\(d.quoteIdentifier(name)) IS \(val)")
                }

            case .createSequence(let rawSQL):
                // Insert at beginning so sequence exists before column default references it
                sqls.insert(rawSQL, at: 0)
            }
        }

        // Execute all ALTER statements
        for sql in sqls {
            do {
                _ = try await adapter.executeRaw(sql: sql)
            } catch {
                // If ALTER TYPE fails due to dependent views, retry with drop/recreate
                let errStr = String(reflecting: error)
                if adapter.databaseType == .postgresql,
                   errStr.contains("0A000"),
                   errStr.contains("used by a view or rule") {
                    do {
                        try await executeWithDependentViews(sql: sql, adapter: adapter, schema: schema ?? "public")
                    } catch {
                        errorMessage = "Failed: \(sql)\n\(DataGridViewState.detailedErrorMessage(error))"
                        return
                    }
                } else {
                    errorMessage = "Failed: \(sql)\n\(DataGridViewState.detailedErrorMessage(error))"
                    return
                }
            }
        }

        pendingChanges.removeAll()
        // Reload structure from database, then force full table recreation
        if let freshDesc = await onStructureChanged?() {
            tableGeneration += 1
            syncFromDescription(freshDesc)
        }
    }

    /// Drop dependent views, execute ALTER, then recreate them.
    private func executeWithDependentViews(sql: String, adapter: any DatabaseAdapter, schema: String) async throws {
        // Query all views that depend on this table (parameterized to prevent SQL injection)
        let depSQL = """
            SELECT DISTINCT v.oid, c.relname AS view_name, n.nspname AS view_schema,
                   pg_get_viewdef(v.oid, true) AS view_def
            FROM pg_depend d
            JOIN pg_rewrite r ON r.oid = d.objid
            JOIN pg_class v ON v.oid = r.ev_class AND v.relkind = 'v'
            JOIN pg_namespace n ON n.oid = v.relnamespace
            JOIN pg_class c ON c.oid = v.oid
            WHERE d.refclassid = 'pg_class'::regclass
              AND d.refobjid = (SELECT oid FROM pg_class WHERE relname = $1 AND relnamespace = (SELECT oid FROM pg_namespace WHERE nspname = $2))
              AND d.deptype = 'n'
            ORDER BY v.oid
            """

        let depResult = try await adapter.executeWithRowValues(sql: depSQL, parameters: [.string(tableName), .string(schema)])

        // Collect view definitions
        var views: [(schema: String, name: String, definition: String)] = []
        for row in depResult.rows {
            guard row.count >= 4,
                  case .string(let vName) = row[1],
                  case .string(let vSchema) = row[2],
                  case .string(let vDef) = row[3] else { continue }
            views.append((schema: vSchema, name: vName, definition: vDef))
        }

        // Wrap in transaction so everything rolls back if any step fails
        _ = try await adapter.executeRaw(sql: "BEGIN")
        do {
            // Drop views in reverse order (handles dependencies between views)
            for view in views.reversed() {
                let dropSQL = "DROP VIEW IF EXISTS \"\(view.schema)\".\"\(view.name)\" CASCADE"
                _ = try await adapter.executeRaw(sql: dropSQL)
            }

            // Execute the ALTER statement
            _ = try await adapter.executeRaw(sql: sql)

            // Recreate views in original order
            for view in views {
                let createSQL = "CREATE VIEW \"\(view.schema)\".\"\(view.name)\" AS \(view.definition)"
                _ = try await adapter.executeRaw(sql: createSQL)
            }

            _ = try await adapter.executeRaw(sql: "COMMIT")
        } catch {
            _ = try? await adapter.executeRaw(sql: "ROLLBACK")
            throw error
        }
    }
}

// MARK: - Query Log Panel

struct QueryLogPanel: View {
    @ObservedObject var appState: AppState
    @State private var syntaxHighlighting = true
    @State private var panelHeight: CGFloat = 120
    @State private var dragStartY: CGFloat? = nil
    @State private var dragStartHeight: CGFloat? = nil

    private let minHeight: CGFloat = 60
    private let maxHeight: CGFloat = 400

    private static let timestampFormatter: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "yyyy-MM-dd HH:mm:ss.SSSS"
        return f
    }()

    var body: some View {
        VStack(spacing: 0) {
            // Resize handle — uses global Y to avoid layout feedback jitter
            Rectangle()
                .fill(Color(nsColor: .separatorColor))
                .frame(height: 5)
                .contentShape(Rectangle())
                .onHover { hovering in
                    if hovering { NSCursor.resizeUpDown.push() }
                    else { NSCursor.pop() }
                }
                .gesture(
                    DragGesture(minimumDistance: 1, coordinateSpace: .global)
                        .onChanged { value in
                            if dragStartY == nil {
                                dragStartY = value.startLocation.y
                                dragStartHeight = panelHeight
                            }
                            let delta = value.location.y - (dragStartY ?? value.startLocation.y)
                            panelHeight = max(minHeight, min(maxHeight, (dragStartHeight ?? 120) - delta))
                        }
                        .onEnded { _ in
                            dragStartY = nil
                            dragStartHeight = nil
                        }
                )

            ScrollViewReader { proxy in
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 0) {
                        ForEach(appState.queryLog) { entry in
                            VStack(alignment: .leading, spacing: 2) {
                                // Timestamp + duration
                                let ts = Self.timestampFormatter.string(from: entry.timestamp)
                                let durationStr = entry.duration.map { String(format: " (%.1fms)", $0 * 1000) } ?? ""
                                Text("-- \(ts)\(durationStr)")
                                    .font(.system(size: 11, design: .monospaced))
                                    .foregroundStyle(.green)

                                // SQL with syntax highlighting
                                Text(syntaxHighlighting ? entry.highlightedColored : entry.highlightedPlain)
                                    .font(.system(size: 12, design: .monospaced))
                                    .textSelection(.enabled)
                            }
                            .padding(.horizontal, 10)
                            .padding(.vertical, 4)
                            .id(entry.id)
                        }
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                }
                .onChange(of: appState.queryLog.count) { _, _ in
                    if let last = appState.queryLog.last {
                        proxy.scrollTo(last.id, anchor: .bottom)
                    }
                }
            }
            .frame(height: panelHeight)
            .background(Color(nsColor: .textBackgroundColor))

            // Bottom toolbar
            Divider()
            HStack(spacing: 12) {
                Button {
                    appState.clearQueryLog()
                } label: {
                    Image(systemName: "trash")
                        .font(.system(size: 11))
                }
                .buttonStyle(.plain)
                .foregroundStyle(.secondary)
                .help("Clear log")

                Toggle(isOn: $syntaxHighlighting) {
                    Text("Enable syntax highlighting")
                        .font(.system(size: 11))
                }
                .toggleStyle(.checkbox)
                .controlSize(.small)

                Spacer()
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 4)
            .frame(height: 26)
            .background(Color(nsColor: .windowBackgroundColor))
        }
    }

}

// MARK: - Commit Preview Sheet

struct CommitPreviewSheet: View {
    @ObservedObject var viewModel: DataGridViewState
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(spacing: 0) {
            HStack {
                Text("Review Changes")
                    .font(.headline)
                Spacer()
                Text("\(viewModel.commitSQL.count) statement(s) (parameterized)")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
            }
            .padding()

            Divider()

            ScrollView {
                VStack(alignment: .leading, spacing: 8) {
                    ForEach(Array(viewModel.commitSQL.enumerated()), id: \.offset) { _, statement in
                        Text(statement.sql + (statement.parameters.isEmpty ? "" : "\n-- params: \(statement.parameters.map(\.description).joined(separator: ", "))"))
                            .font(.system(size: 13, design: .monospaced))
                            .textSelection(.enabled)
                            .padding(8)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .background(Color(nsColor: .controlBackgroundColor))
                            .clipShape(RoundedRectangle(cornerRadius: 4))
                    }
                }
                .padding()
            }
            .frame(minHeight: 150, maxHeight: 300)

            if let error = viewModel.commitError {
                ScrollView {
                    HStack(alignment: .top, spacing: 8) {
                        Image(systemName: "exclamationmark.triangle.fill")
                            .foregroundStyle(.red)
                            .padding(.top, 2)
                        Text(error)
                            .font(.system(size: 12, design: .monospaced))
                            .foregroundStyle(.red)
                            .textSelection(.enabled)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                    .padding(.horizontal)
                    .padding(.vertical, 8)
                }
                .frame(maxHeight: 120)
            }

            Divider()

            HStack {
                Button("Copy SQL") {
                    let allSQL = viewModel.commitSQL.map { $0.sql }.joined(separator: "\n")
                    NSPasteboard.general.clearContents()
                    NSPasteboard.general.setString(allSQL, forType: .string)
                }
                .controlSize(.small)

                Spacer()

                Button("Cancel") { dismiss() }
                    .keyboardShortcut(.cancelAction)

                Button("Execute") {
                    Task { await viewModel.executeCommit() }
                }
                .buttonStyle(.borderedProminent)
                .keyboardShortcut(.defaultAction)
                .disabled(viewModel.isCommitting)
            }
            .padding()
        }
        .frame(width: 550)
    }
}

// MARK: - NSView Helper

private extension NSView {
    func findRowView() -> NSTableRowView? {
        var current: NSView? = superview
        while let view = current {
            if let rowView = view as? NSTableRowView {
                return rowView
            }
            current = view.superview
        }
        return nil
    }
}
