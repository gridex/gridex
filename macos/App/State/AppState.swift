// AppState.swift
// Gridex
//
// Central application state for SwiftUI. Replaces AppCoordinator.

import SwiftUI
import Combine

@MainActor
final class AppState: ObservableObject {

    /// The AppState of the currently focused window. Updated by WindowRoot
    /// when a window appears or gains focus. Used as a fallback for menu commands.
    static weak var active: AppState?

    // MARK: - Dependencies

    /// Shared across all windows so SwiftData ModelContainer is a singleton.
    let container = DependencyContainer.shared

    // MARK: - Navigation State

    @Published var sidebarVisible = true
    @Published var detailsPanelVisible = true
    /// Persisted width of the details panel so toggling doesn't reset it.
    @Published var detailsPanelWidth: CGFloat = 320

    /// Tables the user has marked for deletion in the sidebar.
    /// They are NOT dropped until the user clicks the commit button in the sidebar header.
    /// Key is the table name (per active connection / schema).
    @Published var pendingTableDeletions: [String: PendingTableDeletion] = [:]
    @Published var pendingTableTruncations: Set<String> = []

    struct PendingTableDeletion {
        var tableName: String
        var cascade: Bool
        var ignoreForeignKeys: Bool
    }
    /// Chat messages per connection (keyed by connectionId). In-memory only.
    var aiChatMessages: [UUID: [ChatDisplayMessage]] = [:]
    @Published var aiPanelVisible = false
    @Published var showDBTypePicker = false
    @Published var showConnectionForm = false
    @Published var showSettings = false
    @Published var showDatabaseSwitcher = false
    @Published var showNewTableSheet = false
    @Published var selectedDBType: DatabaseType?
    @Published var selectedSidebarItem: SidebarItemType?

    // MARK: - Home State

    @Published var savedConnections: [ConnectionConfig] = []
    @Published var connectionSearchText = ""
    @Published var connectionGroups: Set<String> = [] {
        didSet {
            UserDefaults.standard.set(Array(connectionGroups), forKey: "connectionGroups")
        }
    }

    // MARK: - Connection State

    @Published var activeConnectionId: UUID?
    @Published var activeAdapter: (any DatabaseAdapter)?
    @Published var activeConfig: ConnectionConfig?
    @Published var sidebarItems: [SidebarItem] = []
    @Published var connectionTitle: String = "Gridex"
    @Published var isConnecting: Bool = false
    @Published var connectionError: String?
    @Published var serverVersion: String?
    @Published var sslInfo: String?

    // MARK: - Tab State

    @Published var tabs: [ContentTab] = []
    @Published var activeTabId: UUID? {
        didSet { syncSidebarFromActiveTab() }
    }

    // Cache DataGridViewState per tab to avoid reloading when switching tabs
    var dataGridCache: [UUID: DataGridViewState] = [:]

    func cachedDataGridState(for tabId: UUID) -> DataGridViewState {
        if let existing = dataGridCache[tabId] { return existing }
        let state = DataGridViewState()
        dataGridCache[tabId] = state
        return state
    }

    // Persist SQL editor text per tab so switching tabs doesn't lose work
    var queryEditorText: [UUID: String] = [:]

    // MARK: - Database List
    @Published var availableDatabases: [String] = []

    // MARK: - Status Bar

    @Published var statusConnection: String?
    @Published var statusSchema: String?
    @Published var statusRowCount: Int?
    @Published var statusQueryTime: TimeInterval?

    // MARK: - Redis State
    @Published var redisDBSize: Int?
    @Published var showFlushDBConfirm = false
    @Published var showRedisAddKey = false

    // MARK: - Query Log (global, shared across all tables)
    @Published var queryLog: [QueryLogEntry] = []
    @Published var showQueryLog: Bool = false

    /// Log a query to the in-memory SQL log panel. Used by all query sources
    /// (data grid loads, structure changes, user queries) for the bottom log panel.
    func logQuery(sql: String, duration: TimeInterval?) {
        let entry = QueryLogEntry(sql: sql, timestamp: Date(), duration: duration)
        queryLog.append(entry)
    }

    /// Counter that increments whenever a query is recorded to history.
    /// Observed by QueryHistoryTab to trigger reload.
    @Published var queryHistoryVersion: Int = 0

    /// Persist a user-executed SQL query to the sidebar History (SwiftData).
    /// Called ONLY from the SQL query editor — not from data grid loads,
    /// structure inspections, or internal DML. Survives app restarts.
    func recordQueryHistory(sql: String, duration: TimeInterval?, rowCount: Int? = nil, error: String? = nil) {
        guard let connectionId = activeConnectionId else { return }
        let database = currentDatabaseName ?? ""
        let historyEntry = QueryHistoryEntry(
            id: UUID(),
            connectionId: connectionId,
            database: database,
            sql: sql,
            executedAt: Date(),
            duration: duration ?? 0,
            rowCount: rowCount,
            status: error == nil ? .success : .error,
            errorMessage: error,
            isFavorite: false
        )
        let repo = container.queryHistoryRepository
        Task {
            try? await repo.save(entry: historyEntry)
            await MainActor.run { self.queryHistoryVersion += 1 }
        }
    }

    func clearQueryLog() {
        queryLog.removeAll()
    }

    // MARK: - Selected Row Details
    @Published var selectedRowDetails: [(column: String, value: String)]?
    var onDetailFieldEdit: ((_ colIndex: Int, _ newValue: String) -> Void)?

    // MARK: - Tab Types

    struct ContentTab: Identifiable {
        let id: UUID
        let type: TabState.TabType
        var title: String
        let tableName: String?
        let schema: String?
        var databaseName: String?
        var initialViewMode: String? // "data" or "structure"
    }

    struct TabGroup: Identifiable {
        let id: String              // keyed by databaseName
        let databaseName: String
        var displayName: String?
        var color: ColorTag
        var isCollapsed: Bool

        var label: String { displayName ?? databaseName }
    }

    // MARK: - Tab Group State

    @Published var tabGroups: [String: TabGroup] = [:]
    @Published var currentDatabaseName: String?

    private static let groupColors: [ColorTag] = [.blue, .green, .purple, .orange, .red, .gray]

    /// Whether tabs should display as groups (2+ databases open)
    var isMultiDatabase: Bool {
        Set(tabs.compactMap(\.databaseName)).count > 1
    }

    /// Tabs grouped by database. Returns flat list when single database.
    var groupedTabs: [(group: TabGroup?, tabs: [ContentTab])] {
        let uniqueDatabases = Set(tabs.compactMap(\.databaseName))

        if uniqueDatabases.count <= 1 {
            return [(group: nil, tabs: tabs)]
        }

        var seen: [String] = []
        var grouped: [String: [ContentTab]] = [:]
        for tab in tabs {
            let key = tab.databaseName ?? "Ungrouped"
            if !seen.contains(key) { seen.append(key) }
            grouped[key, default: []].append(tab)
        }

        return seen.map { key in
            let group = tabGroups[key] ?? TabGroup(
                id: key, databaseName: key, displayName: nil,
                color: autoColor(for: key), isCollapsed: false
            )
            return (group: group, tabs: grouped[key] ?? [])
        }
    }

    private func autoColor(for databaseName: String) -> ColorTag {
        let allNames = Set(tabs.compactMap(\.databaseName)).sorted()
        let index = allNames.firstIndex(of: databaseName) ?? 0
        return Self.groupColors[index % Self.groupColors.count]
    }

    func renameTab(id: UUID, newTitle: String) {
        guard let index = tabs.firstIndex(where: { $0.id == id }) else { return }
        tabs[index].title = newTitle
    }

    // MARK: - Connection

    func connect(config: ConnectionConfig, password: String, sshPassword: String? = nil) async {
        isConnecting = true
        connectionError = nil

        // If the caller didn't provide an SSH password but the config needs one,
        // try to load it from the Keychain.
        let effectiveSSHPassword: String? = {
            if let explicit = sshPassword, !explicit.isEmpty { return explicit }
            guard config.sshConfig != nil else { return nil }
            return try? container.keychainService.load(
                key: "ssh.password.\(config.id.uuidString)")
        }()

        do {
            // Race the connection attempt against a 20s timeout so the UI never
            // stays stuck "Connecting…" forever (e.g. when the server is unreachable
            // or the network stack blocks indefinitely).
            let connectionManager = container.connectionManager
            let connection = try await withThrowingTaskGroup(of: ActiveConnection.self) { group in
                group.addTask {
                    try await connectionManager.connect(
                        config: config, password: password, sshPassword: effectiveSSHPassword
                    )
                }
                group.addTask {
                    try await Task.sleep(nanoseconds: 10_000_000_000) // 10s
                    throw NSError(
                        domain: "Gridex.Connection",
                        code: -1001,
                        userInfo: [NSLocalizedDescriptionKey: "Connection timed out after 10 seconds. Check host, port, and network."]
                    )
                }
                guard let result = try await group.next() else {
                    throw NSError(domain: "Gridex.Connection", code: -1002,
                                  userInfo: [NSLocalizedDescriptionKey: "Connection failed unexpectedly."])
                }
                group.cancelAll()
                return result
            }
            activeConnectionId = config.id
            activeAdapter = connection.adapter
            activeConfig = config
            connectionTitle = "Gridex — \(config.name)"
            statusConnection = config.displayHost
            isConnecting = false

            // Load sidebar immediately (most important for user)
            await loadSidebar(config: config, adapter: connection.adapter)

            // Fetch metadata in background — all parallel
            Task { [weak self] in
                guard let self else { return }
                let adapter = connection.adapter

                await withTaskGroup(of: Void.self) { group in
                    // SSL info
                    group.addTask {
                        if config.sslEnabled {
                            if let sslResult = try? await adapter.executeRaw(sql: "SHOW ssl"),
                               let sslOn = sslResult.rows.first?.first?.stringValue, sslOn == "on" {
                                if let verResult = try? await adapter.executeRaw(sql: "SELECT ssl_version FROM pg_stat_ssl WHERE pid = pg_backend_pid()"),
                                   let tlsVer = verResult.rows.first?.first?.stringValue {
                                    await MainActor.run { self.sslInfo = tlsVer }
                                } else {
                                    await MainActor.run { self.sslInfo = "TLS" }
                                }
                            } else {
                                await MainActor.run { self.sslInfo = "SSL" }
                            }
                        }
                    }

                    // Server version
                    group.addTask {
                        if let fullVersion = try? await adapter.serverVersion() {
                            let parts = fullVersion.split(separator: " ")
                            if parts.count >= 2, let ver = parts.first(where: { $0.first?.isNumber == true }) {
                                await MainActor.run { self.serverVersion = String(ver).trimmingCharacters(in: .punctuationCharacters) }
                            } else {
                                await MainActor.run { self.serverVersion = fullVersion }
                            }
                        }
                    }

                    // Current database + available databases
                    group.addTask {
                        let dbName = try? await adapter.currentDatabase()
                        let databases = (try? await adapter.listDatabases()) ?? []
                        await MainActor.run {
                            self.currentDatabaseName = dbName ?? config.database ?? config.name
                            self.availableDatabases = databases
                        }
                    }

                    // Redis DBSIZE
                    if config.databaseType == .redis, let redis = adapter as? RedisAdapter {
                        group.addTask {
                            let size = try? await redis.dbSize()
                            await MainActor.run { self.redisDBSize = size }
                        }
                    }
                }
            }
        } catch {
            isConnecting = false
            connectionError = error.localizedDescription
            print("Connection failed: \(error)")
        }
    }

    func loadSidebar(config: ConnectionConfig, adapter: any DatabaseAdapter) async {
        do {
            // Run all queries in parallel instead of sequential
            async let tablesTask = adapter.listTables(schema: nil)
            async let viewsTask = adapter.listViews(schema: nil)
            async let functionsTask = adapter.listFunctions(schema: nil)
            async let proceduresTask = adapter.listProcedures(schema: nil)

            let tables = try await tablesTask
            let views = try await viewsTask
            let functions = try await functionsTask
            let procedures = (try? await proceduresTask) ?? []

            let tableItems = tables.map { t in
                SidebarItem(title: t.name, type: .table(t.name), iconName: "")
            }
            let viewItems = views.map { v in
                SidebarItem(title: v.name, type: .view(v.name), iconName: "")
            }
            let functionItems = functions.map { f in
                SidebarItem(title: f, type: .function(f), iconName: "")
            }
            let procedureItems = procedures.map { p in
                SidebarItem(title: p, type: .procedure(p), iconName: "")
            }

            var items: [SidebarItem] = []

            if !functionItems.isEmpty {
                items.append(SidebarItem(title: "Functions", type: .group("functions"), iconName: "", children: functionItems))
            }

            if !procedureItems.isEmpty {
                items.append(SidebarItem(title: "Procedures", type: .group("procedures"), iconName: "", children: procedureItems))
            }

            items.append(SidebarItem(title: "Tables", type: .group("tables"), iconName: "", children: tableItems))

            if !viewItems.isEmpty {
                items.append(SidebarItem(title: "Views", type: .group("views"), iconName: "", children: viewItems))
            }

            sidebarItems = items
        } catch {
            print("Sidebar load error: \(error)")
        }
    }

    // MARK: - Tab Management

    private func syncSidebarFromActiveTab() {
        guard let activeId = activeTabId,
              let tab = tabs.first(where: { $0.id == activeId }),
              let name = tab.tableName else { return }
        switch tab.type {
        case .functionDetail:
            if tab.initialViewMode == "procedure" {
                selectedSidebarItem = .procedure(name)
            } else {
                selectedSidebarItem = .function(name)
            }
        default:
            selectedSidebarItem = .table(name)
        }
    }

    func openTable(name: String, schema: String?) {
        if let existing = tabs.first(where: { $0.type == .dataGrid && $0.tableName == name && $0.schema == schema }) {
            activeTabId = existing.id
            return
        }
        let tab = ContentTab(id: UUID(), type: .dataGrid, title: name, tableName: name, schema: schema, databaseName: currentDatabaseName)
        tabs.append(tab)
        activeTabId = tab.id
        if let db = currentDatabaseName { ensureTabGroup(for: db) }
    }

    func openTableStructure(name: String, schema: String?) {
        if let existing = tabs.first(where: { $0.type == .dataGrid && $0.tableName == name && $0.schema == schema }) {
            activeTabId = existing.id
        } else {
            let tab = ContentTab(id: UUID(), type: .dataGrid, title: name, tableName: name, schema: schema, databaseName: currentDatabaseName, initialViewMode: "structure")
            tabs.append(tab)
            activeTabId = tab.id
            if let db = currentDatabaseName { ensureTabGroup(for: db) }
        }
        // Post notification for already-open tabs to switch mode
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) {
            NotificationCenter.default.post(name: .init("switchToStructure"), object: name)
        }
    }

    func openTableWithFilter(name: String, schema: String?, filterColumn: String, filterValue: String) {
        // Always open a new tab (don't reuse existing — different filter context)
        let tab = ContentTab(id: UUID(), type: .dataGrid, title: name, tableName: name, schema: schema, databaseName: currentDatabaseName)
        tabs.append(tab)
        activeTabId = tab.id
        if let db = currentDatabaseName { ensureTabGroup(for: db) }

        // Pre-configure the cached DataGridViewState with filter
        let state = cachedDataGridState(for: tab.id)
        let filter = FilterExpression(
            conditions: [FilterCondition(column: filterColumn, op: .equal, value: .string(filterValue))],
            combinator: .and
        )
        state.activeFilter = filter
        state.showFilterBar = true
    }

    func openTableList(schema: String?) {
        let schemaName = schema ?? "public"
        let title = "Tables.\(schemaName)"
        if let existing = tabs.first(where: { $0.type == .tableList && $0.schema == schema }) {
            activeTabId = existing.id
            return
        }
        let tab = ContentTab(id: UUID(), type: .tableList, title: title, tableName: nil, schema: schema, databaseName: currentDatabaseName)
        tabs.append(tab)
        activeTabId = tab.id
        if let db = currentDatabaseName { ensureTabGroup(for: db) }
    }

    func openFunction(name: String, schema: String?) {
        if let existing = tabs.first(where: { $0.type == .functionDetail && $0.tableName == name && $0.schema == schema && $0.initialViewMode != "procedure" }) {
            activeTabId = existing.id
            return
        }
        let tab = ContentTab(id: UUID(), type: .functionDetail, title: name, tableName: name, schema: schema, databaseName: currentDatabaseName)
        tabs.append(tab)
        activeTabId = tab.id
        if let db = currentDatabaseName { ensureTabGroup(for: db) }
    }

    func openProcedure(name: String, schema: String?) {
        if let existing = tabs.first(where: { $0.type == .functionDetail && $0.tableName == name && $0.schema == schema && $0.initialViewMode == "procedure" }) {
            activeTabId = existing.id
            return
        }
        let tab = ContentTab(id: UUID(), type: .functionDetail, title: name, tableName: name, schema: schema, databaseName: currentDatabaseName, initialViewMode: "procedure")
        tabs.append(tab)
        activeTabId = tab.id
        if let db = currentDatabaseName { ensureTabGroup(for: db) }
    }

    func openCreateTable(schema: String?) {
        let tab = ContentTab(id: UUID(), type: .createTable, title: "New Table", tableName: nil, schema: schema, databaseName: currentDatabaseName)
        tabs.append(tab)
        activeTabId = tab.id
        if let db = currentDatabaseName { ensureTabGroup(for: db) }
    }

    func openERDiagram(schema: String?) {
        let title = "ER Diagram"
        if let existing = tabs.first(where: { $0.type == .erDiagram && $0.schema == schema }) {
            activeTabId = existing.id
            return
        }
        let tab = ContentTab(id: UUID(), type: .erDiagram, title: title, tableName: nil, schema: schema, databaseName: currentDatabaseName)
        tabs.append(tab)
        activeTabId = tab.id
        if let db = currentDatabaseName { ensureTabGroup(for: db) }
    }

    // MARK: - Redis Tabs

    func openRedisKeyDetail(key: String) {
        if let existing = tabs.first(where: { $0.type == .redisKeyDetail && $0.tableName == key }) {
            activeTabId = existing.id
            return
        }
        let tab = ContentTab(id: UUID(), type: .redisKeyDetail, title: key, tableName: key, schema: nil, databaseName: currentDatabaseName)
        tabs.append(tab)
        activeTabId = tab.id
        if let db = currentDatabaseName { ensureTabGroup(for: db) }
    }

    func openRedisServerInfo() {
        if let existing = tabs.first(where: { $0.type == .redisServerInfo }) {
            activeTabId = existing.id
            return
        }
        let tab = ContentTab(id: UUID(), type: .redisServerInfo, title: "Server Info", tableName: nil, schema: nil, databaseName: currentDatabaseName)
        tabs.append(tab)
        activeTabId = tab.id
        if let db = currentDatabaseName { ensureTabGroup(for: db) }
    }

    func openRedisSlowLog() {
        if let existing = tabs.first(where: { $0.type == .redisSlowLog }) {
            activeTabId = existing.id
            return
        }
        let tab = ContentTab(id: UUID(), type: .redisSlowLog, title: "Slow Log", tableName: nil, schema: nil, databaseName: currentDatabaseName)
        tabs.append(tab)
        activeTabId = tab.id
        if let db = currentDatabaseName { ensureTabGroup(for: db) }
    }

    func openNewQueryTab() {
        let number = tabs.filter { $0.type == .queryEditor }.count + 1
        let tab = ContentTab(id: UUID(), type: .queryEditor, title: "Query \(number)", tableName: nil, schema: nil, databaseName: currentDatabaseName)
        tabs.append(tab)
        activeTabId = tab.id
        if let db = currentDatabaseName { ensureTabGroup(for: db) }
    }

    func closeTab(id: UUID) {
        tabs.removeAll { $0.id == id }
        if let state = dataGridCache.removeValue(forKey: id) {
            state.rows = []
            state.displayCache = []
            state.columns = []
        }
        if activeTabId == id {
            activeTabId = tabs.last?.id
        }
    }

    func closeActiveTab() {
        guard let id = activeTabId else { return }
        closeTab(id: id)
    }

    func closeOtherTabs(except id: UUID) {
        let closedIds = tabs.filter { $0.id != id }.map(\.id)
        tabs.removeAll { $0.id != id }
        for cid in closedIds {
            if let state = dataGridCache.removeValue(forKey: cid) {
                state.rows = []; state.displayCache = []; state.columns = []
            }
        }
        activeTabId = id
    }

    func closeTabsToTheRight(of id: UUID) {
        guard let index = tabs.firstIndex(where: { $0.id == id }) else { return }
        let closedIds = tabs[(index + 1)...].map(\.id)
        tabs.removeSubrange((index + 1)...)
        for cid in closedIds {
            if let state = dataGridCache.removeValue(forKey: cid) {
                state.rows = []; state.displayCache = []; state.columns = []
            }
        }
        if let activeId = activeTabId, !tabs.contains(where: { $0.id == activeId }) {
            activeTabId = id
        }
    }

    func closeAllTabs() {
        tabs.removeAll()
        for state in dataGridCache.values {
            state.rows = []; state.displayCache = []; state.columns = []
        }
        dataGridCache.removeAll()
        queryEditorText.removeAll()
        activeTabId = nil
    }

    /// Switch to a different database on the same connection.
    /// PostgreSQL requires a full reconnect; MySQL/SQLite can use USE.
    func switchDatabase(_ databaseName: String) async {
        guard let adapter = activeAdapter, var config = activeConfig else { return }

        do {
            switch config.databaseType {
            case .mysql:
                // MySQL supports USE to switch database in-place
                _ = try await adapter.executeRaw(sql: "USE `\(databaseName)`")
                currentDatabaseName = databaseName

            case .mssql:
                // SQL Server supports USE to switch database in-place
                _ = try await adapter.executeRaw(sql: "USE [\(databaseName.replacingOccurrences(of: "]", with: "]]"))]")
                currentDatabaseName = databaseName

            case .clickhouse:
                // ClickHouse HTTP is stateless — adapter intercepts USE and updates its default DB.
                _ = try await adapter.executeRaw(sql: "USE `\(databaseName.replacingOccurrences(of: "`", with: "``"))`")
                currentDatabaseName = databaseName

            case .postgresql:
                // PostgreSQL: each connection is tied to one database — must reconnect
                try await adapter.disconnect()
                config.database = databaseName
                let pw = (try? container.keychainService.load(key: "db.password.\(config.id.uuidString)")) ?? ""
                let sshPw = config.sshConfig != nil
                    ? (try? container.keychainService.load(key: "ssh.password.\(config.id.uuidString)"))
                    : nil
                let connection = try await container.connectionManager.connect(config: config, password: pw, sshPassword: sshPw ?? nil)
                activeAdapter = connection.adapter
                activeConfig = config
                currentDatabaseName = databaseName

            case .redis:
                // Redis: SELECT <db_number>
                let dbNum = databaseName.replacingOccurrences(of: "db", with: "")
                _ = try await adapter.executeRaw(sql: "SELECT \(dbNum)")
                currentDatabaseName = databaseName

            case .mongodb:
                // MongoDB: reconnect to the new database
                try await adapter.disconnect()
                config.database = databaseName
                let pw = (try? container.keychainService.load(key: "db.password.\(config.id.uuidString)")) ?? ""
                let sshPw = config.sshConfig != nil
                    ? (try? container.keychainService.load(key: "ssh.password.\(config.id.uuidString)"))
                    : nil
                let connection = try await container.connectionManager.connect(config: config, password: pw, sshPassword: sshPw ?? nil)
                activeAdapter = connection.adapter
                activeConfig = config
                currentDatabaseName = databaseName

            default:
                currentDatabaseName = databaseName
            }

            ensureTabGroup(for: databaseName)

            // Reload sidebar for the new database
            if let cfg = activeConfig, let adp = activeAdapter {
                await loadSidebar(config: cfg, adapter: adp)
            }
        } catch {
            print("Switch database failed: \(error)")
            connectionError = "Failed to switch to \(databaseName): \(error.localizedDescription)"
        }
    }

    // MARK: - Tab Group Management

    func ensureTabGroup(for databaseName: String) {
        guard tabGroups[databaseName] == nil else { return }
        tabGroups[databaseName] = TabGroup(
            id: databaseName, databaseName: databaseName, displayName: nil,
            color: autoColor(for: databaseName), isCollapsed: false
        )
    }

    func toggleGroupCollapsed(_ groupId: String) {
        tabGroups[groupId]?.isCollapsed.toggle()
    }

    func closeGroup(_ groupId: String) {
        let idsToRemove = Set(tabs.filter { $0.databaseName == groupId }.map(\.id))
        tabs.removeAll { idsToRemove.contains($0.id) }
        tabGroups.removeValue(forKey: groupId)
        if let activeId = activeTabId, idsToRemove.contains(activeId) {
            activeTabId = tabs.last?.id
        }
    }

    func renameGroup(_ groupId: String, newName: String) {
        tabGroups[groupId]?.displayName = newName
    }

    func changeGroupColor(_ groupId: String, color: ColorTag) {
        tabGroups[groupId]?.color = color
    }

    func disconnect() {
        if let adapter = activeAdapter {
            Task { try? await adapter.disconnect() }
        }
        if let connId = activeConnectionId {
            aiChatMessages.removeValue(forKey: connId)
        }
        activeAdapter = nil
        activeConfig = nil
        activeConnectionId = nil
        tabs.removeAll()
        activeTabId = nil
        tabGroups.removeAll()
        currentDatabaseName = nil
        availableDatabases.removeAll()
        sidebarItems.removeAll()
        selectedSidebarItem = nil
        selectedRowDetails = nil
        onDetailFieldEdit = nil
        serverVersion = nil
        sslInfo = nil
        statusConnection = nil
        statusSchema = nil
        statusRowCount = nil
        statusQueryTime = nil
        connectionTitle = "Gridex"
        redisDBSize = nil
    }

    func refreshSidebar() {
        guard let adapter = activeAdapter, let config = activeConfig else { return }
        Task { await loadSidebar(config: config, adapter: adapter) }
    }

    /// Re-fetch the database list from the active adapter and publish it.
    /// Call after CREATE DATABASE / DROP DATABASE so pickers and switchers update
    /// without waiting for the user to reconnect.
    func refreshAvailableDatabases() async {
        guard let adapter = activeAdapter else { return }
        do {
            let databases = try await adapter.listDatabases()
            availableDatabases = databases
        } catch {
            print("refreshAvailableDatabases failed: \(error)")
        }
    }

    func loadSavedConnections() async {
        do {
            savedConnections = try await container.connectionRepository.fetchAll()
            let stored = Set(UserDefaults.standard.stringArray(forKey: "connectionGroups") ?? [])
            let fromConnections = Set(savedConnections.compactMap { $0.group })
            connectionGroups = stored.union(fromConnections)
        } catch {
            print("Failed to load saved connections: \(error)")
        }
    }
}
