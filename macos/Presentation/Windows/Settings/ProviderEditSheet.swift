// ProviderEditSheet.swift
// Gridex
//
// Add / edit form for a single LLM provider configuration.
// API key lives in Keychain keyed by the config UUID (ai.apikey.<uuid>), never
// in SwiftData.

import SwiftUI

struct ProviderEditSheet: View {
    /// Existing config to edit, or nil for create.
    let editing: ProviderConfig?
    /// Called when the sheet saves successfully. Parent should reload list.
    let onSaved: (ProviderConfig) -> Void

    @Environment(\.dismiss) private var dismiss

    private let keychain: KeychainServiceProtocol = DependencyContainer.shared.keychainService
    private let repository: any LLMProviderRepository = DependencyContainer.shared.llmProviderRepository

    @State private var name: String = ""
    @State private var type: ProviderType = .anthropic
    @State private var apiBase: String = ProviderType.anthropic.defaultBaseURL
    @State private var apiKey: String = ""
    @State private var model: String = ""
    @State private var enabled: Bool = true

    @State private var availableModels: [LLMModel] = []
    @State private var isLoadingModels = false
    @State private var fetchResult: FetchResult = .none
    @State private var testResult: TestResult = .none
    @State private var isTesting = false
    @State private var urlError: String?
    @State private var saveError: String?

    enum FetchResult: Equatable {
        case none
        case loaded(Int)
        case failure(String)
    }

    enum TestResult: Equatable {
        case none
        case success
        case failure(String)
    }

    private var isEdit: Bool { editing != nil }
    private var canSave: Bool {
        !name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty &&
        urlError == nil &&
        (!type.requiresAPIKey || !apiKey.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty) &&
        !model.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
    }

    var body: some View {
        VStack(spacing: 0) {
            header
            Divider()
            form
            Divider()
            footer
        }
        .frame(width: 560, height: 540)
        .onAppear(perform: load)
    }

    // MARK: - Header

    private var header: some View {
        HStack {
            Image(systemName: type.iconName)
                .font(.system(size: 20))
                .foregroundStyle(.tint)
            Text(isEdit ? "Edit Provider" : "Add Provider")
                .font(.headline)
            Spacer()
        }
        .padding(.horizontal, 20)
        .padding(.vertical, 14)
    }

    // MARK: - Form

    private var form: some View {
        Form {
            Section {
                TextField("Name", text: $name, prompt: Text("e.g. my-claude"))
                    .textFieldStyle(.roundedBorder)

                Picker("Type", selection: $type) {
                    ForEach(ProviderType.Family.allCases, id: \.self) { family in
                        Section(family.rawValue) {
                            ForEach(ProviderType.allCases.filter { $0.family == family }) { t in
                                Label(t.displayName, systemImage: t.iconName).tag(t)
                            }
                        }
                    }
                }
                .onChange(of: type) { _, newType in applyTypeDefaults(newType) }
            }

            Section("Endpoint") {
                TextField("API Base URL", text: $apiBase)
                    .textFieldStyle(.roundedBorder)
                    .onChange(of: apiBase) { _, newValue in validateURL(newValue) }
                if let urlError {
                    Text(urlError).font(.system(size: 11)).foregroundStyle(.red)
                }
                if type.requiresAPIKey {
                    SecureField("API Key", text: $apiKey)
                        .textFieldStyle(.roundedBorder)
                }
            }

            Section {
                if !availableModels.isEmpty {
                    Picker("Available", selection: $model) {
                        ForEach(availableModels) { m in
                            Text(m.name).tag(m.id)
                        }
                    }
                }
                TextField("Model ID", text: $model, prompt: Text("e.g. qwen3-coder-plus, gpt-4o"))
                    .textFieldStyle(.roundedBorder)
                    .onChange(of: model) { _, newValue in
                        // Clear stale red messages once the user acts on them.
                        if case .failure = fetchResult,
                           !newValue.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
                            fetchResult = .none
                        }
                        testResult = .none
                    }
            } header: {
                HStack {
                    Text("Model")
                    Spacer()
                    if isLoadingModels { ProgressView().controlSize(.small) }
                }
            } footer: {
                switch fetchResult {
                case .none:
                    Text("Click **Fetch Models** below to load the list from the API, or type a model ID manually.")
                        .font(.system(size: 11))
                        .foregroundStyle(.secondary)
                case .loaded(let count) where count > 0:
                    Text("\(count) models loaded from API.")
                        .font(.system(size: 11))
                        .foregroundStyle(.green)
                case .loaded:
                    Text("Endpoint responded but returned no models — type a model ID manually.")
                        .font(.system(size: 11))
                        .foregroundStyle(.orange)
                case .failure(let msg):
                    Text(msg)
                        .font(.system(size: 11))
                        .foregroundStyle(.red)
                }
            }

            Section {
                Toggle("Enabled", isOn: $enabled)
            }
        }
        .formStyle(.grouped)
    }

    // MARK: - Footer

    private var footer: some View {
        HStack(spacing: 8) {
            Button(action: fetchModels) {
                HStack(spacing: 6) {
                    if isLoadingModels { ProgressView().controlSize(.small) }
                    Text("Fetch Models")
                }
            }
            .disabled(isLoadingModels || (type.requiresAPIKey && apiKey.isEmpty) || urlError != nil)

            Button(action: testConnection) {
                HStack(spacing: 6) {
                    if isTesting { ProgressView().controlSize(.small) }
                    Text("Test")
                }
            }
            .disabled(isTesting
                      || (type.requiresAPIKey && apiKey.isEmpty)
                      || urlError != nil
                      || model.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
            .help("Send a 1-token request to verify the key + URL + model combination")

            switch testResult {
            case .none: EmptyView()
            case .success:
                Label("Connected", systemImage: "checkmark.circle.fill")
                    .foregroundStyle(.green).font(.system(size: 12))
            case .failure(let msg):
                Label(msg, systemImage: "xmark.circle.fill")
                    .foregroundStyle(.red).font(.system(size: 12)).lineLimit(1)
            }

            Spacer()

            if let saveError {
                Text(saveError).font(.system(size: 11)).foregroundStyle(.red).lineLimit(1)
            }

            Button("Cancel") { dismiss() }
                .keyboardShortcut(.cancelAction)

            Button("Save") { save() }
                .keyboardShortcut(.defaultAction)
                .disabled(!canSave)
        }
        .padding(.horizontal, 20)
        .padding(.vertical, 12)
    }

    // MARK: - Actions

    private func load() {
        if let editing {
            name    = editing.name
            type    = editing.type
            apiBase = editing.apiBase ?? editing.type.defaultBaseURL
            model   = editing.model
            enabled = editing.enabled
            apiKey  = (try? keychain.load(key: Self.keychainKey(id: editing.id))) ?? ""
        } else {
            applyTypeDefaults(type)
        }
        validateURL(apiBase)
    }

    private func applyTypeDefaults(_ newType: ProviderType) {
        apiBase         = newType.defaultBaseURL
        model           = ""          // user must fetch or type — avoids presetting a model the endpoint may not support
        availableModels = []
        fetchResult     = .none
        testResult      = .none
        isLoadingModels = false
        isTesting       = false
        validateURL(apiBase)
    }

    private func validateURL(_ value: String) {
        if value.isEmpty, type == .openAICompatible {
            urlError = "Base URL is required for custom providers"
            return
        }
        do {
            _ = try ProviderURLValidator.validate(value, for: type)
            urlError = nil
        } catch {
            urlError = error.localizedDescription
        }
    }

    /// Verify the key + model combination by sending a 1-token chat request.
    /// Works with endpoints that don't support /models listing (e.g. coding-intl
    /// DashScope). Error messages come from the server, so the user can see why.
    private func testConnection() {
        isTesting = true
        testResult = .none
        let t = type
        let base = apiBase.trimmingCharacters(in: .whitespacesAndNewlines)
        let key = apiKey.trimmingCharacters(in: .whitespacesAndNewlines)
        let probeModel = model.trimmingCharacters(in: .whitespacesAndNewlines)

        Task {
            do {
                let config = ProviderConfig(name: "__probe__", type: t, apiBase: base, model: probeModel)
                let service = ProviderFactory.make(config: config, apiKey: key)
                let ok = try await service.validateAPIKey()
                await MainActor.run {
                    testResult = ok ? .success : .failure("Invalid credentials")
                    isTesting = false
                }
            } catch {
                await MainActor.run {
                    testResult = .failure(error.localizedDescription)
                    isTesting = false
                }
            }
        }
    }

    /// Populate the model picker. Strategy mirrors goclaw:
    ///   - Providers with no live listing (DashScope, Bailian) → use built-in catalog directly.
    ///   - Everything else → call the provider's `availableModels()` and populate from server.
    ///     On failure, fall back to the type's built-in catalog when available.
    private func fetchModels() {
        let t = type
        if t.hasHardcodedCatalog {
            applyBuiltInFallback(reason: "provider uses a built-in catalog")
            return
        }

        isLoadingModels = true
        fetchResult = .none
        let base = apiBase.trimmingCharacters(in: .whitespacesAndNewlines)
        let key = apiKey.trimmingCharacters(in: .whitespacesAndNewlines)

        Task {
            let probeConfig = ProviderConfig(name: "__probe__", type: t, apiBase: base, model: "x")
            let service = ProviderFactory.make(config: probeConfig, apiKey: key)

            do {
                let models = try await service.availableModels()
                await MainActor.run {
                    if models.isEmpty {
                        applyBuiltInFallback(reason: "endpoint returned no models")
                    } else {
                        availableModels = models
                        fetchResult = .loaded(models.count)
                        if model.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
                            model = models[0].id
                        }
                    }
                    isLoadingModels = false
                }
            } catch {
                await MainActor.run {
                    applyBuiltInFallback(reason: error.localizedDescription)
                    isLoadingModels = false
                }
            }
        }
    }

    /// Populate the picker with the provider type's built-in model IDs when the
    /// live API can't give us a list. Non-fatal — user can still type any model.
    private func applyBuiltInFallback(reason: String) {
        let fallbacks = type.fallbackModelIDs
        if fallbacks.isEmpty {
            availableModels = []
            fetchResult = .failure("Couldn't fetch (\(reason)) — type a model ID manually.")
            return
        }
        availableModels = fallbacks.map {
            LLMModel(id: $0, name: $0, provider: type.displayName, contextWindow: 0, supportsStreaming: true)
        }
        fetchResult = .loaded(fallbacks.count)
        if model.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            model = fallbacks[0]
        }
    }

    private func save() {
        let trimmedKey = apiKey.trimmingCharacters(in: .whitespacesAndNewlines)
        let trimmedBase = apiBase.trimmingCharacters(in: .whitespacesAndNewlines)
        let config = ProviderConfig(
            id: editing?.id ?? UUID(),
            name: name.trimmingCharacters(in: .whitespacesAndNewlines),
            type: type,
            apiBase: trimmedBase.isEmpty ? nil : trimmedBase,
            model: model.trimmingCharacters(in: .whitespacesAndNewlines),
            enabled: enabled,
            createdAt: editing?.createdAt ?? Date()
        )

        Task {
            do {
                if isEdit {
                    try await repository.update(config)
                } else {
                    try await repository.save(config)
                }
                // Persist API key (or clear it)
                let keychainKey = Self.keychainKey(id: config.id)
                if trimmedKey.isEmpty {
                    try? keychain.delete(key: keychainKey)
                } else {
                    try keychain.save(key: keychainKey, value: trimmedKey)
                }
                // Update registry
                await DependencyContainer.shared.providerRegistry.register(config, apiKey: trimmedKey)

                await MainActor.run {
                    onSaved(config)
                    dismiss()
                }
            } catch {
                await MainActor.run { saveError = error.localizedDescription }
            }
        }
    }

    // MARK: - Helpers

    static func keychainKey(id: UUID) -> String {
        "ai.apikey.\(id.uuidString)"
    }
}
