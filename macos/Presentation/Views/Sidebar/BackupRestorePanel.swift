// BackupRestorePanel.swift
// Gridex
//
// Native NSPanel for database backup and restore operations.
// Layout: connections | databases | options.

import SwiftUI
import AppKit

// MARK: - Panel Controller

@MainActor
final class BackupRestorePanel {
    private static var current: NSPanel?

    /// Open from sidebar (already connected — pre-selects active connection + database)
    static func openBackup(appState: AppState) {
        open(mode: .backup, appState: appState)
    }

    static func openRestore(appState: AppState) {
        open(mode: .restore, appState: appState)
    }

    /// Open from Home screen (no connection yet — user picks from saved list)
    static func openBackupWizard(appState: AppState) {
        open(mode: .backup, appState: appState)
    }

    static func openRestoreWizard(appState: AppState) {
        open(mode: .restore, appState: appState)
    }

    private static func open(mode: BackupRestoreMode, appState: AppState) {
        current?.close()
        current = nil

        let view = BackupRestoreView(
            mode: mode,
            onClose: { BackupRestorePanel.close() }
        )
        .environmentObject(appState)

        let controller = NSHostingController(rootView: view)

        let panel = NSPanel(
            contentRect: NSRect(x: 0, y: 0, width: 780, height: 520),
            styleMask: [.titled, .closable, .resizable],
            backing: .buffered,
            defer: false
        )
        panel.title = mode == .backup ? "Backup database" : "Restore database"
        panel.contentViewController = controller
        panel.layoutIfNeeded()

        let parentWindow = NSApp.mainWindow
            ?? NSApp.windows.first(where: { $0.canBecomeMain && !($0 is NSPanel) && $0.isVisible })
        if let parent = parentWindow {
            let pf = parent.frame
            let ps = panel.frame.size
            panel.setFrameOrigin(NSPoint(x: pf.midX - ps.width / 2, y: pf.midY - ps.height / 2))
        } else {
            panel.center()
        }

        panel.makeKeyAndOrderFront(nil)
        current = panel
    }

    static func close() {
        current?.close()
        current = nil
    }
}

// MARK: - Mode

enum BackupRestoreMode {
    case backup, restore
}

// MARK: - View

struct BackupRestoreView: View {
    let mode: BackupRestoreMode
    let onClose: () -> Void

    @EnvironmentObject private var appState: AppState

    // Connection selection
    @State private var selectedConnectionId: UUID?
    @State private var connectionSearchText = ""

    // Database selection
    @State private var selectedDatabase: String?
    @State private var databaseSearchText = ""
    @State private var availableDatabases: [String] = []
    @State private var isLoadingDatabases = false

    // The temporary adapter used to list databases (when not already connected)
    @State private var tempAdapter: (any DatabaseAdapter)?

    // Backup options
    @State private var selectedFormat: BackupFormat = .sql
    @State private var compress = true
    @State private var dataOnly = false
    @State private var schemaOnly = false
    @State private var fileName = ""

    // Restore
    @State private var restoreFileURL: URL?
    @State private var restoreFormat: BackupFormat = .sql

    // State
    @State private var isRunning = false
    @State private var result: BackupResult?
    @State private var restoreProgress: Double = 0
    @State private var backupBytesWritten: Int64 = 0
    @State private var backupElapsed: TimeInterval = 0

    private let backupService = BackupService()

    private var selectedConfig: ConnectionConfig? {
        appState.savedConnections.first(where: { $0.id == selectedConnectionId })
    }

    var body: some View {
        VStack(spacing: 0) {
            // File name header (backup only)
            if mode == .backup {
                fileNameHeader
                    .padding(.horizontal, 16)
                    .padding(.vertical, 10)
                Divider()
            }

            // Main 3-column layout
            HStack(spacing: 0) {
                // Left: Connection list
                connectionListColumn
                    .frame(width: 220)

                Divider()

                // Center: Database list
                databaseListColumn
                    .frame(width: 200)

                Divider()

                // Right: Options
                optionsColumn
            }

            Divider()

            // Result + action button
            footerBar
                .padding(.horizontal, 16)
                .padding(.vertical, 10)
        }
        .frame(minWidth: 700, minHeight: 440)
        .onAppear {
            // Pre-select active connection if connected
            if let activeConfig = appState.activeConfig {
                selectedConnectionId = activeConfig.id
                selectedDatabase = appState.currentDatabaseName ?? activeConfig.database
                let available = BackupFormat.available(for: activeConfig.databaseType)
                selectedFormat = available.first ?? .sql
                restoreFormat = available.first ?? .sql
                if let adapter = appState.activeAdapter {
                    loadDatabases(adapter: adapter, config: activeConfig)
                }
            } else if let first = appState.savedConnections.first {
                selectedConnectionId = first.id
                let available = BackupFormat.available(for: first.databaseType)
                selectedFormat = available.first ?? .sql
                restoreFormat = available.first ?? .sql
                connectAndLoadDatabases(first)
            }
            updateFileName()
        }
    }

    // MARK: - File Name Header

    private var fileNameHeader: some View {
        HStack(spacing: 8) {
            Text("File name:")
                .font(.system(size: 12))
                .foregroundStyle(.secondary)
            Text(fileName.isEmpty ? "untitled" : fileName)
                .font(.system(size: 12, weight: .medium))
            Spacer()
            Text("Click to change the file name pattern")
                .font(.system(size: 10))
                .foregroundStyle(.tertiary)
        }
    }

    // MARK: - Connection List Column

    private var connectionListColumn: some View {
        VStack(spacing: 0) {
            // Search
            searchField(text: $connectionSearchText, placeholder: "Search for connection…")
                .padding(8)

            Divider()

            // Grouped connection list
            ScrollView {
                LazyVStack(spacing: 0) {
                    ForEach(filteredConnectionGroups, id: \.name) { group in
                        connectionGroupRow(group)
                    }
                }
            }
        }
    }

    private struct ConnectionGroup: Identifiable {
        var id: String { name }
        let name: String
        let connections: [ConnectionConfig]
    }

    private var filteredConnectionGroups: [ConnectionGroup] {
        let conns = connectionSearchText.isEmpty
            ? appState.savedConnections
            : appState.savedConnections.filter { $0.name.localizedCaseInsensitiveContains(connectionSearchText) }

        var groups: [String: [ConnectionConfig]] = [:]
        for c in conns {
            let g = c.group ?? "Default"
            groups[g, default: []].append(c)
        }
        return groups.sorted(by: { $0.key < $1.key }).map { ConnectionGroup(name: $0.key, connections: $0.value) }
    }

    private func connectionGroupRow(_ group: ConnectionGroup) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            // Group header
            HStack(spacing: 6) {
                Image(systemName: "bolt.fill")
                    .font(.system(size: 10))
                    .foregroundColor(.orange)
                Text(group.name)
                    .font(.system(size: 11, weight: .semibold))
                    .foregroundStyle(.secondary)
                Text("\(group.connections.count)")
                    .font(.system(size: 9, weight: .medium))
                    .foregroundStyle(.tertiary)
                Spacer()
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 6)

            // Connections
            ForEach(group.connections, id: \.id) { conn in
                connectionRow(conn)
            }
        }
    }

    private func connectionRow(_ conn: ConnectionConfig) -> some View {
        let isSelected = selectedConnectionId == conn.id
        return Button {
            selectedConnectionId = conn.id
            selectedDatabase = nil
            // Reset formats to the first available for this DB type
            let available = BackupFormat.available(for: conn.databaseType)
            selectedFormat = available.first ?? .sql
            restoreFormat = available.first ?? .sql
            connectAndLoadDatabases(conn)
        } label: {
            HStack(spacing: 8) {
                // DB type badge
                ZStack {
                    RoundedRectangle(cornerRadius: 5)
                        .fill(dbColor(conn.databaseType).opacity(0.2))
                        .frame(width: 28, height: 28)
                    Text(dbInitials(conn.databaseType))
                        .font(.system(size: 10, weight: .bold, design: .rounded))
                        .foregroundColor(dbColor(conn.databaseType))
                }

                VStack(alignment: .leading, spacing: 1) {
                    Text(conn.name)
                        .font(.system(size: 12, weight: isSelected ? .semibold : .regular))
                        .foregroundColor(isSelected ? .white : .primary)
                        .lineLimit(1)
                    Text(conn.displayHost)
                        .font(.system(size: 10))
                        .foregroundColor(isSelected ? .white.opacity(0.7) : .secondary)
                        .lineLimit(1)
                }
                Spacer()
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 5)
            .background(isSelected ? Color.accentColor : Color.clear)
            .clipShape(RoundedRectangle(cornerRadius: 6))
        }
        .buttonStyle(.plain)
        .padding(.horizontal, 4)
    }

    // MARK: - Database List Column

    private var databaseListColumn: some View {
        VStack(spacing: 0) {
            searchField(text: $databaseSearchText, placeholder: "Search for database…")
                .padding(8)

            Divider()

            if isLoadingDatabases {
                VStack {
                    Spacer()
                    ProgressView()
                        .scaleEffect(0.7)
                    Text("Loading…")
                        .font(.system(size: 11))
                        .foregroundStyle(.secondary)
                    Spacer()
                }
            } else if availableDatabases.isEmpty {
                VStack {
                    Spacer()
                    Text("Select a connection")
                        .font(.system(size: 11))
                        .foregroundStyle(.tertiary)
                    Spacer()
                }
            } else {
                ScrollView {
                    LazyVStack(spacing: 0) {
                        ForEach(filteredDatabases, id: \.self) { db in
                            databaseRow(db)
                        }
                    }
                    .padding(.vertical, 4)
                }
            }
        }
    }

    private var filteredDatabases: [String] {
        if databaseSearchText.isEmpty { return availableDatabases }
        return availableDatabases.filter { $0.localizedCaseInsensitiveContains(databaseSearchText) }
    }

    private func databaseRow(_ name: String) -> some View {
        let isSelected = selectedDatabase == name
        return Button {
            selectedDatabase = name
            updateFileName()
        } label: {
            HStack(spacing: 8) {
                Image(systemName: "cylinder.fill")
                    .font(.system(size: 12))
                    .foregroundColor(isSelected ? .white : .blue.opacity(0.6))
                Text(name)
                    .font(.system(size: 12))
                    .foregroundColor(isSelected ? .white : .primary)
                    .lineLimit(1)
                Spacer()
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 5)
            .background(isSelected ? Color.accentColor : Color.clear)
            .clipShape(RoundedRectangle(cornerRadius: 6))
        }
        .buttonStyle(.plain)
        .padding(.horizontal, 4)
    }

    // MARK: - Options Column

    private var optionsColumn: some View {
        VStack(alignment: .leading, spacing: 0) {
            if mode == .backup {
                backupOptionsView
            } else {
                restoreOptionsView
            }

            Spacer()
        }
        .padding(12)
    }

    private var backupOptionsView: some View {
        VStack(alignment: .leading, spacing: 12) {
            // DB version label
            if let config = selectedConfig {
                HStack {
                    Spacer()
                    Text(config.databaseType.displayName)
                        .font(.system(size: 11, weight: .medium))
                        .foregroundStyle(.secondary)
                        .padding(.horizontal, 10)
                        .padding(.vertical, 4)
                        .background(Color.secondary.opacity(0.1))
                        .clipShape(RoundedRectangle(cornerRadius: 6))
                }
            }

            Divider()

            // Format
            Text("Format")
                .font(.system(size: 11, weight: .medium))
                .foregroundStyle(.secondary)

            if let config = selectedConfig {
                Picker("", selection: $selectedFormat) {
                    ForEach(BackupFormat.available(for: config.databaseType), id: \.self) { fmt in
                        Text("--format=\(fmt.rawValue)").tag(fmt)
                    }
                }
                .labelsHidden()
                .pickerStyle(.menu)
            }

            Divider()

            // Content
            Text("Content")
                .font(.system(size: 11, weight: .medium))
                .foregroundStyle(.secondary)

            VStack(alignment: .leading, spacing: 6) {
                radioButton("Full (schema + data)", isSelected: !dataOnly && !schemaOnly) {
                    dataOnly = false; schemaOnly = false
                }
                radioButton("Schema only", isSelected: schemaOnly) {
                    schemaOnly = true; dataOnly = false
                }
                radioButton("Data only", isSelected: dataOnly) {
                    dataOnly = true; schemaOnly = false
                }
            }

            if selectedFormat == .custom {
                Divider()
                Toggle("Compress file using Gzip", isOn: $compress)
                    .toggleStyle(.checkbox)
                    .font(.system(size: 12))
            }
        }
    }

    private var restoreOptionsView: some View {
        VStack(alignment: .leading, spacing: 12) {
            // File picker
            Text("Backup file")
                .font(.system(size: 11, weight: .medium))
                .foregroundStyle(.secondary)

            Button { pickRestoreFile() } label: {
                HStack(spacing: 6) {
                    Image(systemName: "doc.fill")
                        .font(.system(size: 11))
                        .foregroundStyle(.secondary)
                    if let url = restoreFileURL {
                        Text(url.lastPathComponent)
                            .font(.system(size: 11))
                            .lineLimit(1)
                            .truncationMode(.middle)
                    } else {
                        Text("Browse…")
                            .font(.system(size: 11))
                            .foregroundStyle(.secondary)
                    }
                }
                .padding(.horizontal, 8)
                .padding(.vertical, 5)
                .frame(maxWidth: .infinity, alignment: .leading)
                .background(Color.primary.opacity(0.04))
                .clipShape(RoundedRectangle(cornerRadius: 6))
                .overlay(
                    RoundedRectangle(cornerRadius: 6)
                        .stroke(Color.primary.opacity(0.08), lineWidth: 1)
                )
            }
            .buttonStyle(.plain)

            if let url = restoreFileURL {
                Text(fileSizeString(url))
                    .font(.system(size: 10, design: .monospaced))
                    .foregroundStyle(.tertiary)
            }

            Divider()

            // Format
            Text("Format")
                .font(.system(size: 11, weight: .medium))
                .foregroundStyle(.secondary)

            if let config = selectedConfig {
                Picker("", selection: $restoreFormat) {
                    ForEach(BackupFormat.available(for: config.databaseType), id: \.self) { fmt in
                        Text(fmt.displayName).tag(fmt)
                    }
                }
                .labelsHidden()
                .pickerStyle(.menu)
            }

            Divider()

            // Warning
            HStack(spacing: 6) {
                Image(systemName: "exclamationmark.triangle.fill")
                    .font(.system(size: 11))
                    .foregroundColor(.orange)
                Text("This will overwrite existing data.")
                    .font(.system(size: 11))
                    .foregroundStyle(.secondary)
            }
        }
    }

    private func radioButton(_ label: String, isSelected: Bool, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            HStack(spacing: 4) {
                Image(systemName: isSelected ? "largecircle.fill.circle" : "circle")
                    .font(.system(size: 12))
                    .foregroundColor(isSelected ? .accentColor : .secondary)
                Text(label)
                    .font(.system(size: 12))
            }
        }
        .buttonStyle(.plain)
    }

    // MARK: - Footer

    private var footerBar: some View {
        HStack(spacing: 8) {
            // Result inline
            if let result {
                Image(systemName: result.success ? "checkmark.circle.fill" : "xmark.circle.fill")
                    .foregroundColor(result.success ? .green : .red)
                    .font(.system(size: 13))
                VStack(alignment: .leading, spacing: 1) {
                    Text(result.success
                         ? (mode == .backup ? "Backup completed" : "Restore completed")
                         : (mode == .backup ? "Backup failed" : "Restore failed"))
                        .font(.system(size: 11, weight: .medium))
                    if let err = result.errorMessage, !result.success {
                        Text(String(err.prefix(120)))
                            .font(.system(size: 10))
                            .foregroundColor(.red)
                            .lineLimit(2)
                    }
                    if result.success {
                        HStack(spacing: 6) {
                            if let size = result.fileSize {
                                Text(ByteCountFormatter.string(fromByteCount: size, countStyle: .file))
                            }
                            Text(String(format: "%.1fs", result.duration))
                        }
                        .font(.system(size: 10, design: .monospaced))
                        .foregroundStyle(.secondary)
                    }
                }
            }

            if isRunning {
                if mode == .restore && restoreProgress > 0 {
                    ProgressView(value: restoreProgress)
                        .frame(width: 120)
                    Text("\(Int(restoreProgress * 100))%")
                        .font(.system(size: 11, design: .monospaced))
                        .foregroundStyle(.secondary)
                } else if mode == .backup && backupBytesWritten > 0 {
                    ProgressView()
                        .scaleEffect(0.6)
                    Text("\(ByteCountFormatter.string(fromByteCount: backupBytesWritten, countStyle: .file)) · \(Int(backupElapsed))s")
                        .font(.system(size: 11, design: .monospaced))
                        .foregroundStyle(.secondary)
                } else {
                    ProgressView()
                        .scaleEffect(0.6)
                    Text(mode == .backup ? "Backing up…" : "Restoring…")
                        .font(.system(size: 11))
                        .foregroundStyle(.secondary)
                }
            }

            Spacer()

            Button(mode == .backup ? "Start backup…" : "Start restore…") {
                Task {
                    if mode == .backup { await startBackup() } else { await startRestore() }
                }
            }
            .buttonStyle(.borderedProminent)
            .controlSize(.regular)
            .disabled(isRunning || selectedConnectionId == nil || selectedDatabase == nil
                      || (mode == .restore && restoreFileURL == nil))
        }
    }

    // MARK: - Helpers

    private func searchField(text: Binding<String>, placeholder: String) -> some View {
        HStack(spacing: 6) {
            Image(systemName: "magnifyingglass")
                .font(.system(size: 11))
                .foregroundStyle(.tertiary)
            TextField(placeholder, text: text)
                .textFieldStyle(.plain)
                .font(.system(size: 12))
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 5)
        .background(Color.primary.opacity(0.04))
        .clipShape(RoundedRectangle(cornerRadius: 6))
    }

    private func dbColor(_ type: DatabaseType) -> Color {
        switch type {
        case .postgresql: return .blue
        case .mysql: return .orange
        case .sqlite: return .purple
        case .redis: return .red
        case .mongodb: return .green
        case .mssql: return Color(red: 0.8, green: 0.2, blue: 0.4)
        case .clickhouse: return Color(red: 1.0, green: 0.85, blue: 0.0)
        }
    }

    private func dbInitials(_ type: DatabaseType) -> String {
        switch type {
        case .postgresql: return "Pg"
        case .mysql: return "My"
        case .sqlite: return "SL"
        case .redis: return "Rd"
        case .mongodb: return "Mg"
        case .mssql: return "MS"
        case .clickhouse: return "CH"
        }
    }

    private func updateFileName() {
        fileName = selectedDatabase ?? "untitled"
    }

    // MARK: - Connect + Load Databases

    private func connectAndLoadDatabases(_ conn: ConnectionConfig) {
        // If this is the active connection, use the existing adapter
        if conn.id == appState.activeConfig?.id, let adapter = appState.activeAdapter {
            loadDatabases(adapter: adapter, config: conn)
            return
        }

        // Otherwise, create a temporary connection to list databases
        isLoadingDatabases = true
        availableDatabases = []

        Task {
            let pw = (try? appState.container.keychainService.load(
                key: "db.password.\(conn.id.uuidString)")) ?? ""

            let adapter: any DatabaseAdapter = switch conn.databaseType {
            case .sqlite: SQLiteAdapter()
            case .postgresql: PostgreSQLAdapter()
            case .mysql: MySQLAdapter()
            case .redis: RedisAdapter()
            case .mongodb: MongoDBAdapter()
            case .mssql: MSSQLAdapter()
            case .clickhouse: ClickHouseAdapter()
            }

            do {
                try await adapter.connect(config: conn, password: pw)
                let dbs = try await adapter.listDatabases()
                await MainActor.run {
                    availableDatabases = dbs
                    tempAdapter = adapter
                    isLoadingDatabases = false
                    // Auto-select default database
                    if let db = conn.database, dbs.contains(db) {
                        selectedDatabase = db
                    } else {
                        selectedDatabase = dbs.first
                    }
                    updateFileName()
                }
            } catch {
                await MainActor.run {
                    availableDatabases = []
                    isLoadingDatabases = false
                }
                try? await adapter.disconnect()
            }
        }
    }

    private func loadDatabases(adapter: any DatabaseAdapter, config: ConnectionConfig) {
        isLoadingDatabases = true
        Task {
            let dbs = (try? await adapter.listDatabases()) ?? []
            await MainActor.run {
                availableDatabases = dbs
                isLoadingDatabases = false
                if selectedDatabase == nil {
                    if let db = config.database, dbs.contains(db) {
                        selectedDatabase = db
                    } else {
                        selectedDatabase = dbs.first
                    }
                }
                updateFileName()
            }
        }
    }

    // MARK: - Actions

    private func startBackup() async {
        guard let config = selectedConfig, let database = selectedDatabase else { return }

        let pw = (try? appState.container.keychainService.load(
            key: "db.password.\(config.id.uuidString)")) ?? ""

        let panel = NSSavePanel()
        panel.title = "Save Backup"
        let ext = selectedFormat.fileExtension
        let defaultName = "\(database)_\(dateStamp()).\(ext.isEmpty ? "dump" : ext)"
        panel.nameFieldStringValue = defaultName
        guard panel.runModal() == .OK, let url = panel.url else { return }

        isRunning = true
        result = nil
        backupBytesWritten = 0
        backupElapsed = 0

        let options = BackupOptions(
            format: selectedFormat,
            compress: compress,
            dataOnly: dataOnly,
            schemaOnly: schemaOnly
        )
        // Pass the active adapter if it matches this connection (needed for
        // Redis/MongoDB/MSSQL backups which use the driver directly).
        let adapterForBackup: (any DatabaseAdapter)? = {
            if appState.activeConfig?.id == config.id { return appState.activeAdapter }
            return nil
        }()

        let res = await backupService.backup(
            config: config, password: pw, database: database,
            to: url, options: options,
            adapter: adapterForBackup,
            onProgress: { bytes, elapsed in
                Task { @MainActor in
                    backupBytesWritten = bytes
                    backupElapsed = elapsed
                }
            }
        )
        result = res
        isRunning = false
    }

    private func startRestore() async {
        guard let config = selectedConfig,
              let database = selectedDatabase,
              let fileURL = restoreFileURL else { return }

        let pw = (try? appState.container.keychainService.load(
            key: "db.password.\(config.id.uuidString)")) ?? ""

        isRunning = true
        result = nil
        restoreProgress = 0

        let adapterForRestore: (any DatabaseAdapter)? = {
            if appState.activeConfig?.id == config.id { return appState.activeAdapter }
            return nil
        }()

        let res = await backupService.restore(
            config: config, password: pw, database: database,
            from: fileURL, format: restoreFormat,
            adapter: adapterForRestore,
            onProgress: { progress in
                Task { @MainActor in
                    restoreProgress = progress
                }
            }
        )
        result = res
        isRunning = false

        if res.success { appState.refreshSidebar() }
    }

    private func pickRestoreFile() {
        let panel = NSOpenPanel()
        panel.title = "Select Backup File"
        panel.allowsMultipleSelection = false
        panel.canChooseDirectories = false
        panel.canChooseFiles = true
        if panel.runModal() == .OK, let url = panel.url {
            restoreFileURL = url
            let ext = url.pathExtension.lowercased()
            switch ext {
            case "sql": restoreFormat = .sql
            case "dump": restoreFormat = .custom
            case "tar": restoreFormat = .tar
            default: break
            }
        }
    }

    private func dateStamp() -> String {
        let f = DateFormatter()
        f.dateFormat = "yyyy-MM-dd_HHmmss"
        return f.string(from: Date())
    }

    private func fileSizeString(_ url: URL) -> String {
        guard let attrs = try? FileManager.default.attributesOfItem(atPath: url.path),
              let size = attrs[.size] as? Int64 else { return "" }
        return ByteCountFormatter.string(fromByteCount: size, countStyle: .file)
    }
}
