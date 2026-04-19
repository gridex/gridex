// DBeaverImporter.swift
// Gridex
//
// Imports database connections from DBeaver.

import Foundation

struct DBeaverImporter {

    static let dataSourcesPath = FileManager.default.homeDirectoryForCurrentUser
        .appendingPathComponent("Library/DBeaverData/workspace6/General/.dbeaver/data-sources.json")

    static let credentialsPath = FileManager.default.homeDirectoryForCurrentUser
        .appendingPathComponent("Library/DBeaverData/workspace6/General/.dbeaver/credentials-config.json")

    static var isInstalled: Bool {
        FileManager.default.fileExists(atPath: dataSourcesPath.path)
    }

    static func importConnections() throws -> [ImportedConnection] {
        guard FileManager.default.fileExists(atPath: dataSourcesPath.path) else {
            throw ImportError.fileNotFound
        }

        let data = try Data(contentsOf: dataSourcesPath)
        guard let json = try JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            throw ImportError.invalidFormat
        }

        let credentials = loadCredentials()
        var results: [ImportedConnection] = []

        for (_, folderValue) in json {
            guard let folder = folderValue as? [String: Any],
                  let connections = folder["connections"] as? [String: Any] else { continue }

            for (connId, connValue) in connections {
                guard let conn = connValue as? [String: Any],
                      let name = conn["name"] as? String,
                      let provider = conn["provider"] as? String else { continue }

                let dbType = mapProvider(provider)
                let config = conn["configuration"] as? [String: Any] ?? [:]

                let host = config["host"] as? String
                let port = config["port"] as? String
                let database = config["database"] as? String
                let url = config["url"] as? String

                var username: String?
                var password: String?

                if let authModel = config["auth-model"] as? String, authModel == "native" {
                    username = credentials[connId]?["user"]
                    password = credentials[connId]?["password"]
                }

                let sslEnabled = (config["ssl"] as? String) == "true"

                let imported = ImportedConnection(
                    id: connId,
                    source: .dbeaver,
                    name: name,
                    databaseType: dbType,
                    host: host ?? parseHostFromURL(url),
                    port: port.flatMap { Int($0) } ?? parsePortFromURL(url),
                    database: database ?? parseDatabaseFromURL(url),
                    username: username,
                    password: password,
                    sslEnabled: sslEnabled,
                    filePath: nil,
                    sshHost: nil,
                    sshPort: nil,
                    sshUser: nil,
                    colorHex: nil,
                    group: nil
                )
                results.append(imported)
            }
        }

        return results
    }

    private static func loadCredentials() -> [String: [String: String]] {
        guard FileManager.default.fileExists(atPath: credentialsPath.path),
              let data = try? Data(contentsOf: credentialsPath),
              let decrypted = decryptDBeaverCredentials(data),
              let json = try? JSONSerialization.jsonObject(with: decrypted) as? [String: Any] else {
            return [:]
        }

        var result: [String: [String: String]] = [:]
        for (key, value) in json {
            if let creds = value as? [String: String] {
                let connId = key.replacingOccurrences(of: "data-source|", with: "")
                result[connId] = creds
            }
        }
        return result
    }

    private static func decryptDBeaverCredentials(_ data: Data) -> Data? {
        guard let content = String(data: data, encoding: .utf8) else { return nil }
        guard let decoded = Data(base64Encoded: content) else { return nil }

        let key: [UInt8] = [
            0xBA, 0xBB, 0x4A, 0x9F, 0x7A, 0xEE, 0x8F, 0xC1
        ]

        var decrypted = [UInt8](repeating: 0, count: decoded.count)
        let bytes = [UInt8](decoded)

        for i in 0..<bytes.count {
            decrypted[i] = bytes[i] ^ key[i % key.count]
        }

        return Data(decrypted)
    }

    private static func mapProvider(_ provider: String) -> DatabaseType {
        let lower = provider.lowercased()
        if lower.contains("postgres") { return .postgresql }
        if lower.contains("mysql") || lower.contains("maria") { return .mysql }
        if lower.contains("sqlite") { return .sqlite }
        if lower.contains("redis") { return .redis }
        if lower.contains("mongo") { return .mongodb }
        if lower.contains("sqlserver") || lower.contains("mssql") { return .mssql }
        return .postgresql
    }

    private static func parseHostFromURL(_ url: String?) -> String? {
        guard let url = url else { return nil }
        let pattern = #"://([^:/]+)"#
        guard let regex = try? NSRegularExpression(pattern: pattern),
              let match = regex.firstMatch(in: url, range: NSRange(url.startIndex..., in: url)),
              let range = Range(match.range(at: 1), in: url) else { return nil }
        return String(url[range])
    }

    private static func parsePortFromURL(_ url: String?) -> Int? {
        guard let url = url else { return nil }
        let pattern = #":(\d+)/"#
        guard let regex = try? NSRegularExpression(pattern: pattern),
              let match = regex.firstMatch(in: url, range: NSRange(url.startIndex..., in: url)),
              let range = Range(match.range(at: 1), in: url) else { return nil }
        return Int(url[range])
    }

    private static func parseDatabaseFromURL(_ url: String?) -> String? {
        guard let url = url else { return nil }
        let pattern = #"/([^/?]+)(?:\?|$)"#
        guard let regex = try? NSRegularExpression(pattern: pattern),
              let match = regex.firstMatch(in: url, range: NSRange(url.startIndex..., in: url)),
              let range = Range(match.range(at: 1), in: url) else { return nil }
        return String(url[range])
    }

    enum ImportError: LocalizedError {
        case fileNotFound
        case invalidFormat

        var errorDescription: String? {
            switch self {
            case .fileNotFound:
                return "DBeaver data sources file not found."
            case .invalidFormat:
                return "Unable to parse DBeaver data sources."
            }
        }
    }
}
