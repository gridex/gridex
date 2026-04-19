// LLMProviderRepository.swift
// Gridex

import Foundation

protocol LLMProviderRepository: Sendable {
    func fetchAll() async throws -> [ProviderConfig]
    func fetchByID(_ id: UUID) async throws -> ProviderConfig?
    func save(_ config: ProviderConfig) async throws
    func update(_ config: ProviderConfig) async throws
    func delete(_ id: UUID) async throws
}
