// MCPActivityView.swift
// Gridex
//
// MCP audit log and activity viewer.

import SwiftUI

struct MCPActivityView: View {
    @ObservedObject var state: MCPWindowState

    @State private var selectedEntryId: UUID?
    @State private var filterTool: String? = nil
    @State private var filterStatus: MCPAuditStatus? = nil
    @State private var searchText = ""
    @State private var sortOrder: [KeyPathComparator<MCPAuditEntry>] = [
        KeyPathComparator(\.timestamp, order: .reverse)
    ]

    private var selectedEntry: MCPAuditEntry? {
        guard let id = selectedEntryId else { return nil }
        return state.fullActivity.first { $0.id == id }
    }

    var body: some View {
        VStack(spacing: 0) {
            filterBar
            Divider()
            HSplitView {
                tableSection
                    .frame(minWidth: 440)

                if let entry = selectedEntry {
                    detailPanel(entry)
                        .frame(minWidth: 280, idealWidth: 340, maxWidth: 420)
                }
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .task {
            await state.loadFullActivity()
        }
    }

    // MARK: - Filter Bar

    private var filterBar: some View {
        HStack(spacing: 10) {
            HStack(spacing: 6) {
                Image(systemName: "magnifyingglass")
                    .foregroundStyle(.secondary)
                    .font(.callout)
                TextField("Search tool, SQL, or client", text: $searchText)
                    .textFieldStyle(.plain)
            }
            .padding(.horizontal, 8)
            .padding(.vertical, 5)
            .background(Color(nsColor: .controlBackgroundColor))
            .overlay(
                RoundedRectangle(cornerRadius: 6)
                    .stroke(Color.gray.opacity(0.2), lineWidth: 1)
            )
            .clipShape(RoundedRectangle(cornerRadius: 6))
            .frame(maxWidth: 260)

            Picker("Tool", selection: $filterTool) {
                Text("All Tools").tag(nil as String?)
                Divider()
                ForEach(uniqueTools, id: \.self) { tool in
                    Text(tool).tag(tool as String?)
                }
            }
            .labelsHidden()
            .frame(width: 140)

            Picker("Status", selection: $filterStatus) {
                Text("All Status").tag(nil as MCPAuditStatus?)
                Divider()
                Text("Success").tag(MCPAuditStatus.success as MCPAuditStatus?)
                Text("Error").tag(MCPAuditStatus.error as MCPAuditStatus?)
                Text("Denied").tag(MCPAuditStatus.denied as MCPAuditStatus?)
            }
            .labelsHidden()
            .frame(width: 120)

            Spacer()

            Button {
                Task { await state.loadFullActivity(force: true) }
            } label: {
                Image(systemName: "arrow.clockwise")
            }
            .help("Refresh")

            Menu {
                Button("Export as JSON…") { exportLog() }
                Divider()
                Button("Clear Log…", role: .destructive) { clearLog() }
            } label: {
                Image(systemName: "ellipsis.circle")
            }
            .menuStyle(.borderlessButton)
            .fixedSize()
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
    }

    // MARK: - Table

    @ViewBuilder
    private var tableSection: some View {
        if state.isLoadingActivity && state.fullActivity.isEmpty {
            ProgressView()
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        } else if filteredEntries.isEmpty {
            ContentUnavailableView(
                "No Activity",
                systemImage: "waveform.path.ecg",
                description: Text("MCP tool invocations will appear here.")
            )
        } else {
            Table(filteredEntries, selection: $selectedEntryId, sortOrder: $sortOrder) {
                TableColumn("Time", value: \.timestamp) { entry in
                    Text(formatTime(entry.timestamp))
                        .font(.system(.caption, design: .monospaced))
                        .foregroundStyle(.secondary)
                }
                .width(min: 70, ideal: 80)

                TableColumn("Tool", value: \.tool) { entry in
                    HStack(spacing: 6) {
                        Circle()
                            .fill(colorForTier(entry.tier))
                            .frame(width: 6, height: 6)
                        Text(entry.tool)
                            .font(.system(.caption, design: .monospaced))
                    }
                }
                .width(min: 120, ideal: 160)

                TableColumn("Client") { entry in
                    Text(entry.client.name)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                .width(min: 80, ideal: 100)

                TableColumn("Status") { entry in
                    let status = MCPAuditStatus(rawValue: entry.result.status) ?? .success
                    Text(status.rawValue)
                        .font(.caption)
                        .foregroundStyle(colorForStatus(status))
                }
                .width(60)

                TableColumn("Duration") { entry in
                    Text("\(entry.result.durationMs)ms")
                        .font(.system(.caption, design: .monospaced))
                        .foregroundStyle(.secondary)
                        .monospacedDigit()
                }
                .width(70)
            }
            .tableStyle(.inset(alternatesRowBackgrounds: true))
        }
    }

    // MARK: - Detail

    private func detailPanel(_ entry: MCPAuditEntry) -> some View {
        Form {
            Section("Event") {
                LabeledContent("Tool", value: entry.tool)
                LabeledContent("Tier", value: "Tier \(entry.tier)")
                LabeledContent("Time", value: formatFullTime(entry.timestamp))
                LabeledContent("Event ID") {
                    Text(entry.eventId.uuidString.prefix(8) + "…")
                        .font(.system(.caption, design: .monospaced))
                        .textSelection(.enabled)
                }
            }

            Section("Client") {
                LabeledContent("Name", value: entry.client.name)
                LabeledContent("Version", value: entry.client.version)
                LabeledContent("Transport", value: entry.client.transport)
            }

            if entry.connectionId != nil || entry.connectionType != nil {
                Section("Connection") {
                    if let connType = entry.connectionType {
                        LabeledContent("Database", value: connType)
                    }
                    if let connId = entry.connectionId {
                        LabeledContent("ID") {
                            Text(connId.uuidString.prefix(8) + "…")
                                .font(.system(.caption, design: .monospaced))
                                .textSelection(.enabled)
                        }
                    }
                }
            }

            Section("Result") {
                LabeledContent("Status") {
                    let status = MCPAuditStatus(rawValue: entry.result.status) ?? .success
                    Text(status.rawValue.capitalized)
                        .foregroundStyle(colorForStatus(status))
                }
                LabeledContent("Duration", value: "\(entry.result.durationMs)ms")
                if let rows = entry.result.rowsReturned {
                    LabeledContent("Rows returned", value: "\(rows)")
                }
                if let affected = entry.result.rowsAffected {
                    LabeledContent("Rows affected", value: "\(affected)")
                }
            }

            if let sql = entry.input.sqlPreview {
                Section("SQL") {
                    Text(sql)
                        .font(.system(.caption, design: .monospaced))
                        .textSelection(.enabled)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(8)
                        .background(Color(nsColor: .textBackgroundColor))
                        .clipShape(RoundedRectangle(cornerRadius: 4))
                }
            }

            if let error = entry.error {
                Section("Error") {
                    Text(error)
                        .font(.caption)
                        .foregroundStyle(.red)
                        .textSelection(.enabled)
                }
            }
        }
        .formStyle(.grouped)
    }

    // MARK: - Helpers

    private var filteredEntries: [MCPAuditEntry] {
        var result = state.fullActivity

        if let tool = filterTool {
            result = result.filter { $0.tool == tool }
        }

        if let status = filterStatus {
            result = result.filter { $0.result.status == status.rawValue }
        }

        if !searchText.isEmpty {
            result = result.filter {
                $0.tool.localizedCaseInsensitiveContains(searchText) ||
                $0.client.name.localizedCaseInsensitiveContains(searchText) ||
                ($0.input.sqlPreview?.localizedCaseInsensitiveContains(searchText) ?? false)
            }
        }

        return result.sorted(using: sortOrder)
    }

    private var uniqueTools: [String] {
        Array(Set(state.fullActivity.map(\.tool))).sorted()
    }

    private func exportLog() {
        // TODO: Implement export
    }

    private func clearLog() {
        // TODO: Implement clear with confirmation
    }

    private func formatTime(_ date: Date) -> String {
        let formatter = DateFormatter()
        formatter.dateFormat = "HH:mm:ss"
        return formatter.string(from: date)
    }

    private func formatFullTime(_ date: Date) -> String {
        let formatter = DateFormatter()
        formatter.dateStyle = .medium
        formatter.timeStyle = .medium
        return formatter.string(from: date)
    }

    private func colorForTier(_ tier: Int) -> Color {
        switch tier {
        case 1: return .blue
        case 2: return .green
        case 3: return .orange
        case 4: return .red
        case 5: return .purple
        default: return .gray
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
}
