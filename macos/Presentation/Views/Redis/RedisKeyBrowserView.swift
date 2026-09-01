// RedisKeyBrowserView.swift
// Gridex
//
// Redis-specific sidebar browser backed by a names-only SCAN.

import SwiftUI

enum RedisKeyBrowserBehavior {
    static func effectiveDelimiter(for delimiter: String) -> String {
        delimiter.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
            ? ":"
            : delimiter
    }

    static func shouldPublish(
        capturedNonce: Int,
        currentNonce: Int,
        isCancelled: Bool
    ) -> Bool {
        !isCancelled && capturedNonce == currentNonce
    }
}

enum RedisKeyBrowserMode: String, CaseIterable, Identifiable {
    case tree, flat

    var id: String { rawValue }
}

struct RedisKeyBrowserView: View {
    @EnvironmentObject private var appState: AppState
    @AppStorage("redis.keyBrowser.delimiter") private var delimiter = ":"
    @State private var mode: RedisKeyBrowserMode = .tree
    @State private var lastSuccessfulResult = RedisKeyScanResult(keys: [], isTruncated: false)
    @State private var hasSuccessfulResult = false
    @State private var isLoading = false
    @State private var errorMessage: String?

    private var effectiveDelimiter: String {
        RedisKeyBrowserBehavior.effectiveDelimiter(for: delimiter)
    }

    private var namespaceNodes: [RedisKeyNamespaceNode] {
        RedisKeyNamespaceTree.build(
            keys: lastSuccessfulResult.keys,
            delimiter: effectiveDelimiter
        )
    }

    var body: some View {
        VStack(spacing: 0) {
            toolbar
            Divider()

            if mode == .flat {
                flatContent
            } else {
                treeContent
            }
        }
        .onChange(of: mode) { _, newMode in
            if newMode == .flat {
                appState.openRedisFlatKeyList()
            }
        }
        .onReceive(NotificationCenter.default.publisher(for: .reloadData)) { _ in
            appState.requestRedisKeyBrowserRefresh()
        }
        .task(id: appState.redisKeyBrowserRefreshNonce) {
            let capturedNonce = appState.redisKeyBrowserRefreshNonce
            await scanKeys(capturedNonce: capturedNonce)
        }
    }

    private var toolbar: some View {
        VStack(alignment: .leading, spacing: 5) {
            HStack(spacing: 6) {
                Picker("Mode", selection: $mode) {
                    ForEach(RedisKeyBrowserMode.allCases) { mode in
                        Text(mode.rawValue.capitalized).tag(mode)
                    }
                }
                .labelsHidden()
                .pickerStyle(.segmented)
                .frame(width: 118)

                Spacer()

                Button {
                    appState.requestRedisKeyBrowserRefresh()
                } label: {
                    HStack(spacing: 4) {
                        if isLoading {
                            ProgressView()
                                .controlSize(.small)
                                .frame(width: 12, height: 12)
                        } else {
                            Image(systemName: "arrow.clockwise")
                                .font(.system(size: 10))
                                .frame(width: 12, height: 12)
                        }
                        Text("Refresh")
                            .font(.system(size: 11))
                    }
                }
                .buttonStyle(.plain)
                .help("Refresh Redis keys")
            }

            Text("Delimiter: \(effectiveDelimiter)")
                .font(.system(size: 10, design: .monospaced))
                .foregroundStyle(.secondary)
                .lineLimit(1)
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 6)
    }

    @ViewBuilder
    private var treeContent: some View {
        if !hasSuccessfulResult {
            if let errorMessage {
                errorView(errorMessage)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                ProgressView()
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        } else {
            VStack(spacing: 0) {
                if let errorMessage {
                    errorBanner(errorMessage)
                    Divider()
                }

                if lastSuccessfulResult.keys.isEmpty {
                    Text("No keys found")
                        .font(.system(size: 12))
                        .foregroundStyle(.secondary)
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                } else {
                    ScrollView {
                        LazyVStack(alignment: .leading, spacing: 1) {
                            ForEach(namespaceNodes) { node in
                                RedisKeyNamespaceNodeView(node: node) { key in
                                    appState.openRedisKeyDetail(key: key)
                                }
                            }
                        }
                        .padding(.horizontal, 7)
                        .padding(.vertical, 5)
                    }
                }

                if lastSuccessfulResult.isTruncated {
                    Divider()
                    Text("Showing the first 10,000 keys. Use Flat mode to filter with SCAN.")
                        .font(.system(size: 10))
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                        .padding(8)
                }
            }
        }
    }

    private var flatContent: some View {
        VStack(spacing: 8) {
            Spacer()
            Image(systemName: "list.bullet.rectangle")
                .font(.system(size: 24))
                .foregroundStyle(.secondary)
            Text("The existing Keys tab is open.")
                .font(.system(size: 12, weight: .medium))
            Text("Use its SCAN filter to narrow the flat key list.")
                .font(.system(size: 11))
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
            Button("Show Tree") { mode = .tree }
                .controlSize(.small)
            Spacer()
        }
        .padding()
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private func errorView(_ message: String) -> some View {
        VStack(spacing: 8) {
            Image(systemName: "exclamationmark.triangle")
                .foregroundStyle(.orange)
            Text(message)
                .font(.system(size: 11))
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .textSelection(.enabled)
            Button("Retry") { appState.requestRedisKeyBrowserRefresh() }
                .controlSize(.small)
        }
        .padding()
    }

    private func errorBanner(_ message: String) -> some View {
        HStack(spacing: 6) {
            Image(systemName: "exclamationmark.triangle.fill")
                .foregroundStyle(.orange)
            Text(message)
                .font(.system(size: 10))
                .foregroundStyle(.secondary)
                .lineLimit(2)
            Spacer()
            Button("Retry") { appState.requestRedisKeyBrowserRefresh() }
                .controlSize(.mini)
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 5)
    }

    @MainActor
    private func scanKeys(capturedNonce: Int) async {
        isLoading = true
        errorMessage = nil

        guard let redis = appState.activeAdapter as? RedisAdapter else {
            publishError("Not connected to Redis", capturedNonce: capturedNonce)
            return
        }

        do {
            let result = try await redis.scanKeyNames()
            guard RedisKeyBrowserBehavior.shouldPublish(
                capturedNonce: capturedNonce,
                currentNonce: appState.redisKeyBrowserRefreshNonce,
                isCancelled: Task.isCancelled
            ) else { return }

            lastSuccessfulResult = result
            hasSuccessfulResult = true
            errorMessage = nil
            isLoading = false
        } catch {
            publishError(error.localizedDescription, capturedNonce: capturedNonce)
        }
    }

    @MainActor
    private func publishError(_ message: String, capturedNonce: Int) {
        guard RedisKeyBrowserBehavior.shouldPublish(
            capturedNonce: capturedNonce,
            currentNonce: appState.redisKeyBrowserRefreshNonce,
            isCancelled: Task.isCancelled
        ) else { return }

        errorMessage = message
        isLoading = false
    }
}

private struct RedisKeyNamespaceNodeView: View {
    let node: RedisKeyNamespaceNode
    let openKey: (String) -> Void

    var body: some View {
        if node.children.isEmpty {
            leafRow
        } else {
            DisclosureGroup {
                ForEach(node.children) { child in
                    RedisKeyNamespaceNodeView(node: child, openKey: openKey)
                }
            } label: {
                namespaceRow
            }
        }
    }

    private var namespaceRow: some View {
        HStack(spacing: 5) {
            Image(systemName: "folder")
                .font(.system(size: 11))
                .foregroundStyle(.secondary)
            Text(RedisKeyNamespaceTree.displayLabel(for: node.segment))
                .font(.system(size: 12, design: .monospaced))
                .lineLimit(1)
            Spacer(minLength: 4)
            Text("\(node.descendantKeyCount)")
                .font(.system(size: 9, design: .monospaced))
                .foregroundStyle(.tertiary)
            if let concreteKey = node.concreteKey {
                Button {
                    openKey(concreteKey)
                } label: {
                    Image(systemName: "key.fill")
                        .font(.system(size: 9))
                        .frame(width: 18, height: 18)
                }
                .buttonStyle(.plain)
                .help("Open key \(concreteKey)")
            }
        }
        .padding(.vertical, 2)
    }

    @ViewBuilder
    private var leafRow: some View {
        if let concreteKey = node.concreteKey {
            Button {
                openKey(concreteKey)
            } label: {
                HStack(spacing: 5) {
                    Image(systemName: "key")
                        .font(.system(size: 10))
                        .foregroundStyle(.secondary)
                    Text(RedisKeyNamespaceTree.displayLabel(for: node.segment))
                        .font(.system(size: 12, design: .monospaced))
                        .lineLimit(1)
                    Spacer()
                }
                .padding(.vertical, 3)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .help("Open key \(concreteKey)")
        } else {
            Text(RedisKeyNamespaceTree.displayLabel(for: node.segment))
                .font(.system(size: 12, design: .monospaced))
                .padding(.vertical, 3)
        }
    }
}
