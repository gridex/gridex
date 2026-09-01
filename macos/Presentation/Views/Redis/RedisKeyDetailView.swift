// RedisKeyDetailView.swift
// Gridex
//
// Detailed view for a single Redis key — shows hash fields, list items, set members, etc.

import SwiftUI

struct RedisKeyDetailView: View {
    let keyName: String
    let redisContext: AppState.RedisTabContext?
    @EnvironmentObject private var appState: AppState
    @State private var detail: RedisKeyDetail?
    @State private var isLoading = true
    @State private var errorMessage: String?

    // Editing
    @State private var showRename = false
    @State private var newKeyName = ""
    @State private var showTTLInput = false
    @State private var ttlInput = ""
    @State private var newFieldName = ""
    @State private var newFieldValue = ""

    var body: some View {
        VStack(spacing: 0) {
            // Header
            headerBar
            Divider()

            if !isCapturedContextActive {
                contextMismatchView
            } else if isLoading {
                ProgressView().frame(maxWidth: .infinity, maxHeight: .infinity)
            } else if let err = errorMessage {
                Text(err).foregroundStyle(.red).padding()
            } else if let detail {
                detailContent(detail)
            }
        }
        .task { await loadDetail() }
    }

    // MARK: - Header

    private var headerBar: some View {
        HStack(spacing: 8) {
            Image(systemName: "key.fill")
                .foregroundStyle(.secondary)
            Text(keyName)
                .font(.system(size: 13, weight: .semibold, design: .monospaced))

            if let detail {
                Text(detail.type.rawValue.uppercased())
                    .font(.system(size: 10, weight: .bold))
                    .padding(.horizontal, 6)
                    .padding(.vertical, 2)
                    .background(Color.accentColor.opacity(0.15))
                    .clipShape(RoundedRectangle(cornerRadius: 3))

                if let ttl = detail.ttl {
                    Text("TTL: \(ttl)s")
                        .font(.system(size: 11, design: .monospaced))
                        .foregroundStyle(.secondary)
                }

                if let mem = detail.memoryBytes {
                    Text(formatBytes(mem))
                        .font(.system(size: 11, design: .monospaced))
                        .foregroundStyle(.secondary)
                }
            }

            Spacer()

            Button("Set TTL") { showTTLInput = true }
                .font(.system(size: 11))
                .disabled(!isCapturedContextActive)
            Button("Rename") {
                newKeyName = keyName
                showRename = true
            }
            .font(.system(size: 11))
            .disabled(!isCapturedContextActive)
            Button { Task { await loadDetail() } } label: {
                Image(systemName: "arrow.clockwise").font(.system(size: 11))
            }
            .disabled(!isCapturedContextActive)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .alert("Rename Key", isPresented: $showRename) {
            TextField("New name", text: $newKeyName)
            Button("Cancel", role: .cancel) {}
            Button("Rename") { Task { await renameKey() } }
        }
        .alert("Set TTL", isPresented: $showTTLInput) {
            TextField("Seconds (0 = remove)", text: $ttlInput)
            Button("Cancel", role: .cancel) {}
            Button("Set") { Task { await setTTL() } }
        }
    }

    // MARK: - Detail Content

    @ViewBuilder
    private func detailContent(_ d: RedisKeyDetail) -> some View {
        switch d.data {
        case .string(let value):
            stringView(value)
        case .hash(let fields):
            hashView(fields)
        case .list(let items):
            listView(items)
        case .set(let members):
            setView(members)
        case .zset(let members):
            zsetView(members)
        }
    }

    private func stringView(_ value: String) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Value").font(.system(size: 11, weight: .semibold)).foregroundStyle(.secondary)
            ScrollView {
                Text(value)
                    .font(.system(size: 12, design: .monospaced))
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(8)
            }
        }
        .padding()
    }

    private func hashView(_ fields: [(field: String, value: String)]) -> some View {
        VStack(spacing: 0) {
            HStack {
                Text("\(fields.count) fields").font(.system(size: 11)).foregroundStyle(.secondary)
                Spacer()
                HStack(spacing: 4) {
                    TextField("field", text: $newFieldName).frame(width: 100).textFieldStyle(.roundedBorder).font(.system(size: 11))
                    TextField("value", text: $newFieldValue).frame(width: 120).textFieldStyle(.roundedBorder).font(.system(size: 11))
                    Button("Add") {
                        guard !newFieldName.isEmpty else { return }
                        Task {
                            guard let redis = capturedRedisAdapter() else { return }
                            try? await redis.updateHashField(key: keyName, field: newFieldName, value: newFieldValue)
                            newFieldName = ""; newFieldValue = ""
                            await loadDetail()
                        }
                    }.font(.system(size: 11))
                }
            }
            .padding(.horizontal, 12).padding(.vertical, 6)

            tableGrid(headers: ["Field", "Value"], rows: fields.map { [$0.field, $0.value] }) { row in
                if let field = row.first {
                    Button("Delete Field") {
                        Task {
                            guard let redis = capturedRedisAdapter() else { return }
                            try? await redis.deleteHashField(key: keyName, field: field)
                            await loadDetail()
                        }
                    }
                }
            }
        }
    }

    private func listView(_ items: [String]) -> some View {
        VStack(spacing: 0) {
            HStack {
                Text("\(items.count) items").font(.system(size: 11)).foregroundStyle(.secondary)
                Spacer()
            }.padding(.horizontal, 12).padding(.vertical, 6)

            tableGrid(headers: ["Index", "Value"], rows: items.enumerated().map { [String($0.offset), $0.element] })
        }
    }

    private func setView(_ members: [String]) -> some View {
        VStack(spacing: 0) {
            HStack {
                Text("\(members.count) members").font(.system(size: 11)).foregroundStyle(.secondary)
                Spacer()
                HStack(spacing: 4) {
                    TextField("member", text: $newFieldName).frame(width: 140).textFieldStyle(.roundedBorder).font(.system(size: 11))
                    Button("Add") {
                        guard !newFieldName.isEmpty else { return }
                        Task {
                            guard let redis = capturedRedisAdapter() else { return }
                            try? await redis.addSetMember(key: keyName, member: newFieldName)
                            newFieldName = ""
                            await loadDetail()
                        }
                    }.font(.system(size: 11))
                }
            }.padding(.horizontal, 12).padding(.vertical, 6)

            tableGrid(headers: ["Member"], rows: members.map { [$0] }) { row in
                if let member = row.first {
                    Button("Remove") {
                        Task {
                            guard let redis = capturedRedisAdapter() else { return }
                            try? await redis.removeSetMember(key: keyName, member: member)
                            await loadDetail()
                        }
                    }
                }
            }
        }
    }

    private func zsetView(_ members: [(member: String, score: Double)]) -> some View {
        VStack(spacing: 0) {
            HStack {
                Text("\(members.count) members").font(.system(size: 11)).foregroundStyle(.secondary)
                Spacer()
                HStack(spacing: 4) {
                    TextField("member", text: $newFieldName).frame(width: 100).textFieldStyle(.roundedBorder).font(.system(size: 11))
                    TextField("score", text: $newFieldValue).frame(width: 60).textFieldStyle(.roundedBorder).font(.system(size: 11))
                    Button("Add") {
                        guard !newFieldName.isEmpty else { return }
                        Task {
                            guard let redis = capturedRedisAdapter() else { return }
                            try? await redis.addZSetMember(key: keyName, member: newFieldName, score: Double(newFieldValue) ?? 0)
                            newFieldName = ""; newFieldValue = ""
                            await loadDetail()
                        }
                    }.font(.system(size: 11))
                }
            }.padding(.horizontal, 12).padding(.vertical, 6)

            tableGrid(headers: ["Member", "Score"], rows: members.map { [$0.member, String($0.score)] }) { row in
                if let member = row.first {
                    Button("Remove") {
                        Task {
                            guard let redis = capturedRedisAdapter() else { return }
                            try? await redis.removeZSetMember(key: keyName, member: member)
                            await loadDetail()
                        }
                    }
                }
            }
        }
    }

    // MARK: - Reusable Table Grid

    private func tableGrid(headers: [String], rows: [[String]], @ViewBuilder contextMenuBuilder: @escaping ([String]) -> some View = { _ in EmptyView() }) -> some View {
        ScrollView {
            LazyVStack(spacing: 0) {
                HStack(spacing: 0) {
                    ForEach(headers, id: \.self) { h in
                        Text(h)
                            .font(.system(size: 11, weight: .semibold))
                            .foregroundStyle(.secondary)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .padding(.horizontal, 8)
                            .padding(.vertical, 4)
                    }
                }
                .background(Color(nsColor: .controlBackgroundColor))
                Divider()

                ForEach(Array(rows.enumerated()), id: \.offset) { _, row in
                    HStack(spacing: 0) {
                        ForEach(Array(row.enumerated()), id: \.offset) { _, cell in
                            Text(cell)
                                .font(.system(size: 12, design: .monospaced))
                                .lineLimit(3)
                                .textSelection(.enabled)
                                .frame(maxWidth: .infinity, alignment: .leading)
                                .padding(.horizontal, 8)
                                .padding(.vertical, 3)
                        }
                    }
                    .contextMenu {
                        contextMenuBuilder(row)
                        Button("Copy") {
                            NSPasteboard.general.clearContents()
                            NSPasteboard.general.setString(row.joined(separator: "\t"), forType: .string)
                        }
                    }
                    Divider()
                }
            }
        }
    }

    // MARK: - Actions

    private var isCapturedContextActive: Bool {
        capturedRedisAdapter() != nil
    }

    private var contextMismatchView: some View {
        VStack(spacing: 8) {
            Image(systemName: "exclamationmark.triangle")
                .foregroundStyle(.orange)
            Text("This key belongs to another Redis connection or database.")
                .font(.system(size: 12, weight: .medium))
            Text("Reopen it from the current Redis key browser to continue.")
                .font(.system(size: 11))
                .foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private func capturedRedisAdapter() -> RedisAdapter? {
        guard let redisContext else { return nil }
        return appState.activeRedisAdapter(for: redisContext)
    }

    private func loadDetail() async {
        guard let redis = capturedRedisAdapter() else {
            isLoading = false
            return
        }
        isLoading = true
        do {
            detail = try await redis.fetchKeyDetail(key: keyName)
            isLoading = false
        } catch {
            errorMessage = error.localizedDescription
            isLoading = false
        }
    }

    private func renameKey() async {
        guard let redis = capturedRedisAdapter(), !newKeyName.isEmpty else { return }
        do {
            try await redis.renameKey(oldName: keyName, newName: newKeyName)
            NotificationCenter.default.post(name: .reloadData, object: nil)
        } catch { errorMessage = error.localizedDescription }
    }

    private func setTTL() async {
        guard let redis = capturedRedisAdapter() else { return }
        let seconds = Int(ttlInput) ?? 0
        do {
            if seconds <= 0 {
                try await redis.removeTTL(key: keyName)
            } else {
                try await redis.setTTL(key: keyName, seconds: seconds)
            }
            await loadDetail()
        } catch { errorMessage = error.localizedDescription }
    }

    private func formatBytes(_ bytes: Int) -> String {
        if bytes < 1024 { return "\(bytes) B" }
        if bytes < 1024 * 1024 { return String(format: "%.1f KB", Double(bytes) / 1024) }
        return String(format: "%.1f MB", Double(bytes) / 1024 / 1024)
    }
}
