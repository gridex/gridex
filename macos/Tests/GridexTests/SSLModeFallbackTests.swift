// SSLModeFallbackTests.swift — pure-logic tests for ConnectionConfig.effectiveSSLMode.
// Verifies the back-compat ladder added in PR #43 / incident 6a4aad0.

import XCTest
@testable import Gridex

final class SSLModeFallbackTests: XCTestCase {

    // MARK: - effectiveSSLMode

    func test_effectiveSSLMode_prefersExplicitMode_overLegacyBool() {
        // Explicit sslMode wins regardless of legacy sslEnabled.
        let cases: [(SSLMode, Bool)] = [
            (.disabled, true),
            (.preferred, true),
            (.required, false),
            (.verifyCA, true),
            (.verifyIdentity, false),
        ]
        for (mode, legacy) in cases {
            let cfg = makeConfig(sslEnabled: legacy, sslMode: mode)
            XCTAssertEqual(cfg.effectiveSSLMode, mode,
                "explicit sslMode=\(mode) must win over sslEnabled=\(legacy)")
        }
    }

    func test_effectiveSSLMode_legacyTrue_mapsToPreferred() {
        // Pre-PR rows: sslEnabled=true / sslMode=nil → adapter used .prefer(tls).
        // effectiveSSLMode must preserve that exact behavior, not silently upgrade
        // to .required (would break users on servers without TLS).
        let cfg = makeConfig(sslEnabled: true, sslMode: nil)
        XCTAssertEqual(cfg.effectiveSSLMode, .preferred)
    }

    func test_effectiveSSLMode_legacyFalse_mapsToDisabled() {
        let cfg = makeConfig(sslEnabled: false, sslMode: nil)
        XCTAssertEqual(cfg.effectiveSSLMode, .disabled)
    }

    // MARK: - Codable back-compat

    func test_codable_decodesLegacyJSON_withoutSSLMode() throws {
        // A JSON payload as it would have looked before sslMode existed.
        let legacyJSON = """
        {
          "id": "00000000-0000-0000-0000-000000000001",
          "name": "Legacy",
          "databaseType": "postgresql",
          "host": "127.0.0.1",
          "port": 5432,
          "username": "postgres",
          "sslEnabled": true,
          "mcpMode": "locked"
        }
        """.data(using: .utf8)!

        let decoded = try JSONDecoder().decode(ConnectionConfig.self, from: legacyJSON)
        XCTAssertNil(decoded.sslMode, "missing field decodes as nil, not a default")
        XCTAssertTrue(decoded.sslEnabled)
        XCTAssertEqual(decoded.effectiveSSLMode, .preferred,
            "legacy sslEnabled=true must continue to behave as .preferred")
    }

    func test_codable_roundTripsExplicitMode() throws {
        let original = makeConfig(sslEnabled: true, sslMode: .verifyIdentity,
                                  certPaths: (key: "/tmp/k.pem", cert: "/tmp/c.pem", ca: "/tmp/ca.pem"))
        let data = try JSONEncoder().encode(original)
        let restored = try JSONDecoder().decode(ConnectionConfig.self, from: data)
        XCTAssertEqual(restored.sslMode, .verifyIdentity)
        XCTAssertEqual(restored.effectiveSSLMode, .verifyIdentity)
        XCTAssertEqual(restored.sslKeyPath, "/tmp/k.pem")
        XCTAssertEqual(restored.sslCertPath, "/tmp/c.pem")
        XCTAssertEqual(restored.sslCACertPath, "/tmp/ca.pem")
    }

    // MARK: - Helpers

    private func makeConfig(
        sslEnabled: Bool,
        sslMode: SSLMode?,
        certPaths: (key: String?, cert: String?, ca: String?) = (nil, nil, nil)
    ) -> ConnectionConfig {
        ConnectionConfig(
            id: UUID(),
            name: "test",
            databaseType: .postgresql,
            host: "127.0.0.1",
            port: 5432,
            database: "postgres",
            username: "postgres",
            sslEnabled: sslEnabled,
            sslMode: sslMode,
            sslKeyPath: certPaths.key,
            sslCertPath: certPaths.cert,
            sslCACertPath: certPaths.ca
        )
    }
}
