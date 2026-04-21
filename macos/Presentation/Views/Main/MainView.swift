// MainView.swift
// Gridex
//
// Top-level SwiftUI view: routes between Home (no connection) and Workspace (connected).

import SwiftUI
import UniformTypeIdentifiers

// MARK: - Drag & Drop Transfer Type

struct ConnectionTransfer: Codable, Transferable {
    let connectionId: String

    static var transferRepresentation: some TransferRepresentation {
        CodableRepresentation(contentType: .json)
    }
}

// MARK: - Window Root (per-window AppState owner)

/// Each window owns its own AppState via @StateObject, enabling multiple
/// simultaneous database connections in separate windows.
struct WindowRoot: View {
    @StateObject private var appState = AppState()

    var body: some View {
        MainView()
            .environmentObject(appState)
            .focusedObject(appState)
            .onAppear {
                // Register as the active AppState for menu command routing
                AppState.active = appState

                // Shrink window to compact size so connection picker feels focused
                DispatchQueue.main.async {
                    if let window = NSApp.keyWindow ?? NSApp.windows.last(where: { $0.isVisible && !($0 is NSPanel) }) {
                        let target = NSSize(width: 900, height: 500)
                        var frame = window.frame
                        frame.origin.x += (frame.width - target.width) / 2
                        frame.origin.y += (frame.height - target.height) / 2
                        frame.size = target
                        window.setFrame(frame, display: true, animate: false)
                        window.center()
                    }
                }
            }
            .onReceive(NotificationCenter.default.publisher(for: NSWindow.didBecomeKeyNotification)) { _ in
                // Update active AppState when this window gains focus
                AppState.active = appState
            }
            .onChange(of: appState.activeConnectionId) { _, newValue in
                guard newValue != nil else { return }
                DispatchQueue.main.async {
                    if let window = NSApp.keyWindow ?? NSApp.mainWindow {
                        if !window.isZoomed { window.zoom(nil) }
                    }
                }
            }
            .onDisappear {
                if appState.activeAdapter != nil { appState.disconnect() }
                if AppState.active === appState { AppState.active = nil }
            }
    }
}

// MARK: - Main View

struct MainView: View {
    @EnvironmentObject private var appState: AppState

    var body: some View {
        Group {
            if appState.activeAdapter != nil {
                WorkspaceView()
            } else {
                HomeView()
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .sheet(isPresented: $appState.showDBTypePicker) {
            DatabaseTypePickerView()
        }
        .sheet(isPresented: $appState.showDatabaseSwitcher) {
            DatabaseSwitcherDialog()
        }
        .sheet(isPresented: $appState.showNewTableSheet) {
            NewTableSheet(schema: "public")
        }
        .onChange(of: appState.showConnectionForm) { _, show in
            if show {
                appState.showConnectionForm = false
                ConnectionFormPanel.open(
                    databaseType: appState.selectedDBType ?? .postgresql,
                    appState: appState
                )
            }
        }
        .onReceive(NotificationCenter.default.publisher(for: .windowCloseRequested)) { _ in
            // Only handle if this is the last main window (multi-window closes directly)
            let mainWindows = NSApp.windows.filter { $0.canBecomeMain && !($0 is NSPanel) && $0.isVisible }
            guard mainWindows.count <= 1 else { return }

            if appState.activeAdapter != nil {
                appState.disconnect()
            } else {
                NSApplication.shared.terminate(nil)
            }
        }
    }
}

// MARK: - Home View

struct HomeView: View {
    @EnvironmentObject private var appState: AppState
    @State private var selectedConnectionId: UUID?
    @State private var collapsedGroups: Set<String> = []
    @State private var showNewGroupAlert = false
    @State private var newGroupName = ""
    @State private var renamingGroup: String?
    @State private var renameGroupName = ""
    @State private var editingConnection: ConnectionConfig?
    @State private var showConnectionError = false
    @State private var showTablePlusImport = false
    @ObservedObject private var updater = UpdaterService.shared

    var body: some View {
        HStack(spacing: 0) {
            // Left panel — branding
            brandingPanel

            Divider()

            // Right panel — connections
            connectionsPanel
        }
        .overlay {
            if appState.isConnecting {
                ZStack {
                    Color.black.opacity(0.3)
                    VStack(spacing: 12) {
                        ProgressView()
                            .scaleEffect(1.2)
                        Text("Connecting...")
                            .font(.system(size: 13, weight: .medium))
                            .foregroundStyle(.white)
                    }
                    .padding(24)
                    .background(.ultraThinMaterial)
                    .clipShape(RoundedRectangle(cornerRadius: 12))
                }
                .ignoresSafeArea()
            }
        }
        .alert("Connection Failed", isPresented: $showConnectionError) {
            Button("OK") {
                showConnectionError = false
                appState.connectionError = nil
            }
        } message: {
            Text(appState.connectionError ?? "Unknown error")
        }
        .onChange(of: appState.connectionError) { _, newValue in
            if newValue != nil {
                showConnectionError = true
            }
        }
        .task {
            await appState.loadSavedConnections()
        }
        .sheet(isPresented: $showNewGroupAlert) {
            GroupNameSheet(
                title: "New Group",
                placeholder: "Group name",
                initialValue: "",
                onConfirm: { name in
                    if !name.isEmpty {
                        appState.connectionGroups.insert(name)
                    }
                    showNewGroupAlert = false
                },
                onCancel: { showNewGroupAlert = false }
            )
        }
        .sheet(isPresented: Binding(
            get: { renamingGroup != nil },
            set: { if !$0 { renamingGroup = nil } }
        )) {
            GroupNameSheet(
                title: "Rename Group",
                placeholder: "Group name",
                initialValue: renamingGroup ?? "",
                onConfirm: { name in
                    if let oldName = renamingGroup, !name.isEmpty {
                        renameGroup(from: oldName, to: name)
                    }
                    renamingGroup = nil
                },
                onCancel: { renamingGroup = nil }
            )
        }
        .onChange(of: editingConnection) { _, conn in
            guard let conn else { return }
            editingConnection = nil
            ConnectionFormPanel.open(
                databaseType: conn.databaseType,
                existingConfig: conn,
                existingPassword: (try? appState.container.keychainService.load(
                    key: "db.password.\(conn.id.uuidString)")) ?? "",
                existingSSHPassword: (try? appState.container.keychainService.load(
                    key: "ssh.password.\(conn.id.uuidString)")) ?? "",
                appState: appState
            )
        }
        .sheet(isPresented: $showTablePlusImport) {
            ImportConnectionsSheet()
        }
    }

    // MARK: - Left Branding Panel

    private var brandingPanel: some View {
        VStack(spacing: 0) {
            Spacer()

            VStack(spacing: 16) {
                AppLogoImage()
                    .frame(width: 100, height: 100)

                VStack(spacing: 4) {
                    Text("Gridex")
                        .font(.system(size: 26, weight: .bold, design: .default))
                    Text("AI-Native Database IDE")
                        .font(.system(size: 12))
                        .foregroundStyle(.tertiary)

                    // Version + Check for Updates
                    HStack(spacing: 6) {
                        Text("v\(Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "0.0.0")")
                            .font(.system(size: 10))
                            .foregroundStyle(.quaternary)

                        Button {
                            updater.checkForUpdates()
                        } label: {
                            Text("Check for Updates")
                                .font(.system(size: 10))
                                .foregroundStyle(.blue)
                        }
                        .buttonStyle(.plain)
                        .disabled(!updater.canCheckForUpdates)
                    }
                    .padding(.top, 2)
                }
            }

            Spacer()

            VStack(spacing: 2) {
                HomeActionButton(icon: "square.and.arrow.down.fill", title: "Import Connections") {
                    showTablePlusImport = true
                }

                HomeActionButton(icon: "arrow.up.doc.fill", title: "Backup database…") {
                    BackupRestorePanel.openBackupWizard(appState: appState)
                }
                HomeActionButton(icon: "arrow.down.doc.fill", title: "Restore database…") {
                    BackupRestorePanel.openRestoreWizard(appState: appState)
                }

                Divider()
                    .padding(.vertical, 6)
                    .padding(.horizontal, 14)

                HomeActionButton(icon: "bolt.horizontal.circle.fill", title: "MCP config") {
                    MCPWindowController.show()
                }
            }
            .padding(.horizontal, 20)
            .padding(.bottom, 28)
        }
        .frame(width: 260)
        .background(Color.primary.opacity(0.02))
    }

    // MARK: - Right Connections Panel

    private var connectionsPanel: some View {
        VStack(spacing: 0) {
            // Toolbar
            HStack(spacing: 10) {
                HStack(spacing: 6) {
                    Image(systemName: "magnifyingglass")
                        .font(.system(size: 12))
                        .foregroundStyle(.tertiary)
                    TextField("Search connections...", text: $appState.connectionSearchText)
                        .textFieldStyle(.plain)
                        .font(.system(size: 13))
                }
                .padding(.horizontal, 10)
                .padding(.vertical, 7)
                .background(Color.primary.opacity(0.04))
                .clipShape(RoundedRectangle(cornerRadius: 8))
                .overlay(
                    RoundedRectangle(cornerRadius: 8)
                        .stroke(Color.primary.opacity(0.06), lineWidth: 1)
                )

                toolbarButton(icon: "plus", tooltip: "New Connection") {
                    appState.showDBTypePicker = true
                }
                toolbarButton(icon: "folder.badge.plus", tooltip: "New Group") {
                    newGroupName = ""
                    showNewGroupAlert = true
                }
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 10)

            Divider()

            // Connection list
            if appState.savedConnections.isEmpty {
                emptyState
            } else {
                connectionList
            }
        }
        .frame(maxWidth: .infinity)
        .background(.background)
    }

    private func toolbarButton(icon: String, tooltip: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: icon)
                .font(.system(size: 13, weight: .medium))
                .foregroundStyle(.secondary)
                .frame(width: 28, height: 28)
                .background(Color.primary.opacity(0.04))
                .clipShape(RoundedRectangle(cornerRadius: 6))
                .overlay(
                    RoundedRectangle(cornerRadius: 6)
                        .stroke(Color.primary.opacity(0.06), lineWidth: 1)
                )
        }
        .buttonStyle(.plain)
        .help(tooltip)
    }

    private var emptyState: some View {
        VStack(spacing: 16) {
            Spacer()
            Image(systemName: "externaldrive.badge.plus")
                .font(.system(size: 40))
                .foregroundStyle(.quaternary)
            VStack(spacing: 4) {
                Text("No connections yet")
                    .font(.system(size: 14, weight: .medium))
                    .foregroundStyle(.secondary)
                Text("Create your first database connection to get started")
                    .font(.system(size: 12))
                    .foregroundStyle(.tertiary)
            }
            Button("Create Connection") {
                appState.showDBTypePicker = true
            }
            .buttonStyle(.borderedProminent)
            .controlSize(.regular)
            Spacer()
        }
        .frame(maxWidth: .infinity)
    }

    private var connectionList: some View {
        ScrollView {
            VStack(spacing: 2) {
                ForEach(groupNames, id: \.self) { groupName in
                    ConnectionGroupSection(
                        groupName: groupName,
                        connections: connectionsForGroup(groupName),
                        isCollapsed: collapsedGroups.contains(groupName),
                        selectedConnectionId: selectedConnectionId,
                        allGroups: groupNames,
                        onToggle: {
                            withAnimation(.spring(response: 0.25, dampingFraction: 0.9)) {
                                if collapsedGroups.contains(groupName) {
                                    collapsedGroups.remove(groupName)
                                } else {
                                    collapsedGroups.insert(groupName)
                                }
                            }
                        },
                        onSelect: { id in selectedConnectionId = id },
                        onConnect: { conn in connectToDatabase(conn) },
                        onMoveToGroup: { conn, group in
                            if let actual = appState.savedConnections.first(where: { $0.id == conn.id }) {
                                moveConnection(actual, toGroup: group)
                            } else {
                                moveConnection(conn, toGroup: group)
                            }
                        },
                        onRenameGroup: { name in
                            renameGroupName = name
                            renamingGroup = name
                        },
                        onDeleteGroup: { name in
                            deleteGroup(name)
                        },
                        onNewGroup: {
                            newGroupName = ""
                            showNewGroupAlert = true
                        },
                        onNewConnection: {
                            appState.showDBTypePicker = true
                        },
                        onDeleteConnection: { conn in
                            deleteConnection(conn)
                        },
                        onEditConnection: { conn in
                            editingConnection = conn
                        }
                    )
                }

                ForEach(ungroupedConnections) { conn in
                    ConnectionRow(
                        connection: conn,
                        isSelected: selectedConnectionId == conn.id,
                        allGroups: groupNames,
                        onSelect: { selectedConnectionId = conn.id },
                        onConnect: { connectToDatabase(conn) },
                        onMoveToGroup: { group in
                            moveConnection(conn, toGroup: group)
                        },
                        onNewGroup: {
                            newGroupName = ""
                            showNewGroupAlert = true
                        },
                        onNewConnection: {
                            appState.showDBTypePicker = true
                        },
                        onDelete: {
                            deleteConnection(conn)
                        },
                        onEdit: {
                            editingConnection = conn
                        }
                    )
                }
            }
            .padding(.vertical, 8)
            .padding(.horizontal, 12)
        }
    }

    // MARK: - Grouping Logic

    private var groupNames: [String] {
        let fromConnections = Set(filteredConnections.compactMap { $0.group })
        return fromConnections.union(appState.connectionGroups).sorted()
    }

    private func connectionsForGroup(_ group: String) -> [ConnectionConfig] {
        filteredConnections.filter { $0.group == group }
    }

    private var ungroupedConnections: [ConnectionConfig] {
        filteredConnections.filter { $0.group == nil || $0.group?.isEmpty == true }
    }

    private var filteredConnections: [ConnectionConfig] {
        let search = appState.connectionSearchText.lowercased()
        if search.isEmpty { return appState.savedConnections }
        return appState.savedConnections.filter {
            $0.name.lowercased().contains(search) ||
            ($0.host ?? "").lowercased().contains(search) ||
            ($0.database ?? "").lowercased().contains(search)
        }
    }

    private func moveConnection(_ conn: ConnectionConfig, toGroup group: String?) {
        var updated = conn
        updated.group = group
        Task { @MainActor in
            try? await appState.container.connectionRepository.update(updated)
            await appState.loadSavedConnections()
        }
    }

    private func renameGroup(from oldName: String, to newName: String) {
        let connections = appState.savedConnections.filter { $0.group == oldName }
        Task { @MainActor in
            for var conn in connections {
                conn.group = newName
                try? await appState.container.connectionRepository.update(conn)
            }
            appState.connectionGroups.remove(oldName)
            appState.connectionGroups.insert(newName)
            await appState.loadSavedConnections()
        }
    }

    private func deleteGroup(_ name: String) {
        let connections = appState.savedConnections.filter { $0.group == name }
        Task { @MainActor in
            for var conn in connections {
                conn.group = nil
                try? await appState.container.connectionRepository.update(conn)
            }
            appState.connectionGroups.remove(name)
            await appState.loadSavedConnections()
        }
    }

    private func connectToDatabase(_ conn: ConnectionConfig) {
        // SQLite doesn't need a password — connect directly
        if conn.databaseType == .sqlite {
            Task { await appState.connect(config: conn, password: "") }
            return
        }

        Task {
            // Load keychain passwords off the main thread.
            let keychain = appState.container.keychainService
            let hasSSH = conn.sshConfig != nil
            let credentials = await Task.detached { () -> (String?, String?) in
                let pw = try? keychain.load(key: "db.password.\(conn.id.uuidString)")
                let sshPw = hasSSH
                    ? (try? keychain.load(key: "ssh.password.\(conn.id.uuidString)"))
                    : nil
                return (pw, sshPw ?? nil)
            }.value

            // Connect even with empty/nil password — Redis and some databases don't require one
            await appState.connect(
                config: conn,
                password: credentials.0 ?? "",
                sshPassword: credentials.1
            )
        }
    }

    private func deleteConnection(_ conn: ConnectionConfig) {
        Task { @MainActor in
            try? await appState.container.connectionRepository.delete(conn.id)
            try? appState.container.keychainService.delete(key: "db.password.\(conn.id.uuidString)")
            try? appState.container.keychainService.delete(key: "ssh.password.\(conn.id.uuidString)")
            if selectedConnectionId == conn.id { selectedConnectionId = nil }
            await appState.loadSavedConnections()
        }
    }
}

// MARK: - Connection Group Section

struct ConnectionGroupSection: View {
    let groupName: String
    let connections: [ConnectionConfig]
    let isCollapsed: Bool
    let selectedConnectionId: UUID?
    let allGroups: [String]
    let onToggle: () -> Void
    let onSelect: (UUID) -> Void
    let onConnect: (ConnectionConfig) -> Void
    let onMoveToGroup: (ConnectionConfig, String?) -> Void
    let onRenameGroup: (String) -> Void
    let onDeleteGroup: (String) -> Void
    let onNewGroup: () -> Void
    let onNewConnection: () -> Void
    let onDeleteConnection: (ConnectionConfig) -> Void
    var onEditConnection: ((ConnectionConfig) -> Void)?
    @State private var isHovered = false
    @State private var isDropTargeted = false

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            // Section header — minimal, always uppercase
            groupHeader
                .padding(.top, 14)
                .padding(.bottom, 4)

            // Connections
            if !isCollapsed {
                VStack(spacing: 2) {
                    ForEach(connections) { conn in
                        ConnectionRow(
                            connection: conn,
                            isSelected: selectedConnectionId == conn.id,
                            allGroups: allGroups,
                            onSelect: { onSelect(conn.id) },
                            onConnect: { onConnect(conn) },
                            onMoveToGroup: { group in onMoveToGroup(conn, group) },
                            onNewGroup: onNewGroup,
                            onNewConnection: onNewConnection,
                            onDelete: { onDeleteConnection(conn) },
                            onEdit: { onEditConnection?(conn) }
                        )
                        .transition(.opacity)
                    }
                }
            }
        }
    }

    private var groupHeader: some View {
        HStack(spacing: 8) {
            // Disclosure chevron — subtle
            Image(systemName: "chevron.right")
                .font(.system(size: 9, weight: .semibold))
                .foregroundStyle(.tertiary)
                .frame(width: 10)
                .rotationEffect(.degrees(isCollapsed ? 0 : 90))

            // Group name in section-header style
            Text(groupName.uppercased())
                .font(.system(size: 11, weight: .semibold))
                .foregroundStyle(.secondary)
                .tracking(0.8)
                .lineLimit(1)

            // Count — quiet, inline
            Text("\(connections.count)")
                .font(.system(size: 11, weight: .regular, design: .rounded))
                .foregroundStyle(.tertiary)
                .monospacedDigit()

            Spacer()

            // Inline "+" — always rendered, opacity toggles on hover
            Button(action: onNewConnection) {
                Image(systemName: "plus")
                    .font(.system(size: 10, weight: .semibold))
                    .foregroundStyle(.secondary)
                    .frame(width: 18, height: 18)
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .help("New connection in this group")
            .opacity(isHovered ? 1 : 0)
            .allowsHitTesting(isHovered)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 4)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(
            RoundedRectangle(cornerRadius: 4)
                .strokeBorder(
                    isDropTargeted ? Color.accentColor.opacity(0.5) : Color.clear,
                    lineWidth: 1.5
                )
                .background(
                    isDropTargeted ? Color.accentColor.opacity(0.10) : Color.clear
                )
        )
        .contentShape(Rectangle())
        .onHover { isHovered = $0 }
        .onTapGesture { onToggle() }
        .dropDestination(for: ConnectionTransfer.self) { items, _ in
            guard let item = items.first,
                  let uuid = UUID(uuidString: item.connectionId) else { return false }
            onMoveToGroup(ConnectionConfig(id: uuid, name: "", databaseType: .postgresql), groupName)
            return true
        } isTargeted: { targeted in
            withAnimation(.easeInOut(duration: 0.15)) { isDropTargeted = targeted }
        }
        .contextMenu {
            Menu("New") {
                Button("Group...") { onNewGroup() }
                Button("Connection...") { onNewConnection() }
            }
            Button("Rename...") { onRenameGroup(groupName) }
            Divider()
            Button("Delete Group", role: .destructive) { onDeleteGroup(groupName) }
        }
    }
}

// MARK: - Connection Row

struct ConnectionRow: View {
    let connection: ConnectionConfig
    let isSelected: Bool
    let allGroups: [String]
    let onSelect: () -> Void
    let onConnect: () -> Void
    var onMoveToGroup: ((String?) -> Void)?
    var onNewGroup: (() -> Void)?
    var onNewConnection: (() -> Void)?
    var onDelete: (() -> Void)?
    var onEdit: (() -> Void)?
    @State private var isHovered = false
    @State private var lastClickTime: Date = .distantPast

    var body: some View {
        HStack(spacing: 12) {
            // Environment color tag bar (only visible when set)
            RoundedRectangle(cornerRadius: 2)
                .fill(connection.colorTag?.swiftUIColor ?? Color.clear)
                .frame(width: 3, height: 28)

            // DB type icon
            DatabaseTypeIcon(type: connection.databaseType, size: 32)

            // Name + subtitle
            VStack(alignment: .leading, spacing: 3) {
                HStack(spacing: 6) {
                    Text(connection.name)
                        .font(.system(size: 13, weight: .semibold))
                        .foregroundStyle(isSelected ? .white : .primary)
                        .lineLimit(1)

                    if let color = connection.colorTag {
                        Text(color.environmentHint)
                            .font(.system(size: 9, weight: .bold))
                            .tracking(0.3)
                            .foregroundStyle(isSelected ? .white : color.swiftUIColor)
                            .padding(.horizontal, 5)
                            .padding(.vertical, 1)
                            .background(
                                RoundedRectangle(cornerRadius: 3)
                                    .fill(isSelected ? Color.white.opacity(0.2) : color.swiftUIColor.opacity(0.14))
                            )
                    }
                }

                Text(connectionSubtitle)
                    .font(.system(size: 11, design: .monospaced))
                    .foregroundStyle(isSelected ? Color.white.opacity(0.7) : Color.secondary)
                    .lineLimit(1)
                    .truncationMode(.middle)
            }

            Spacer(minLength: 8)

            // DB type label — quiet, tabular
            Text(connection.databaseType.displayName.uppercased())
                .font(.system(size: 9, weight: .semibold))
                .tracking(0.5)
                .foregroundStyle(isSelected ? AnyShapeStyle(Color.white.opacity(0.55)) : AnyShapeStyle(.tertiary))
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 8)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(
            RoundedRectangle(cornerRadius: 7)
                .fill(
                    isSelected
                        ? Color.accentColor
                        : (isHovered ? Color.primary.opacity(0.055) : Color.clear)
                )
        )
        .contentShape(Rectangle())
        .onHover { isHovered = $0 }
        .draggable(ConnectionTransfer(connectionId: connection.id.uuidString))
        .onTapGesture(count: 1) {
            let now = Date()
            if now.timeIntervalSince(lastClickTime) < 0.3 {
                onConnect()
                lastClickTime = .distantPast
            } else {
                onSelect()
                lastClickTime = now
            }
        }
        .contextMenu {
            Button("Connect") { onConnect() }
            Button("Edit...") { onEdit?() }

            Divider()

            Menu("New") {
                Button("Connection...") { onNewConnection?() }
                Button("Group...") { onNewGroup?() }
            }

            if !allGroups.isEmpty {
                Menu("Move to Group") {
                    ForEach(allGroups, id: \.self) { group in
                        Button(group) { onMoveToGroup?(group) }
                    }
                    if connection.group != nil {
                        Divider()
                        Button("Remove from Group") { onMoveToGroup?(nil) }
                    }
                }
            }

            Menu("Sort By") {
                Button("Name") { }
                Button("Type") { }
                Button("Date Created") { }
            }

            Divider()

            Button("Delete", role: .destructive) { onDelete?() }
        }
    }

    private var connectionSubtitle: String {
        if connection.databaseType == .sqlite {
            return connection.filePath ?? "No file"
        }
        var parts: [String] = []
        parts.append(connection.displayHost)
        if let db = connection.database, !db.isEmpty {
            parts.append(" / \(db)")
        }
        return parts.joined()
    }
}

// MARK: - Database Type Icon

struct DatabaseTypeIcon: View {
    let type: DatabaseType
    var size: CGFloat = 36

    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: size * 0.22)
                .fill(iconGradient)
                .frame(width: size, height: size)

            Text(iconText)
                .font(.system(size: size * 0.32, weight: .bold, design: .rounded))
                .foregroundStyle(.white)
        }
    }

    private var iconGradient: LinearGradient {
        switch type {
        case .postgresql:
            return LinearGradient(colors: [Color(red: 0.2, green: 0.45, blue: 0.85), Color(red: 0.15, green: 0.35, blue: 0.7)], startPoint: .top, endPoint: .bottom)
        case .mysql:
            return LinearGradient(colors: [Color(red: 0.0, green: 0.55, blue: 0.8), Color(red: 0.0, green: 0.4, blue: 0.65)], startPoint: .top, endPoint: .bottom)
        case .sqlite:
            return LinearGradient(colors: [Color(red: 0.45, green: 0.35, blue: 0.75), Color(red: 0.35, green: 0.25, blue: 0.6)], startPoint: .top, endPoint: .bottom)
        case .redis:
            return LinearGradient(colors: [Color(red: 0.85, green: 0.2, blue: 0.2), Color(red: 0.7, green: 0.15, blue: 0.15)], startPoint: .top, endPoint: .bottom)
        case .mongodb:
            return LinearGradient(colors: [Color(red: 0.30, green: 0.65, blue: 0.35), Color(red: 0.20, green: 0.50, blue: 0.25)], startPoint: .top, endPoint: .bottom)
        case .mssql:
            return LinearGradient(colors: [Color(red: 0.80, green: 0.20, blue: 0.40), Color(red: 0.65, green: 0.10, blue: 0.30)], startPoint: .top, endPoint: .bottom)
        case .clickhouse:
            return LinearGradient(colors: [Color(red: 1.00, green: 0.85, blue: 0.00), Color(red: 0.85, green: 0.65, blue: 0.00)], startPoint: .top, endPoint: .bottom)
        }
    }

    private var iconText: String {
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
}

// MARK: - Home Action Button

struct HomeActionButton: View {
    let icon: String
    let title: String
    let action: () -> Void
    @State private var isHovered = false

    var body: some View {
        Button(action: action) {
            HStack(spacing: 10) {
                Image(systemName: icon)
                    .font(.system(size: 14))
                    .foregroundStyle(isHovered ? .blue : .secondary)
                    .frame(width: 20)
                Text(title)
                    .font(.system(size: 13, weight: .medium))
                    .foregroundStyle(isHovered ? .primary : .secondary)
                Spacer()
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 9)
            .background(isHovered ? Color.accentColor.opacity(0.08) : Color.clear)
            .clipShape(RoundedRectangle(cornerRadius: 8))
        }
        .buttonStyle(.plain)
        .onHover { isHovered = $0 }
    }
}

// MARK: - Group Name Sheet

struct GroupNameSheet: View {
    let title: String
    let placeholder: String
    let initialValue: String
    let onConfirm: (String) -> Void
    let onCancel: () -> Void
    @State private var name: String = ""
    @FocusState private var isFocused: Bool

    var body: some View {
        VStack(spacing: 20) {
            VStack(spacing: 4) {
                Text(title)
                    .font(.system(size: 15, weight: .semibold))
                Text(title == "New Group" ? "Create a group to organize connections" : "Enter a new name for the group")
                    .font(.system(size: 12))
                    .foregroundStyle(.secondary)
            }

            TextField(placeholder, text: $name)
                .textFieldStyle(.roundedBorder)
                .frame(width: 280)
                .focused($isFocused)
                .onSubmit {
                    if !name.isEmpty { onConfirm(name) }
                }

            HStack(spacing: 12) {
                Button("Cancel") { onCancel() }
                    .keyboardShortcut(.cancelAction)
                Button(title == "New Group" ? "Create" : "Rename") {
                    if !name.isEmpty { onConfirm(name) }
                }
                .buttonStyle(.borderedProminent)
                .disabled(name.isEmpty)
                .keyboardShortcut(.defaultAction)
            }
        }
        .padding(28)
        .frame(width: 360)
        .onAppear {
            name = initialValue
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) {
                isFocused = true
            }
        }
    }
}

// MARK: - Database Type Picker

struct DatabaseTypePickerView: View {
    @EnvironmentObject private var appState: AppState
    @State private var searchText = ""
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(spacing: 20) {
            VStack(spacing: 4) {
                Text("New Connection")
                    .font(.system(size: 15, weight: .semibold))
                Text("Select a database type")
                    .font(.system(size: 12))
                    .foregroundStyle(.secondary)
            }
            .padding(.top, 20)

            HStack(spacing: 6) {
                Image(systemName: "magnifyingglass")
                    .font(.system(size: 12))
                    .foregroundStyle(.tertiary)
                TextField("Search", text: $searchText)
                    .textFieldStyle(.plain)
                    .font(.system(size: 13))
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 7)
            .background(Color.primary.opacity(0.04))
            .clipShape(RoundedRectangle(cornerRadius: 8))
            .overlay(
                RoundedRectangle(cornerRadius: 8)
                    .stroke(Color.primary.opacity(0.06), lineWidth: 1)
            )
            .padding(.horizontal, 24)

            LazyVGrid(columns: Array(repeating: GridItem(.flexible(), spacing: 16), count: 4), spacing: 16) {
                ForEach(filteredTypes) { type in
                    DatabaseTypeCard(type: type) {
                        appState.selectedDBType = type
                        dismiss()
                        DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) {
                            appState.showConnectionForm = true
                        }
                    }
                }
            }
            .padding(.horizontal, 24)

            Spacer()

            HStack {
                Button("Cancel") { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Spacer()
            }
            .padding(.horizontal, 24)
            .padding(.bottom, 16)
        }
        .frame(width: 440, height: 280)
    }

    private var filteredTypes: [DatabaseType] {
        if searchText.isEmpty { return DatabaseType.allCases }
        return DatabaseType.allCases.filter { $0.displayName.localizedCaseInsensitiveContains(searchText) }
    }
}

struct DatabaseTypeCard: View {
    let type: DatabaseType
    let action: () -> Void
    @State private var isHovered = false

    var body: some View {
        Button(action: action) {
            VStack(spacing: 8) {
                DatabaseTypeIcon(type: type, size: 44)
                Text(type.displayName)
                    .font(.system(size: 11, weight: .medium))
                    .foregroundStyle(.primary)
            }
            .frame(width: 86, height: 86)
            .background(isHovered ? Color.accentColor.opacity(0.08) : Color.primary.opacity(0.02))
            .clipShape(RoundedRectangle(cornerRadius: 10))
            .overlay(
                RoundedRectangle(cornerRadius: 10)
                    .stroke(isHovered ? Color.accentColor.opacity(0.4) : Color.primary.opacity(0.06), lineWidth: 1)
            )
        }
        .buttonStyle(.plain)
        .onHover { isHovered = $0 }
    }
}

// MARK: - Workspace View (connected state)

struct WorkspaceView: View {
    @EnvironmentObject private var appState: AppState
    @State private var showConnectionPicker = false
    @State private var showDatabasePicker = false
    private let sidebarWidth: CGFloat = 280

    /// Max width for the right details panel: half of the main screen.
    private var detailsPanelMaxWidth: CGFloat {
        let screenWidth = NSScreen.main?.visibleFrame.width ?? 1440
        return screenWidth * 0.5
    }

    var body: some View {
        HStack(spacing: 0) {
            // Sidebar — always in hierarchy; width animates between 0 and sidebarWidth
            SidebarView()
                .frame(width: sidebarWidth)
                .frame(width: appState.sidebarVisible ? sidebarWidth : 0, alignment: .leading)
                .clipped()

            // Thin separator line
            Rectangle()
                .fill(Color(NSColor.separatorColor))
                .frame(width: appState.sidebarVisible ? 1 : 0)

            // Main content area
            VStack(spacing: 0) {
                if !appState.tabs.isEmpty {
                    TabBarSwiftUIView()
                }

                ZStack(alignment: .topLeading) {
                    if let activeId = appState.activeTabId,
                       let tab = appState.tabs.first(where: { $0.id == activeId }) {
                        tabContent(for: tab)
                            .id(tab.id)
                    } else {
                        ConnectedWelcomeView()
                    }
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)

                StatusBarSwiftUIView()
            }
            .frame(minWidth: 400, maxWidth: .infinity, maxHeight: .infinity)

            // Resize handle + details panel — fixed width persisted in AppState
            if appState.detailsPanelVisible {
                ResizeHandle(width: $appState.detailsPanelWidth,
                             minWidth: 220,
                             maxWidth: detailsPanelMaxWidth)
                    .frame(width: 1)

                DetailsPanel()
                    .frame(width: appState.detailsPanelWidth)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .transaction { $0.disablesAnimations = true }
        .toolbar {
            // Left: sidebar toggle + database picker
            ToolbarItemGroup(placement: .navigation) {
                Button {
                    appState.sidebarVisible.toggle()
                } label: {
                    Image(systemName: "sidebar.left")
                }
                .help("Toggle Sidebar")

                // Database selector
                if appState.activeAdapter != nil {
                    Button {
                        showDatabasePicker.toggle()
                    } label: {
                        HStack(spacing: 5) {
                            Image(systemName: "cylinder")
                                .font(.system(size: 11))
                            Text(appState.currentDatabaseName ?? "Database")
                                .font(.system(size: 11, weight: .medium))
                                .lineLimit(1)
                            Image(systemName: "chevron.up.chevron.down")
                                .font(.system(size: 8, weight: .semibold))
                                .foregroundStyle(.secondary)
                        }
                    }
                    .help("Switch Database")
                    .popover(isPresented: $showDatabasePicker, arrowEdge: .bottom) {
                        DatabasePickerPopover(isPresented: $showDatabasePicker)
                    }
                }
            }

            // Connection breadcrumb in title bar
            ToolbarItem(placement: .principal) {
                ConnectionBreadcrumb()
            }

            // Right: unified icon toolbar
            ToolbarItemGroup(placement: .primaryAction) {
                // Data actions
                Button {
                    NotificationCenter.default.post(name: .deleteSelectedRows, object: nil)
                } label: {
                    Image(systemName: "trash")
                }
                .help("Delete Selected Rows")

                Button {
                    NotificationCenter.default.post(name: .commitChanges, object: nil)
                } label: {
                    Image(systemName: "text.insert")
                }
                .help("Commit Changes")

                // ER Diagram (SQL databases only)
                if appState.activeConfig?.databaseType.isSQL == true {
                    Button { appState.openERDiagram(schema: nil) } label: {
                        Image(systemName: "point.3.connected.trianglepath.dotted")
                    }.help("ER Diagram")
                }

                // SQL Editor
                Button { appState.openNewQueryTab() } label: {
                    Image(systemName: "chevron.left.forwardslash.chevron.right")
                }.help("New SQL Query (⌘⇧N)")

                // Redis-specific
                if appState.activeConfig?.databaseType == .redis {
                    Button { appState.openRedisServerInfo() } label: {
                        Image(systemName: "chart.bar")
                    }.help("Server Info")

                    Button { appState.openRedisSlowLog() } label: {
                        Image(systemName: "tortoise")
                    }.help("Slow Log")
                }

                // Panel toggles
                Button { appState.detailsPanelVisible.toggle() } label: {
                    Image(systemName: "sidebar.trailing")
                }
                .help("Toggle Details Panel")
            }
        }
        .alert("Flush Database", isPresented: $appState.showFlushDBConfirm) {
            Button("Cancel", role: .cancel) {}
            Button("Flush", role: .destructive) {
                Task {
                    if let redis = appState.activeAdapter as? RedisAdapter {
                        try? await redis.flushDB()
                        appState.redisDBSize = 0
                        NotificationCenter.default.post(name: .reloadData, object: nil)
                    }
                }
            }
        } message: { Text("This will permanently delete ALL keys in the current database. This cannot be undone.") }
        .sheet(isPresented: $appState.showRedisAddKey) {
            RedisAddKeySheet()
        }
    }

    @ViewBuilder
    private func tabContent(for tab: AppState.ContentTab) -> some View {
        switch tab.type {
        case .dataGrid:
            if let tableName = tab.tableName {
                DataGridView(tableName: tableName, schema: tab.schema, tabId: tab.id, initialViewMode: tab.initialViewMode)
            }
        case .queryEditor:
            QueryEditorView(tabId: tab.id)
        case .tableStructure:
            if let tableName = tab.tableName {
                TableStructureView(tableName: tableName, schema: tab.schema)
            }
        case .tableList:
            TableListView(schema: tab.schema)
        case .functionDetail:
            if let funcName = tab.tableName {
                FunctionDetailView(
                    functionName: funcName,
                    schema: tab.schema,
                    isProcedure: tab.initialViewMode == "procedure"
                )
            }
        case .createTable:
            CreateTableView(schema: tab.schema)
        case .erDiagram:
            ERDiagramView(schema: tab.schema)
                .onReceive(NotificationCenter.default.publisher(for: .erDiagramOpenTable)) { notif in
                    if let name = notif.userInfo?["tableName"] as? String {
                        let schema = notif.userInfo?["schema"] as? String
                        appState.openTable(name: name, schema: schema)
                    }
                }
        // Redis-specific tabs
        case .redisKeyDetail:
            if let keyName = tab.tableName {
                RedisKeyDetailView(keyName: keyName)
            }
        case .redisServerInfo:
            RedisServerInfoView()
        case .redisSlowLog:
            RedisSlowLogView()
        }
    }
}

// MARK: - Connection picker popover

struct ConnectionPickerPopover: View {
    @EnvironmentObject private var appState: AppState
    @Binding var isPresented: Bool

    var body: some View {
        VStack(spacing: 0) {
            Text("Connections")
                .font(.system(size: 12, weight: .semibold))
                .foregroundStyle(.secondary)
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(.horizontal, 12)
                .padding(.vertical, 8)

            Divider()

            if appState.savedConnections.isEmpty {
                Text("No saved connections")
                    .font(.system(size: 12))
                    .foregroundStyle(.secondary)
                    .padding(20)
            } else {
                ScrollView {
                    VStack(spacing: 0) {
                        ForEach(appState.savedConnections) { conn in
                            Button {
                                isPresented = false
                                let keychain = appState.container.keychainService
                                let pw = (try? keychain.load(
                                    key: "db.password.\(conn.id.uuidString)")) ?? ""
                                let sshPw: String? = conn.sshConfig != nil
                                    ? (try? keychain.load(key: "ssh.password.\(conn.id.uuidString)")) ?? nil
                                    : nil
                                Task { await appState.connect(config: conn, password: pw, sshPassword: sshPw) }
                            } label: {
                                HStack(spacing: 8) {
                                    Image(systemName: conn.databaseType.iconName)
                                        .font(.system(size: 12))
                                        .foregroundStyle(.secondary)
                                        .frame(width: 16)

                                    VStack(alignment: .leading, spacing: 1) {
                                        Text(conn.name)
                                            .font(.system(size: 12))
                                            .foregroundStyle(.primary)
                                        if let host = conn.host {
                                            Text(host)
                                                .font(.system(size: 10))
                                                .foregroundStyle(.secondary)
                                        }
                                    }

                                    Spacer()

                                    if appState.activeConnectionId == conn.id {
                                        Image(systemName: "checkmark")
                                            .font(.system(size: 10, weight: .semibold))
                                            .foregroundStyle(.green)
                                    }
                                }
                                .contentShape(Rectangle())
                                .padding(.horizontal, 12)
                                .padding(.vertical, 7)
                            }
                            .buttonStyle(.plain)
                            .background(
                                appState.activeConnectionId == conn.id
                                    ? Color.accentColor.opacity(0.1) as Color
                                    : Color.clear
                            )

                            Divider().padding(.leading, 36)
                        }
                    }
                }
                .frame(maxHeight: 280)
            }

            Divider()

            HStack(spacing: 0) {
                Button {
                    isPresented = false
                    appState.showDBTypePicker = true
                } label: {
                    HStack(spacing: 6) {
                        Image(systemName: "plus")
                            .font(.system(size: 11))
                        Text("New Connection")
                            .font(.system(size: 12))
                    }
                    .foregroundColor(.accentColor)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.horizontal, 12)
                    .padding(.vertical, 8)
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)

                if appState.activeAdapter != nil {
                    Divider().frame(height: 16)
                    Button {
                        isPresented = false
                        appState.disconnect()
                    } label: {
                        HStack(spacing: 4) {
                            Image(systemName: "xmark.circle")
                                .font(.system(size: 11))
                            Text("Disconnect")
                                .font(.system(size: 12))
                        }
                        .foregroundColor(.red)
                        .padding(.horizontal, 12)
                        .padding(.vertical, 8)
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                }
            }
        }
        .frame(width: 280)
    }
}

// MARK: - Database picker popover

struct DatabasePickerPopover: View {
    @EnvironmentObject private var appState: AppState
    @Binding var isPresented: Bool

    var body: some View {
        VStack(spacing: 0) {
            Text("Databases")
                .font(.system(size: 12, weight: .semibold))
                .foregroundStyle(.secondary)
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(.horizontal, 12)
                .padding(.vertical, 8)

            Divider()

            if appState.availableDatabases.isEmpty {
                Text("No databases found")
                    .font(.system(size: 12))
                    .foregroundStyle(.secondary)
                    .padding(20)
            } else {
                ScrollView {
                    VStack(spacing: 0) {
                        ForEach(appState.availableDatabases, id: \.self) { dbName in
                            Button {
                                isPresented = false
                                Task { await appState.switchDatabase(dbName) }
                            } label: {
                                HStack(spacing: 8) {
                                    Image(systemName: "cylinder")
                                        .font(.system(size: 11))
                                        .foregroundStyle(.secondary)
                                        .frame(width: 16)

                                    Text(dbName)
                                        .font(.system(size: 12))
                                        .foregroundStyle(.primary)

                                    Spacer()

                                    if dbName == appState.currentDatabaseName {
                                        Image(systemName: "checkmark")
                                            .font(.system(size: 10, weight: .semibold))
                                            .foregroundStyle(.green)
                                    }
                                }
                                .contentShape(Rectangle())
                                .padding(.horizontal, 12)
                                .padding(.vertical, 7)
                            }
                            .buttonStyle(.plain)
                            .background(
                                dbName == appState.currentDatabaseName
                                    ? Color.accentColor.opacity(0.1)
                                    : Color.clear
                            )

                            Divider().padding(.leading, 36)
                        }
                    }
                }
                .frame(maxHeight: 320)
            }
        }
        .frame(width: 240)
    }
}

// MARK: - Database Switcher Dialog (Cmd+K)

struct DatabaseSwitcherDialog: View {
    @EnvironmentObject private var appState: AppState
    @Environment(\.dismiss) private var dismiss
    @State private var searchText = ""
    @State private var selected: String?
    @State private var showNewDBSheet = false
    @State private var showDeleteConfirm = false
    @State private var deleteError: String?

    private var filteredDatabases: [String] {
        if searchText.isEmpty {
            return appState.availableDatabases
        }
        return appState.availableDatabases.filter {
            $0.localizedCaseInsensitiveContains(searchText)
        }
    }

    var body: some View {
        VStack(spacing: 0) {
            // Title
            Text("Open database")
                .font(.system(size: 14, weight: .semibold))
                .padding(.top, 16)
                .padding(.bottom, 12)

            // Search field
            HStack(spacing: 6) {
                Image(systemName: "magnifyingglass")
                    .foregroundStyle(.secondary)
                    .font(.system(size: 13))
                TextField("Search for database...", text: $searchText)
                    .textFieldStyle(.plain)
                    .font(.system(size: 13))
                    .onSubmit { openSelected() }
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 7)
            .background(Color(nsColor: .controlBackgroundColor))
            .clipShape(RoundedRectangle(cornerRadius: 6))
            .overlay(
                RoundedRectangle(cornerRadius: 6)
                    .stroke(Color.accentColor, lineWidth: 2)
            )
            .padding(.horizontal, 16)

            // Database list — native List selection for instant response
            List(filteredDatabases, id: \.self, selection: $selected) { dbName in
                HStack(spacing: 8) {
                    Image(systemName: "cylinder.fill")
                        .font(.system(size: 12))
                        .foregroundStyle(dbName == appState.currentDatabaseName ? .blue : .secondary)
                        .frame(width: 18)

                    Text(dbName)
                        .font(.system(size: 13))

                    Spacer()
                }
                .padding(.vertical, 2)
                .tag(dbName)
            }
            .listStyle(.plain)
            .frame(minHeight: 200, maxHeight: 360)
            .environment(\.defaultMinListRowHeight, 30)
            .onDoubleClick { openSelected() }

            Divider()

            // Bottom buttons
            HStack {
                Button("Cancel") { dismiss() }
                    .keyboardShortcut(.cancelAction)

                Spacer()

                // Delete database
                Button(role: .destructive) {
                    showDeleteConfirm = true
                } label: {
                    Image(systemName: "trash")
                }
                .buttonStyle(.plain)
                .foregroundStyle(.red)
                .help("Delete database")
                .disabled(selected == nil || selected == appState.currentDatabaseName)

                Button("New...") {
                    showNewDBSheet = true
                }
                .sheet(isPresented: $showNewDBSheet) {
                    NewDatabaseSheet(onCreated: { dbName in
                        dismiss()
                        Task { await appState.switchDatabase(dbName) }
                    })
                }

                Button("Open") { openSelected() }
                    .keyboardShortcut(.defaultAction)
                    .disabled(selected == nil)
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 12)
        }
        .frame(width: 400)
        .background(Color(nsColor: .windowBackgroundColor))
        .onAppear {
            selected = appState.currentDatabaseName ?? filteredDatabases.first
        }
        .onChange(of: searchText) { _, _ in
            selected = filteredDatabases.first
        }
        .alert("Delete Database", isPresented: $showDeleteConfirm) {
            Button("Cancel", role: .cancel) {}
            Button("Delete", role: .destructive) {
                Task { await deleteSelected() }
            }
        } message: {
            Text("Are you sure you want to delete \"\(selected ?? "")\"?\nThis will permanently remove all data. This cannot be undone.")
        }
        .alert("Delete Failed", isPresented: Binding(
            get: { deleteError != nil },
            set: { if !$0 { deleteError = nil } }
        )) {
            Button("OK", role: .cancel) {}
        } message: {
            Text(deleteError ?? "")
        }
    }

    private func deleteSelected() async {
        guard let name = selected, let adapter = appState.activeAdapter else { return }
        do {
            try await adapter.dropDatabase(name: name)
            // Refresh database list
            if let databases = try? await adapter.listDatabases() {
                appState.availableDatabases = databases
            }
            selected = appState.currentDatabaseName ?? filteredDatabases.first
        } catch {
            deleteError = error.localizedDescription
        }
    }

    private func openSelected() {
        guard let name = selected else { return }
        dismiss()
        Task { await appState.switchDatabase(name) }
    }
}

// MARK: - App Logo (resolves from .module or main bundle)

private struct AppLogoImage: View {
    var body: some View {
        if let nsImage = loadLogo() {
            Image(nsImage: nsImage)
                .resizable()
                .aspectRatio(contentMode: .fit)
        } else {
            Image(systemName: "cylinder.split.1x2")
                .resizable()
                .aspectRatio(contentMode: .fit)
        }
    }

    private func loadLogo() -> NSImage? {
        // Load from the main .app bundle (Contents/Resources/Assets.car), which is
        // populated by scripts/build-app.sh from macos/Resources/Assets.xcassets.
        //
        // Do NOT use Bundle.module here — it's an SPM-generated lazy static that
        // fatalErrors if the "Gridex_Gridex.bundle" subbundle lookup fails, which
        // happens when the .app is run on a machine different from the build host.
        if let img = Bundle.main.image(forResource: "AppLogo") { return img }

        // Legacy fallback for older builds that shipped the SPM sub-bundle.
        let resourcePath = Bundle.main.bundlePath + "/Contents/Resources/Gridex_Gridex.bundle"
        if let bundle = Bundle(path: resourcePath),
           let img = bundle.image(forResource: "AppLogo") { return img }
        return nil
    }
}

// Double-click modifier for List rows
private struct DoubleClickModifier: ViewModifier {
    let action: () -> Void

    func body(content: Content) -> some View {
        content.background(DoubleClickView(action: action))
    }
}

private struct DoubleClickView: NSViewRepresentable {
    let action: () -> Void

    func makeNSView(context: Context) -> NSView {
        let view = DoubleClickNSView(action: action)
        return view
    }
    func updateNSView(_ nsView: NSView, context: Context) {}

    class DoubleClickNSView: NSView {
        let action: () -> Void
        init(action: @escaping () -> Void) {
            self.action = action
            super.init(frame: .zero)
        }
        required init?(coder: NSCoder) { fatalError() }

        override func mouseDown(with event: NSEvent) {
            super.mouseDown(with: event)
            if event.clickCount == 2 { action() }
        }
    }
}

private extension View {
    func onDoubleClick(_ action: @escaping () -> Void) -> some View {
        modifier(DoubleClickModifier(action: action))
    }
}

// MARK: - New Database Sheet

struct NewDatabaseSheet: View {
    var onCreated: (String) -> Void
    @EnvironmentObject private var appState: AppState
    @Environment(\.dismiss) private var dismiss

    @State private var dbName = ""
    @State private var encoding = "Default"
    @State private var collation = "Default"
    @State private var isCreating = false
    @State private var errorMessage: String?

    private var encodings: [String] {
        guard let adapter = appState.activeAdapter else { return ["Default"] }
        switch adapter.databaseType {
        case .postgresql: return ["Default", "UTF8", "LATIN1", "SQL_ASCII"]
        case .mysql: return ["Default", "utf8mb4", "utf8", "latin1"]
        case .sqlite, .redis, .mongodb, .mssql, .clickhouse: return ["Default"]
        }
    }

    private var collations: [String] {
        guard let adapter = appState.activeAdapter else { return ["Default"] }
        switch adapter.databaseType {
        case .postgresql: return ["Default", "C", "POSIX", "en_US.UTF-8"]
        case .mysql: return ["Default", "utf8mb4_unicode_ci", "utf8mb4_general_ci", "utf8_general_ci"]
        case .sqlite, .redis, .mongodb, .mssql, .clickhouse: return ["Default"]
        }
    }

    var body: some View {
        VStack(spacing: 0) {
            Text("New Database")
                .font(.system(size: 14, weight: .semibold))
                .padding(.top, 16)
                .padding(.bottom, 12)

            Grid(alignment: .trailing, horizontalSpacing: 10, verticalSpacing: 10) {
                GridRow {
                    Text("Name:").font(.system(size: 12))
                    TextField("", text: $dbName)
                        .textFieldStyle(.roundedBorder)
                        .frame(width: 220)
                }
                GridRow {
                    Text("Encoding:").font(.system(size: 12))
                    Picker("", selection: $encoding) {
                        ForEach(encodings, id: \.self) { Text($0).tag($0) }
                    }
                    .labelsHidden()
                    .frame(maxWidth: .infinity)
                }
                GridRow {
                    Text("Collation:").font(.system(size: 12))
                    Picker("", selection: $collation) {
                        ForEach(collations, id: \.self) { Text($0).tag($0) }
                    }
                    .labelsHidden()
                    .frame(maxWidth: .infinity)
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

            HStack {
                Button("Cancel") { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Spacer()
                Button {
                    Task { await createDatabase() }
                } label: {
                    if isCreating { ProgressView().controlSize(.small) }
                    Text("OK")
                }
                .keyboardShortcut(.defaultAction)
                .disabled(dbName.isEmpty || isCreating)
            }
            .padding(.horizontal, 20)
            .padding(.vertical, 12)
        }
        .frame(width: 360)
    }

    private func createDatabase() async {
        guard let adapter = appState.activeAdapter else { return }
        isCreating = true
        errorMessage = nil
        defer { isCreating = false }

        let d = adapter.databaseType.sqlDialect
        var sql = "CREATE DATABASE \(d.quoteIdentifier(dbName))"

        var skipExecuteRaw = false
        switch adapter.databaseType {
        case .postgresql:
            var opts: [String] = []
            if encoding != "Default" { opts.append("ENCODING '\(encoding)'") }
            if collation != "Default" { opts.append("LC_COLLATE '\(collation)'") }
            if !opts.isEmpty { sql += " WITH " + opts.joined(separator: " ") }
        case .mysql:
            if encoding != "Default" { sql += " CHARACTER SET \(encoding)" }
            if collation != "Default" { sql += " COLLATE \(collation)" }
        case .sqlite, .redis:
            break // SQLite/Redis don't support CREATE DATABASE
        case .mssql:
            break // SQL Server: use plain CREATE DATABASE without options for MVP
        case .clickhouse:
            break // ClickHouse: use plain CREATE DATABASE (options go via SETTINGS)
        case .mongodb:
            // MongoDB: use the adapter's createDatabase method
            skipExecuteRaw = true
            do {
                try await adapter.createDatabase(name: dbName)
            } catch {
                errorMessage = DataGridViewState.detailedErrorMessage(error)
                return
            }
        }

        do {
            if !skipExecuteRaw {
                _ = try await adapter.executeRaw(sql: sql)
            }
        } catch {
            errorMessage = DataGridViewState.detailedErrorMessage(error)
            return
        }

        // Refresh database list and switch
        if let databases = try? await adapter.listDatabases() {
            appState.availableDatabases = databases
        }
        dismiss()
        onCreated(dbName)
    }
}

// MARK: - Connection breadcrumb bar (Gridex style)

struct ConnectionBreadcrumb: View {
    @EnvironmentObject private var appState: AppState

    private var activeTablePath: String? {
        guard let activeId = appState.activeTabId,
              let tab = appState.tabs.first(where: { $0.id == activeId }) else { return nil }
        if let tableName = tab.tableName {
            let schema = tab.schema ?? "public"
            return "\(schema).\(tableName)"
        }
        if tab.type == .tableList {
            return tab.title // e.g. "Tables.public"
        }
        return nil
    }

    private var tagColor: Color {
        appState.activeConfig?.colorTag?.swiftUIColor ?? Color(red: 0.22, green: 0.54, blue: 0.87)
    }

    var body: some View {
        if let config = appState.activeConfig {
            HStack(spacing: 0) {
                Circle()
                    .fill(tagColor)
                    .frame(width: 8, height: 8)
                    .padding(.trailing, 6)

                Text(config.name.uppercased())
                    .font(.system(size: 11, weight: .bold))

                breadcrumbSep

                let dbLabel = appState.serverVersion.map { "\(config.databaseType.displayName) \($0)" } ?? config.databaseType.displayName
                Text(dbLabel)
                    .font(.system(size: 11))
                    .foregroundStyle(.secondary)

                if let ssl = appState.sslInfo {
                    breadcrumbColon
                    Text(ssl)
                        .font(.system(size: 11))
                        .foregroundStyle(.secondary)
                }

                if let user = config.username, !user.isEmpty {
                    breadcrumbColon
                    Text(user)
                        .font(.system(size: 11))
                        .foregroundStyle(.secondary)
                }

                if let db = config.database, !db.isEmpty {
                    breadcrumbColon
                    Text(db)
                        .font(.system(size: 11))
                        .foregroundStyle(.secondary)
                }

                if let path = activeTablePath {
                    breadcrumbColon
                    Text(path)
                        .font(.system(size: 11, weight: .semibold))
                }
            }
            .padding(.horizontal, 12)
            .frame(minWidth: 400)
        }
    }

    private var breadcrumbSep: some View {
        Text("  |  ")
            .font(.system(size: 11))
            .foregroundStyle(.tertiary)
    }

    private var breadcrumbColon: some View {
        Text("  :  ")
            .font(.system(size: 11))
            .foregroundStyle(.tertiary)
    }
}

// MARK: - Details panel (right side)

struct DetailsPanel: View {
    @EnvironmentObject private var appState: AppState
    @State private var activeTab = 0

    var body: some View {
        VStack(spacing: 0) {
            // Tab bar
            HStack(spacing: 0) {
                tabButton("Details", index: 0)
                tabButton("Assistant", index: 1)
            }
            .background(.bar)

            Divider()

            if activeTab == 0 {
                detailsContent
            } else {
                AIChatView()
            }
        }
    }

    private func tabButton(_ title: String, index: Int) -> some View {
        Button { activeTab = index } label: {
            Text(title)
                .font(.system(size: 12, weight: activeTab == index ? .semibold : .regular))
                .foregroundStyle(activeTab == index ? Color.primary : Color.secondary)
                .frame(maxWidth: .infinity)
                .padding(.vertical, 7)
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .overlay(alignment: .bottom) {
            if activeTab == index {
                Rectangle().fill(Color.accentColor).frame(height: 2)
            }
        }
    }

    @State private var detailsSearchText = ""

    private var detailsContent: some View {
        VStack(spacing: 0) {
            // Search bar
            HStack(spacing: 5) {
                Image(systemName: "magnifyingglass")
                    .font(.system(size: 11))
                    .foregroundStyle(.tertiary)
                TextField("Search for field...", text: $detailsSearchText)
                    .textFieldStyle(.plain)
                    .font(.system(size: 12))
                if !detailsSearchText.isEmpty {
                    Button {
                        detailsSearchText = ""
                    } label: {
                        Image(systemName: "xmark.circle.fill")
                            .font(.system(size: 10))
                            .foregroundStyle(.tertiary)
                    }
                    .buttonStyle(.plain)
                }
            }
            .padding(.horizontal, 8)
            .padding(.vertical, 6)

            Divider()

            if let details = appState.selectedRowDetails {
                let filtered: [(index: Int, column: String, value: String)] = {
                    let indexed = details.enumerated().map { (index: $0.offset, column: $0.element.column, value: $0.element.value) }
                    if detailsSearchText.isEmpty { return indexed }
                    return indexed.filter {
                        $0.column.localizedCaseInsensitiveContains(detailsSearchText) ||
                        $0.value.localizedCaseInsensitiveContains(detailsSearchText)
                    }
                }()
                ScrollView {
                    LazyVStack(spacing: 0) {
                        ForEach(filtered, id: \.index) { field in
                            DetailFieldRow(
                                column: field.column,
                                value: field.value,
                                onCommit: { newValue in
                                    appState.onDetailFieldEdit?(field.index, newValue)
                                }
                            )
                            Divider().padding(.leading, 10)
                        }
                    }
                }
            } else {
                VStack(spacing: 6) {
                    Text("No row selected")
                        .font(.system(size: 12))
                        .foregroundStyle(.secondary)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
    }

    private var emptyAssistant: some View {
        VStack(spacing: 8) {
            Image(systemName: "brain")
                .font(.system(size: 28))
                .foregroundStyle(.quaternary)
            Text("AI Assistant")
                .font(.system(size: 12))
                .foregroundStyle(.secondary)
            Button("Enable") { appState.aiPanelVisible = true }
                .font(.system(size: 12))
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

// MARK: - Detail Field Row

struct DetailFieldRow: View {
    let column: String
    let value: String
    let onCommit: (String) -> Void

    @State private var editText: String = ""
    @State private var isEditing = false
    @FocusState private var isFocused: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 3) {
            Text(column)
                .font(.system(size: 11, weight: .medium))
                .foregroundStyle(.secondary)
            if isEditing {
                TextField("", text: $editText, axis: .vertical)
                    .font(.system(size: 12))
                    .textFieldStyle(.plain)
                    .focused($isFocused)
                    .onSubmit {
                        commitEdit()
                    }
                    .onChange(of: isFocused) { _, focused in
                        if !focused {
                            commitEdit()
                        }
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
            } else {
                Text(value)
                    .font(.system(size: 12))
                    .foregroundStyle(value == "NULL" ? .tertiary : .primary)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .contentShape(Rectangle())
                    .onTapGesture {
                        editText = value == "NULL" ? "" : value
                        isEditing = true
                        isFocused = true
                    }
            }
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 6)
        .onChange(of: value) { _, newVal in
            // Sync if external change
            if !isEditing {
                editText = newVal
            }
        }
    }

    private func commitEdit() {
        isEditing = false
        let trimmed = editText
        if trimmed != value {
            onCommit(trimmed)
        }
    }
}

// MARK: - Connected Welcome

struct ConnectedWelcomeView: View {
    var body: some View {
        VStack(spacing: 0) {
            Spacer()

            VStack(spacing: 16) {
                // Placeholder illustration (robot + fishbowl style)
                ZStack {
                    Circle()
                        .fill(Color.secondary.opacity(0.06))
                        .frame(width: 120, height: 120)
                    Image(systemName: "terminal")
                        .font(.system(size: 38))
                        .foregroundStyle(Color.secondary.opacity(0.3))
                }

                VStack(spacing: 6) {
                    Text("Console log, please execute a query")
                        .font(.system(size: 13))
                        .foregroundStyle(.secondary)
                    Text("or select a table to see the log")
                        .font(.system(size: 13))
                        .foregroundStyle(.secondary)
                }
            }

            Spacer()

            // Bottom bar (matches Gridex style)
            Divider()
            HStack(spacing: 12) {
                Button {
                } label: {
                    Image(systemName: "trash")
                        .font(.system(size: 12))
                        .foregroundStyle(.secondary)
                }
                .buttonStyle(.plain)

                Divider().frame(height: 14)

                Toggle(isOn: .constant(true)) {
                    Text("Enable syntax highlighting")
                        .font(.system(size: 12))
                        .foregroundStyle(.secondary)
                }
                .toggleStyle(.checkbox)

                Spacer()
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 6)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

// MARK: - Resize Handle

/// Draggable vertical divider backed by an NSView to avoid SwiftUI jitter.
private struct ResizeHandle: NSViewRepresentable {
    @Binding var width: CGFloat
    let minWidth: CGFloat
    let maxWidth: CGFloat

    func makeNSView(context: Context) -> ResizeHandleView {
        let view = ResizeHandleView()
        view.setWidth = { newWidth in
            self.width = min(max(newWidth, self.minWidth), self.maxWidth)
        }
        view.getWidth = { self.width }
        return view
    }

    func updateNSView(_ nsView: ResizeHandleView, context: Context) {
        nsView.setWidth = { newWidth in
            self.width = min(max(newWidth, self.minWidth), self.maxWidth)
        }
        nsView.getWidth = { self.width }
    }
}

final class ResizeHandleView: NSView {
    var setWidth: ((CGFloat) -> Void)?
    var getWidth: (() -> CGFloat)?

    private var dragStartX: CGFloat = 0
    private var dragStartWidth: CGFloat = 0

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        wantsLayer = true
        layer?.backgroundColor = NSColor.separatorColor.cgColor
    }
    required init?(coder: NSCoder) { fatalError() }

    override var intrinsicContentSize: NSSize {
        NSSize(width: 1, height: NSView.noIntrinsicMetric)
    }

    override func resetCursorRects() {
        let hitArea = NSRect(x: -3, y: 0, width: 7, height: bounds.height)
        addCursorRect(hitArea, cursor: .resizeLeftRight)
    }

    override func mouseDown(with event: NSEvent) {
        dragStartX = event.locationInWindow.x
        dragStartWidth = getWidth?() ?? 0
    }

    override func mouseDragged(with event: NSEvent) {
        let deltaX = event.locationInWindow.x - dragStartX
        // Drag right (positive delta) → panel shrinks
        let newWidth = dragStartWidth - deltaX
        setWidth?(newWidth)
    }
}

// MARK: - TablePlus Import Sheet

struct ImportConnectionsSheet: View {
    @EnvironmentObject private var appState: AppState
    @Environment(\.dismiss) private var dismiss
    @State private var selectedSource: ImportSource?
    @State private var connections: [ImportedConnection] = []
    @State private var selected: Set<String> = []
    @State private var isLoading = false
    @State private var errorMessage: String?
    @State private var isImporting = false
    @State private var navicatFileURL: URL?

    private var availableSources: [ImportSource] {
        ImportSource.allCases
    }

    var body: some View {
        VStack(spacing: 0) {
            Text("Import Connections")
                .font(.system(size: 15, weight: .semibold))
                .padding(.top, 20)
                .padding(.bottom, 4)

            Text(selectedSource == nil ? "Select a source to import from" : "Select connections to import")
                .font(.system(size: 12))
                .foregroundStyle(.secondary)
                .padding(.bottom, 16)

            if selectedSource == nil {
                sourceSelectionView
            } else if isLoading {
                ProgressView()
                    .padding(40)
            } else if let error = errorMessage {
                errorView(error)
            } else if connections.isEmpty {
                emptyView
            } else {
                connectionsList
            }

            Divider()
            bottomBar
        }
        .frame(width: 520, height: 450)
    }

    private var sourceSelectionView: some View {
        VStack(spacing: 12) {
            ForEach(availableSources, id: \.self) { source in
                SourceRow(source: source) {
                    if source == .navicat {
                        pickNavicatFile()
                    } else {
                        selectedSource = source
                        loadConnections(from: source)
                    }
                }
            }

            if availableSources.isEmpty {
                VStack(spacing: 12) {
                    Image(systemName: "app.dashed")
                        .font(.system(size: 32))
                        .foregroundStyle(.tertiary)
                    Text("No supported database tools detected")
                        .font(.system(size: 12))
                        .foregroundStyle(.secondary)
                }
                .padding(40)
            }
        }
        .padding(.horizontal, 20)
        .frame(maxHeight: .infinity)
    }

    private func errorView(_ error: String) -> some View {
        VStack(spacing: 12) {
            Image(systemName: "exclamationmark.triangle")
                .font(.system(size: 32))
                .foregroundStyle(.orange)
            Text(error)
                .font(.system(size: 12))
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
        }
        .padding(40)
    }

    private var emptyView: some View {
        VStack(spacing: 12) {
            Image(systemName: "tray")
                .font(.system(size: 32))
                .foregroundStyle(.tertiary)
            Text("No connections found")
                .font(.system(size: 12))
                .foregroundStyle(.secondary)
        }
        .padding(40)
    }

    private var connectionsList: some View {
        List(connections, id: \.id) { conn in
            HStack(spacing: 12) {
                Image(systemName: selected.contains(conn.id) ? "checkmark.circle.fill" : "circle")
                    .font(.system(size: 16))
                    .foregroundStyle(selected.contains(conn.id) ? Color.blue : Color.secondary.opacity(0.4))

                DatabaseTypeIcon(type: conn.databaseType, size: 28)

                VStack(alignment: .leading, spacing: 2) {
                    HStack(spacing: 6) {
                        Text(conn.name)
                            .font(.system(size: 13, weight: .medium))

                        if let group = conn.group {
                            Text(group)
                                .font(.system(size: 9, weight: .medium))
                                .foregroundStyle(.secondary)
                                .padding(.horizontal, 5)
                                .padding(.vertical, 1)
                                .background(Color.secondary.opacity(0.1))
                                .clipShape(RoundedRectangle(cornerRadius: 3))
                        }
                    }

                    Text(connectionSubtitle(conn))
                        .font(.system(size: 11, design: .monospaced))
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                }

                Spacer()

                Text(conn.databaseType.displayName)
                    .font(.system(size: 10, weight: .medium))
                    .foregroundStyle(.tertiary)
            }
            .padding(.vertical, 4)
            .contentShape(Rectangle())
            .onTapGesture {
                if selected.contains(conn.id) {
                    selected.remove(conn.id)
                } else {
                    selected.insert(conn.id)
                }
            }
        }
        .listStyle(.plain)
    }

    private var bottomBar: some View {
        HStack {
            if selectedSource != nil {
                Button {
                    selectedSource = nil
                    connections = []
                    selected = []
                    errorMessage = nil
                } label: {
                    HStack(spacing: 4) {
                        Image(systemName: "chevron.left")
                            .font(.system(size: 10, weight: .semibold))
                        Text("Back")
                    }
                }
                .buttonStyle(.plain)
                .foregroundStyle(.blue)
            }

            if !connections.isEmpty && errorMessage == nil {
                Button(selected.count == connections.count ? "Deselect All" : "Select All") {
                    if selected.count == connections.count {
                        selected.removeAll()
                    } else {
                        selected = Set(connections.map(\.id))
                    }
                }
                .buttonStyle(.plain)
                .foregroundStyle(.blue)
            }

            Spacer()

            Button("Cancel") { dismiss() }
                .keyboardShortcut(.cancelAction)

            if selectedSource != nil && !connections.isEmpty {
                Button {
                    Task { await importSelected() }
                } label: {
                    if isImporting {
                        ProgressView().controlSize(.small)
                    }
                    Text("Import \(selected.count)")
                }
                .buttonStyle(.borderedProminent)
                .disabled(selected.isEmpty || isImporting)
                .keyboardShortcut(.defaultAction)
            }
        }
        .padding(.horizontal, 20)
        .padding(.vertical, 14)
    }

    private func connectionSubtitle(_ conn: ImportedConnection) -> String {
        if conn.databaseType == .sqlite {
            return conn.filePath ?? "No file"
        }
        var parts: [String] = []
        if let host = conn.host {
            parts.append(host)
            if let port = conn.port {
                parts.append(":\(port)")
            }
        }
        if let db = conn.database, !db.isEmpty {
            parts.append(" / \(db)")
        }
        return parts.joined()
    }

    private func pickNavicatFile() {
        let panel = NSOpenPanel()
        panel.title = "Select Navicat Export File (.ncx)"
        panel.allowsMultipleSelection = false
        panel.canChooseDirectories = false
        panel.canChooseFiles = true
        panel.allowedContentTypes = [.xml, .data]

        if panel.runModal() == .OK, let url = panel.url {
            selectedSource = .navicat
            loadNavicatConnections(from: url)
        }
    }

    private func loadNavicatConnections(from url: URL) {
        isLoading = true
        errorMessage = nil

        do {
            connections = try NavicatImporter.importFromNCX(at: url)
            selected = Set(connections.map(\.id))
        } catch {
            errorMessage = error.localizedDescription
        }

        isLoading = false
    }

    private func loadConnections(from source: ImportSource) {
        isLoading = true
        errorMessage = nil

        do {
            switch source {
            case .tableplus:
                connections = try TablePlusImporter.importConnections()
            case .dbeaver:
                connections = try DBeaverImporter.importConnections()
            case .datagrip:
                connections = try DataGripImporter.importConnections()
            case .navicat:
                break
            }
            selected = Set(connections.map(\.id))
        } catch {
            errorMessage = error.localizedDescription
        }

        isLoading = false
    }

    private func importSelected() async {
        isImporting = true

        let toImport = connections.filter { selected.contains($0.id) }
        let keychain = appState.container.keychainService

        for imported in toImport {
            let config = imported.toConnectionConfig()

            do {
                try await appState.container.connectionRepository.save(config)

                if let pw = imported.password, !pw.isEmpty {
                    try? keychain.save(key: "db.password.\(config.id.uuidString)", value: pw)
                }

                if let group = imported.group {
                    appState.connectionGroups.insert(group)
                }
            } catch {
                // Continue importing others
            }
        }

        await appState.loadSavedConnections()
        isImporting = false
        dismiss()
    }
}

struct SourceRow: View {
    let source: ImportSource
    let action: () -> Void
    @State private var isHovered = false

    private var isEnabled: Bool {
        source.isInstalled || source == .navicat
    }

    var body: some View {
        Button(action: action) {
            HStack(spacing: 14) {
                ZStack {
                    RoundedRectangle(cornerRadius: 8)
                        .fill(sourceGradient)
                        .frame(width: 40, height: 40)
                    Text(source.iconLetter)
                        .font(.system(size: 14, weight: .bold, design: .rounded))
                        .foregroundStyle(.white)
                }

                VStack(alignment: .leading, spacing: 2) {
                    Text(source.rawValue)
                        .font(.system(size: 14, weight: .medium))
                    Text(sourceDescription)
                        .font(.system(size: 11))
                        .foregroundStyle(.secondary)
                }

                Spacer()

                if source.isInstalled {
                    Text("Installed")
                        .font(.system(size: 10, weight: .medium))
                        .foregroundStyle(.green)
                        .padding(.horizontal, 8)
                        .padding(.vertical, 3)
                        .background(Color.green.opacity(0.1))
                        .clipShape(RoundedRectangle(cornerRadius: 4))
                } else if source == .navicat {
                    Text("Select .ncx")
                        .font(.system(size: 10, weight: .medium))
                        .foregroundStyle(.blue)
                        .padding(.horizontal, 8)
                        .padding(.vertical, 3)
                        .background(Color.blue.opacity(0.1))
                        .clipShape(RoundedRectangle(cornerRadius: 4))
                } else {
                    Text("Not Found")
                        .font(.system(size: 10, weight: .medium))
                        .foregroundStyle(.secondary)
                        .padding(.horizontal, 8)
                        .padding(.vertical, 3)
                        .background(Color.secondary.opacity(0.1))
                        .clipShape(RoundedRectangle(cornerRadius: 4))
                }

                Image(systemName: "chevron.right")
                    .font(.system(size: 12, weight: .semibold))
                    .foregroundStyle(.tertiary)
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 12)
            .background(isHovered && isEnabled ? Color.primary.opacity(0.05) : Color.clear)
            .clipShape(RoundedRectangle(cornerRadius: 10))
            .overlay(
                RoundedRectangle(cornerRadius: 10)
                    .stroke(Color.primary.opacity(0.08), lineWidth: 1)
            )
        }
        .buttonStyle(.plain)
        .onHover { isHovered = $0 }
        .disabled(!isEnabled)
        .opacity(source.isInstalled || source == .navicat ? 1 : 0.5)
    }

    private var sourceGradient: LinearGradient {
        switch source {
        case .tableplus:
            return LinearGradient(colors: [Color(red: 0.95, green: 0.55, blue: 0.15), Color(red: 0.85, green: 0.40, blue: 0.10)], startPoint: .top, endPoint: .bottom)
        case .dbeaver:
            return LinearGradient(colors: [Color(red: 0.55, green: 0.45, blue: 0.35), Color(red: 0.40, green: 0.30, blue: 0.25)], startPoint: .top, endPoint: .bottom)
        case .datagrip:
            return LinearGradient(colors: [Color(red: 0.20, green: 0.75, blue: 0.60), Color(red: 0.10, green: 0.55, blue: 0.45)], startPoint: .top, endPoint: .bottom)
        case .navicat:
            return LinearGradient(colors: [Color(red: 0.0, green: 0.65, blue: 0.85), Color(red: 0.0, green: 0.50, blue: 0.70)], startPoint: .top, endPoint: .bottom)
        }
    }

    private var sourceDescription: String {
        switch source {
        case .tableplus: return "Import from TablePlus favorites"
        case .dbeaver: return "Import from DBeaver workspace"
        case .datagrip: return "Import from JetBrains DataGrip"
        case .navicat: return "Import from Navicat .ncx export file"
        }
    }
}
