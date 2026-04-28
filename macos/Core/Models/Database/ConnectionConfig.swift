// ConnectionConfig.swift
// Gridex
//
// Configuration needed to establish a database connection.

import Foundation

struct ConnectionConfig: Codable, Sendable, Hashable, Identifiable {
    let id: UUID
    var name: String
    var databaseType: DatabaseType
    var host: String?
    var port: Int?
    var database: String?
    var username: String?
    var sslEnabled: Bool
    /// Fine-grained SSL posture; `nil` falls back to `sslEnabled` via
    /// `effectiveSSLMode` for rows persisted before incident `6a4aad0`.
    var sslMode: SSLMode?
    var colorTag: ColorTag?
    var group: String?

    // SSL/TLS certificates (for mTLS, e.g., Teleport)
    var sslKeyPath: String?
    var sslCertPath: String?
    var sslCACertPath: String?

    // SQLite-specific
    var filePath: String?

    // SSH Tunnel
    var sshConfig: SSHTunnelConfig?

    // MCP Access
    var mcpMode: MCPConnectionMode

    /// MongoDB-specific URI options preserved from the user's connection string
    /// (e.g. `authSource`, `authMechanism`, `replicaSet`, `readPreference`).
    /// `tls`/`ssl` are handled via `sslEnabled` and are never stored here.
    var mongoOptions: [String: String]?

    init(
        id: UUID = UUID(),
        name: String,
        databaseType: DatabaseType,
        host: String? = nil,
        port: Int? = nil,
        database: String? = nil,
        username: String? = nil,
        sslEnabled: Bool = false,
        sslMode: SSLMode? = nil,
        colorTag: ColorTag? = nil,
        group: String? = nil,
        sslKeyPath: String? = nil,
        sslCertPath: String? = nil,
        sslCACertPath: String? = nil,
        filePath: String? = nil,
        sshConfig: SSHTunnelConfig? = nil,
        mcpMode: MCPConnectionMode = .locked,
        mongoOptions: [String: String]? = nil
    ) {
        self.id = id
        self.name = name
        self.databaseType = databaseType
        self.host = host
        self.port = port
        self.database = database
        self.username = username
        self.sslEnabled = sslEnabled
        self.sslMode = sslMode
        self.colorTag = colorTag
        self.group = group
        self.sslKeyPath = sslKeyPath
        self.sslCertPath = sslCertPath
        self.sslCACertPath = sslCACertPath
        self.filePath = filePath
        self.sshConfig = sshConfig
        self.mcpMode = mcpMode
        self.mongoOptions = mongoOptions
    }

    var displayHost: String {
        if databaseType == .sqlite {
            return filePath ?? "Unknown"
        }
        return "\(host ?? "localhost"):\(port ?? databaseType.defaultPort)"
    }

    /// Resolved SSL posture. Honors the persisted 5-state `sslMode`; falls back
    /// to `sslEnabled` for rows saved before the field existed (legacy
    /// `sslEnabled=true` maps to `.preferred`, matching the pre-fix adapter).
    var effectiveSSLMode: SSLMode {
        sslMode ?? (sslEnabled ? .preferred : .disabled)
    }
}

struct SSHTunnelConfig: Codable, Sendable, Hashable {
    var host: String
    var port: Int
    var username: String
    var authMethod: SSHAuthMethod
    var keyPath: String?

    init(host: String, port: Int = 22, username: String, authMethod: SSHAuthMethod = .password, keyPath: String? = nil) {
        self.host = host
        self.port = port
        self.username = username
        self.authMethod = authMethod
        self.keyPath = keyPath
    }
}
