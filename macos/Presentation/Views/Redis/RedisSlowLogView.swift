// RedisSlowLogView.swift
// Gridex
//
// Displays SLOWLOG GET entries in a table.

import SwiftUI

struct RedisSlowLogView: View {
    @EnvironmentObject private var appState: AppState

    let redisContext: AppState.RedisTabContext

    @State private var entries: [RedisSlowLogEntry] = []
    @State private var isLoading = true
    @State private var showResetConfirm = false
    @State private var errorMessage: String?

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 8) {
                Image(systemName: "tortoise.fill").foregroundStyle(.secondary)
                Text("Redis Slow Log")
                    .font(.system(size: 13, weight: .semibold))
                Text("\(entries.count) entries")
                    .font(.system(size: 11))
                    .foregroundStyle(.secondary)
                Spacer()
                Button("Reset") { showResetConfirm = true }
                    .font(.system(size: 11))
                Button { Task { await load() } } label: {
                    Image(systemName: "arrow.clockwise").font(.system(size: 11))
                }
            }
            .padding(.horizontal, 12).padding(.vertical, 8)
            Divider()

            if let errorMessage {
                VStack(spacing: 8) {
                    Image(systemName: "exclamationmark.triangle")
                        .font(.system(size: 28))
                        .foregroundStyle(.orange)
                    Text(errorMessage)
                        .font(.system(size: 12))
                        .foregroundStyle(.secondary)
                        .textSelection(.enabled)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else if isLoading {
                ProgressView().frame(maxWidth: .infinity, maxHeight: .infinity)
            } else if entries.isEmpty {
                VStack(spacing: 8) {
                    Image(systemName: "checkmark.circle").font(.system(size: 32)).foregroundStyle(.green)
                    Text("No slow queries recorded")
                        .font(.system(size: 13)).foregroundStyle(.secondary)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                ScrollView {
                    LazyVStack(spacing: 0) {
                        // Header
                        HStack(spacing: 0) {
                            headerCell("ID", width: 50)
                            headerCell("Time", width: 160)
                            headerCell("Duration", width: 90)
                            headerCell("Command", width: nil)
                            headerCell("Client", width: 140)
                        }
                        .background(Color(nsColor: .controlBackgroundColor))
                        Divider()

                        ForEach(entries) { entry in
                            HStack(spacing: 0) {
                                cell(String(entry.id), width: 50)
                                cell(dateFormatter.string(from: entry.timestamp), width: 160)
                                cell(formatDuration(entry.durationMicros), width: 90)
                                    .foregroundStyle(entry.durationMicros > 100_000 ? .red : .primary)
                                cell(entry.command, width: nil)
                                cell(entry.clientInfo, width: 140)
                            }
                            Divider()
                        }
                    }
                }
            }
        }
        .task(id: redisContext) { await load() }
        .alert("Reset Slow Log", isPresented: $showResetConfirm) {
            Button("Cancel", role: .cancel) {}
            Button("Reset", role: .destructive) {
                Task { await reset() }
            }
        } message: { Text("This will clear all slow log entries.") }
    }

    @ViewBuilder
    private func headerCell(_ text: String, width: CGFloat? = nil) -> some View {
        if let w = width {
            Text(text)
                .font(.system(size: 11, weight: .semibold))
                .foregroundStyle(.secondary)
                .frame(width: w, alignment: .leading)
                .padding(.horizontal, 8).padding(.vertical, 4)
        } else {
            Text(text)
                .font(.system(size: 11, weight: .semibold))
                .foregroundStyle(.secondary)
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(.horizontal, 8).padding(.vertical, 4)
        }
    }

    @ViewBuilder
    private func cell(_ text: String, width: CGFloat? = nil) -> some View {
        if let w = width {
            Text(text)
                .font(.system(size: 12, design: .monospaced))
                .lineLimit(2)
                .textSelection(.enabled)
                .frame(width: w, alignment: .leading)
                .padding(.horizontal, 8).padding(.vertical, 3)
        } else {
            Text(text)
                .font(.system(size: 12, design: .monospaced))
                .lineLimit(2)
                .textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(.horizontal, 8).padding(.vertical, 3)
        }
    }

    private func formatDuration(_ micros: Int) -> String {
        if micros < 1000 { return "\(micros) \u{00B5}s" }
        if micros < 1_000_000 { return String(format: "%.1f ms", Double(micros) / 1000) }
        return String(format: "%.2f s", Double(micros) / 1_000_000)
    }

    private var dateFormatter: DateFormatter {
        let f = DateFormatter()
        f.dateFormat = "yyyy-MM-dd HH:mm:ss"
        return f
    }

    private func load() async {
        isLoading = true
        let result = await appState.performRedisOperation(for: redisContext) { redis in
            try await redis.slowLog()
        }
        publish(result)
    }

    private func reset() async {
        isLoading = true
        let result = await appState.performRedisOperation(for: redisContext) { redis in
            _ = try await redis.executeRaw(sql: "SLOWLOG RESET")
            return try await redis.slowLog()
        }
        publish(result)
    }

    private func publish(_ result: Result<[RedisSlowLogEntry], Error>?) {
        switch result {
        case .success(let loadedEntries)?:
            entries = loadedEntries
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
