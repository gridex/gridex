// SidebarView.swift
// Gridex
//
// Sidebar: Items / Queries / History tabs, tree, bottom schema bar.

import SwiftUI

// MARK: - Sidebar Tab

private enum SidebarTab: String, CaseIterable {
    case items, queries, history

    var title: String { rawValue.capitalized }

    var iconName: String {
        switch self {
        case .items: return "folder"
        case .queries: return "doc.text"
        case .history: return "clock"
        }
    }
}

// MARK: - SidebarView

struct SidebarView: View {
    @EnvironmentObject private var appState: AppState
    @State private var searchText = ""
    @State private var activeTab: SidebarTab = .items
    @State private var selectedSchema = "public"
    @State private var showNewTableSheet = false
    @State private var deleteErrorMessage: String?

    var body: some View {
        VStack(spacing: 0) {
            tabBar
            Divider()

            switch activeTab {
            case .items:
                itemsTab
            case .queries:
                emptyTab(icon: "doc.text", label: "No Queries")
            case .history:
                QueryHistoryTab()
            }
        }
        .alert("Delete Failed", isPresented: Binding(
            get: { deleteErrorMessage != nil },
            set: { if !$0 { deleteErrorMessage = nil } }
        )) {
            Button("OK", role: .cancel) {}
        } message: {
            Text(deleteErrorMessage ?? "")
        }
    }

    // MARK: - Tab bar

    private var tabBar: some View {
        HStack(spacing: 1) {
            ForEach(SidebarTab.allCases, id: \.self) { tab in
                Button {
                    activeTab = tab
                } label: {
                    Image(systemName: tab.iconName)
                        .font(.system(size: 13))
                        .foregroundStyle(activeTab == tab ? Color.accentColor : Color.secondary)
                        .frame(maxWidth: .infinity)
                        .frame(height: 28)
                        .background(
                            activeTab == tab
                                ? Color.accentColor.opacity(0.12)
                                : Color.clear
                        )
                        .clipShape(RoundedRectangle(cornerRadius: 5))
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .help(tab.title)
            }
        }
        .padding(.horizontal, 6)
        .padding(.vertical, 4)
        .background(Color(NSColor.windowBackgroundColor))
    }

    // MARK: - Items tab

    private var itemsTab: some View {
        VStack(spacing: 0) {
            searchBar
            Divider()

            List {
                ForEach(filteredItems, id: \.id) { item in
                    SidebarItemRow(item: item, searchText: searchText)
                }
            }
            .listStyle(.plain)
            .scrollContentBackground(.hidden)
            .background(Color(NSColor.windowBackgroundColor))

            Divider()
            bottomBar
        }
        .onReceive(NotificationCenter.default.publisher(for: .commitChanges)) { _ in
            // Commit pending table deletions when the toolbar ✓ button is clicked.
            // Only handle if the current workspace focus is the sidebar (no data-grid tab with pending changes).
            guard !appState.pendingTableDeletions.isEmpty || !appState.pendingTableTruncations.isEmpty else { return }
            Task {
                await commitPendingTruncations()
                await commitPendingDeletions()
            }
        }
    }

    private func commitPendingTruncations() async {
        guard let adapter = appState.activeAdapter else { return }
        let d = adapter.databaseType.sqlDialect
        for name in appState.pendingTableTruncations {
            let quoted = d.quoteIdentifier(name)
            let sql: String
            if adapter.databaseType == .sqlite {
                sql = "DELETE FROM \(quoted)"
            } else {
                sql = "TRUNCATE TABLE \(quoted)"
            }
            _ = try? await adapter.executeRaw(sql: sql)
        }
        appState.pendingTableTruncations.removeAll()
        appState.refreshSidebar()
        NotificationCenter.default.post(name: .reloadData, object: nil)
    }

    private func commitPendingDeletions() async {
        guard let adapter = appState.activeAdapter else { return }
        let d = adapter.databaseType.sqlDialect
        let schemaName = "public"  // TODO: use selected schema

        // MySQL-specific: disable FK checks globally if any table has ignoreForeignKeys
        let shouldIgnoreFK = appState.pendingTableDeletions.values.contains { $0.ignoreForeignKeys }
        if shouldIgnoreFK && adapter.databaseType == .mysql {
            _ = try? await adapter.executeRaw(sql: "SET FOREIGN_KEY_CHECKS = 0")
        }

        var failedTables: [String: String] = [:]

        for pending in appState.pendingTableDeletions.values {
            let qualified: String
            if adapter.databaseType == .sqlite {
                qualified = d.quoteIdentifier(pending.tableName)
            } else {
                qualified = "\(d.quoteIdentifier(schemaName)).\(d.quoteIdentifier(pending.tableName))"
            }
            var sql = "DROP TABLE IF EXISTS \(qualified)"
            if pending.cascade && adapter.databaseType == .postgresql {
                sql += " CASCADE"
            }
            do {
                _ = try await adapter.executeRaw(sql: sql)
                // Close any open tabs for this table
                appState.tabs.removeAll { $0.tableName == pending.tableName && $0.type == .dataGrid }
                appState.pendingTableDeletions.removeValue(forKey: pending.tableName)
            } catch {
                failedTables[pending.tableName] = error.localizedDescription
            }
        }

        if shouldIgnoreFK && adapter.databaseType == .mysql {
            _ = try? await adapter.executeRaw(sql: "SET FOREIGN_KEY_CHECKS = 1")
        }

        if !failedTables.isEmpty {
            let details = failedTables.map { "\($0.key): \($0.value)" }.joined(separator: "\n")
            deleteErrorMessage = "Failed to delete:\n\(details)"
        }

        appState.refreshSidebar()
    }

    private var searchBar: some View {
        HStack(spacing: 5) {
            Image(systemName: "magnifyingglass")
                .font(.system(size: 11))
                .foregroundStyle(.tertiary)

            TextField("Search for item...", text: $searchText)
                .textFieldStyle(.plain)
                .font(.system(size: 12))

            if searchText.isEmpty {
                Button {
                } label: {
                    Image(systemName: "line.3.horizontal.decrease")
                        .font(.system(size: 11))
                        .foregroundStyle(.tertiary)
                }
                .buttonStyle(.plain)
            } else {
                Button { searchText = "" } label: {
                    Image(systemName: "xmark.circle.fill")
                        .font(.system(size: 11))
                        .foregroundStyle(.secondary)
                }
                .buttonStyle(.plain)
            }
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 6)
    }

    private var bottomBar: some View {
        HStack(spacing: 2) {
            // Backup
            Button { BackupRestorePanel.openBackup(appState: appState) } label: {
                Image(systemName: "arrow.up.doc")
                    .font(.system(size: 12))
                    .frame(width: 28, height: 22)
            }
            .buttonStyle(.plain)
            .help("Backup database…")

            // Restore
            Button { BackupRestorePanel.openRestore(appState: appState) } label: {
                Image(systemName: "arrow.down.doc")
                    .font(.system(size: 12))
                    .frame(width: 28, height: 22)
            }
            .buttonStyle(.plain)
            .help("Restore database…")

            Spacer()

            Picker("", selection: $selectedSchema) {
                Text("public").tag("public")
            }
            .labelsHidden()
            .pickerStyle(.menu)
            .font(.system(size: 12))
            .frame(width: 80)
        }
        .padding(.horizontal, 6)
        .padding(.vertical, 4)
    }

    // MARK: - Empty tabs

    private func emptyTab(icon: String, label: String) -> some View {
        VStack(spacing: 8) {
            Image(systemName: icon)
                .font(.system(size: 28))
                .foregroundStyle(.quaternary)
            Text(label)
                .font(.system(size: 12))
                .foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    // MARK: - Filtering

    private var filteredItems: [SidebarItem] {
        guard !searchText.isEmpty else { return appState.sidebarItems }
        return appState.sidebarItems.compactMap { filterItem($0, query: searchText.lowercased()) }
    }

    private func filterItem(_ item: SidebarItem, query: String) -> SidebarItem? {
        if item.children.isEmpty {
            return item.title.lowercased().contains(query) ? item : nil
        }
        let matched = item.children.compactMap { filterItem($0, query: query) }
        if matched.isEmpty && !item.title.lowercased().contains(query) { return nil }
        return SidebarItem(id: item.id, title: item.title, type: item.type, iconName: item.iconName,
                           children: matched.isEmpty ? item.children : matched, badge: item.badge)
    }
}

// MARK: - Sidebar Item Row

struct SidebarItemRow: View {
    let item: SidebarItem
    let searchText: String
    @EnvironmentObject private var appState: AppState
    @State private var isExpanded = true
    @State private var showTruncateConfirm = false
    @State private var showDeleteSheet = false
    @State private var showExportSheet = false
    @State private var showImportCSVSheet = false
    @State private var importSQLFile: IdentifiableURL?
    @State private var importResultMessage: String?

    var body: some View {
        if item.children.isEmpty {
            leafRow
        } else {
            groupRow
        }
    }

    // Leaf item (table, view, function)
    private var leafRow: some View {
        let isActive = appState.selectedSidebarItem == item.type
        let isPendingDelete: Bool = {
            if case .table(let name) = item.type {
                return appState.pendingTableDeletions[name] != nil
            }
            return false
        }()
        let isPendingTruncate: Bool = {
            if case .table(let name) = item.type {
                return appState.pendingTableTruncations.contains(name)
            }
            return false
        }()

        return HStack(spacing: 6) {
            tableIcon
            Text(item.title)
                .font(.system(size: 12))
                .lineLimit(1)
                .strikethrough(isPendingDelete)
                .foregroundStyle(
                    isPendingDelete ? Color.white.opacity(0.8)
                    : (isPendingTruncate || isActive) ? .white
                    : .primary
                )
            Spacer()
        }
        .padding(.vertical, 3)
        .padding(.horizontal, 6)
        .background(
            isPendingDelete
                ? Color.red.opacity(0.35)
                : isPendingTruncate
                    ? Color.orange.opacity(0.6)
                    : isActive
                        ? Color.accentColor
                        : Color.clear
        )
        .clipShape(RoundedRectangle(cornerRadius: 5))
        .tag(item.type)
        .contentShape(Rectangle())
        .pointerCursor()
        .onTapGesture {
            appState.selectedSidebarItem = item.type
            // Open table/view on single click
            switch item.type {
            case .table(let name):
                appState.openTable(name: name, schema: nil)
            case .view(let name):
                appState.openTable(name: name, schema: nil)
            case .function(let name):
                appState.openFunction(name: name, schema: nil)
            case .procedure(let name):
                appState.openProcedure(name: name, schema: nil)
            default:
                break
            }
        }
        .contextMenu { tableContextMenu }
        .alert("Truncate Table", isPresented: $showTruncateConfirm) {
            Button("Cancel", role: .cancel) {}
            Button("Truncate", role: .destructive) {
                if case .table(let name) = item.type {
                    appState.pendingTableTruncations.insert(name)
                }
            }
        } message: {
            if case .table(let name) = item.type {
                Text("Are you sure you want to remove all rows from \"\(name)\"? This cannot be undone.")
            }
        }
        .sheet(isPresented: $showDeleteSheet) {
            if case .table(let name) = item.type {
                DeleteTableSheet(tableName: name) { cascade, ignoreFK in
                    appState.pendingTableDeletions[name] = AppState.PendingTableDeletion(
                        tableName: name,
                        cascade: cascade,
                        ignoreForeignKeys: ignoreFK
                    )
                }
            }
        }
        .sheet(isPresented: $showExportSheet) {
            if case .table(let name) = item.type {
                ExportTableSheet(tableName: name)
            }
        }
        .sheet(isPresented: $showImportCSVSheet) {
            if case .table(let name) = item.type {
                ImportCSVSheet(tableName: name)
            }
        }
        .sheet(item: $importSQLFile, onDismiss: {
            // Refresh sidebar after the wizard closes so newly-imported tables appear
            appState.refreshSidebar()
        }) { wrapper in
            ImportSQLWizard(fileURL: wrapper.url) { content in
                await executeSQLDump(content: content, fileName: wrapper.url.lastPathComponent)
            }
        }
        .alert("Import Result", isPresented: Binding(
            get: { importResultMessage != nil },
            set: { if !$0 { importResultMessage = nil } }
        )) {
            Button("OK") { importResultMessage = nil }
        } message: {
            Text(importResultMessage ?? "")
        }
    }

    // Group row (Functions, Tables, Views)
    private var groupRow: some View {
        DisclosureGroup(isExpanded: $isExpanded) {
            ForEach(item.children, id: \.id) { child in
                SidebarItemRow(item: child, searchText: searchText)
            }
        } label: {
            HStack(spacing: 5) {
                Text(item.title)
                    .font(.system(size: 12, weight: .medium))
                    .foregroundStyle(.secondary)
                    .onTapGesture {
                        if case .group(let kind) = item.type, kind == "tables" {
                            appState.openTableList(schema: nil)
                        }
                    }
                Spacer()
                if let badge = item.badge {
                    Text(badge)
                        .font(.system(size: 10))
                        .foregroundStyle(.tertiary)
                        .padding(.trailing, 2)
                }
                // Inline "+" button for the Tables group to create a new table
                if case .group(let kind) = item.type, kind == "tables" {
                    Button {
                        appState.openCreateTable(schema: nil)
                    } label: {
                        Image(systemName: "plus")
                            .font(.system(size: 10, weight: .semibold))
                            .foregroundStyle(.secondary)
                            .frame(width: 18, height: 18)
                            .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .pointerCursor()
                    .help("New Table")
                }
            }
            .contentShape(Rectangle())
            .contextMenu { groupContextMenu }
        }
    }

    @ViewBuilder
    private var groupContextMenu: some View {
        if case .group(let kind) = item.type {
            switch kind {
            case "tables":
                Button("New Table…") {
                    appState.openCreateTable(schema: nil)
                }
                Button("Open Table List") {
                    appState.openTableList(schema: nil)
                }
                Button("ER Diagram") {
                    appState.openERDiagram(schema: nil)
                }
                Divider()
                Button("Refresh") { appState.refreshSidebar() }
            default:
                Button("Refresh") { appState.refreshSidebar() }
            }
        }
    }

    // Small grid icon for tables/views
    @ViewBuilder
    private var tableIcon: some View {
        switch item.type {
        case .table:
            Image(systemName: "tablecells")
                .font(.system(size: 11))
                .foregroundStyle(Color(red: 0.8, green: 0.5, blue: 0.2))
                .frame(width: 14)
        case .view:
            Image(systemName: "eye")
                .font(.system(size: 11))
                .foregroundStyle(.purple)
                .frame(width: 14)
        case .procedure:
            Image(systemName: "play.square")
                .font(.system(size: 11))
                .foregroundStyle(.green)
                .frame(width: 14)
        default:
            Image(systemName: "function")
                .font(.system(size: 11))
                .foregroundStyle(.blue)
                .frame(width: 14)
        }
    }

    // MARK: - Context Menu

    @ViewBuilder
    private var tableContextMenu: some View {
        if appState.activeConfig?.databaseType == .redis {
            // Redis-specific context menu
            if case .table(_) = item.type {
                Button("Browse Keys") {
                    appState.openTable(name: "Keys", schema: nil)
                }
                Button("Add Key…") {
                    appState.showRedisAddKey = true
                }
                Divider()
                Button("Server Info") { appState.openRedisServerInfo() }
                Button("Slow Log") { appState.openRedisSlowLog() }
                Divider()
                Button("Flush Database…") { appState.showFlushDBConfirm = true }
                Divider()
                Button("Refresh") { appState.refreshSidebar() }
            } else {
                Button("Refresh") { appState.refreshSidebar() }
            }
        } else if case .table(let name) = item.type {
            Button("Open in new tab") {
                appState.openTable(name: name, schema: nil)
            }

            Button("Open structure") {
                appState.openTableStructure(name: name, schema: nil)
            }

            Divider()

            Button("New Table…") {
                appState.openCreateTable(schema: nil)
            }

            Button("Copy name") {
                NSPasteboard.general.clearContents()
                NSPasteboard.general.setString(name, forType: .string)
            }

            Divider()

            Button("Export...") {
                showExportSheet = true
            }

            Menu("Import") {
                Button("From CSV…") {
                    DispatchQueue.main.async {
                        showImportCSVSheet = true
                    }
                }
                Button("From SQL Dump…") {
                    DispatchQueue.main.async {
                        pickSQLDumpFile()
                    }
                }
            }

            Divider()

            Menu("Copy Script As") {
                Button("CREATE TABLE") {
                    Task { await copyCreateScript(name) }
                }
                Button("SELECT") {
                    copyToClipboard("SELECT * FROM \(quoted(name)) LIMIT 100;")
                }
                Button("INSERT") {
                    Task { await copyInsertScript(name) }
                }
                Button("UPDATE") {
                    Task { await copyUpdateScript(name) }
                }
                Button("DELETE") {
                    copyToClipboard("DELETE FROM \(quoted(name)) WHERE <condition>;")
                }
            }

            Divider()

            if appState.pendingTableTruncations.contains(name) {
                Button("Undo Truncate") {
                    appState.pendingTableTruncations.remove(name)
                }
            } else {
                Button("Truncate...") {
                    showTruncateConfirm = true
                }
            }

            if appState.pendingTableDeletions[name] != nil {
                Button("Undo Delete") {
                    appState.pendingTableDeletions.removeValue(forKey: name)
                }
            } else {
                Button("Delete...") {
                    showDeleteSheet = true
                }
            }
        } else {
            Button("New Table…") {
                appState.openCreateTable(schema: nil)
            }
            Divider()
            Button("Refresh") { appState.refreshSidebar() }
        }
    }

    // MARK: - Context Menu Actions

    private func quoted(_ name: String) -> String {
        guard let adapter = appState.activeAdapter else { return "\"\(name)\"" }
        return adapter.databaseType.sqlDialect.quoteIdentifier(name)
    }

    private func copyToClipboard(_ text: String) {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(text, forType: .string)
    }

    private func copyCreateScript(_ name: String) async {
        guard let adapter = appState.activeAdapter else { return }
        if let desc = try? await adapter.describeTable(name: name, schema: nil) {
            let ddl = desc.toDDL(dialect: adapter.databaseType.sqlDialect)
            copyToClipboard(ddl)
        }
    }

    private func copyInsertScript(_ name: String) async {
        guard let adapter = appState.activeAdapter else { return }
        if let desc = try? await adapter.describeTable(name: name, schema: nil) {
            let cols = desc.columns.map { adapter.databaseType.sqlDialect.quoteIdentifier($0.name) }.joined(separator: ", ")
            let vals = desc.columns.map { _ in "?" }.joined(separator: ", ")
            copyToClipboard("INSERT INTO \(quoted(name)) (\(cols)) VALUES (\(vals));")
        }
    }

    private func copyUpdateScript(_ name: String) async {
        guard let adapter = appState.activeAdapter else { return }
        if let desc = try? await adapter.describeTable(name: name, schema: nil) {
            let sets = desc.columns.filter { !$0.isPrimaryKey }.map { "\(adapter.databaseType.sqlDialect.quoteIdentifier($0.name)) = ?" }.joined(separator: ", ")
            let pk = desc.columns.first(where: \.isPrimaryKey)
            let whereClause = pk.map { "\(adapter.databaseType.sqlDialect.quoteIdentifier($0.name)) = ?" } ?? "<condition>"
            copyToClipboard("UPDATE \(quoted(name)) SET \(sets) WHERE \(whereClause);")
        }
    }

    /// Show NSOpenPanel to pick a .sql file, then open the ImportSQLWizard
    /// with a preview before executing.
    private func pickSQLDumpFile() {
        let panel = NSOpenPanel()
        panel.title = "Import SQL Dump"
        panel.allowsMultipleSelection = false
        panel.canChooseDirectories = false
        panel.canChooseFiles = true
        panel.allowedContentTypes = [.data]
        if panel.runModal() == .OK, let url = panel.url {
            importSQLFile = IdentifiableURL(url: url)
        }
    }

    /// Execute the SQL content from the wizard (called after user clicks Import).
    /// Returns an ImportSQLResult so the wizard can display it inline.
    /// Does NOT call refreshSidebar() here — the wizard would re-render the
    /// parent and dismiss itself. The sidebar is refreshed when the wizard closes.
    private func executeSQLDump(content: String, fileName: String) async -> ImportSQLResult {
        guard let adapter = appState.activeAdapter else {
            return ImportSQLResult(success: 0, total: 0, firstError: "No active database connection")
        }

        let statements = Self.splitSQLStatements(content)

        var successCount = 0
        var firstError: String?
        for stmt in statements {
            do {
                _ = try await adapter.executeRaw(sql: stmt)
                successCount += 1
            } catch {
                if firstError == nil {
                    firstError = error.localizedDescription
                }
            }
        }

        return ImportSQLResult(success: successCount, total: statements.count, firstError: firstError)
    }

    /// Split a SQL dump into individual statements. Correctly handles:
    /// - `;` inside single-quoted strings
    /// - `;` inside double-quoted identifiers
    /// - `--` line comments
    /// - `/* ... */` block comments
    /// - escaped quotes `''` inside strings
    static func splitSQLStatements(_ sql: String) -> [String] {
        var result: [String] = []
        var current = ""
        var inSingle = false
        var inDouble = false
        var inLineComment = false
        var inBlockComment = false

        let chars = Array(sql)
        var i = 0
        while i < chars.count {
            let c = chars[i]

            if inLineComment {
                current.append(c)
                if c == "\n" { inLineComment = false }
                i += 1
                continue
            }
            if inBlockComment {
                current.append(c)
                if c == "*" && i + 1 < chars.count && chars[i + 1] == "/" {
                    current.append("/")
                    inBlockComment = false
                    i += 2
                    continue
                }
                i += 1
                continue
            }
            if inSingle {
                current.append(c)
                if c == "'" {
                    // Check for escaped ''
                    if i + 1 < chars.count && chars[i + 1] == "'" {
                        current.append("'")
                        i += 2
                        continue
                    }
                    inSingle = false
                }
                i += 1
                continue
            }
            if inDouble {
                current.append(c)
                if c == "\"" { inDouble = false }
                i += 1
                continue
            }

            // Not inside any literal
            if c == "-" && i + 1 < chars.count && chars[i + 1] == "-" {
                inLineComment = true
                current.append("--")
                i += 2
                continue
            }
            if c == "/" && i + 1 < chars.count && chars[i + 1] == "*" {
                inBlockComment = true
                current.append("/*")
                i += 2
                continue
            }
            if c == "'" {
                inSingle = true
                current.append(c)
                i += 1
                continue
            }
            if c == "\"" {
                inDouble = true
                current.append(c)
                i += 1
                continue
            }
            if c == ";" {
                let trimmed = current.trimmingCharacters(in: .whitespacesAndNewlines)
                if !trimmed.isEmpty {
                    result.append(trimmed)
                }
                current = ""
                i += 1
                continue
            }
            current.append(c)
            i += 1
        }

        let trailing = current.trimmingCharacters(in: .whitespacesAndNewlines)
        if !trailing.isEmpty {
            result.append(trailing)
        }

        return result
    }
}

// MARK: - New Table Sheet

struct NewTableSheet: View {
    let schema: String
    @EnvironmentObject private var appState: AppState
    @Environment(\.dismiss) private var dismiss

    @State private var tableName = ""
    @State private var encoding = "Default"
    @State private var collation = "Default"
    @State private var isCreating = false
    @State private var errorMessage: String?

    private let encodings = ["Default", "UTF8", "LATIN1", "SQL_ASCII"]
    private let collations = ["Default", "C", "POSIX", "en_US.UTF-8"]

    var body: some View {
        VStack(spacing: 0) {
            // Title
            Text("New Table")
                .font(.system(size: 13, weight: .semibold))
                .padding(.top, 16)
                .padding(.bottom, 12)

            // Form
            Grid(alignment: .trailing, horizontalSpacing: 10, verticalSpacing: 10) {
                GridRow {
                    Text("Name:").font(.system(size: 12))
                    TextField("", text: $tableName)
                        .textFieldStyle(.roundedBorder)
                        .frame(width: 220)
                }
                GridRow {
                    Text("Schema:").font(.system(size: 12))
                    Text(schema)
                        .font(.system(size: 12))
                        .foregroundStyle(.secondary)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
            }
            .padding(.horizontal, 20)

            if let errorMessage {
                Text(errorMessage)
                    .font(.system(size: 11))
                    .foregroundStyle(.red)
                    .padding(.horizontal, 20)
                    .padding(.top, 8)
            }

            Divider().padding(.top, 16)

            // Buttons
            HStack {
                Button("Cancel") { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Spacer()
                Button {
                    Task { await create() }
                } label: {
                    if isCreating {
                        ProgressView().controlSize(.small)
                    }
                    Text("OK")
                }
                .keyboardShortcut(.defaultAction)
                .disabled(tableName.isEmpty || isCreating)
            }
            .padding(.horizontal, 20)
            .padding(.vertical, 12)
        }
        .frame(width: 320)
    }

    private func create() async {
        guard let adapter = appState.activeAdapter else { return }
        isCreating = true
        errorMessage = nil
        defer { isCreating = false }

        let d = adapter.databaseType.sqlDialect
        let qualifiedTable = adapter.databaseType == .sqlite
            ? d.quoteIdentifier(tableName)
            : "\(d.quoteIdentifier(schema)).\(d.quoteIdentifier(tableName))"

        // Create table with a default id column
        let idType: String
        switch adapter.databaseType {
        case .postgresql: idType = "serial PRIMARY KEY"
        case .mysql: idType = "int AUTO_INCREMENT PRIMARY KEY"
        case .sqlite: idType = "INTEGER PRIMARY KEY AUTOINCREMENT"
        case .mssql: idType = "INT IDENTITY(1,1) PRIMARY KEY"
        case .clickhouse: idType = "UInt64) ENGINE = MergeTree ORDER BY (id" // NB: no FK/IDENTITY — MergeTree requires an ORDER BY
        case .redis, .mongodb: return // Non-SQL databases don't support CREATE TABLE
        }

        let sql = "CREATE TABLE \(qualifiedTable) (id \(idType))"
        do {
            _ = try await adapter.executeRaw(sql: sql)
        } catch {
            errorMessage = DataGridViewState.detailedErrorMessage(error)
            return
        }

        dismiss()
        appState.refreshSidebar()
        appState.openTable(name: tableName, schema: schema)
    }
}

// MARK: - Helper wrapper so we can use sheet(item:) with a URL

struct IdentifiableURL: Identifiable {
    let url: URL
    var id: String { url.absoluteString }
}
