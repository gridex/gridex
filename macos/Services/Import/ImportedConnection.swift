// ImportedConnection.swift
// Gridex
//
// Shared model for imported connections from external tools.

import Foundation

enum ImportSource: String, CaseIterable {
    case tableplus = "TablePlus"
    case dbeaver = "DBeaver"
    case datagrip = "DataGrip"
    case navicat = "Navicat"

    var iconLetter: String {
        switch self {
        case .tableplus: return "T+"
        case .dbeaver: return "DB"
        case .datagrip: return "DG"
        case .navicat: return "N"
        }
    }

    var isInstalled: Bool {
        switch self {
        case .tableplus: return TablePlusImporter.isInstalled
        case .dbeaver: return DBeaverImporter.isInstalled
        case .datagrip: return DataGripImporter.isInstalled
        case .navicat: return NavicatImporter.isInstalled
        }
    }
}

struct ImportedConnection: Identifiable {
    let id: String
    let source: ImportSource
    let name: String
    let databaseType: DatabaseType
    let host: String?
    let port: Int?
    let database: String?
    let username: String?
    let password: String?
    let sslEnabled: Bool
    let filePath: String?
    let sshHost: String?
    let sshPort: Int?
    let sshUser: String?
    let colorHex: String?
    let group: String?

    func toConnectionConfig() -> ConnectionConfig {
        var sshConfig: SSHTunnelConfig?
        if let sshHost = sshHost, let sshUser = sshUser {
            sshConfig = SSHTunnelConfig(
                host: sshHost,
                port: sshPort ?? 22,
                username: sshUser,
                authMethod: .password
            )
        }

        let colorTag = colorHex.flatMap { ColorTag.fromHex($0) }

        return ConnectionConfig(
            id: UUID(),
            name: name,
            databaseType: databaseType,
            host: host,
            port: port,
            database: database,
            username: username,
            sslEnabled: sslEnabled,
            colorTag: colorTag,
            group: group,
            filePath: filePath,
            sshConfig: sshConfig
        )
    }
}
