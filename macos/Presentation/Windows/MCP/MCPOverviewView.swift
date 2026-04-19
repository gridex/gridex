// MCPOverviewView.swift
// Gridex
//
// MCP Server dashboard overview.

import SwiftUI

struct MCPOverviewView: View {
    @AppStorage("mcp.enabled") private var mcpEnabled = false
    @AppStorage("mcp.httpEnabled") private var httpEnabled = false
    @AppStorage("mcp.httpPort") private var httpPort = 3333

    @ObservedObject var state: MCPWindowState
    let switchTab: (MCPTab) -> Void

    var body: some View {
        Form {
            Section {
                serverStatusRow
            }

            Section("Access") {
                LabeledContent("Locked") {
                    accessValue(count: countFor(.locked), color: .red)
                }
                LabeledContent("Read-only") {
                    accessValue(count: countFor(.readOnly), color: .blue)
                }
                LabeledContent("Read-write") {
                    accessValue(count: countFor(.readWrite), color: .green)
                }

                Button("Manage Connections…") {
                    switchTab(.connections)
                }
                .controlSize(.small)
            }

            Section("Transport") {
                LabeledContent("stdio") {
                    HStack(spacing: 6) {
                        Text("Local clients")
                            .foregroundStyle(.secondary)
                        Image(systemName: "checkmark.circle.fill")
                            .foregroundStyle(.green)
                            .font(.caption)
                    }
                }

                Toggle("HTTP", isOn: $httpEnabled)

                if httpEnabled {
                    LabeledContent("Port") {
                        HStack(spacing: 4) {
                            TextField("", value: $httpPort, format: .number.grouping(.never))
                                .textFieldStyle(.roundedBorder)
                                .frame(width: 80)
                                .multilineTextAlignment(.trailing)
                            Text("localhost only")
                                .foregroundStyle(.secondary)
                                .font(.caption)
                        }
                    }
                }
            }

            Section {
                if state.recentActivity.isEmpty {
                    HStack {
                        Text("No activity yet")
                            .foregroundStyle(.secondary)
                            .font(.callout)
                        Spacer()
                    }
                    .padding(.vertical, 4)
                } else {
                    ForEach(state.recentActivity.prefix(5)) { entry in
                        activityRow(entry)
                    }

                    Button("View All Activity…") {
                        switchTab(.activity)
                    }
                    .controlSize(.small)
                }
            } header: {
                HStack {
                    Text("Recent Activity")
                    Spacer()
                    if !state.recentActivity.isEmpty {
                        Text("\(state.recentActivity.count) events")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            .textCase(nil)
                    }
                }
            }

            if !state.connections.isEmpty {
                Section("Connections") {
                    ForEach(state.connections.prefix(5)) { config in
                        connectionRow(config)
                    }

                    if state.connections.count > 5 {
                        Button("Show all \(state.connections.count) connections…") {
                            switchTab(.connections)
                        }
                        .controlSize(.small)
                    }
                }
            }
        }
        .formStyle(.grouped)
    }

    // MARK: - Server Status Row

    private var serverStatusRow: some View {
        HStack(spacing: 14) {
            ZStack {
                Circle()
                    .fill((mcpEnabled ? Color.green : Color.secondary).opacity(0.15))
                    .frame(width: 38, height: 38)

                Image(systemName: mcpEnabled ? "checkmark.circle.fill" : "pause.circle.fill")
                    .font(.system(size: 22))
                    .foregroundStyle(mcpEnabled ? Color.green : Color.secondary)
            }

            VStack(alignment: .leading, spacing: 2) {
                Text(mcpEnabled ? "MCP Server is running" : "MCP Server is stopped")
                    .font(.headline)
                Text(overviewStatusDetail)
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
            }

            Spacer()
        }
        .padding(.vertical, 6)
    }

    private var overviewStatusDetail: String {
        if !mcpEnabled {
            return "Start the server from the header to allow AI clients to access your databases."
        }
        var parts: [String] = []
        if state.uptime > 0 {
            parts.append("Running for \(formatUptime(state.uptime))")
        }
        let activeCount = state.connections.filter { $0.mcpMode != .locked }.count
        parts.append("\(activeCount) of \(state.connections.count) connection\(state.connections.count == 1 ? "" : "s") exposed")
        return parts.joined(separator: " · ")
    }

    // MARK: - Rows

    private func accessValue(count: Int, color: Color) -> some View {
        HStack(spacing: 8) {
            Circle()
                .fill(color)
                .frame(width: 6, height: 6)
            Text("\(count)")
                .font(.system(.callout, design: .rounded).weight(.medium))
                .monospacedDigit()
                .foregroundStyle(count > 0 ? .primary : .secondary)
        }
    }

    private func activityRow(_ entry: MCPAuditEntry) -> some View {
        LabeledContent {
            HStack(spacing: 8) {
                Text("\(entry.result.durationMs)ms")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .monospacedDigit()

                Circle()
                    .fill(colorForStatus(MCPAuditStatus(rawValue: entry.result.status) ?? .success))
                    .frame(width: 6, height: 6)
            }
        } label: {
            VStack(alignment: .leading, spacing: 1) {
                HStack(spacing: 6) {
                    Text(entry.tool)
                        .font(.system(.callout, design: .monospaced))
                    Text("·")
                        .foregroundStyle(.tertiary)
                    Text(entry.client.name)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                Text(formatRelativeTime(entry.timestamp))
                    .font(.caption2)
                    .foregroundStyle(.tertiary)
            }
        }
    }

    private func connectionRow(_ config: ConnectionConfig) -> some View {
        LabeledContent {
            Text(config.mcpMode.displayName)
                .font(.caption)
                .foregroundStyle(modeColor(config.mcpMode))
        } label: {
            HStack(spacing: 8) {
                Image(systemName: config.databaseType.iconName)
                    .foregroundStyle(.secondary)
                    .frame(width: 16)

                Text(config.name)

                Text(config.displayHost)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }
        }
    }

    // MARK: - Helpers

    private func countFor(_ mode: MCPConnectionMode) -> Int {
        state.connections.filter { $0.mcpMode == mode }.count
    }

    private func modeColor(_ mode: MCPConnectionMode) -> Color {
        switch mode {
        case .locked: return .red
        case .readOnly: return .blue
        case .readWrite: return .green
        }
    }

    private func colorForStatus(_ status: MCPAuditStatus) -> Color {
        switch status {
        case .success: return .green
        case .error: return .red
        case .denied: return .orange
        case .timeout: return .yellow
        }
    }

    private func formatRelativeTime(_ date: Date) -> String {
        let interval = Date().timeIntervalSince(date)
        if interval < 60 { return "just now" }
        if interval < 3600 { return "\(Int(interval / 60))m ago" }
        if interval < 86400 { return "\(Int(interval / 3600))h ago" }
        return "\(Int(interval / 86400))d ago"
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
