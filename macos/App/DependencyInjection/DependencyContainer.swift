// DependencyContainer.swift
// Gridex
//
// Central dependency injection container.

import Foundation
import SwiftData

@MainActor
final class DependencyContainer {

    /// Shared instance — ensures SwiftData ModelContainer is created only once,
    /// even when multiple windows each own their own AppState.
    static let shared = DependencyContainer()

    // MARK: - SwiftData

    lazy var modelContainer: ModelContainer = {
        let schema = Schema([
            SavedConnectionEntity.self,
            QueryHistoryEntity.self,
            LLMProviderEntity.self,
        ])
        let config = ModelConfiguration(schema: schema, isStoredInMemoryOnly: false)
        do {
            return try ModelContainer(for: schema, configurations: [config])
        } catch {
            fatalError("Failed to create ModelContainer: \(error)")
        }
    }()

    // MARK: - Core Services

    lazy var keychainService: KeychainServiceProtocol = KeychainService()
    lazy var schemaCache = SchemaCache()
    lazy var connectionManager = ConnectionManager()

    // MARK: - Repositories

    lazy var connectionRepository: any ConnectionRepository = SwiftDataConnectionRepository(modelContainer: modelContainer)
    lazy var queryHistoryRepository: any QueryHistoryRepository = SwiftDataQueryHistoryRepository(modelContainer: modelContainer)
    lazy var llmProviderRepository: any LLMProviderRepository = SwiftDataLLMProviderRepository(modelContainer: modelContainer)

    // MARK: - Services

    lazy var queryEngine: QueryEngine = {
        QueryEngine(connectionManager: connectionManager, historyRepository: queryHistoryRepository)
    }()

    lazy var schemaInspector = SchemaInspectorService(cache: schemaCache, connectionManager: connectionManager)
    lazy var aiContextEngine = AIContextEngine(schemaCache: schemaCache)
    lazy var sshTunnelService = SSHTunnelService()
    lazy var exportService = ExportService()

    // MARK: - MCP Server

    lazy var mcpServer: MCPServer = {
        MCPServer(
            connectionManager: connectionManager,
            connectionRepository: connectionRepository,
            serverVersion: Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "1.0.0"
        )
    }()

    /// Start MCP server if enabled. Call once at app startup.
    func bootstrapMCPServer() async {
        let enabled = UserDefaults.standard.bool(forKey: "mcp.enabled")
        guard enabled else { return }

        // Set start time for uptime tracking
        let currentStartTime = UserDefaults.standard.double(forKey: "mcp.startTime")
        if currentStartTime == 0 {
            UserDefaults.standard.set(Date().timeIntervalSince1970, forKey: "mcp.startTime")
        }

        // Load MCP modes for all connections
        let configs = (try? await connectionRepository.fetchAll()) ?? []
        for config in configs {
            await mcpServer.setConnectionMode(config.mcpMode, for: config.id)
        }

        await mcpServer.start()
    }

    // MARK: - AI

    lazy var providerRegistry = ProviderRegistry()

    /// Load all enabled providers from persistence and register them with the registry.
    /// Call once at app startup. Safe to call again after config changes.
    func bootstrapProviderRegistry() async {
        await providerRegistry.removeAll()
        let configs = (try? await llmProviderRepository.fetchAll()) ?? []
        for config in configs where config.enabled {
            let apiKey = (try? keychainService.load(key: "ai.apikey.\(config.id.uuidString)")) ?? ""
            await providerRegistry.register(config, apiKey: apiKey)
        }
    }

    /// Build a one-off LLMService. Primary path for the single-active-provider UI;
    /// multi-provider callers should go through `providerRegistry`.
    func makeLLMService(provider: String, apiKey: String, baseURL: String? = nil) -> any LLMService {
        let type = ProviderType(rawValue: provider) ?? .anthropic
        return ProviderFactory.make(type: type, apiKey: apiKey, baseURL: baseURL)
    }
}
