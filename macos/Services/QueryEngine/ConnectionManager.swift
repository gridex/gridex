// ConnectionManager.swift
// Gridex
//
// Manages active database connections and adapter lifecycle.

import Foundation

actor ConnectionManager {
    private var activeConnections: [UUID: ActiveConnection] = [:]
    private let sshService = SSHTunnelService()

    func connect(config: ConnectionConfig, password: String?, sshPassword: String?) async throws -> ActiveConnection {
        let adapter = createAdapter(for: config.databaseType)

        var effectiveConfig = config

        // If SSH tunnel is configured, establish it first and redirect connection through localhost
        if let ssh = config.sshConfig {
            let remoteHost = config.host ?? "127.0.0.1"
            let remotePort = config.port ?? config.databaseType.defaultPort
            let localPort = try await sshService.establish(
                connectionId: config.id,
                config: ssh,
                remoteHost: remoteHost,
                remotePort: remotePort,
                password: sshPassword
            )
            effectiveConfig.host = "127.0.0.1"
            effectiveConfig.port = Int(localPort)
        }

        try await adapter.connect(config: effectiveConfig, password: password)

        let connection = ActiveConnection(
            id: config.id,
            config: config,
            adapter: adapter,
            initialSchema: nil,
            connectedAt: Date()
        )

        activeConnections[config.id] = connection
        return connection
    }

    func disconnect(connectionId: UUID) async throws {
        guard let connection = activeConnections[connectionId] else { return }
        try await connection.adapter.disconnect()
        await sshService.disconnect(connectionId: connectionId)
        activeConnections[connectionId] = nil
    }

    func disconnectAll() async {
        for (_, connection) in activeConnections {
            try? await connection.adapter.disconnect()
        }
        await sshService.disconnectAll()
        activeConnections.removeAll()
    }

    func activeConnection(for connectionId: UUID) -> ActiveConnection? {
        activeConnections[connectionId]
    }

    func isConnected(_ connectionId: UUID) -> Bool {
        activeConnections[connectionId] != nil
    }

    var allActiveConnections: [ActiveConnection] {
        Array(activeConnections.values)
    }

    private func createAdapter(for type: DatabaseType) -> any DatabaseAdapter {
        switch type {
        case .sqlite: return SQLiteAdapter()
        case .postgresql: return PostgreSQLAdapter()
        case .mysql: return MySQLAdapter()
        case .redis: return RedisAdapter()
        case .mongodb: return MongoDBAdapter()
        case .mssql: return MSSQLAdapter()
        case .clickhouse: return ClickHouseAdapter()
        }
    }
}
