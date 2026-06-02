// main.swift
// Gridex
//
// Application entry point. Supports both GUI and MCP CLI modes.

import SwiftUI
import SwiftData
import Dispatch

// Check for MCP CLI mode
if CommandLine.arguments.contains("--mcp-stdio") {
    runMCPServer()
} else {
    // Run normal GUI app
    GridexApp.main()
}

// MARK: - MCP Server Mode

func runMCPServer() -> Never {
    // Disable buffering for stdio
    setbuf(stdout, nil)
    setbuf(stderr, nil)

    // Initialize SwiftData with same schema as main app
    let schema = Schema([
        SavedConnectionEntity.self,
        QueryHistoryEntity.self,
        LLMProviderEntity.self,
    ])

    let config = ModelConfiguration(schema: schema, isStoredInMemoryOnly: false)

    guard let modelContainer = try? ModelContainer(for: schema, configurations: [config]) else {
        fputs("Error: Failed to initialize database\n", stderr)
        exit(1)
    }

    // Run async server on a detached task (not MainActor-bound).
    // Main thread must remain free for dispatchMain() to service it.
    Task.detached {
        await runMCPServerAsync(modelContainer: modelContainer)
    }

    // Keep main thread alive; lets Swift concurrency executors run.
    dispatchMain()
}

func runMCPServerAsync(modelContainer: ModelContainer) async {
    // Create services
    let connectionManager = ConnectionManager()
    let connectionRepository = SwiftDataConnectionRepository(modelContainer: modelContainer)
    let keychain = KeychainService()

    // Create MCP server with stdio transport for CLI mode
    let mcpServer = MCPServer(
        connectionManager: connectionManager,
        connectionRepository: connectionRepository,
        serverVersion: Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "1.0.0",
        transportMode: .stdio
    )

    // Load connections, register their MCP modes, and pre-open DB connections so
    // tools that require an active adapter (list_tables, query, …) work without
    // requiring the GUI app to be open. The stdio entrypoint runs in a separate
    // process from the main GUI, so it cannot share live adapters with it.
    do {
        let configs = try await connectionRepository.fetchAll()
        for config in configs where config.mcpMode != .locked {
            await mcpServer.setConnectionMode(config.mcpMode, for: config.id)

            let password = (try? keychain.loadPassword(connectionId: config.id)) ?? nil
            let sshPassword = config.sshConfig != nil
                ? ((try? keychain.loadSSHPassword(connectionId: config.id)) ?? nil)
                : nil
            do {
                _ = try await connectionManager.connect(
                    config: config,
                    password: password,
                    sshPassword: sshPassword
                )
            } catch {
                fputs("Warning: Failed to open connection '\(config.name)': \(error)\n", stderr)
            }
        }
    } catch {
        fputs("Warning: Failed to load connections: \(error)\n", stderr)
    }

    // Start server
    await mcpServer.start()

    // Keep MCPServer alive — if this function returns, the transport's
    // weak delegate on the server becomes nil and requests stop working.
    while !Task.isCancelled {
        try? await Task.sleep(nanoseconds: 1_000_000_000)
    }
    _ = mcpServer
}
