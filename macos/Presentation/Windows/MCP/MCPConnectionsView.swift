// MCPConnectionsView.swift
// Gridex
//
// MCP connection access management.

import SwiftUI

struct MCPConnectionsView: View {
    @ObservedObject var state: MCPWindowState

    @State private var searchText = ""
    @State private var filterMode: MCPConnectionMode? = nil
    @State private var selection: UUID?
    @State private var sortOrder: [KeyPathComparator<ConnectionConfig>] = [
        KeyPathComparator(\.name)
    ]

    var body: some View {
        VStack(spacing: 0) {
            filterBar
            Divider()
            contentBody
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    // MARK: - Filter bar

    private var filterBar: some View {
        HStack(spacing: 10) {
            HStack(spacing: 6) {
                Image(systemName: "magnifyingglass")
                    .foregroundStyle(.secondary)
                    .font(.callout)
                TextField("Search connections", text: $searchText)
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
            .frame(maxWidth: 280)

            Picker("Filter", selection: $filterMode) {
                Text("All Access").tag(nil as MCPConnectionMode?)
                Divider()
                ForEach(MCPConnectionMode.allCases) { mode in
                    Text(mode.displayName).tag(mode as MCPConnectionMode?)
                }
            }
            .labelsHidden()
            .frame(width: 140)

            Spacer()

            Text("\(filteredConnections.count) of \(state.connections.count)")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
    }

    // MARK: - Content

    @ViewBuilder
    private var contentBody: some View {
        if filteredConnections.isEmpty {
            ContentUnavailableView(
                searchText.isEmpty && filterMode == nil ? "No Connections" : "No Results",
                systemImage: searchText.isEmpty && filterMode == nil
                    ? "externaldrive.badge.questionmark"
                    : "magnifyingglass",
                description: Text(
                    searchText.isEmpty && filterMode == nil
                        ? "Add a database connection in Gridex to enable MCP access."
                        : "Try a different search or filter."
                )
            )
        } else {
            connectionsTable
        }
    }

    private var connectionsTable: some View {
        Table(filteredConnections, selection: $selection, sortOrder: $sortOrder) {
            TableColumn("") { config in
                Circle()
                    .fill(modeColor(config.mcpMode))
                    .frame(width: 8, height: 8)
            }
            .width(16)

            TableColumn("Name", value: \.name) { config in
                HStack(spacing: 6) {
                    Image(systemName: config.databaseType.iconName)
                        .foregroundStyle(.secondary)
                    Text(config.name)
                }
            }
            .width(min: 120, ideal: 180)

            TableColumn("Type") { config in
                Text(config.databaseType.displayName)
                    .foregroundStyle(.secondary)
            }
            .width(min: 70, ideal: 90)

            TableColumn("Host") { config in
                Text(config.displayHost)
                    .foregroundStyle(.secondary)
            }
            .width(min: 120, ideal: 200)

            TableColumn("Access") { config in
                Picker("", selection: Binding(
                    get: { config.mcpMode },
                    set: { newMode in
                        Task { await state.updateConnectionMode(config, newMode) }
                    }
                )) {
                    ForEach(MCPConnectionMode.allCases) { mode in
                        Text(mode.displayName).tag(mode)
                    }
                }
                .labelsHidden()
                .pickerStyle(.menu)
            }
            .width(min: 110, ideal: 130)

            TableColumn("Description") { config in
                Text(config.mcpMode.description)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            .width(min: 140, ideal: 220)
        }
        .tableStyle(.inset(alternatesRowBackgrounds: true))
    }

    // MARK: - Helpers

    private var filteredConnections: [ConnectionConfig] {
        var result = state.connections

        if !searchText.isEmpty {
            result = result.filter {
                $0.name.localizedCaseInsensitiveContains(searchText) ||
                $0.displayHost.localizedCaseInsensitiveContains(searchText)
            }
        }

        if let mode = filterMode {
            result = result.filter { $0.mcpMode == mode }
        }

        return result.sorted(using: sortOrder)
    }

    private func modeColor(_ mode: MCPConnectionMode) -> Color {
        switch mode {
        case .locked: return .red
        case .readOnly: return .blue
        case .readWrite: return .green
        }
    }
}
