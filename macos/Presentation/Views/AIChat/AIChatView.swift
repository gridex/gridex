// AIChatView.swift
// Gridex
//
// SwiftUI AI chat panel with streaming responses.

import SwiftUI

// Observable message class — mutations don't trigger ForEach diffing
final class ChatDisplayMessage: ObservableObject, Identifiable {
    let id: UUID
    let role: ChatRole
    @Published var content: String

    enum ChatRole {
        case user, assistant
    }

    init(role: ChatRole, content: String) {
        self.id = UUID()
        self.role = role
        self.content = content
    }
}

struct AIChatView: View {
    @EnvironmentObject private var appState: AppState
    @AppStorage("ai.activeProviderID") private var activeProviderID: String = ""
    @AppStorage("ai.activeModel")      private var activeModel: String = ""
    // Legacy single-provider settings — kept for backward compat until user
    // migrates to the multi-provider list in Settings → AI.
    @AppStorage("ai.provider") private var providerType = "gemini"
    @AppStorage("ai.provider.baseURL") private var baseURL = "https://generativelanguage.googleapis.com/v1beta/openai"
    @AppStorage("ai.provider.enabled") private var isEnabled = true
    @AppStorage("ai.provider.model") private var selectedModel = "gemini-2.5-flash"
    @State private var inputText = ""
    @State private var isStreaming = false
    @State private var errorMessage: String?
    @State private var apiKeyLoaded = false
    @State private var attachedTables: Set<String> = []
    @State private var showTablePicker = false
    @State private var providers: [ProviderConfig] = []

    private var messages: [ChatDisplayMessage] {
        get {
            guard let connId = appState.activeConnectionId else { return [] }
            return appState.aiChatMessages[connId] ?? []
        }
    }

    private func appendMessage(_ msg: ChatDisplayMessage) {
        guard let connId = appState.activeConnectionId else { return }
        if appState.aiChatMessages[connId] == nil {
            appState.aiChatMessages[connId] = []
        }
        appState.aiChatMessages[connId]!.append(msg)
        appState.objectWillChange.send()
    }

    private func clearMessages() {
        guard let connId = appState.activeConnectionId else { return }
        appState.aiChatMessages[connId] = []
        appState.objectWillChange.send()
    }

    var body: some View {
        VStack(spacing: 0) {
            header

            if !apiKeyLoaded {
                setupPrompt
            } else if messages.isEmpty {
                welcomeScreen
            } else {
                messageList
            }

            inputBar
        }
        .background(Color(nsColor: .windowBackgroundColor))
        .alert("Error", isPresented: Binding(
            get: { errorMessage != nil },
            set: { if !$0 { errorMessage = nil } }
        )) {
            Button("OK", role: .cancel) {}
        } message: {
            Text(errorMessage ?? "")
        }
        .onAppear {
            refreshAPIKeyState()
            Task { await loadProviders() }
        }
        .onChange(of: activeProviderID) { _, _ in refreshAPIKeyState() }
        .onChange(of: providerType) { _, _ in refreshAPIKeyState() }
        .onChange(of: isEnabled) { _, _ in refreshAPIKeyState() }
    }

    // MARK: - Subviews

    private var header: some View {
        HStack(spacing: 10) {
            // Animated brand icon with gradient
            ZStack {
                LinearGradient(
                    colors: [Color.purple, Color.blue],
                    startPoint: .topLeading,
                    endPoint: .bottomTrailing
                )
                .frame(width: 26, height: 26)
                .clipShape(RoundedRectangle(cornerRadius: 7, style: .continuous))
                Image(systemName: "sparkles")
                    .font(.system(size: 12, weight: .semibold))
                    .foregroundStyle(.white)
            }

            VStack(alignment: .leading, spacing: 1) {
                Text("AI Assistant")
                    .font(.system(size: 13, weight: .semibold))
                providerPicker
            }

            Spacer()

            if !messages.isEmpty {
                Button(action: clearMessages) {
                    Image(systemName: "arrow.counterclockwise")
                        .font(.system(size: 11, weight: .medium))
                        .foregroundStyle(.secondary)
                        .frame(width: 26, height: 26)
                        .background(Color.primary.opacity(0.06))
                        .clipShape(Circle())
                }
                .buttonStyle(.plain)
                .help("New chat")
            }
            SettingsLink {
                Image(systemName: "gearshape")
                    .font(.system(size: 11, weight: .medium))
                    .foregroundStyle(.secondary)
                    .frame(width: 26, height: 26)
                    .background(Color.primary.opacity(0.06))
                    .clipShape(Circle())
            }
            .buttonStyle(.plain)
            .help("AI Settings")
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
        .background(Color(nsColor: .windowBackgroundColor))
        .overlay(alignment: .bottom) {
            Divider().opacity(0.5)
        }
    }

    // MARK: - Provider / model picker (inline switcher)

    /// Status pill under "AI Assistant" — tapping opens a Menu to switch provider
    /// and model without leaving the chat.
    private var providerPicker: some View {
        Menu {
            if providers.isEmpty {
                Text("No providers configured — open Settings → AI")
                Divider()
                SettingsLink { Text("Open Settings") }
            } else {
                ForEach(providers) { p in
                    Section(p.name) {
                        ForEach(modelOptions(for: p), id: \.self) { modelID in
                            Button(action: { select(provider: p, model: modelID) }) {
                                if p.id.uuidString == activeProviderID &&
                                   (activeModel.isEmpty ? p.model : activeModel) == modelID {
                                    Label(modelID, systemImage: "checkmark")
                                } else {
                                    Label(modelID, systemImage: p.type.iconName)
                                }
                            }
                        }
                    }
                }
                Divider()
                SettingsLink { Label("Manage providers…", systemImage: "gearshape") }
            }
        } label: {
            HStack(spacing: 4) {
                Circle()
                    .fill(apiKeyLoaded ? Color.green : Color.orange)
                    .frame(width: 6, height: 6)
                Text(currentProviderLabel)
                    .font(.system(size: 10))
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                Image(systemName: "chevron.down")
                    .font(.system(size: 7, weight: .semibold))
                    .foregroundStyle(.secondary)
            }
        }
        .menuStyle(.borderlessButton)
        .menuIndicator(.hidden)
        .fixedSize()
    }

    private var currentProviderLabel: String {
        if !activeProviderID.isEmpty {
            if let p = providers.first(where: { $0.id.uuidString == activeProviderID }) {
                let m = activeModel.isEmpty ? p.model : activeModel
                return "\(p.name) · \(m)"
            }
            // Provider ID set but list not loaded yet
            if providers.isEmpty {
                return "Loading…"
            }
            // Provider ID doesn't match any — stale reference
            return "Not configured"
        }
        // Legacy single-provider mode
        return apiKeyLoaded ? selectedModel : "Not configured"
    }

    /// Models to show under a provider in the menu: fetched list union with the
    /// configured default so the user-set model is always visible.
    private func modelOptions(for p: ProviderConfig) -> [String] {
        var ids = p.type.fallbackModelIDs
        if !p.model.isEmpty, !ids.contains(p.model) {
            ids.insert(p.model, at: 0)
        }
        return ids
    }

    private func select(provider: ProviderConfig, model: String) {
        activeProviderID = provider.id.uuidString
        activeModel = model
        refreshAPIKeyState()
    }

    @MainActor
    private func loadProviders() async {
        if let list = try? await DependencyContainer.shared.llmProviderRepository.fetchAll() {
            providers = list.filter(\.enabled)
        }
    }

    private var setupPrompt: some View {
        VStack(spacing: 16) {
            Spacer()
            ZStack {
                LinearGradient(
                    colors: [Color.purple.opacity(0.2), Color.blue.opacity(0.2)],
                    startPoint: .topLeading,
                    endPoint: .bottomTrailing
                )
                .frame(width: 72, height: 72)
                .clipShape(Circle())
                Image(systemName: "sparkles")
                    .font(.system(size: 30, weight: .medium))
                    .foregroundStyle(
                        LinearGradient(
                            colors: [.purple, .blue],
                            startPoint: .topLeading,
                            endPoint: .bottomTrailing
                        )
                    )
            }
            VStack(spacing: 6) {
                Text("Welcome to AI Assistant")
                    .font(.system(size: 15, weight: .semibold))
                Text("Connect an AI provider to start chatting\nwith your database.")
                    .font(.system(size: 12))
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
            }
            SettingsLink {
                HStack(spacing: 6) {
                    Image(systemName: "gearshape.fill")
                        .font(.system(size: 11))
                    Text("Configure AI Settings")
                        .font(.system(size: 12, weight: .medium))
                }
                .padding(.horizontal, 14)
                .padding(.vertical, 8)
                .background(
                    LinearGradient(
                        colors: [.purple, .blue],
                        startPoint: .leading,
                        endPoint: .trailing
                    )
                )
                .foregroundStyle(.white)
                .clipShape(Capsule())
            }
            .buttonStyle(.plain)
            Spacer()
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding()
    }

    /// Welcome screen with suggested prompts shown when chat is empty
    private var welcomeScreen: some View {
        ScrollView {
            VStack(spacing: 18) {
                Spacer().frame(height: 30)
                ZStack {
                    LinearGradient(
                        colors: [Color.purple.opacity(0.18), Color.blue.opacity(0.18)],
                        startPoint: .topLeading,
                        endPoint: .bottomTrailing
                    )
                    .frame(width: 64, height: 64)
                    .clipShape(Circle())
                    Image(systemName: "sparkles")
                        .font(.system(size: 26, weight: .medium))
                        .foregroundStyle(
                            LinearGradient(
                                colors: [.purple, .blue],
                                startPoint: .topLeading,
                                endPoint: .bottomTrailing
                            )
                        )
                }

                VStack(spacing: 4) {
                    Text("How can I help you?")
                        .font(.system(size: 16, weight: .semibold))
                    Text("Ask me anything about your database")
                        .font(.system(size: 12))
                        .foregroundStyle(.secondary)
                }

                // Suggested prompts
                VStack(spacing: 8) {
                    ForEach(suggestedPrompts, id: \.0) { prompt in
                        Button {
                            inputText = prompt.1
                        } label: {
                            HStack(spacing: 10) {
                                Image(systemName: prompt.0)
                                    .font(.system(size: 12))
                                    .foregroundStyle(.purple)
                                    .frame(width: 22, height: 22)
                                    .background(Color.purple.opacity(0.12))
                                    .clipShape(RoundedRectangle(cornerRadius: 6, style: .continuous))
                                Text(prompt.1)
                                    .font(.system(size: 12))
                                    .foregroundStyle(.primary)
                                    .multilineTextAlignment(.leading)
                                Spacer()
                                Image(systemName: "arrow.up.right")
                                    .font(.system(size: 9))
                                    .foregroundStyle(.tertiary)
                            }
                            .padding(.horizontal, 12)
                            .padding(.vertical, 10)
                            .background(Color.primary.opacity(0.04))
                            .clipShape(RoundedRectangle(cornerRadius: 10, style: .continuous))
                            .overlay {
                                RoundedRectangle(cornerRadius: 10, style: .continuous)
                                    .stroke(Color.primary.opacity(0.06), lineWidth: 1)
                            }
                        }
                        .buttonStyle(.plain)
                    }
                }
                .padding(.horizontal, 14)
                .padding(.top, 4)
                Spacer()
            }
        }
    }

    private var suggestedPrompts: [(String, String)] {
        [
            ("doc.text.magnifyingglass", "Show me the schema of all tables"),
            ("chart.bar.fill", "Find the top 10 records by date"),
            ("text.alignleft", "Generate a SQL query to count rows"),
            ("lightbulb.fill", "Explain the relationships between tables"),
        ]
    }

    private var messageList: some View {
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 14) {
                    ForEach(messages) { message in
                        MessageBubble(message: message)
                            .id(message.id)
                            .transition(.asymmetric(
                                insertion: .opacity.combined(with: .offset(y: 8)),
                                removal: .opacity
                            ))
                    }

                    if isStreaming, let last = messages.last, last.role == .assistant, last.content.isEmpty {
                        TypingIndicator()
                            .padding(.leading, 38)
                    }
                }
                .padding(.horizontal, 14)
                .padding(.vertical, 16)
            }
            .onChange(of: messages.count) { _, _ in
                withAnimation(.spring(response: 0.4, dampingFraction: 0.8)) {
                    if let last = messages.last {
                        proxy.scrollTo(last.id, anchor: .bottom)
                    }
                }
            }
        }
    }

    private var inputBar: some View {
        VStack(spacing: 0) {
            Divider().opacity(0.5)

            VStack(spacing: 6) {
                // Attached tables chips
                if !attachedTables.isEmpty {
                    ScrollView(.horizontal, showsIndicators: false) {
                        HStack(spacing: 6) {
                            ForEach(Array(attachedTables).sorted(), id: \.self) { table in
                                HStack(spacing: 4) {
                                    Image(systemName: "tablecells.fill")
                                        .font(.system(size: 9))
                                        .foregroundStyle(.purple)
                                    Text(table)
                                        .font(.system(size: 11, weight: .medium))
                                    Button {
                                        attachedTables.remove(table)
                                    } label: {
                                        Image(systemName: "xmark")
                                            .font(.system(size: 8, weight: .bold))
                                            .foregroundStyle(.secondary)
                                    }
                                    .buttonStyle(.plain)
                                }
                                .padding(.horizontal, 8)
                                .padding(.vertical, 4)
                                .background(Color.purple.opacity(0.12))
                                .clipShape(Capsule())
                            }
                        }
                        .padding(.horizontal, 14)
                    }
                    .padding(.top, 10)
                }

                // Input capsule
                HStack(spacing: 6) {
                    // Attach button
                    Button { showTablePicker.toggle() } label: {
                        Image(systemName: "plus")
                            .font(.system(size: 14, weight: .medium))
                            .foregroundStyle(.secondary)
                            .frame(width: 28, height: 28)
                            .background(Color.primary.opacity(0.06))
                            .clipShape(Circle())
                    }
                    .buttonStyle(.plain)
                    .help("Attach tables for context")
                    .popover(isPresented: $showTablePicker, arrowEdge: .top) {
                        tablePickerPopover
                    }

                    TextField("Ask anything...", text: $inputText, axis: .vertical)
                        .textFieldStyle(.plain)
                        .font(.system(size: 13))
                        .lineLimit(1...5)
                        .onSubmit { sendMessage() }
                        .padding(.horizontal, 4)

                    // Send button — gradient when ready
                    Button(action: sendMessage) {
                        ZStack {
                            if canSend {
                                LinearGradient(
                                    colors: [.purple, .blue],
                                    startPoint: .topLeading,
                                    endPoint: .bottomTrailing
                                )
                            } else {
                                Color.primary.opacity(0.08)
                            }
                            Image(systemName: "arrow.up")
                                .font(.system(size: 13, weight: .bold))
                                .foregroundStyle(canSend ? .white : .secondary)
                        }
                        .frame(width: 28, height: 28)
                        .clipShape(Circle())
                    }
                    .buttonStyle(.plain)
                    .disabled(!canSend)
                    .animation(.easeInOut(duration: 0.15), value: canSend)
                }
                .padding(.horizontal, 8)
                .padding(.vertical, 6)
                .background(Color.primary.opacity(0.04))
                .clipShape(RoundedRectangle(cornerRadius: 18, style: .continuous))
                .overlay {
                    RoundedRectangle(cornerRadius: 18, style: .continuous)
                        .stroke(Color.primary.opacity(0.08), lineWidth: 1)
                }
                .padding(.horizontal, 12)
                .padding(.top, attachedTables.isEmpty ? 10 : 4)
                .padding(.bottom, 12)
            }
        }
    }

    private var canSend: Bool {
        apiKeyLoaded && !inputText.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty && !isStreaming
    }

    private var tablePickerPopover: some View {
        let tables = availableTableNames
        return VStack(spacing: 0) {
            HStack(spacing: 6) {
                Image(systemName: "tablecells.fill")
                    .font(.system(size: 11))
                    .foregroundStyle(.purple)
                Text("Attach Tables")
                    .font(.system(size: 12, weight: .semibold))
                Spacer()
                if !attachedTables.isEmpty {
                    Text("\(attachedTables.count)")
                        .font(.system(size: 10, weight: .semibold))
                        .foregroundStyle(.purple)
                        .padding(.horizontal, 6)
                        .padding(.vertical, 1)
                        .background(Color.purple.opacity(0.15))
                        .clipShape(Capsule())
                }
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 10)

            Divider().opacity(0.5)

            if tables.isEmpty {
                VStack(spacing: 6) {
                    Image(systemName: "tray")
                        .font(.system(size: 20))
                        .foregroundStyle(.tertiary)
                    Text("No tables available")
                        .font(.system(size: 11))
                        .foregroundStyle(.secondary)
                }
                .frame(maxWidth: .infinity)
                .padding(20)
            } else {
                ScrollView {
                    VStack(alignment: .leading, spacing: 1) {
                        ForEach(tables, id: \.self) { name in
                            let isSelected = attachedTables.contains(name)
                            Button {
                                if isSelected {
                                    attachedTables.remove(name)
                                } else {
                                    attachedTables.insert(name)
                                }
                            } label: {
                                HStack(spacing: 8) {
                                    Image(systemName: isSelected ? "checkmark.circle.fill" : "circle")
                                        .font(.system(size: 13))
                                        .foregroundStyle(isSelected ? Color.purple : Color.secondary)
                                    Image(systemName: "tablecells")
                                        .font(.system(size: 10))
                                        .foregroundStyle(.secondary)
                                    Text(name)
                                        .font(.system(size: 12))
                                        .foregroundStyle(.primary)
                                    Spacer()
                                }
                                .padding(.horizontal, 10)
                                .padding(.vertical, 6)
                                .background(isSelected ? Color.purple.opacity(0.08) : Color.clear)
                                .clipShape(RoundedRectangle(cornerRadius: 6, style: .continuous))
                                .contentShape(Rectangle())
                            }
                            .buttonStyle(.plain)
                        }
                    }
                    .padding(6)
                }
                .frame(maxHeight: 280)
            }
        }
        .frame(width: 240)
    }

    private var availableTableNames: [String] {
        appState.sidebarItems.flatMap { item -> [String] in
            item.children.compactMap { child in
                switch child.type {
                case .table(let name): return name
                case .view(let name): return name
                default: return nil
                }
            }
        }.sorted()
    }

    // MARK: - Logic

    private func refreshAPIKeyState() {
        // Multi-provider path: an active provider ID is set.
        if !activeProviderID.isEmpty, let uuid = UUID(uuidString: activeProviderID) {
            let key = try? DependencyContainer.shared.keychainService.load(key: "ai.apikey.\(uuid.uuidString)")
            // Ollama has no key but is still usable → treat presence of an active ID as ready
            // unless we can verify the config requires a key and none is set.
            Task {
                let config = try? await DependencyContainer.shared.llmProviderRepository.fetchByID(uuid)
                await MainActor.run {
                    if let config, config.enabled {
                        apiKeyLoaded = !config.type.requiresAPIKey || (key != nil && !key!.isEmpty)
                    } else {
                        apiKeyLoaded = false
                    }
                }
            }
            return
        }
        // Legacy path.
        guard isEnabled else { apiKeyLoaded = false; return }
        let key = try? DependencyContainer.shared.keychainService.load(key: "ai.apikey.\(providerType)")
        apiKeyLoaded = key != nil && !key!.isEmpty
    }

    /// Resolve the LLMService + model to use for the next request.
    /// Prefers the multi-provider registry; falls back to legacy @AppStorage.
    /// If the user picked a different model from the header menu (stored in
    /// `activeModel`), it overrides the config's default.
    private func resolveLLMService() async -> (service: any LLMService, model: String)? {
        if !activeProviderID.isEmpty, let uuid = UUID(uuidString: activeProviderID),
           let config = try? await DependencyContainer.shared.llmProviderRepository.fetchByID(uuid),
           config.enabled {
            let apiKey = (try? DependencyContainer.shared.keychainService.load(
                key: ProviderEditSheet.keychainKey(id: uuid)
            )) ?? ""
            if config.type.requiresAPIKey, apiKey.isEmpty { return nil }
            let service = ProviderFactory.make(config: config, apiKey: apiKey)
            let modelToUse = activeModel.isEmpty ? config.model : activeModel
            return (service, modelToUse)
        }
        // Legacy
        guard let apiKey = try? DependencyContainer.shared.keychainService.load(key: "ai.apikey.\(providerType)"),
              !apiKey.isEmpty else { return nil }
        let service = DependencyContainer.shared.makeLLMService(
            provider: providerType,
            apiKey: apiKey,
            baseURL: baseURL
        )
        return (service, selectedModel)
    }

    private func sendMessage() {
        let text = inputText.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty else { return }

        let userMsg = ChatDisplayMessage(role: .user, content: text)
        appendMessage(userMsg)
        inputText = ""
        isStreaming = true

        // Capture attached tables for this message
        let currentAttached = attachedTables

        Task {
            guard let resolved = await resolveLLMService() else {
                errorMessage = "No active AI provider configured. Open Settings → AI to add one."
                isStreaming = false
                return
            }
            let service = resolved.service
            let modelID = resolved.model

            // Build system prompt — general rules only
            var systemPrompt = "You are a helpful database assistant. Answer concisely."
            if let connectionId = appState.activeConnectionId,
               let config = appState.activeConfig {
                let contextEngine = DependencyContainer.shared.aiContextEngine
                if let schema = await DependencyContainer.shared.schemaCache.get(connectionId: connectionId) {
                    let context = try? await contextEngine.buildContext(for: text, schema: schema, tokenBudget: 8000)
                    if let context {
                        systemPrompt = contextEngine.buildSystemPrompt(
                            databaseType: config.databaseType,
                            connectionInfo: "\(config.host ?? "localhost"):\(config.port)",
                            context: context
                        )
                    }
                }
            }

            // Build user message content — attach table schemas inline
            var userContent = ""
            if !currentAttached.isEmpty, let adapter = appState.activeAdapter {
                var schemaLines: [String] = []
                for tableName in currentAttached.sorted() {
                    if let desc = try? await adapter.describeTable(name: tableName, schema: nil) {
                        let cols = desc.columns.map {
                            "\($0.name) \($0.dataType)\($0.isPrimaryKey ? " PK" : "")\($0.isNullable ? "" : " NOT NULL")"
                        }.joined(separator: ", ")
                        schemaLines.append("  \(tableName) (\(cols))")
                    }
                }
                if !schemaLines.isEmpty {
                    userContent += "--- Schema Context ---\n" + schemaLines.joined(separator: "\n") + "\n\n"
                }
            }
            userContent += "--- Question ---\n" + text

            // Build conversation history for API
            // Previous messages go as-is, last user message replaced with enriched content
            var llmMessages: [LLMMessage] = []
            let allMessages = messages
            for (i, msg) in allMessages.enumerated() {
                if i == allMessages.count - 1 && msg.role == .user {
                    // Last user message — use enriched content with schema
                    llmMessages.append(LLMMessage(role: .user, content: userContent))
                } else {
                    llmMessages.append(LLMMessage(
                        role: msg.role == .user ? .user : .assistant,
                        content: msg.content
                    ))
                }
            }

            // Stream response
            let assistantMsg = ChatDisplayMessage(role: .assistant, content: "")
            appendMessage(assistantMsg)

            do {
                let stream = service.stream(
                    messages: llmMessages,
                    systemPrompt: systemPrompt,
                    model: modelID,
                    maxTokens: AppConstants.AI.defaultMaxTokens,
                    temperature: AppConstants.AI.defaultTemperature
                )
                for try await token in stream {
                    assistantMsg.content += token
                }
            } catch {
                if assistantMsg.content.isEmpty {
                    assistantMsg.content = "Error: \(error.localizedDescription)"
                }
            }
            isStreaming = false
        }
    }
}

// MARK: - Message Bubble

struct MessageBubble: View {
    @ObservedObject var message: ChatDisplayMessage

    var body: some View {
        HStack(alignment: .top, spacing: 10) {
            if message.role == .assistant {
                avatar
            } else {
                Spacer(minLength: 40)
            }

            VStack(alignment: message.role == .user ? .trailing : .leading, spacing: 3) {
                // Sender label
                Text(message.role == .user ? "You" : "Assistant")
                    .font(.system(size: 10, weight: .semibold))
                    .foregroundStyle(.tertiary)
                    .padding(.horizontal, 2)

                // Bubble content — either plain text or parsed markdown (with code blocks)
                MessageContent(text: message.content.isEmpty ? " " : message.content, role: message.role)
            }

            if message.role == .user {
                avatar
            } else {
                Spacer(minLength: 40)
            }
        }
    }

    @ViewBuilder
    private var avatar: some View {
        if message.role == .assistant {
            ZStack {
                LinearGradient(
                    colors: [.purple, .blue],
                    startPoint: .topLeading,
                    endPoint: .bottomTrailing
                )
                .frame(width: 28, height: 28)
                .clipShape(Circle())
                Image(systemName: "sparkles")
                    .font(.system(size: 12, weight: .semibold))
                    .foregroundStyle(.white)
            }
        } else {
            ZStack {
                Color.primary.opacity(0.1)
                    .frame(width: 28, height: 28)
                    .clipShape(Circle())
                Image(systemName: "person.fill")
                    .font(.system(size: 12, weight: .semibold))
                    .foregroundStyle(.secondary)
            }
        }
    }
}

// MARK: - Message Content (parses markdown code fences)

/// A message content view that parses markdown fenced code blocks (```lang ... ```)
/// and renders them as styled code blocks with language label and copy button.
/// Regular text is rendered as a normal bubble.
struct MessageContent: View {
    let text: String
    let role: ChatDisplayMessage.ChatRole

    var body: some View {
        let segments = parseSegments(text)
        VStack(alignment: role == .user ? .trailing : .leading, spacing: 6) {
            ForEach(Array(segments.enumerated()), id: \.offset) { _, segment in
                switch segment {
                case .text(let content):
                    textBubble(content)
                case .code(let language, let code):
                    CodeBlockView(language: language, code: code)
                }
            }
        }
    }

    @ViewBuilder
    private func textBubble(_ content: String) -> some View {
        Text(content)
            .font(.system(size: 13))
            .lineSpacing(2)
            .textSelection(.enabled)
            .multilineTextAlignment(.leading)
            .padding(.horizontal, 12)
            .padding(.vertical, 9)
            .background(textBubbleBackground)
            .clipShape(textBubbleShape)
            .overlay {
                textBubbleShape
                    .stroke(role == .user ? Color.clear : Color.primary.opacity(0.06), lineWidth: 1)
            }
            .foregroundStyle(role == .user ? Color.white : Color.primary)
    }

    private var textBubbleShape: some Shape {
        UnevenRoundedRectangle(
            cornerRadii: .init(
                topLeading: 14,
                bottomLeading: role == .user ? 14 : 4,
                bottomTrailing: role == .user ? 4 : 14,
                topTrailing: 14
            ),
            style: .continuous
        )
    }

    @ViewBuilder
    private var textBubbleBackground: some View {
        if role == .user {
            LinearGradient(
                colors: [.purple, .blue],
                startPoint: .topLeading,
                endPoint: .bottomTrailing
            )
        } else {
            Color.primary.opacity(0.05)
        }
    }

    // MARK: - Parser

    enum Segment {
        case text(String)
        case code(language: String, code: String)
    }

    /// Parse the input for markdown fenced code blocks (```lang\ncode\n```).
    /// Returns alternating text/code segments, skipping empty text segments.
    private func parseSegments(_ input: String) -> [Segment] {
        var segments: [Segment] = []
        let lines = input.components(separatedBy: "\n")
        var i = 0
        var currentText: [String] = []

        func flushText() {
            let joined = currentText.joined(separator: "\n").trimmingCharacters(in: .whitespacesAndNewlines)
            if !joined.isEmpty {
                segments.append(.text(joined))
            }
            currentText = []
        }

        while i < lines.count {
            let line = lines[i]
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            // Detect opening fence: ``` or ```lang
            if trimmed.hasPrefix("```") {
                let lang = String(trimmed.dropFirst(3)).trimmingCharacters(in: .whitespaces)
                flushText()
                // Scan until closing fence
                var codeLines: [String] = []
                i += 1
                while i < lines.count {
                    let codeLine = lines[i]
                    if codeLine.trimmingCharacters(in: .whitespaces) == "```" {
                        break
                    }
                    codeLines.append(codeLine)
                    i += 1
                }
                segments.append(.code(language: lang.isEmpty ? "plain" : lang, code: codeLines.joined(separator: "\n")))
                i += 1
                continue
            }
            currentText.append(line)
            i += 1
        }
        flushText()
        return segments
    }
}

// MARK: - Code Block View

struct CodeBlockView: View {
    let language: String
    let code: String
    @State private var copied = false

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            // Header: language label + copy button
            HStack(spacing: 6) {
                Image(systemName: languageIcon)
                    .font(.system(size: 10))
                    .foregroundStyle(.secondary)
                Text(language.uppercased())
                    .font(.system(size: 10, weight: .semibold, design: .monospaced))
                    .foregroundStyle(.secondary)
                Spacer()
                Button(action: copyCode) {
                    HStack(spacing: 4) {
                        Image(systemName: copied ? "checkmark" : "doc.on.doc")
                            .font(.system(size: 10))
                        Text(copied ? "Copied" : "Copy")
                            .font(.system(size: 10, weight: .medium))
                    }
                    .foregroundStyle(copied ? .green : .secondary)
                }
                .buttonStyle(.plain)
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 6)
            .background(Color.primary.opacity(0.06))

            Divider().opacity(0.3)

            // Code content
            ScrollView(.horizontal, showsIndicators: false) {
                Text(code)
                    .font(.system(size: 12, design: .monospaced))
                    .textSelection(.enabled)
                    .lineSpacing(2)
                    .padding(.horizontal, 12)
                    .padding(.vertical, 10)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            .background(Color(nsColor: .textBackgroundColor).opacity(0.5))
        }
        .clipShape(RoundedRectangle(cornerRadius: 10, style: .continuous))
        .overlay {
            RoundedRectangle(cornerRadius: 10, style: .continuous)
                .stroke(Color.primary.opacity(0.1), lineWidth: 1)
        }
    }

    private var languageIcon: String {
        let lang = language.lowercased()
        if lang == "sql" || lang == "mysql" || lang == "postgresql" || lang == "mssql" {
            return "cylinder.split.1x2"
        }
        if lang == "json" || lang == "javascript" || lang == "js" || lang == "typescript" || lang == "ts" {
            return "curlybraces"
        }
        if lang == "python" || lang == "py" || lang == "swift" || lang == "rust" || lang == "go" {
            return "chevron.left.forwardslash.chevron.right"
        }
        return "text.alignleft"
    }

    private func copyCode() {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(code, forType: .string)
        copied = true
        Task {
            try? await Task.sleep(nanoseconds: 2_000_000_000)
            copied = false
        }
    }
}

// MARK: - Typing Indicator (animated dots)

struct TypingIndicator: View {
    @State private var phase: CGFloat = 0

    var body: some View {
        HStack(spacing: 4) {
            ForEach(0..<3) { i in
                Circle()
                    .fill(Color.secondary)
                    .frame(width: 6, height: 6)
                    .scaleEffect(scaleForDot(at: i))
                    .opacity(opacityForDot(at: i))
            }
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 10)
        .background(Color.primary.opacity(0.05))
        .clipShape(
            UnevenRoundedRectangle(
                cornerRadii: .init(
                    topLeading: 14,
                    bottomLeading: 4,
                    bottomTrailing: 14,
                    topTrailing: 14
                ),
                style: .continuous
            )
        )
        .onAppear {
            withAnimation(.easeInOut(duration: 0.9).repeatForever(autoreverses: false)) {
                phase = 1
            }
        }
    }

    private func scaleForDot(at index: Int) -> CGFloat {
        let offset = CGFloat(index) * 0.2
        let progress = (phase + offset).truncatingRemainder(dividingBy: 1)
        return 0.7 + 0.5 * sin(progress * .pi)
    }

    private func opacityForDot(at index: Int) -> CGFloat {
        let offset = CGFloat(index) * 0.2
        let progress = (phase + offset).truncatingRemainder(dividingBy: 1)
        return 0.4 + 0.6 * sin(progress * .pi)
    }
}
