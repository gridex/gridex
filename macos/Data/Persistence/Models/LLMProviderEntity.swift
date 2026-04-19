// LLMProviderEntity.swift
// Gridex
//
// SwiftData model for persisted LLM provider configurations.
// The API key is stored in the Keychain (key = "ai.apikey.<id>"), never in DB.

import Foundation
import SwiftData

@Model
final class LLMProviderEntity {
    @Attribute(.unique) var id: UUID
    var name: String
    var typeRaw: String
    var apiBase: String?
    var model: String
    var enabled: Bool
    var createdAt: Date

    init(
        id: UUID = UUID(),
        name: String,
        typeRaw: String,
        apiBase: String? = nil,
        model: String,
        enabled: Bool = true,
        createdAt: Date = Date()
    ) {
        self.id = id
        self.name = name
        self.typeRaw = typeRaw
        self.apiBase = apiBase
        self.model = model
        self.enabled = enabled
        self.createdAt = createdAt
    }

    func toConfig() -> ProviderConfig {
        ProviderConfig(
            id: id,
            name: name,
            type: ProviderType(rawValue: typeRaw) ?? .anthropic,
            apiBase: apiBase,
            model: model,
            enabled: enabled,
            createdAt: createdAt
        )
    }

    func apply(_ config: ProviderConfig) {
        name = config.name
        typeRaw = config.type.rawValue
        apiBase = config.apiBase
        model = config.model
        enabled = config.enabled
    }
}
