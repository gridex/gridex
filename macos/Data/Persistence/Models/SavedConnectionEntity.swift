// SavedConnectionEntity.swift
// Gridex
//
// SwiftData model for persisted connections.

import Foundation
import SwiftData

@Model
final class SavedConnectionEntity {
    @Attribute(.unique) var id: UUID
    var name: String
    var databaseType: String
    var host: String?
    var port: Int?
    var database: String?
    var username: String?
    var sslEnabled: Bool
    /// Fine-grained SSL posture (raw value of `SSLMode`). Optional for SwiftData
    /// lightweight migration of pre-existing rows. Read site falls back to `sslEnabled`.
    var sslMode: String?
    /// Optional SSL certificate paths. Kept optional for lightweight migration.
    var sslKeyPath: String?
    var sslCertPath: String?
    var sslCACertPath: String?
    var sshEnabled: Bool
    var sshHost: String?
    var sshPort: Int?
    var sshUsername: String?
    var sshAuthMethod: String?
    var sshKeyPath: String?
    var colorTag: String?
    var group: String?
    var sortOrder: Int
    var lastConnectedAt: Date?
    var createdAt: Date
    var filePath: String?
    var mcpMode: String?
    /// MongoDB URI options serialized as JSON (`authSource`, `authMechanism`, etc.)
    /// Kept as a string so SwiftData lightweight migration is trivial.
    var mongoOptionsJSON: String?

    init(
        id: UUID = UUID(),
        name: String,
        databaseType: String,
        host: String? = nil,
        port: Int? = nil,
        database: String? = nil,
        username: String? = nil,
        sslEnabled: Bool = false,
        sslMode: String? = nil,
        sslKeyPath: String? = nil,
        sslCertPath: String? = nil,
        sslCACertPath: String? = nil,
        sshEnabled: Bool = false,
        sshHost: String? = nil,
        sshPort: Int? = nil,
        sshUsername: String? = nil,
        sshAuthMethod: String? = nil,
        sshKeyPath: String? = nil,
        colorTag: String? = nil,
        group: String? = nil,
        sortOrder: Int = 0,
        lastConnectedAt: Date? = nil,
        createdAt: Date = Date(),
        filePath: String? = nil,
        mcpMode: String? = nil,
        mongoOptionsJSON: String? = nil
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
        self.sslKeyPath = sslKeyPath
        self.sslCertPath = sslCertPath
        self.sslCACertPath = sslCACertPath
        self.sshEnabled = sshEnabled
        self.sshHost = sshHost
        self.sshPort = sshPort
        self.sshUsername = sshUsername
        self.sshAuthMethod = sshAuthMethod
        self.sshKeyPath = sshKeyPath
        self.colorTag = colorTag
        self.group = group
        self.sortOrder = sortOrder
        self.lastConnectedAt = lastConnectedAt
        self.createdAt = createdAt
        self.filePath = filePath
        self.mcpMode = mcpMode
        self.mongoOptionsJSON = mongoOptionsJSON
    }

    private static let mongoOptionsDecoder = JSONDecoder()

    func toConfig() -> ConnectionConfig {
        var sshConfig: SSHTunnelConfig?
        if sshEnabled, let sshHost, let sshUsername {
            sshConfig = SSHTunnelConfig(
                host: sshHost,
                port: sshPort ?? 22,
                username: sshUsername,
                authMethod: SSHAuthMethod(rawValue: sshAuthMethod ?? "password") ?? .password,
                keyPath: sshKeyPath
            )
        }

        var mongoOptions: [String: String]?
        if let json = mongoOptionsJSON, !json.isEmpty,
           let data = json.data(using: .utf8),
           let decoded = try? Self.mongoOptionsDecoder.decode([String: String].self, from: data),
           !decoded.isEmpty {
            mongoOptions = decoded
        }

        return ConnectionConfig(
            id: id,
            name: name,
            databaseType: DatabaseType(rawValue: databaseType) ?? .sqlite,
            host: host,
            port: port,
            database: database,
            username: username,
            sslEnabled: sslEnabled,
            sslMode: sslMode.flatMap { SSLMode(rawValue: $0) },
            colorTag: colorTag.flatMap { ColorTag(rawValue: $0) },
            group: group,
            sslKeyPath: sslKeyPath,
            sslCertPath: sslCertPath,
            sslCACertPath: sslCACertPath,
            filePath: filePath,
            sshConfig: sshConfig,
            mcpMode: mcpMode.flatMap { MCPConnectionMode(rawValue: $0) } ?? .locked,
            mongoOptions: mongoOptions
        )
    }
}
