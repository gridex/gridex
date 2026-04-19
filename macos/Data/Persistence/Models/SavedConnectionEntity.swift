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

    init(
        id: UUID = UUID(),
        name: String,
        databaseType: String,
        host: String? = nil,
        port: Int? = nil,
        database: String? = nil,
        username: String? = nil,
        sslEnabled: Bool = false,
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
        mcpMode: String? = nil
    ) {
        self.id = id
        self.name = name
        self.databaseType = databaseType
        self.host = host
        self.port = port
        self.database = database
        self.username = username
        self.sslEnabled = sslEnabled
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
    }

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

        return ConnectionConfig(
            id: id,
            name: name,
            databaseType: DatabaseType(rawValue: databaseType) ?? .sqlite,
            host: host,
            port: port,
            database: database,
            username: username,
            sslEnabled: sslEnabled,
            colorTag: colorTag.flatMap { ColorTag(rawValue: $0) },
            group: group,
            filePath: filePath,
            sshConfig: sshConfig,
            mcpMode: mcpMode.flatMap { MCPConnectionMode(rawValue: $0) } ?? .locked
        )
    }
}
