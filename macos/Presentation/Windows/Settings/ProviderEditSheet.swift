// ProviderEditSheet.swift
// Gridex
//
// Add / edit form for a single LLM provider configuration.
// API key lives in Keychain keyed by the config UUID (ai.apikey.<uuid>), never
// in SwiftData.

import os
import SwiftUI

struct ProviderEditSheet: View {
    /// Existing config to edit, or nil for create.
    let editing: ProviderConfig?
    /// Called when the sheet saves successfully. Parent should reload list.
    let onSaved: (ProviderConfig) -> Void

    init(editing: ProviderConfig?, onSaved: @escaping (ProviderConfig) -> Void) {
        self.editing = editing
        self.onSaved = onSaved

        if let editing {
            _resolvedId = State(initialValue: editing.id)
            _name = State(initialValue: editing.name)
            _type = State(initialValue: editing.type)
            _apiBase = State(initialValue: editing.apiBase ?? editing.type.defaultBaseURL)
            _model = State(initialValue: editing.model)
            _enabled = State(initialValue: editing.enabled)
        }
    }

    @Environment(\.dismiss) private var dismiss

    private static let log = Logger(subsystem: "com.gridex.gridex", category: "ProviderEditSheet")

    private let keychain: KeychainServiceProtocol = DependencyContainer.shared.keychainService
    private let repository: any LLMProviderRepository = DependencyContainer.shared.llmProviderRepository

    /// Provider id used by both the OAuth flow (`ai.chatgpt.tokens.<id>` keychain
    /// key) and the eventual SwiftData row. Generated eagerly so a Sign-in click
    /// on a brand-new provider persists tokens under the id we'll reuse on Save.
    @State private var resolvedId: UUID = UUID()

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

    // ChatGPT OAuth state (only used when type == .chatGPT)
    @State private var chatGPTStatus: ChatGPTOAuthService.SignInStatus = .signedOut
    @State private var isSigningIn: Bool = false
    @State private var signInError: String?

    /// True when this session may produce a Keychain token bundle that hasn't
    /// been confirmed by Save. Add mode and edits that switch a non-ChatGPT row
    /// to ChatGPT both need rollback on dismiss; editing an existing ChatGPT row
    /// is exempt because the bundle already belongs to that persisted provider.
    @State private var dirtyChatGPTSignIn: Bool = false
    @State private var chatGPTSignInTask: Task<Void, Never>?

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
        let nameOK  = !name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
        let modelOK = !model.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty

        if type == .chatGPT {
            if case .signedIn = chatGPTStatus {
                return nameOK && modelOK
            }
            return false
        }
        return nameOK
            && urlError == nil
            && (!type.requiresAPIKey || !apiKey.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
            && modelOK
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
        .onDisappear {
            let pendingSignIn = chatGPTSignInTask
            chatGPTSignInTask = nil
            pendingSignIn?.cancel()

            // Add-mode sign-in may still complete after the sheet closes. Wait
            // for cancellation/completion, then clear any bundle that may have
            // been written under an unsaved provider id.
            guard dirtyChatGPTSignIn else { return }
            let pid = resolvedId
            Task {
                if let pendingSignIn {
                    await pendingSignIn.value
                }
                try? await DependencyContainer.shared.chatGPTOAuthService.signOut(providerId: pid)
            }
        }
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

            Section(type == .chatGPT ? "Authentication" : "Endpoint") {
                if type == .chatGPT {
                    chatGPTAuthBlock
                } else {
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
            }

            if shouldShowModelSection {
                modelSection
            }

            Section {
                Toggle("Enabled", isOn: $enabled)
            }
        }
        .formStyle(.grouped)
    }

    private var shouldShowModelSection: Bool {
        guard type == .chatGPT else { return true }
        if case .signedIn = chatGPTStatus {
            return true
        }
        return false
    }

    private var modelSection: some View {
        Section {
            if type == .chatGPT {
                if !availableModels.isEmpty {
                    Picker("Model", selection: $model) {
                        Text("Select a model").tag("")
                        if !model.isEmpty, !availableModels.contains(where: { $0.id == model }) {
                            Text(model).tag(model)
                        }
                        ForEach(availableModels) { m in
                            Text(m.name).tag(m.id)
                        }
                    }
                    .onChange(of: model) { _, _ in testResult = .none }
                }
            } else {
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
            }
        } header: {
            HStack {
                Text("Model")
                Spacer()
                if isLoadingModels { ProgressView().controlSize(.small) }
            }
        } footer: {
            modelSectionFooter
        }
    }

    @ViewBuilder
    private var modelSectionFooter: some View {
        switch fetchResult {
        case .none:
            if type != .chatGPT {
                Text("Click **Fetch Models** below to load the list from the API, or type a model ID manually.")
                    .font(.system(size: 11))
                    .foregroundStyle(.secondary)
            }
        case .loaded(let count) where count > 0:
            Text("\(count) models loaded from API.")
                .font(.system(size: 11))
                .foregroundStyle(.green)
        case .loaded:
            Text("Endpoint responded but returned no models.")
                .font(.system(size: 11))
                .foregroundStyle(.orange)
        case .failure(let msg):
            Text(msg)
                .font(.system(size: 11))
                .foregroundStyle(.red)
        }
    }

    // MARK: - Footer

    private var footer: some View {
        HStack(spacing: 8) {
            // Fetch Models / Test only apply to API-key providers — ChatGPT
            // uses OAuth and populates models implicitly after sign-in.
            if type != .chatGPT {
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
            if editing.type == .chatGPT {
                // No API key for OAuth providers; status comes from the keychain bundle.
                apiKey = ""
                Task { await refreshChatGPTStatus() }
            } else {
                apiKey = (try? keychain.load(key: Self.keychainKey(id: editing.id))) ?? ""
            }
        } else {
            applyTypeDefaults(type)
        }
        validateURL(apiBase)
    }

    private func applyTypeDefaults(_ newType: ProviderType) {
        if newType == .chatGPT {
            // Endpoint and key are owned by the OAuth flow — clear them so the
            // form state can't poison the eventual save.
            apiBase         = ""
            apiKey          = ""
            model           = ""
            availableModels = []
            fetchResult     = .none
            testResult      = .none
            urlError        = nil
            chatGPTStatus   = .signedOut
            signInError     = nil
            Task { await refreshChatGPTStatus() }
            return
        }
        // Switching OUT of .chatGPT mid-flow: cancel any in-flight OAuth task so
        // a tardy callback doesn't write tokens to the keychain after the form
        // has already moved on, and reset the ChatGPT-side UI state so a later
        // switch back to .chatGPT starts clean.
        chatGPTSignInTask?.cancel()
        chatGPTSignInTask = nil
        isSigningIn       = false
        signInError       = nil
        chatGPTStatus     = .signedOut

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
        // ChatGPT doesn't expose an API base URL field — its endpoint is fixed.
        if type == .chatGPT { urlError = nil; return }

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

    // MARK: - ChatGPT OAuth UI block

    @ViewBuilder
    private var chatGPTAuthBlock: some View {
        switch chatGPTStatus {
        case .signedIn(let email, let plan):
            HStack(spacing: 8) {
                Image(systemName: "checkmark.seal.fill").foregroundStyle(.green)
                VStack(alignment: .leading, spacing: 2) {
                    Text("Signed in as \(email ?? "(unknown)")")
                        .font(.system(size: 12, weight: .medium))
                    if let plan {
                        Text("Plan: \(plan)")
                            .font(.system(size: 11))
                            .foregroundStyle(.secondary)
                    }
                }
                Spacer()
                Button("Sign out", role: .destructive) { signOutChatGPT() }
                    .controlSize(.small)
            }
        case .signedOut:
            VStack(alignment: .leading, spacing: 8) {
                Button(action: signInChatGPT) {
                    HStack(spacing: 6) {
                        if isSigningIn { ProgressView().controlSize(.small) }
                        Image(systemName: "person.crop.circle.badge.checkmark")
                        Text(isSigningIn ? "Waiting for browser…" : "Sign in with OpenAI")
                    }
                }
                .buttonStyle(.borderedProminent)
                .disabled(isSigningIn)

                if let signInError {
                    Text(signInError)
                        .font(.system(size: 11))
                        .foregroundStyle(.red)
                }

                Text("Uses OpenAI's Codex CLI public OAuth client. Not officially supported by OpenAI. Requires a paid ChatGPT plan; may break if OpenAI changes the API. Credentials never leave this device.")
                    .font(.system(size: 11))
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
    }

    private func signInChatGPT() {
        let pid = resolvedId
        let shouldRollbackOnCancel = editing?.type != .chatGPT
        isSigningIn = true
        signInError = nil
        if shouldRollbackOnCancel {
            dirtyChatGPTSignIn = true
        }

        chatGPTSignInTask?.cancel()
        chatGPTSignInTask = Task {
            let svc = DependencyContainer.shared.chatGPTOAuthService
            do {
                _ = try await svc.signIn(providerId: pid)
                // The bundle is unconfirmed until Save unless this is a
                // re-sign-in for a row that was already ChatGPT when opened.
                await MainActor.run { if shouldRollbackOnCancel { dirtyChatGPTSignIn = true } }
                // refreshChatGPTStatus() itself fans out to loadChatGPTModels()
                // when availableModels is empty (true post-sign-in), so calling
                // it twice would double-hit /models.
                await refreshChatGPTStatus()
                await MainActor.run {
                    isSigningIn = false
                    chatGPTSignInTask = nil
                }
            } catch {
                await MainActor.run {
                    if shouldRollbackOnCancel {
                        dirtyChatGPTSignIn = false
                    }
                    signInError = error.localizedDescription
                    isSigningIn = false
                    chatGPTSignInTask = nil
                }
            }
        }
    }

    private func signOutChatGPT() {
        let pid = resolvedId
        Task {
            let svc = DependencyContainer.shared.chatGPTOAuthService
            try? await svc.signOut(providerId: pid)
            await refreshChatGPTStatus()
            await MainActor.run {
                chatGPTSignInTask?.cancel()
                chatGPTSignInTask = nil
                availableModels = []
                model = ""
                fetchResult = .none
                // Bundle is gone; nothing for onDisappear to clean up.
                dirtyChatGPTSignIn = false
            }
        }
    }

    private func refreshChatGPTStatus() async {
        let pid = resolvedId
        let status = await DependencyContainer.shared.chatGPTOAuthService.currentStatus(providerId: pid)
        // Hop to the main actor only to update view state; decide on the
        // follow-up model load there too (it reads availableModels), then
        // await it in the caller's task instead of detaching a new Task.
        // Detached tasks here would outlive the sheet on dismiss.
        let needsModelLoad: Bool = await MainActor.run {
            chatGPTStatus = status
            if case .signedIn = status, availableModels.isEmpty {
                return true
            }
            return false
        }
        if needsModelLoad {
            await loadChatGPTModels()
        }
    }

    /// Populate `availableModels` from the ChatGPT backend after sign-in.
    /// Falls back to `type.fallbackModelIDs` when the network call fails.
    private func loadChatGPTModels() async {
        await MainActor.run { isLoadingModels = true }
        let pid = resolvedId
        let svc = DependencyContainer.shared.chatGPTOAuthService
        // Bypass ProviderRegistry on purpose: in add-mode the SwiftData row
        // doesn't exist yet (Save hasn't run), so the registry can't resolve
        // by name. The OAuth service is the only stateful dep — sheet-local
        // construction is safe and disposable. After Save, ProviderRegistry
        // would build an equivalent stateless ChatGPTProvider with the same
        // providerId + oauthService actor and default resolvedBaseURL.
        let provider = ChatGPTProvider(providerId: pid, oauthService: svc)
        do {
            let models = try await provider.availableModels()
            await MainActor.run {
                isLoadingModels = false
                if models.isEmpty {
                    applyBuiltInFallback(reason: "endpoint returned no models")
                } else {
                    availableModels = models
                    fetchResult = .loaded(models.count)
                    Self.log.debug("Loaded \(models.count) OpenAI sign-in model(s); selected=\(model, privacy: .public)")
                }
            }
        } catch GridexError.aiAPIKeyMissing {
            await MainActor.run {
                isLoadingModels = false
                chatGPTStatus = .signedOut
                availableModels = []
                fetchResult = .none
                signInError = "Sign-in expired. Please sign in again."
            }
        } catch {
            await MainActor.run {
                isLoadingModels = false
                applyBuiltInFallback(reason: error.localizedDescription)
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
        // ChatGPT: ignore apiBase/apiKey form fields entirely. The endpoint is
        // baked into the type, the credentials live under ai.chatgpt.tokens.<id>.
        let config = ProviderConfig(
            id: resolvedId,
            name: name.trimmingCharacters(in: .whitespacesAndNewlines),
            type: type,
            apiBase: type == .chatGPT ? nil : (trimmedBase.isEmpty ? nil : trimmedBase),
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

                if config.type == .chatGPT {
                    // Tokens are already in Keychain from the Sign-in flow.
                    // Drop any stale API key under the same id — user may
                    // have switched type from API-key to ChatGPT mid-edit.
                    try? keychain.delete(key: Self.keychainKey(id: config.id))
                    await DependencyContainer.shared.providerRegistry.register(
                        config,
                        apiKey: "",
                        chatGPTOAuthService: DependencyContainer.shared.chatGPTOAuthService
                    )
                } else {
                    // Reverse cleanup: discard any orphan ChatGPT token bundle
                    // left behind if the user signed in then switched type.
                    try? keychain.deleteChatGPTTokens(providerId: config.id)
                    // Persist API key (or clear it)
                    let keychainKey = Self.keychainKey(id: config.id)
                    if trimmedKey.isEmpty {
                        try? keychain.delete(key: keychainKey)
                    } else {
                        try keychain.save(key: keychainKey, value: trimmedKey)
                    }
                    // Update registry
                    await DependencyContainer.shared.providerRegistry.register(config, apiKey: trimmedKey)
                }

                await MainActor.run {
                    // SwiftData row now owns the id — clear the rollback arm
                    // before dismiss(), or onDisappear will sign us back out.
                    dirtyChatGPTSignIn = false
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
