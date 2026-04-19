// SwiftDataLLMProviderRepository.swift
// Gridex

import Foundation
import SwiftData

final class SwiftDataLLMProviderRepository: LLMProviderRepository, @unchecked Sendable {
    private let modelContainer: ModelContainer

    init(modelContainer: ModelContainer) {
        self.modelContainer = modelContainer
    }

    @MainActor
    func fetchAll() async throws -> [ProviderConfig] {
        let context = modelContainer.mainContext
        let descriptor = FetchDescriptor<LLMProviderEntity>(
            sortBy: [SortDescriptor(\.createdAt)]
        )
        return try context.fetch(descriptor).map { $0.toConfig() }
    }

    @MainActor
    func fetchByID(_ id: UUID) async throws -> ProviderConfig? {
        let context = modelContainer.mainContext
        var descriptor = FetchDescriptor<LLMProviderEntity>(
            predicate: #Predicate { $0.id == id }
        )
        descriptor.fetchLimit = 1
        return try context.fetch(descriptor).first?.toConfig()
    }

    @MainActor
    func save(_ config: ProviderConfig) async throws {
        let context = modelContainer.mainContext
        let entity = LLMProviderEntity(
            id: config.id,
            name: config.name,
            typeRaw: config.type.rawValue,
            apiBase: config.apiBase,
            model: config.model,
            enabled: config.enabled,
            createdAt: config.createdAt
        )
        context.insert(entity)
        try context.save()
    }

    @MainActor
    func update(_ config: ProviderConfig) async throws {
        let context = modelContainer.mainContext
        let id = config.id
        var descriptor = FetchDescriptor<LLMProviderEntity>(
            predicate: #Predicate { $0.id == id }
        )
        descriptor.fetchLimit = 1
        guard let entity = try context.fetch(descriptor).first else { return }
        entity.apply(config)
        try context.save()
    }

    @MainActor
    func delete(_ id: UUID) async throws {
        let context = modelContainer.mainContext
        let descriptor = FetchDescriptor<LLMProviderEntity>(
            predicate: #Predicate { $0.id == id }
        )
        if let entity = try context.fetch(descriptor).first {
            context.delete(entity)
            try context.save()
        }
    }
}
