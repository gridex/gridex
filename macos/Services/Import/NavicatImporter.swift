// NavicatImporter.swift
// Gridex
//
// Imports database connections from Navicat via NCX export files.

import Foundation
import CommonCrypto

struct NavicatImporter {

    static let profilesPath = FileManager.default.homeDirectoryForCurrentUser
        .appendingPathComponent("Library/Application Support/PremiumSoft CyberTech/Navicat CC/Navicat Premium/profiles")

    static var isInstalled: Bool {
        let appSupport = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Application Support/PremiumSoft CyberTech")
        return FileManager.default.fileExists(atPath: appSupport.path)
    }

    static func importFromNCX(at url: URL) throws -> [ImportedConnection] {
        let data = try Data(contentsOf: url)
        let parser = NavicatNCXParser(data: data)
        return parser.parse()
    }

    static func decryptPassword(_ encrypted: String) -> String? {
        guard !encrypted.isEmpty else { return nil }

        let keyString = "3DC5CA39"
        guard let keyData = keyString.data(using: .ascii) else { return nil }

        var sha1Hash = [UInt8](repeating: 0, count: Int(CC_SHA1_DIGEST_LENGTH))
        keyData.withUnsafeBytes { ptr in
            _ = CC_SHA1(ptr.baseAddress, CC_LONG(keyData.count), &sha1Hash)
        }

        guard let encryptedData = hexStringToData(encrypted) else { return nil }

        var decrypted = [UInt8]()
        var previousBlock = [UInt8](repeating: 0xFF, count: 8)

        let bytes = [UInt8](encryptedData)
        var i = 0
        while i < bytes.count {
            let blockSize = min(8, bytes.count - i)
            var block = Array(bytes[i..<i+blockSize])

            while block.count < 8 {
                block.append(0)
            }

            let decryptedBlock = blowfishDecrypt(block: block, key: sha1Hash)

            for j in 0..<blockSize {
                decrypted.append(decryptedBlock[j] ^ previousBlock[j])
            }

            previousBlock = block
            i += 8
        }

        while let last = decrypted.last, last == 0 {
            decrypted.removeLast()
        }

        return String(bytes: decrypted, encoding: .utf8)
    }

    private static func hexStringToData(_ hex: String) -> Data? {
        var data = Data()
        var index = hex.startIndex
        while index < hex.endIndex {
            let nextIndex = hex.index(index, offsetBy: 2, limitedBy: hex.endIndex) ?? hex.endIndex
            if let byte = UInt8(hex[index..<nextIndex], radix: 16) {
                data.append(byte)
            }
            index = nextIndex
        }
        return data.isEmpty ? nil : data
    }

    private static func blowfishDecrypt(block: [UInt8], key: [UInt8]) -> [UInt8] {
        var outBuffer = [UInt8](repeating: 0, count: 8)
        var numBytesDecrypted: size_t = 0

        key.withUnsafeBytes { keyPtr in
            block.withUnsafeBytes { dataPtr in
                outBuffer.withUnsafeMutableBytes { outPtr in
                    CCCrypt(
                        CCOperation(kCCDecrypt),
                        CCAlgorithm(kCCAlgorithmBlowfish),
                        CCOptions(kCCOptionECBMode),
                        keyPtr.baseAddress, min(key.count, kCCKeySizeMaxBlowfish),
                        nil,
                        dataPtr.baseAddress, 8,
                        outPtr.baseAddress, 8,
                        &numBytesDecrypted
                    )
                }
            }
        }

        return outBuffer
    }

    enum ImportError: LocalizedError {
        case fileNotFound
        case invalidFormat

        var errorDescription: String? {
            switch self {
            case .fileNotFound:
                return "Navicat NCX file not found."
            case .invalidFormat:
                return "Unable to parse Navicat NCX file."
            }
        }
    }
}

private class NavicatNCXParser: NSObject, XMLParserDelegate {
    private let data: Data
    private var connections: [ImportedConnection] = []
    private var currentConnection: [String: String] = [:]
    private var currentElement = ""
    private var inConnection = false
    private var elementStack: [String] = []

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
        elementStack.append(elementName)
        currentElement = elementName

        if elementName == "Connection" {
            inConnection = true
            currentConnection = [:]
            if let connType = attributes["ConnType"] {
                currentConnection["connType"] = connType
            }
        }
    }

    func parser(_ parser: XMLParser, foundCharacters string: String) {
        guard inConnection else { return }
        let trimmed = string.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return }

        currentConnection[currentElement] = (currentConnection[currentElement] ?? "") + trimmed
    }

    func parser(_ parser: XMLParser, didEndElement elementName: String, namespaceURI: String?, qualifiedName qName: String?) {
        if elementName == "Connection" {
            if let name = currentConnection["ConnectionName"] ?? currentConnection["Name"] {
                let connType = currentConnection["connType"] ?? currentConnection["ConnType"] ?? ""
                let dbType = mapConnType(connType)

                let encryptedPw = currentConnection["Password"] ?? ""
                let password = NavicatImporter.decryptPassword(encryptedPw)

                let conn = ImportedConnection(
                    id: UUID().uuidString,
                    source: .navicat,
                    name: name,
                    databaseType: dbType,
                    host: currentConnection["Host"],
                    port: currentConnection["Port"].flatMap { Int($0) },
                    database: currentConnection["DatabaseName"] ?? currentConnection["InitialDatabase"],
                    username: currentConnection["UserName"],
                    password: password,
                    sslEnabled: currentConnection["SSL"] == "true" || currentConnection["UseSSL"] == "1",
                    filePath: currentConnection["DatabaseFile"],
                    sshHost: currentConnection["SSH_Host"],
                    sshPort: currentConnection["SSH_Port"].flatMap { Int($0) },
                    sshUser: currentConnection["SSH_UserName"],
                    colorHex: nil,
                    group: nil
                )
                connections.append(conn)
            }
            inConnection = false
            currentConnection = [:]
        }

        if !elementStack.isEmpty {
            elementStack.removeLast()
        }
    }

    private func mapConnType(_ connType: String) -> DatabaseType {
        let lower = connType.lowercased()
        if lower.contains("postgres") { return .postgresql }
        if lower.contains("mysql") || lower.contains("maria") { return .mysql }
        if lower.contains("sqlite") { return .sqlite }
        if lower.contains("redis") { return .redis }
        if lower.contains("mongo") { return .mongodb }
        if lower.contains("sqlserver") || lower.contains("mssql") { return .mssql }
        if lower.contains("clickhouse") { return .clickhouse }
        if lower == "1" { return .mysql }
        if lower == "2" { return .postgresql }
        if lower == "3" { return .sqlite }
        if lower == "7" { return .mssql }
        if lower == "8" { return .mongodb }
        return .postgresql
    }
}
