// RedisKeyDetailView.swift
// Gridex
//
// Detailed view for a single Redis key — shows hash fields, list items, set members, etc.

import SwiftUI

@MainActor
final class RedisKeyDetailViewLifecycle: ObservableObject {
    private var generation: UInt64 = 0
    private var isActive = false

    func activate() {
        generation &+= 1
        isActive = true
    }

    func deactivate() {
        generation &+= 1
        isActive = false
    }

    @discardableResult
    func task(
        _ operation: @escaping @MainActor () async -> Void
    ) -> Task<Void, Never>? {
        guard isActive else { return nil }
        let capturedGeneration = generation

        return Task { @MainActor [weak self] in
            guard let self,
                  self.isActive,
                  self.generation == capturedGeneration else { return }
            await operation()
        }
    }
}

@MainActor
final class RedisKeyDetailViewState: ObservableObject {
    @Published private(set) var detail: RedisKeyDetail?
    @Published private(set) var isLoading = false
    @Published private(set) var isMutating = false
    @Published private(set) var errorMessage: String?

    private var activeContext: AppState.RedisTabContext?
    private let loadCoordinator = RedisRequestCoordinator()
    private let mutationCoordinator = RedisRequestCoordinator()

    func load(
        in context: AppState.RedisTabContext,
        execute: () async -> Result<RedisKeyDetail, Error>?
    ) async {
        transitionIfNeeded(to: context)
        isLoading = true
        errorMessage = nil

        let completion = await loadCoordinator.perform(
            for: context,
            execute: execute
        )

        switch completion {
        case .current(.success(let detail)):
            self.detail = detail
            errorMessage = nil
            isLoading = false
        case .current(.failure(let error)):
            errorMessage = error.localizedDescription
            isLoading = false
        case .stale:
            isLoading = false
        case .superseded:
            break
        }
    }

    @discardableResult
    func mutate(
        in context: AppState.RedisTabContext,
        execute: () async -> Result<RedisKeyDetail?, Error>?,
        onCurrentSuccess: () -> Void
    ) async -> Bool {
        transitionIfNeeded(to: context)
        isMutating = true
        errorMessage = nil

        let completion = await mutationCoordinator.perform(
            for: context,
            execute: execute
        )

        switch completion {
        case .current(.success(let detail)):
            if let detail {
                self.detail = detail
            }
            errorMessage = nil
            isMutating = false
            onCurrentSuccess()
            return true
        case .current(.failure(let error)):
            errorMessage = error.localizedDescription
            isMutating = false
            return false
        case .stale:
            isMutating = false
            return false
        case .superseded:
            return false
        }
    }

    func deactivate() {
        transitionIfNeeded(to: nil)
    }

    private func transitionIfNeeded(to context: AppState.RedisTabContext?) {
        guard activeContext != context else { return }
        activeContext = context
        loadCoordinator.invalidate()
        mutationCoordinator.invalidate()
        detail = nil
        errorMessage = nil
        isLoading = false
        isMutating = false
    }
}

struct RedisKeyDetailView: View {
    let keyName: String
    let redisContext: AppState.RedisTabContext?
    @EnvironmentObject private var appState: AppState
    @StateObject private var viewState = RedisKeyDetailViewState()
    @StateObject private var lifecycle = RedisKeyDetailViewLifecycle()

    // Editing
    @State private var showRename = false
    @State private var newKeyName = ""
    @State private var showTTLInput = false
    @State private var ttlInput = ""
    @State private var newFieldName = ""
    @State private var newFieldValue = ""

    private struct LoadRequest: Hashable {
        let keyName: String
        let context: AppState.RedisTabContext?
        let isActive: Bool
    }

    var body: some View {
        let loadRequest = LoadRequest(
            keyName: keyName,
            context: redisContext,
            isActive: isCapturedContextActive
        )

        return VStack(spacing: 0) {
            // Header
            headerBar
            Divider()

            if !isCapturedContextActive {
                contextMismatchView
            } else if viewState.isLoading {
                ProgressView().frame(maxWidth: .infinity, maxHeight: .infinity)
            } else if let err = viewState.errorMessage {
                Text(err).foregroundStyle(.red).padding()
            } else if let detail = viewState.detail {
                detailContent(detail)
            }
        }
        .task(id: loadRequest) {
            guard !Task.isCancelled else { return }
            guard loadRequest.isActive,
                  let context = loadRequest.context else {
                lifecycle.deactivate()
                viewState.deactivate()
                return
            }
            lifecycle.activate()
            await loadDetail(in: context)
        }
        .onDisappear {
            lifecycle.deactivate()
            viewState.deactivate()
        }
    }

    // MARK: - Header

    private var headerBar: some View {
        HStack(spacing: 8) {
            Image(systemName: "key.fill")
                .foregroundStyle(.secondary)
            Text(keyName)
                .font(.system(size: 13, weight: .semibold, design: .monospaced))

            if let detail = viewState.detail {
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
            Button {
                guard let context = redisContext else { return }
                startPresentedTask { await loadDetail(in: context) }
            } label: {
                Image(systemName: "arrow.clockwise").font(.system(size: 11))
            }
            .disabled(!isCapturedContextActive)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .alert("Rename Key", isPresented: $showRename) {
            TextField("New name", text: $newKeyName)
            Button("Cancel", role: .cancel) {}
            Button("Rename") {
                guard let context = redisContext, !newKeyName.isEmpty else { return }
                let replacementName = newKeyName
                startPresentedTask {
                    await renameKey(
                        to: replacementName,
                        in: context
                    )
                }
            }
        }
        .alert("Set TTL", isPresented: $showTTLInput) {
            TextField("Seconds (0 = remove)", text: $ttlInput)
            Button("Cancel", role: .cancel) {}
            Button("Set") {
                guard let context = redisContext else { return }
                let seconds = Int(ttlInput) ?? 0
                startPresentedTask { await setTTL(seconds, in: context) }
            }
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
                        guard let context = redisContext, !newFieldName.isEmpty else { return }
                        let field = newFieldName
                        let value = newFieldValue
                        startPresentedTask {
                            await mutateAndReload(in: context, operation: { redis in
                                try await redis.updateHashField(
                                    key: keyName,
                                    field: field,
                                    value: value
                                )
                            }, onCurrentSuccess: {
                                newFieldName = ""
                                newFieldValue = ""
                            })
                        }
                    }.font(.system(size: 11))
                }
            }
            .padding(.horizontal, 12).padding(.vertical, 6)

            tableGrid(headers: ["Field", "Value"], rows: fields.map { [$0.field, $0.value] }) { row in
                if let field = row.first {
                    Button("Delete Field") {
                        guard let context = redisContext else { return }
                        startPresentedTask {
                            await mutateAndReload(in: context) { redis in
                                try await redis.deleteHashField(
                                    key: keyName,
                                    field: field
                                )
                            }
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
                        guard let context = redisContext, !newFieldName.isEmpty else { return }
                        let member = newFieldName
                        startPresentedTask {
                            await mutateAndReload(in: context, operation: { redis in
                                try await redis.addSetMember(
                                    key: keyName,
                                    member: member
                                )
                            }, onCurrentSuccess: {
                                newFieldName = ""
                            })
                        }
                    }.font(.system(size: 11))
                }
            }.padding(.horizontal, 12).padding(.vertical, 6)

            tableGrid(headers: ["Member"], rows: members.map { [$0] }) { row in
                if let member = row.first {
                    Button("Remove") {
                        guard let context = redisContext else { return }
                        startPresentedTask {
                            await mutateAndReload(in: context) { redis in
                                try await redis.removeSetMember(
                                    key: keyName,
                                    member: member
                                )
                            }
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
                        guard let context = redisContext, !newFieldName.isEmpty else { return }
                        let member = newFieldName
                        let score = Double(newFieldValue) ?? 0
                        startPresentedTask {
                            await mutateAndReload(in: context, operation: { redis in
                                try await redis.addZSetMember(
                                    key: keyName,
                                    member: member,
                                    score: score
                                )
                            }, onCurrentSuccess: {
                                newFieldName = ""
                                newFieldValue = ""
                            })
                        }
                    }.font(.system(size: 11))
                }
            }.padding(.horizontal, 12).padding(.vertical, 6)

            tableGrid(headers: ["Member", "Score"], rows: members.map { [$0.member, String($0.score)] }) { row in
                if let member = row.first {
                    Button("Remove") {
                        guard let context = redisContext else { return }
                        startPresentedTask {
                            await mutateAndReload(in: context) { redis in
                                try await redis.removeZSetMember(
                                    key: keyName,
                                    member: member
                                )
                            }
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
        guard let redisContext else { return false }
        return appState.activeRedisAdapter(for: redisContext) != nil
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

    private func startPresentedTask(
        _ operation: @escaping @MainActor () async -> Void
    ) {
        lifecycle.task(operation)
    }

    private func loadDetail(in context: AppState.RedisTabContext) async {
        await viewState.load(in: context) {
            await appState.performRedisOperation(for: context) { redis in
                try await redis.fetchKeyDetail(key: keyName)
            }
        }
    }

    private func renameKey(
        to replacementName: String,
        in context: AppState.RedisTabContext
    ) async {
        await viewState.mutate(
            in: context,
            execute: {
                await appState.performRedisOperation(for: context) { redis -> RedisKeyDetail? in
                    try await redis.renameKey(
                        oldName: keyName,
                        newName: replacementName
                    )
                    return nil
                }
            },
            onCurrentSuccess: {
                newKeyName = ""
                NotificationCenter.default.post(name: .reloadData, object: nil)
            }
        )
    }

    private func setTTL(
        _ seconds: Int,
        in context: AppState.RedisTabContext
    ) async {
        await mutateAndReload(in: context, operation: { redis in
            if seconds <= 0 {
                try await redis.removeTTL(key: keyName)
            } else {
                try await redis.setTTL(key: keyName, seconds: seconds)
            }
        }, onCurrentSuccess: {
            ttlInput = ""
        })
    }

    private func mutateAndReload(
        in context: AppState.RedisTabContext,
        operation: (RedisAdapter) async throws -> Void,
        onCurrentSuccess: () -> Void = {}
    ) async {
        await viewState.mutate(
            in: context,
            execute: {
                await appState.performRedisOperation(for: context) { redis -> RedisKeyDetail? in
                    try await operation(redis)
                    return try await redis.fetchKeyDetail(key: keyName)
                }
            },
            onCurrentSuccess: onCurrentSuccess
        )
    }

    private func formatBytes(_ bytes: Int) -> String {
        if bytes < 1024 { return "\(bytes) B" }
        if bytes < 1024 * 1024 { return String(format: "%.1f KB", Double(bytes) / 1024) }
        return String(format: "%.1f MB", Double(bytes) / 1024 / 1024)
    }
}
