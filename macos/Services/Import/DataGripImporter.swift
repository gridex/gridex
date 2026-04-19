// DataGripImporter.swift
// Gridex
//
// Imports database connections from JetBrains DataGrip.

import Foundation

struct DataGripImporter {

    static var dataGripBasePath: URL? {
        let appSupport = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Application Support/JetBrains")

        guard FileManager.default.fileExists(atPath: appSupport.path) else { return nil }

        let contents = (try? FileManager.default.contentsOfDirectory(at: appSupport, includingPropertiesForKeys: nil)) ?? []
        let dataGripDirs = contents.filter { $0.lastPathComponent.hasPrefix("DataGrip") }
            .sorted { $0.lastPathComponent > $1.lastPathComponent }

        return dataGripDirs.first
    }

    static var isInstalled: Bool {
        dataGripBasePath != nil
    }

    static func importConnections() throws -> [ImportedConnection] {
        guard let basePath = dataGripBasePath else {
            throw ImportError.fileNotFound
        }

        var results: [ImportedConnection] = []

        let optionsPath = basePath.appendingPathComponent("options/dataSources.xml")
        if FileManager.default.fileExists(atPath: optionsPath.path) {
            results.append(contentsOf: try parseDataSourcesXML(at: optionsPath))
        }

        let projectsPath = basePath.appendingPathComponent("projects")
        if FileManager.default.fileExists(atPath: projectsPath.path) {
            let projects = (try? FileManager.default.contentsOfDirectory(at: projectsPath, includingPropertiesForKeys: nil)) ?? []
            for project in projects {
                let dsPath = project.appendingPathComponent(".idea/dataSources.xml")
                if FileManager.default.fileExists(atPath: dsPath.path) {
                    results.append(contentsOf: (try? parseDataSourcesXML(at: dsPath)) ?? [])
                }
            }
        }

        return results
    }

    private static func parseDataSourcesXML(at url: URL) throws -> [ImportedConnection] {
        let data = try Data(contentsOf: url)
        let parser = DataGripXMLParser(data: data)
        return parser.parse()
    }

    enum ImportError: LocalizedError {
        case fileNotFound
        case invalidFormat

        var errorDescription: String? {
            switch self {
            case .fileNotFound:
                return "DataGrip configuration not found."
            case .invalidFormat:
                return "Unable to parse DataGrip data sources."
            }
        }
    }
}

private class DataGripXMLParser: NSObject, XMLParserDelegate {
    private let data: Data
    private var connections: [ImportedConnection] = []
    private var currentDataSource: [String: String] = [:]
    private var currentElement = ""
    private var inDataSource = false

    init(data: Data) {
        self.data = data
    }

    func parse() -> [ImportedConnection] {
        let parser = XMLParser(data: data)
        parser.delegate = self
        parser.parse()
        return connections
    }

    func parser(_ parser: XMLParser, didStartElement elementName: String, namespaceURI: String?,
                qualifiedName qName: String?, attributes: [String: String] = [:]) {
        currentElement = elementName

        if elementName == "data-source" {
            inDataSource = true
            currentDataSource = [:]
            if let name = attributes["name"] { currentDataSource["name"] = name }
            if let uuid = attributes["uuid"] { currentDataSource["id"] = uuid }
        }

        if inDataSource {
            if elementName == "driver-ref" {
                currentDataSource["driver"] = attributes[""]
            }
            if elementName == "jdbc-url", let text = attributes["value"] {
                currentDataSource["url"] = text
            }
        }
    }

    func parser(_ parser: XMLParser, foundCharacters string: String) {
        guard inDataSource else { return }
        let trimmed = string.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return }

        switch currentElement {
        case "driver-ref":
            currentDataSource["driver"] = trimmed
        case "jdbc-url":
            currentDataSource["url"] = trimmed
        case "jdbc-driver":
            currentDataSource["jdbcDriver"] = trimmed
        default:
            break
        }
    }

    func parser(_ parser: XMLParser, didEndElement elementName: String, namespaceURI: String?, qualifiedName qName: String?) {
        if elementName == "data-source" {
            if let name = currentDataSource["name"] {
                let driver = currentDataSource["driver"] ?? currentDataSource["jdbcDriver"] ?? ""
                let url = currentDataSource["url"] ?? ""

                let dbType = mapDriver(driver, url: url)
                let (host, port, database) = parseJDBCUrl(url)

                let conn = ImportedConnection(
                    id: currentDataSource["id"] ?? UUID().uuidString,
                    source: .datagrip,
                    name: name,
                    databaseType: dbType,
                    host: host,
                    port: port,
                    database: database,
                    username: nil,
                    password: nil,
                    sslEnabled: url.contains("ssl=true"),
                    filePath: nil,
                    sshHost: nil,
                    sshPort: nil,
                    sshUser: nil,
                    colorHex: nil,
                    group: nil
                )
                connections.append(conn)
            }
            inDataSource = false
            currentDataSource = [:]
        }
    }

    private func mapDriver(_ driver: String, url: String) -> DatabaseType {
        let combined = (driver + url).lowercased()
        if combined.contains("postgres") { return .postgresql }
        if combined.contains("mysql") || combined.contains("maria") { return .mysql }
        if combined.contains("sqlite") { return .sqlite }
        if combined.contains("redis") { return .redis }
        if combined.contains("mongo") { return .mongodb }
        if combined.contains("sqlserver") || combined.contains("mssql") { return .mssql }
        return .postgresql
    }

    private func parseJDBCUrl(_ url: String) -> (String?, Int?, String?) {
        var host: String?
        var port: Int?
        var database: String?

        let hostPattern = #"://([^:/]+)"#
        if let regex = try? NSRegularExpression(pattern: hostPattern),
           let match = regex.firstMatch(in: url, range: NSRange(url.startIndex..., in: url)),
           let range = Range(match.range(at: 1), in: url) {
            host = String(url[range])
        }

        let portPattern = #":(\d+)[/;]"#
        if let regex = try? NSRegularExpression(pattern: portPattern),
           let match = regex.firstMatch(in: url, range: NSRange(url.startIndex..., in: url)),
           let range = Range(match.range(at: 1), in: url) {
            port = Int(url[range])
        }

        let dbPatterns = [
            #"/([^/?;]+)(?:\?|;|$)"#,
            #"databaseName=([^;&]+)"#
        ]
        for pattern in dbPatterns {
            if let regex = try? NSRegularExpression(pattern: pattern),
               let match = regex.firstMatch(in: url, range: NSRange(url.startIndex..., in: url)),
               let range = Range(match.range(at: 1), in: url) {
                database = String(url[range])
                break
            }
        }

        return (host, port, database)
    }
}
