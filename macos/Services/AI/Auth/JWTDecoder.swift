// JWTDecoder.swift
// Gridex
//
// Minimal JWT payload decoder. We never verify the signature here — the issuer
// is auth.openai.com over TLS, and refresh failures are surfaced by the server.
// We only need to read claims (`exp`, `email`, `chatgpt_account_id`, …).

import Foundation

enum JWTDecoder {
    enum DecodeError: Error, CustomStringConvertible {
        case malformed(String)
        var description: String {
            switch self {
            case .malformed(let why): return "Malformed JWT: \(why)"
            }
        }
    }

    /// Decodes the second segment of a JWT (the payload) into a JSON dictionary.
    /// Throws `DecodeError.malformed` if the token doesn't have the expected
    /// `header.payload.signature` shape or the payload isn't a JSON object.
    static func payload(of jwt: String) throws -> [String: Any] {
        let parts = jwt.split(separator: ".", omittingEmptySubsequences: false)
        guard parts.count == 3 else {
            throw DecodeError.malformed("expected 3 segments, got \(parts.count)")
        }
        guard let data = base64URLDecode(String(parts[1])) else {
            throw DecodeError.malformed("payload is not valid base64url")
        }
        guard
            let json = try JSONSerialization.jsonObject(with: data) as? [String: Any]
        else {
            throw DecodeError.malformed("payload is not a JSON object")
        }
        return json
    }

    /// Convenience: extract the `exp` claim and convert to a Date. Returns nil
    /// if the claim is missing or not a number — caller should treat that as
    /// "expired" (forces a refresh).
    static func expiration(of jwt: String) -> Date? {
        guard let claims = try? payload(of: jwt) else { return nil }
        guard let exp = claims["exp"] as? TimeInterval else {
            // Some JWTs encode exp as Int — try that too
            if let expInt = claims["exp"] as? Int {
                return Date(timeIntervalSince1970: TimeInterval(expInt))
            }
            return nil
        }
        return Date(timeIntervalSince1970: exp)
    }

    // MARK: - Internal

    /// base64url → Data. Re-pads to a multiple of 4 with `=` and reverses the
    /// URL-safe substitutions before delegating to `Data(base64Encoded:)`.
    static func base64URLDecode(_ s: String) -> Data? {
        var str = s
            .replacingOccurrences(of: "-", with: "+")
            .replacingOccurrences(of: "_", with: "/")
        let pad = str.count % 4
        if pad > 0 { str.append(String(repeating: "=", count: 4 - pad)) }
        return Data(base64Encoded: str)
    }
}
