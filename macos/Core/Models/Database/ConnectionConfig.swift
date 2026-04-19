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

    init(
        id: UUID = UUID(),
        name: String,
        databaseType: DatabaseType,
        host: String? = nil,
        port: Int? = nil,
        database: String? = nil,
        username: String? = nil,
        sslEnabled: Bool = false,
        colorTag: ColorTag? = nil,
        group: String? = nil,
        sslKeyPath: String? = nil,
        sslCertPath: String? = nil,
        sslCACertPath: String? = nil,
        filePath: String? = nil,
        sshConfig: SSHTunnelConfig? = nil,
        mcpMode: MCPConnectionMode = .locked
    ) {
        self.id = id
        self.name = name
        self.databaseType = databaseType
        self.host = host
        self.port = port
        self.database = database
        self.username = username
        self.sslEnabled = sslEnabled
        self.colorTag = colorTag
        self.group = group
        self.sslKeyPath = sslKeyPath
        self.sslCertPath = sslCertPath
        self.sslCACertPath = sslCACertPath
        self.filePath = filePath
        self.sshConfig = sshConfig
        self.mcpMode = mcpMode
    }

    var displayHost: String {
        if databaseType == .sqlite {
            return filePath ?? "Unknown"
        }
        return "\(host ?? "localhost"):\(port ?? databaseType.defaultPort)"
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
