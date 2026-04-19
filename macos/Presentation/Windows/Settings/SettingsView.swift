// SettingsView.swift
// Gridex
//
// Settings window using SwiftUI.

import SwiftUI

struct SettingsView: View {
    var body: some View {
        TabView {
            GeneralSettingsView()
                .tabItem {
                    Label("General", systemImage: "gear")
                }

            AISettingsView()
                .tabItem {
                    Label("AI", systemImage: "brain")
                }

            EditorSettingsView()
                .tabItem {
                    Label("Editor", systemImage: "pencil")
                }
        }
        .frame(width: 580, height: 420)
    }
}

struct GeneralSettingsView: View {
    @AppStorage("general.pageSize") private var pageSize = 300
    @AppStorage("general.confirmBeforeDelete") private var confirmBeforeDelete = true
    @AppStorage("general.autoRefreshSidebar") private var autoRefreshSidebar = true
    @AppStorage("general.refreshInterval") private var refreshInterval = 300 // seconds
    @AppStorage("general.showQueryLog") private var showQueryLog = false

    var body: some View {
        Form {
            Section("Data Grid") {
                Picker("Default page size", selection: $pageSize) {
                    Text("100").tag(100)
                    Text("300").tag(300)
                    Text("500").tag(500)
                    Text("1000").tag(1000)
                }
                Toggle("Confirm before deleting rows", isOn: $confirmBeforeDelete)
            }

            Section("Sidebar") {
                Toggle("Auto-refresh schema", isOn: $autoRefreshSidebar)
                if autoRefreshSidebar {
                    Picker("Refresh interval", selection: $refreshInterval) {
                        Text("1 minute").tag(60)
                        Text("5 minutes").tag(300)
                        Text("15 minutes").tag(900)
                        Text("Never").tag(0)
                    }
                }
            }

            Section("Query") {
                Toggle("Show query log panel by default", isOn: $showQueryLog)
            }
        }
        .formStyle(.grouped)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

struct AISettingsView: View {
    private let keychain: KeychainServiceProtocol = DependencyContainer.shared.keychainService
    private let repository: any LLMProviderRepository = DependencyContainer.shared.llmProviderRepository

    @AppStorage("ai.activeProviderID") private var activeProviderID: String = ""

    @State private var providers: [ProviderConfig] = []
    @State private var editing: EditingTarget?
    @State private var loadError: String?

    private enum EditingTarget: Identifiable {
        case add
        case edit(ProviderConfig)

        var id: String {
            switch self {
            case .add:            return "__add__"
            case .edit(let c):    return c.id.uuidString
            }
        }
    }

    var body: some View {
        VStack(spacing: 0) {
            activeSection
            Divider()
            providersList
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .onAppear(perform: reload)
        .sheet(item: $editing) { target in
            switch target {
            case .add:
                ProviderEditSheet(editing: nil) { _ in reload() }
            case .edit(let config):
                ProviderEditSheet(editing: config) { _ in reload() }
            }
        }
    }

    // MARK: - Active picker

    private var activeSection: some View {
        Form {
            Section("Active Provider") {
                if enabledProviders.isEmpty {
                    Text("No provider configured yet. Click + to add one.")
                        .foregroundStyle(.secondary)
                        .font(.system(size: 12))
                } else {
                    Picker("Used by AI chat", selection: $activeProviderID) {
                        ForEach(enabledProviders) { p in
                            Label("\(p.name) — \(p.type.displayName)", systemImage: p.type.iconName)
                                .tag(p.id.uuidString)
                        }
                    }
                }
            }
        }
        .formStyle(.grouped)
        .frame(height: 100)
    }

    // MARK: - Providers list

    private var providersList: some View {
        VStack(spacing: 0) {
            HStack {
                Text("Providers").font(.headline)
                Spacer()
                Button(action: { editing = .add }) {
                    Label("Add Provider", systemImage: "plus")
                }
            }
            .padding(.horizontal, 20)
            .padding(.top, 12)
            .padding(.bottom, 8)

            if providers.isEmpty {
                VStack(spacing: 8) {
                    Image(systemName: "brain")
                        .font(.system(size: 32))
                        .foregroundStyle(.secondary)
                    Text("No providers yet")
                        .foregroundStyle(.secondary)
                    Text("Add Anthropic, OpenAI, Gemini, Ollama, Groq, DeepSeek and more.")
                        .font(.system(size: 11))
                        .foregroundStyle(.tertiary)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                List {
                    ForEach(providers) { p in
                        providerRow(p)
                    }
                }
                .listStyle(.inset(alternatesRowBackgrounds: true))
            }

            if let loadError {
                Text(loadError)
                    .font(.system(size: 11))
                    .foregroundStyle(.red)
                    .padding(.horizontal, 20)
                    .padding(.bottom, 8)
            }
        }
    }

    private func providerRow(_ p: ProviderConfig) -> some View {
        HStack(spacing: 10) {
            Image(systemName: p.type.iconName)
                .font(.system(size: 16))
                .foregroundStyle(.tint)
                .frame(width: 24)

            VStack(alignment: .leading, spacing: 2) {
                HStack(spacing: 6) {
                    Text(p.name).font(.system(size: 13, weight: .medium))
                    if p.id.uuidString == activeProviderID {
                        Text("ACTIVE")
                            .font(.system(size: 9, weight: .bold))
                            .padding(.horizontal, 5).padding(.vertical, 1)
                            .background(Color.accentColor.opacity(0.18))
                            .foregroundStyle(.tint)
                            .clipShape(RoundedRectangle(cornerRadius: 3))
                    }
                }
                Text("\(p.type.displayName) · \(p.model)")
                    .font(.system(size: 11))
                    .foregroundStyle(.secondary)
            }

            Spacer()

            Toggle("", isOn: Binding(
                get: { p.enabled },
                set: { toggleEnabled(p, $0) }
            ))
            .labelsHidden()
            .toggleStyle(.switch)
            .controlSize(.small)

            Button(action: { editing = .edit(p) }) {
                Image(systemName: "pencil")
            }
            .buttonStyle(.plain)
            .help("Edit")

            Button(action: { delete(p) }) {
                Image(systemName: "trash")
            }
            .buttonStyle(.plain)
            .foregroundStyle(.red)
            .help("Delete")
        }
        .padding(.vertical, 4)
    }

    // MARK: - Data

    private var enabledProviders: [ProviderConfig] {
        providers.filter(\.enabled)
    }

    private func reload() {
        Task {
            do {
                let list = try await repository.fetchAll()
                await MainActor.run {
                    providers = list
                    loadError = nil
                    // Ensure activeProviderID points at a valid enabled entry
                    if !list.contains(where: { $0.id.uuidString == activeProviderID && $0.enabled }),
                       let first = list.first(where: { $0.enabled }) {
                        activeProviderID = first.id.uuidString
                    }
                }
            } catch {
                await MainActor.run { loadError = error.localizedDescription }
            }
        }
    }

    private func toggleEnabled(_ p: ProviderConfig, _ enabled: Bool) {
        var updated = p
        updated.enabled = enabled
        Task {
            try? await repository.update(updated)
            let apiKey = (try? keychain.load(key: ProviderEditSheet.keychainKey(id: p.id))) ?? ""
            if enabled {
                await DependencyContainer.shared.providerRegistry.register(updated, apiKey: apiKey)
            } else {
                await DependencyContainer.shared.providerRegistry.unregister(updated.name)
            }
            await MainActor.run { reload() }
        }
    }

    private func delete(_ p: ProviderConfig) {
        Task {
            try? await repository.delete(p.id)
            try? keychain.delete(key: ProviderEditSheet.keychainKey(id: p.id))
            await DependencyContainer.shared.providerRegistry.unregister(p.name)
            await MainActor.run { reload() }
        }
    }
}

struct EditorSettingsView: View {
    @AppStorage("editor.fontSize") private var fontSize = 12.0
    @AppStorage("editor.tabSize") private var tabSize = 4
    @AppStorage("editor.useSpaces") private var useSpaces = true
    @AppStorage("editor.wordWrap") private var wordWrap = false
    @AppStorage("editor.showLineNumbers") private var showLineNumbers = true
    @AppStorage("editor.highlightCurrentLine") private var highlightCurrentLine = true

    var body: some View {
        Form {
            Section("Font") {
                HStack {
                    Text("Font size")
                    Spacer()
                    TextField("", value: $fontSize, format: .number)
                        .frame(width: 50)
                        .textFieldStyle(.roundedBorder)
                    Stepper("", value: $fontSize, in: 8...24, step: 1)
                        .labelsHidden()
                }
            }

            Section("Indentation") {
                Picker("Tab size", selection: $tabSize) {
                    Text("2 spaces").tag(2)
                    Text("4 spaces").tag(4)
                    Text("8 spaces").tag(8)
                }
                Toggle("Use spaces instead of tabs", isOn: $useSpaces)
            }

            Section("Display") {
                Toggle("Word wrap", isOn: $wordWrap)
                Toggle("Show line numbers", isOn: $showLineNumbers)
                Toggle("Highlight current line", isOn: $highlightCurrentLine)
            }
        }
        .formStyle(.grouped)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}
