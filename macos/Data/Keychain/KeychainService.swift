// KeychainService.swift
// Gridex
//
// Wrapper around macOS Keychain Services.
// Prefers the Data Protection Keychain (iOS-style, no ACL prompts).
// Falls back to the standard file-based Keychain for unsigned/dev builds.

import Foundation
import Security

protocol KeychainServiceProtocol: Sendable {
    func save(key: String, value: String) throws
    func load(key: String) throws -> String?
    func delete(key: String) throws
    func update(key: String, value: String) throws

    // ChatGPT OAuth bundle (per-provider). Implemented in terms of save/load/delete
    // above, but lives on the protocol so call sites holding only the protocol
    // type (e.g. ProviderEditSheet, SettingsView) can use them.
    func saveChatGPTTokens(providerId: UUID, bundle: ChatGPTTokenBundle) throws
    func loadChatGPTTokens(providerId: UUID) throws -> ChatGPTTokenBundle?
    func deleteChatGPTTokens(providerId: UUID) throws
}

final class KeychainService: KeychainServiceProtocol, Sendable {
    private let serviceName = "com.gridex.credentials"

    /// Whether the Data Protection Keychain is available (requires code signing + entitlements).
    private let useDataProtection: Bool

    init() {
        // Probe: try a save+delete with Data Protection to see if it works.
        let probeKey = "__keychain_probe__"
        let probeQuery: [String: Any] = [
            kSecClass as String:                     kSecClassGenericPassword,
            kSecAttrService as String:               "com.gridex.probe",
            kSecAttrAccount as String:               probeKey,
            kSecUseDataProtectionKeychain as String:  true,
            kSecValueData as String:                  Data("probe".utf8),
        ]
        let status = SecItemAdd(probeQuery as CFDictionary, nil)
        if status == errSecSuccess || status == errSecDuplicateItem {
            // Cleanup probe
            let deleteQuery: [String: Any] = [
                kSecClass as String:                     kSecClassGenericPassword,
                kSecAttrService as String:               "com.gridex.probe",
                kSecAttrAccount as String:               probeKey,
                kSecUseDataProtectionKeychain as String:  true,
            ]
            SecItemDelete(deleteQuery as CFDictionary)
            useDataProtection = true
        } else {
            useDataProtection = false
        }
    }

    // MARK: - Core Operations

    func save(key: String, value: String) throws {
        guard let data = value.data(using: .utf8) else { return }

        var query = baseQuery(key: key)
        query[kSecValueData as String] = data
        if useDataProtection {
            query[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlock
        } else {
            // For the file-based Keychain, create a SecAccess that trusts the
            // current application so macOS won't show a password prompt on access.
            if let access = createTrustedAccess() {
                query[kSecAttrAccess as String] = access
            }
        }

        let status = SecItemAdd(query as CFDictionary, nil)

        if status == errSecDuplicateItem {
            try update(key: key, value: value)
        } else if status != errSecSuccess {
            throw GridexError.keychainError("Save failed (OSStatus \(status))")
        }
    }

    func load(key: String) throws -> String? {
        var query = baseQuery(key: key)
        query[kSecReturnData as String] = true
        query[kSecMatchLimit as String] = kSecMatchLimitOne

        var result: AnyObject?
        let status = SecItemCopyMatching(query as CFDictionary, &result)

        if status == errSecSuccess,
           let data = result as? Data,
           let value = String(data: data, encoding: .utf8) {
            // For file-based Keychain: re-save with trusted access so future reads
            // won't prompt. Each rebuild changes the binary hash, invalidating the
            // old ACL. Delete the old item and re-create with current app's trust.
            if !useDataProtection {
                let deleteQuery = baseQuery(key: key)
                SecItemDelete(deleteQuery as CFDictionary)
                try? save(key: key, value: value)
            }
            return value
        }

        // Try the other keychain variant (migration between Data Protection ↔ standard)
        var altQuery = baseQuery(key: key, forceDataProtection: !useDataProtection)
        altQuery[kSecReturnData as String] = true
        altQuery[kSecMatchLimit as String] = kSecMatchLimitOne

        var altResult: AnyObject?
        let altStatus = SecItemCopyMatching(altQuery as CFDictionary, &altResult)

        if altStatus == errSecSuccess,
           let data = altResult as? Data,
           let value = String(data: data, encoding: .utf8) {
            // Migrate to preferred keychain
            try? save(key: key, value: value)
            SecItemDelete(altQuery as CFDictionary)
            return value
        }

        // Migrate from legacy UserDefaults Base64 storage (used by older versions)
        if let base64 = UserDefaults.standard.string(forKey: "kc.fallback.\(key)"),
           let data = Data(base64Encoded: base64),
           let value = String(data: data, encoding: .utf8) {
            // Migrate to real Keychain and clean up UserDefaults
            try? save(key: key, value: value)
            UserDefaults.standard.removeObject(forKey: "kc.fallback.\(key)")
            return value
        }

        return nil
    }

    func delete(key: String) throws {
        let query = baseQuery(key: key)
        SecItemDelete(query as CFDictionary)

        // Also clean up from the other keychain variant
        let altQuery = baseQuery(key: key, forceDataProtection: !useDataProtection)
        SecItemDelete(altQuery as CFDictionary)
    }

    func update(key: String, value: String) throws {
        guard let data = value.data(using: .utf8) else { return }

        let query = baseQuery(key: key)
        let attributes: [String: Any] = [kSecValueData as String: data]

        let status = SecItemUpdate(query as CFDictionary, attributes as CFDictionary)
        if status != errSecSuccess {
            throw GridexError.keychainError("Update failed (OSStatus \(status))")
        }
    }

    // MARK: - Access Control

    /// Creates a SecAccess that trusts the current application, allowing Keychain
    /// access without macOS showing a password/permission prompt to the user.
    /// Only needed for the file-based Keychain (not Data Protection Keychain).
    private func createTrustedAccess() -> SecAccess? {
        var trustedApp: SecTrustedApplication?
        // nil path = current application
        let appStatus = SecTrustedApplicationCreateFromPath(nil, &trustedApp)
        guard appStatus == errSecSuccess, let app = trustedApp else { return nil }

        var access: SecAccess?
        let accessStatus = SecAccessCreate(
            "Gridex Credentials" as CFString,
            [app] as CFArray,
            &access
        )
        guard accessStatus == errSecSuccess else { return nil }
        return access
    }

    // MARK: - Query Builder

    private func baseQuery(key: String, forceDataProtection: Bool? = nil) -> [String: Any] {
        let dp = forceDataProtection ?? useDataProtection
        var query: [String: Any] = [
            kSecClass as String:        kSecClassGenericPassword,
            kSecAttrService as String:  serviceName,
            kSecAttrAccount as String:  key,
        ]
        if dp {
            query[kSecUseDataProtectionKeychain as String] = true
        }
        return query
    }

    // MARK: - Convenience

    func savePassword(connectionId: UUID, password: String) throws {
        try save(key: "db.password.\(connectionId.uuidString)", value: password)
    }

    func loadPassword(connectionId: UUID) throws -> String? {
        try load(key: "db.password.\(connectionId.uuidString)")
    }

    func saveSSHPassword(connectionId: UUID, password: String) throws {
        try save(key: "ssh.password.\(connectionId.uuidString)", value: password)
    }

    func loadSSHPassword(connectionId: UUID) throws -> String? {
        try load(key: "ssh.password.\(connectionId.uuidString)")
    }

    func saveAPIKey(provider: String, key: String) throws {
        try save(key: "ai.apikey.\(provider)", value: key)
    }

    func loadAPIKey(provider: String) throws -> String? {
        try load(key: "ai.apikey.\(provider)")
    }

    // MARK: - ChatGPT OAuth tokens
    //
    // Stored as a JSON-encoded `ChatGPTTokenBundle` under a per-provider key.
    // Single blob keeps refresh writes atomic — partial updates after a crash
    // are not possible.

    private func chatGPTTokensKey(_ providerId: UUID) -> String {
        "ai.chatgpt.tokens.\(providerId.uuidString)"
    }

    func saveChatGPTTokens(providerId: UUID, bundle: ChatGPTTokenBundle) throws {
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .iso8601
        let data = try encoder.encode(bundle)
        guard let json = String(data: data, encoding: .utf8) else {
            throw GridexError.keychainError("Failed to encode ChatGPT token bundle")
        }
        try save(key: chatGPTTokensKey(providerId), value: json)
    }

    func loadChatGPTTokens(providerId: UUID) throws -> ChatGPTTokenBundle? {
        guard let json = try load(key: chatGPTTokensKey(providerId)) else { return nil }
        guard let data = json.data(using: .utf8) else {
            throw GridexError.keychainError("ChatGPT token bundle keychain blob is not valid UTF-8")
        }
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        return try decoder.decode(ChatGPTTokenBundle.self, from: data)
    }

    func deleteChatGPTTokens(providerId: UUID) throws {
        try delete(key: chatGPTTokensKey(providerId))
    }
}
