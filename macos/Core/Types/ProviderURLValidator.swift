// ProviderURLValidator.swift
// Gridex
//
// SSRF-conscious URL validation for provider apiBase entries.
// Mirrors goclaw internal/http/providers.go host validation: require http(s),
// reject raw IP + loopback + link-local unless the provider type legitimately
// targets local hosts (Ollama).

import Foundation

enum ProviderURLValidationError: LocalizedError {
    case emptyURL
    case invalidURL
    case unsupportedScheme(String)
    case privateHostNotAllowed(String)

    var errorDescription: String? {
        switch self {
        case .emptyURL:                       return "URL is empty"
        case .invalidURL:                     return "URL is not valid"
        case .unsupportedScheme(let s):       return "Scheme '\(s)' is not supported — use http or https"
        case .privateHostNotAllowed(let h):   return "Host '\(h)' is not allowed for this provider"
        }
    }
}

enum ProviderURLValidator {
    /// Validate `raw` for a provider of `type`. Returns the normalised URL string.
    /// - Loopback/private hosts are allowed only when `type.allowsPrivateHosts` is true.
    static func validate(_ raw: String, for type: ProviderType) throws -> String {
        let trimmed = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { throw ProviderURLValidationError.emptyURL }
        guard let url = URL(string: trimmed),
              let scheme = url.scheme?.lowercased(),
              let host = url.host, !host.isEmpty else {
            throw ProviderURLValidationError.invalidURL
        }
        guard scheme == "http" || scheme == "https" else {
            throw ProviderURLValidationError.unsupportedScheme(scheme)
        }
        if !type.allowsPrivateHosts, isPrivateHost(host) {
            throw ProviderURLValidationError.privateHostNotAllowed(host)
        }
        // Strip any trailing slash.
        return trimmed.hasSuffix("/") ? String(trimmed.dropLast()) : trimmed
    }

    // MARK: - Private

    static func isPrivateHost(_ host: String) -> Bool {
        let lower = host.lowercased()
        if lower == "localhost" || lower == "0.0.0.0" { return true }
        if lower.hasSuffix(".local") || lower.hasSuffix(".internal") { return true }

        // IPv4 literal?
        let parts = lower.split(separator: ".")
        if parts.count == 4, let octets = try? parts.map({ (p: Substring) -> Int in
            guard let n = Int(p), (0...255).contains(n) else { throw GridexError.internalError("bad octet") }
            return n
        }) {
            return isPrivateIPv4(octets)
        }

        // IPv6 loopback / link-local
        if lower == "::1" { return true }
        if lower.hasPrefix("fe80:") || lower.hasPrefix("fc") || lower.hasPrefix("fd") { return true }
        return false
    }

    private static func isPrivateIPv4(_ o: [Int]) -> Bool {
        guard o.count == 4 else { return false }
        // 10.0.0.0/8
        if o[0] == 10 { return true }
        // 172.16.0.0/12
        if o[0] == 172, (16...31).contains(o[1]) { return true }
        // 192.168.0.0/16
        if o[0] == 192 && o[1] == 168 { return true }
        // 127.0.0.0/8 loopback
        if o[0] == 127 { return true }
        // 169.254.0.0/16 link-local
        if o[0] == 169 && o[1] == 254 { return true }
        return false
    }
}
