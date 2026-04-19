// ProviderConfig.swift
// Gridex
//
// Runtime-level configuration for an LLM provider instance.
// Persisted form lives in LLMProviderEntity; this struct is the in-memory DTO
// passed from repository → factory → provider.

import Foundation

struct ProviderConfig: Sendable, Identifiable, Equatable {
    let id: UUID
    var name: String            // user-defined slug, e.g. "my-claude"
    var type: ProviderType
    var apiBase: String?        // nil → type.defaultBaseURL
    var model: String           // default model for this instance
    var enabled: Bool
    var createdAt: Date

    init(
        id: UUID = UUID(),
        name: String,
        type: ProviderType,
        apiBase: String? = nil,
        model: String? = nil,
        enabled: Bool = true,
        createdAt: Date = Date()
    ) {
        self.id = id
        self.name = name
        self.type = type
        self.apiBase = apiBase
        self.model = model ?? type.defaultModel
        self.enabled = enabled
        self.createdAt = createdAt
    }

    /// Resolved base URL — falls back to the type default when apiBase is nil/empty.
    var resolvedBaseURL: String {
        if let apiBase, !apiBase.isEmpty { return apiBase }
        return type.defaultBaseURL
    }
}
