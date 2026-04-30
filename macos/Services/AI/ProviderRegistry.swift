// ProviderRegistry.swift
// Gridex
//
// Thread-safe registry of live LLMService instances keyed by user-defined name.
// Mirrors goclaw internal/providers/registry.go — on replacement the prior
// instance is released (concrete providers hold only short-lived URLSession
// tasks, so nothing further is required).

import Foundation

actor ProviderRegistry {
    private struct Entry {
        let config: ProviderConfig
        let service: any LLMService
    }

    private var entries: [String: Entry] = [:]

    /// Register or replace a provider by name.
    /// Pass `chatGPTOAuthService` when `config.type == .chatGPT`; ignored otherwise.
    func register(
        _ config: ProviderConfig,
        apiKey: String,
        chatGPTOAuthService: ChatGPTOAuthService? = nil
    ) {
        let service = ProviderFactory.make(
            config: config,
            apiKey: apiKey,
            chatGPTOAuthService: chatGPTOAuthService
        )
        entries[config.name] = Entry(config: config, service: service)
    }

    /// Remove a provider by name. No-op if missing.
    func unregister(_ name: String) {
        entries.removeValue(forKey: name)
    }

    /// Remove all providers.
    func removeAll() {
        entries.removeAll()
    }

    /// Resolve a provider by name. Returns nil if not registered.
    func resolve(_ name: String) -> (any LLMService)? {
        entries[name]?.service
    }

    /// Resolve the first enabled provider of a given type. Handy when the UI
    /// still drives selection by type rather than name.
    func resolve(type: ProviderType) -> (any LLMService)? {
        entries.values.first(where: { $0.config.type == type && $0.config.enabled })?.service
    }

    /// All registered configs, for UI listing.
    func listConfigs() -> [ProviderConfig] {
        entries.values.map(\.config).sorted { $0.createdAt < $1.createdAt }
    }
}
