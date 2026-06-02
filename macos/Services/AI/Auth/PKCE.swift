// PKCE.swift
// Gridex
//
// RFC 7636 PKCE primitives for the ChatGPT OAuth flow. Pure functions, no I/O.
//
// All outputs are base64url-no-pad: standard base64, then `+` → `-`, `/` → `_`,
// trailing `=` stripped.

import CryptoKit
import Foundation
import Security

enum PKCE {
    /// 32 random bytes → base64url-no-pad (43 chars). Suitable as `code_verifier`.
    static func makeVerifier() -> String {
        base64URL(randomBytes(count: 32))
    }

    /// SHA256(verifier) → base64url-no-pad. Used as `code_challenge`
    /// with `code_challenge_method=S256`.
    static func challenge(for verifier: String) -> String {
        let data = Data(verifier.utf8)
        let hash = SHA256.hash(data: data)
        return base64URL(Data(hash))
    }

    /// 32 random bytes → base64url-no-pad. Used as the OAuth `state` param to
    /// detect CSRF on the loopback callback.
    static func makeState() -> String {
        base64URL(randomBytes(count: 32))
    }

    /// Standard base64 → URL-safe variant (no padding).
    /// Exposed for tests that need to verify against RFC 7636 vectors.
    static func base64URL(_ data: Data) -> String {
        data.base64EncodedString()
            .replacingOccurrences(of: "+", with: "-")
            .replacingOccurrences(of: "/", with: "_")
            .replacingOccurrences(of: "=", with: "")
    }

    // MARK: - Internal

    /// Cryptographically secure random bytes via Security framework's CSPRNG.
    /// Falls back to `SystemRandomNumberGenerator` only if `SecRandomCopyBytes`
    /// somehow fails — should never happen on healthy macOS hosts.
    private static func randomBytes(count: Int) -> Data {
        var bytes = [UInt8](repeating: 0, count: count)
        let status = SecRandomCopyBytes(kSecRandomDefault, count, &bytes)
        guard status == errSecSuccess else {
            // Last-resort fallback — keeps the API total without crashing.
            var rng = SystemRandomNumberGenerator()
            for i in 0..<count { bytes[i] = UInt8.random(in: 0...255, using: &rng) }
            return Data(bytes)
        }
        return Data(bytes)
    }
}
