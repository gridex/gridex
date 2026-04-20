// TablePlusImporter.swift
// Gridex
//
// Imports database connections from TablePlus.

import Foundation
import Security

struct TablePlusImporter {

    static let connectionsPlistPath = FileManager.default.homeDirectoryForCurrentUser
        .appendingPathComponent("Library/Application Support/com.tinyapp.TablePlus/Data/Connections.plist")

    static let groupsPlistPath = FileManager.default.homeDirectoryForCurrentUser
        .appendingPathComponent("Library/Application Support/com.tinyapp.TablePlus/Data/ConnectionGroups.plist")

    static var isInstalled: Bool {
        FileManager.default.fileExists(atPath: connectionsPlistPath.path)
    }

    static func importConnections() throws -> [ImportedConnection] {
        guard FileManager.default.fileExists(atPath: connectionsPlistPath.path) else {
            throw ImportError.fileNotFound
        }

        let data = try Data(contentsOf: connectionsPlistPath)
        guard let plist = try PropertyListSerialization.propertyList(from: data, format: nil) as? [[String: Any]] else {
            throw ImportError.invalidFormat
        }

        let groups = loadGroups()
        // Load all TablePlus passwords in a single keychain query. macOS shows
        // one access prompt — user clicks "Always Allow" once, not per connection.
        let passwords = loadAllPasswords()

        return plist.compactMap { dict -> ImportedConnection? in
            guard let id = dict["ID"] as? String,
                  let name = dict["ConnectionName"] as? String,
                  let driver = dict["Driver"] as? String else { return nil }

            let dbType = mapDriver(driver)
            let host = dict["DatabaseHost"] as? String
            let portStr = dict["DatabasePort"] as? String
            let port = portStr.flatMap { Int($0) }
            let database = dict["DatabaseName"] as? String
            let username = dict["DatabaseUser"] as? String
            let tlsMode = dict["tLSMode"] as? Int
            let sslEnabled = (tlsMode ?? 0) > 0

            let filePath = dict["DatabasePath"] as? String

            let isOverSSH = dict["isOverSSH"] as? Bool ?? false
            var sshHost: String?
            var sshPort: Int?
            var sshUser: String?
            if isOverSSH {
                sshHost = dict["ServerAddress"] as? String
                let sshPortStr = dict["ServerPort"] as? String
                sshPort = sshPortStr.flatMap { Int($0) } ?? 22
                sshUser = dict["ServerUser"] as? String
            }

            let colorHex = dict["statusColor"] as? String
            let groupId = dict["GroupID"] as? String
            let groupName = groupId.flatMap { groups[$0] }

            let dbPassword = passwords["\(id)_database"]

            return ImportedConnection(
                id: id,
                source: .tableplus,
                name: name,
                databaseType: dbType,
                host: host,
                port: port,
                database: database,
                username: username,
                password: dbPassword,
                sslEnabled: sslEnabled,
                filePath: filePath,
                sshHost: sshHost,
                sshPort: sshPort,
                sshUser: sshUser,
                colorHex: colorHex,
                group: groupName
            )
        }
    }

    /// Fetch all TablePlus passwords in one keychain call. Triggers a single
    /// macOS access prompt covering every item with service `com.tableplus.TablePlus`.
    /// Returns a dictionary keyed by account name (e.g. "<id>_database", "<id>_server").
    static func loadAllPasswords() -> [String: String] {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: "com.tableplus.TablePlus",
            kSecReturnData as String: true,
            kSecReturnAttributes as String: true,
            kSecMatchLimit as String: kSecMatchLimitAll
        ]

        var result: AnyObject?
        let status = SecItemCopyMatching(query as CFDictionary, &result)

        guard status == errSecSuccess, let items = result as? [[String: Any]] else {
            return [:]
        }

        var passwords: [String: String] = [:]
        for item in items {
            guard let account = item[kSecAttrAccount as String] as? String,
                  let data = item[kSecValueData as String] as? Data,
                  let password = String(data: data, encoding: .utf8) else { continue }
            passwords[account] = password
        }
        return passwords
    }

    static func loadPassword(connectionId: String, type: PasswordType) -> String? {
        let account: String
        switch type {
        case .database:
            account = "\(connectionId)_database"
        case .ssh:
            account = "\(connectionId)_server"
        }

        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: "com.tableplus.TablePlus",
            kSecAttrAccount as String: account,
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne
        ]

        var result: AnyObject?
        let status = SecItemCopyMatching(query as CFDictionary, &result)

        guard status == errSecSuccess,
              let data = result as? Data,
              let password = String(data: data, encoding: .utf8) else {
            return nil
        }
        return password
    }

    enum PasswordType {
        case database
        case ssh
    }

    enum ImportError: LocalizedError {
        case fileNotFound
        case invalidFormat

        var errorDescription: String? {
            switch self {
            case .fileNotFound:
                return "TablePlus connections file not found."
            case .invalidFormat:
                return "Unable to parse TablePlus connections file."
            }
        }
    }

    private static func loadGroups() -> [String: String] {
        guard FileManager.default.fileExists(atPath: groupsPlistPath.path),
              let data = try? Data(contentsOf: groupsPlistPath),
              let plist = try? PropertyListSerialization.propertyList(from: data, format: nil) as? [[String: Any]] else {
            return [:]
        }

        var groups: [String: String] = [:]
        for dict in plist {
            if let id = dict["ID"] as? String,
               let name = dict["GroupName"] as? String {
                groups[id] = name
            }
        }
        return groups
    }

    private static func mapDriver(_ driver: String) -> DatabaseType {
        let lower = driver.lowercased()
        if lower.contains("postgres") { return .postgresql }
        if lower.contains("mysql") || lower.contains("maria") { return .mysql }
        if lower.contains("sqlite") { return .sqlite }
        if lower.contains("redis") { return .redis }
        if lower.contains("mongo") { return .mongodb }
        if lower.contains("mssql") || lower.contains("sqlserver") { return .mssql }
        return .postgresql
    }
}
