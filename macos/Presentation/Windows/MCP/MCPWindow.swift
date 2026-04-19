// MCPWindow.swift
// Gridex
//
// Dedicated MCP Server management window.

import SwiftUI

@MainActor
final class MCPWindowState: ObservableObject {
    @Published var connections: [ConnectionConfig] = []
    @Published var recentActivity: [MCPAuditEntry] = []
    @Published var fullActivity: [MCPAuditEntry] = []
    @Published var uptime: TimeInterval = 0
    @Published var isLoadingActivity = false

    private var timer: Timer?
    private var hasLoadedConnections = false
    private var hasLoadedActivity = false

    private let connectionRepository: any ConnectionRepository = DependencyContainer.shared.connectionRepository

    func start() {
        startTimer()
        Task { await loadConnections() }
        Task { await loadRecentActivity() }
    }

    func stop() {
        timer?.invalidate()
        timer = nil
    }

    func loadConnections(force: Bool = false) async {
        if hasLoadedConnections && !force { return }
        let configs = (try? await connectionRepository.fetchAll()) ?? []
        connections = configs
        hasLoadedConnections = true
    }

    func loadRecentActivity() async {
        let auditLogger = await DependencyContainer.shared.mcpServer.auditLog
        let recent = (try? await auditLogger.recentEntries(limit: 10)) ?? []
        recentActivity = recent
    }

    func loadFullActivity(force: Bool = false) async {
        if hasLoadedActivity && !force { return }
        isLoadingActivity = true
        let auditLogger = await DependencyContainer.shared.mcpServer.auditLog
        let all = (try? await auditLogger.recentEntries(limit: 500)) ?? []
        fullActivity = all
        isLoadingActivity = false
        hasLoadedActivity = true
    }

    func updateConnectionMode(_ config: ConnectionConfig, _ mode: MCPConnectionMode) async {
        var updated = config
        updated.mcpMode = mode
        try? await connectionRepository.update(updated)
        await DependencyContainer.shared.mcpServer.setConnectionMode(mode, for: config.id)
        if let idx = connections.firstIndex(where: { $0.id == config.id }) {
            connections[idx] = updated
        }
    }

    private func startTimer() {
        refreshUptime()
        timer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.refreshUptime() }
        }
    }

    private func refreshUptime() {
        let enabled = UserDefaults.standard.bool(forKey: "mcp.enabled")
        let startTime = UserDefaults.standard.double(forKey: "mcp.startTime")
        if enabled && startTime > 0 {
            uptime = Date().timeIntervalSince1970 - startTime
        } else {
            uptime = 0
        }
    }
}

struct MCPWindow: View {
    @AppStorage("mcp.enabled") private var mcpEnabled = false
    @AppStorage("mcp.startTime") private var serverStartTime: Double = 0

    @State private var selectedTab: MCPTab = .overview
    @StateObject private var state = MCPWindowState()

    var body: some View {
        VStack(spacing: 0) {
            headerBar
            Divider()
            tabBar
            Divider()
            content
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
        .frame(minWidth: 860, minHeight: 580)
        .background(Color(nsColor: .windowBackgroundColor))
        .onAppear { state.start() }
        .onDisappear { state.stop() }
        .onChange(of: mcpEnabled) { _, newValue in
            if newValue {
                serverStartTime = Date().timeIntervalSince1970
            } else {
                serverStartTime = 0
            }
        }
    }

    // MARK: - Header

    private var headerBar: some View {
        HStack(spacing: 12) {
            HStack(spacing: 8) {
                ZStack {
                    Circle()
                        .fill((mcpEnabled ? Color.green : Color.secondary).opacity(0.18))
                        .frame(width: 24, height: 24)
                    Circle()
                        .fill(mcpEnabled ? Color.green : Color.secondary)
                        .frame(width: 8, height: 8)
                }

                VStack(alignment: .leading, spacing: 1) {
                    Text("MCP Server")
                        .font(.system(size: 13, weight: .semibold))
                    Text(statusDetail)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .monospacedDigit()
                }
            }

            Spacer()

            Button {
                toggleServer()
            } label: {
                Text(mcpEnabled ? "Stop Server" : "Start Server")
                    .frame(minWidth: 84)
            }
            .buttonStyle(.borderedProminent)
            .controlSize(.regular)
            .tint(mcpEnabled ? .red : .accentColor)
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 10)
    }

    private var statusDetail: String {
        if !mcpEnabled { return "Server is stopped" }
        if state.uptime > 0 {
            return "Running · \(formatUptime(state.uptime))"
        }
        return "Running"
    }

    private func toggleServer() {
        let newValue = !mcpEnabled
        mcpEnabled = newValue
        Task {
            if newValue {
                await DependencyContainer.shared.bootstrapMCPServer()
            } else {
                await DependencyContainer.shared.mcpServer.stop()
            }
            MCPStatusBarController.shared.refresh()
        }
    }

    // MARK: - Tab Bar

    private var tabBar: some View {
        HStack {
            Picker("", selection: $selectedTab) {
                ForEach(MCPTab.allCases) { tab in
                    Text(tab.title).tag(tab)
                }
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            .frame(maxWidth: 520)
        }
        .frame(maxWidth: .infinity)
        .padding(.horizontal, 16)
        .padding(.vertical, 8)
    }

    // MARK: - Content

    @ViewBuilder
    private var content: some View {
        switch selectedTab {
        case .overview:
            MCPOverviewView(state: state, switchTab: { selectedTab = $0 })
        case .connections:
            MCPConnectionsView(state: state)
        case .activity:
            MCPActivityView(state: state)
        case .setup:
            MCPSetupView()
        case .config:
            MCPAdvancedView()
        }
    }

    private func formatUptime(_ interval: TimeInterval) -> String {
        let total = Int(interval)
        let h = total / 3600
        let m = (total % 3600) / 60
        let s = total % 60
        if h > 0 { return "\(h)h \(m)m" }
        if m > 0 { return "\(m)m \(s)s" }
        return "\(s)s"
    }
}

// MARK: - Tab Model

enum MCPTab: String, Hashable, CaseIterable, Identifiable {
    case overview
    case connections
    case activity
    case setup
    case config

    var id: String { rawValue }

    var title: String {
        switch self {
        case .overview: return "Overview"
        case .connections: return "Connections"
        case .activity: return "Activity"
        case .setup: return "Setup"
        case .config: return "Config"
        }
    }
}

// MARK: - Window Controller

class MCPWindowController: NSWindowController {
    static var shared: MCPWindowController?

    convenience init() {
        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 1000, height: 700),
            styleMask: [.titled, .closable, .miniaturizable, .resizable],
            backing: .buffered,
            defer: false
        )
        window.title = "MCP Server"
        window.isOpaque = true
        window.backgroundColor = NSColor.windowBackgroundColor
        window.center()
        window.contentView = NSHostingView(rootView: MCPWindow())
        window.isReleasedWhenClosed = false

        self.init(window: window)
    }

    static func show() {
        if shared == nil {
            shared = MCPWindowController()
        }
        shared?.showWindow(nil)
        shared?.window?.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }
}
