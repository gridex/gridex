// RedisServerInfoView.swift
// Gridex
//
// Dashboard showing Redis server INFO sections.

import SwiftUI

struct RedisServerInfoView: View {
    @EnvironmentObject private var appState: AppState

    let redisContext: AppState.RedisTabContext

    @State private var sections: [RedisInfoSection] = []
    @State private var databaseSize: Int?
    @State private var isLoading = true
    @State private var errorMessage: String?
    @State private var autoRefresh = false
    @State private var refreshTask: Task<Void, Never>?

    var body: some View {
        VStack(spacing: 0) {
            // Toolbar
            HStack(spacing: 8) {
                Image(systemName: "chart.bar.fill").foregroundStyle(.secondary)
                Text("Redis Server Info")
                    .font(.system(size: 13, weight: .semibold))
                Spacer()
                Toggle("Auto-refresh", isOn: $autoRefresh)
                    .toggleStyle(.checkbox)
                    .font(.system(size: 11))
                Button { Task { await load() } } label: {
                    Image(systemName: "arrow.clockwise").font(.system(size: 11))
                }
            }
            .padding(.horizontal, 12).padding(.vertical, 8)
            Divider()

            // Key metrics
            if !sections.isEmpty {
                keyMetrics
                Divider()
            }

            // Sections
            if let err = errorMessage {
                VStack(spacing: 8) {
                    Image(systemName: "exclamationmark.triangle").font(.system(size: 28)).foregroundStyle(.orange)
                    Text(err).font(.system(size: 12)).foregroundStyle(.secondary).textSelection(.enabled)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else if isLoading {
                ProgressView().frame(maxWidth: .infinity, maxHeight: .infinity)
            } else if sections.isEmpty {
                VStack(spacing: 8) {
                    Image(systemName: "info.circle").font(.system(size: 28)).foregroundStyle(.secondary)
                    Text("No server info available").font(.system(size: 12)).foregroundStyle(.secondary)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 0) {
                        ForEach(Array(sections.enumerated()), id: \.offset) { _, section in
                            sectionView(section)
                        }
                    }
                }
            }
        }
        .task(id: redisContext) { await load() }
        .onChange(of: autoRefresh) { _, on in
            refreshTask?.cancel()
            if on {
                refreshTask = Task {
                    while !Task.isCancelled {
                        try? await Task.sleep(nanoseconds: 5_000_000_000)
                        guard !Task.isCancelled else { break }
                        await load()
                    }
                }
            }
        }
        .onDisappear { refreshTask?.cancel() }
    }

    // MARK: - Key Metrics

    private var keyMetrics: some View {
        HStack(spacing: 20) {
            metricCard("Memory", value: findValue("used_memory_human") ?? "—")
            metricCard("Clients", value: findValue("connected_clients") ?? "—")
            metricCard("Uptime", value: uptimeString)
            metricCard("Keys", value: databaseSize.map { "\($0)" } ?? "—")
            metricCard("Ops/sec", value: findValue("instantaneous_ops_per_sec") ?? "—")
            metricCard("Hit Rate", value: hitRateString)
            Spacer()
        }
        .padding(.horizontal, 12).padding(.vertical, 8)
    }

    private func metricCard(_ title: String, value: String) -> some View {
        VStack(spacing: 2) {
            Text(value)
                .font(.system(size: 16, weight: .semibold, design: .monospaced))
            Text(title)
                .font(.system(size: 10))
                .foregroundStyle(.secondary)
        }
        .frame(minWidth: 70)
    }

    // MARK: - Section View

    private func sectionView(_ section: RedisInfoSection) -> some View {
        DisclosureGroup(section.name) {
            ForEach(Array(section.entries.enumerated()), id: \.offset) { _, entry in
                HStack {
                    Text(entry.key)
                        .font(.system(size: 12, design: .monospaced))
                        .foregroundStyle(.secondary)
                        .frame(width: 280, alignment: .leading)
                    Text(entry.value)
                        .font(.system(size: 12, design: .monospaced))
                        .textSelection(.enabled)
                    Spacer()
                }
                .padding(.horizontal, 8).padding(.vertical, 1)
            }
        }
        .padding(.horizontal, 12).padding(.vertical, 4)
    }

    // MARK: - Helpers

    private func findValue(_ key: String) -> String? {
        for s in sections {
            for e in s.entries where e.key == key { return e.value }
        }
        return nil
    }

    private var uptimeString: String {
        guard let days = findValue("uptime_in_days") else { return "—" }
        return "\(days)d"
    }

    private var hitRateString: String {
        guard let hitsStr = findValue("keyspace_hits"), let hits = Double(hitsStr),
              let missStr = findValue("keyspace_misses"), let misses = Double(missStr) else { return "—" }
        let total = hits + misses
        guard total > 0 else { return "—" }
        return String(format: "%.1f%%", hits / total * 100)
    }

    private func load() async {
        isLoading = true
        let result = await appState.performRedisOperation(for: redisContext) { redis in
            let loadedSections = try await redis.serverInfoSections()
            let loadedDatabaseSize = try await redis.dbSize()
            return (sections: loadedSections, databaseSize: loadedDatabaseSize)
        }

        switch result {
        case .success(let snapshot)?:
            sections = snapshot.sections
            databaseSize = snapshot.databaseSize
            appState.redisDBSize = snapshot.databaseSize
            errorMessage = nil
            isLoading = false
        case .failure(let error)?:
            errorMessage = error.localizedDescription
            isLoading = false
        case nil:
            isLoading = false
        }
    }
}
