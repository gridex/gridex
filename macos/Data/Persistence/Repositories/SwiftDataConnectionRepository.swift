// SwiftDataConnectionRepository.swift
// Gridex
//
// SwiftData implementation of ConnectionRepository.

import Foundation
import SwiftData

final class SwiftDataConnectionRepository: ConnectionRepository, @unchecked Sendable {
    private let modelContainer: ModelContainer

    init(modelContainer: ModelContainer) {
        self.modelContainer = modelContainer
    }

    @MainActor
    func fetchAll() async throws -> [ConnectionConfig] {
        let context = modelContainer.mainContext
        let descriptor = FetchDescriptor<SavedConnectionEntity>(
            sortBy: [SortDescriptor(\.sortOrder)]
        )
        let entities = try context.fetch(descriptor)
        return entities.map { $0.toConfig() }
    }

    @MainActor
    func fetchByID(_ id: UUID) async throws -> ConnectionConfig? {
        let context = modelContainer.mainContext
        var descriptor = FetchDescriptor<SavedConnectionEntity>(
            predicate: #Predicate { $0.id == id }
        )
        descriptor.fetchLimit = 1
        return try context.fetch(descriptor).first?.toConfig()
    }

    @MainActor
    func fetchByGroup(_ group: String) async throws -> [ConnectionConfig] {
        let context = modelContainer.mainContext
        let descriptor = FetchDescriptor<SavedConnectionEntity>(
            predicate: #Predicate { $0.group == group },
            sortBy: [SortDescriptor(\.sortOrder)]
        )
        return try context.fetch(descriptor).map { $0.toConfig() }
    }

    @MainActor
    func save(_ config: ConnectionConfig) async throws {
        let context = modelContainer.mainContext
        let entity = SavedConnectionEntity(
            id: config.id,
            name: config.name,
            databaseType: config.databaseType.rawValue,
            host: config.host,
            port: config.port,
            database: config.database,
            username: config.username,
            sslEnabled: config.sslEnabled,
            sshEnabled: config.sshConfig != nil,
            sshHost: config.sshConfig?.host,
            sshPort: config.sshConfig?.port,
            sshUsername: config.sshConfig?.username,
            sshAuthMethod: config.sshConfig?.authMethod.rawValue,
            sshKeyPath: config.sshConfig?.keyPath,
            colorTag: config.colorTag?.rawValue,
            group: config.group,
            filePath: config.filePath,
            mcpMode: config.mcpMode.rawValue
        )
        context.insert(entity)
        try context.save()
    }

    @MainActor
    func update(_ config: ConnectionConfig) async throws {
        let context = modelContainer.mainContext
        let id = config.id
        var descriptor = FetchDescriptor<SavedConnectionEntity>(
            predicate: #Predicate { $0.id == id }
        )
        descriptor.fetchLimit = 1
        guard let entity = try context.fetch(descriptor).first else { return }

        entity.name = config.name
        entity.databaseType = config.databaseType.rawValue
        entity.host = config.host
        entity.port = config.port
        entity.database = config.database
        entity.username = config.username
        entity.sslEnabled = config.sslEnabled
        entity.colorTag = config.colorTag?.rawValue
        entity.group = config.group
        entity.filePath = config.filePath
        entity.sshEnabled = config.sshConfig != nil
        entity.sshHost = config.sshConfig?.host
        entity.sshPort = config.sshConfig?.port
        entity.sshUsername = config.sshConfig?.username
        entity.sshAuthMethod = config.sshConfig?.authMethod.rawValue
        entity.sshKeyPath = config.sshConfig?.keyPath
        entity.mcpMode = config.mcpMode.rawValue

        try context.save()
    }

    @MainActor
    func delete(_ id: UUID) async throws {
        let context = modelContainer.mainContext
        let descriptor = FetchDescriptor<SavedConnectionEntity>(
            predicate: #Predicate { $0.id == id }
        )
        if let entity = try context.fetch(descriptor).first {
            context.delete(entity)
            try context.save()
        }
    }

    @MainActor
    func updateLastConnected(_ id: UUID, date: Date) async throws {
        let context = modelContainer.mainContext
        var descriptor = FetchDescriptor<SavedConnectionEntity>(
            predicate: #Predicate { $0.id == id }
        )
        descriptor.fetchLimit = 1
        if let entity = try context.fetch(descriptor).first {
            entity.lastConnectedAt = date
            try context.save()
        }
    }

    @MainActor
    func reorder(ids: [UUID]) async throws {
        // TODO: Update sortOrder for each entity
    }
}
