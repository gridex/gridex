// ConnectionRepositoryTests.swift — verifies bug #3 in PR #43:
// the SwiftData entity now persists sslMode + sslKeyPath / sslCertPath / sslCACertPath,
// so a saved mTLS connection survives a "restart" (reload from store).

import XCTest
import SwiftData
@testable import Gridex

@MainActor
final class ConnectionRepositoryTests: XCTestCase {

    private var container: ModelContainer!
    private var repository: SwiftDataConnectionRepository!

    override func setUpWithError() throws {
        let schema = Schema([
            SavedConnectionEntity.self,
            QueryHistoryEntity.self,
            LLMProviderEntity.self,
        ])
        let cfg = ModelConfiguration("test-\(UUID().uuidString)",
                                     schema: schema,
                                     isStoredInMemoryOnly: true)
        container = try ModelContainer(for: schema, configurations: [cfg])
        repository = SwiftDataConnectionRepository(modelContainer: container)
    }

    // MARK: - Bug #3 — mTLS cert paths must round-trip

    func test_save_then_fetch_preservesAllCertPaths() async throws {
        let id = UUID()
        let original = ConnectionConfig(
            id: id,
            name: "mTLS Postgres",
            databaseType: .postgresql,
            host: "db.example.com",
            port: 5432,
            database: "app",
            username: "service",
            sslEnabled: true,
            sslMode: .verifyIdentity,
            sslKeyPath: "/Users/me/.teleport/key.pem",
            sslCertPath: "/Users/me/.teleport/cert.pem",
            sslCACertPath: "/Users/me/.teleport/ca.pem"
        )

        try await repository.save(original)
        let restored = try await repository.fetchByID(id)

        XCTAssertNotNil(restored, "saved row must be fetchable")
        XCTAssertEqual(restored?.sslMode, .verifyIdentity, "sslMode lost on round-trip")
        XCTAssertEqual(restored?.sslKeyPath, "/Users/me/.teleport/key.pem",
                       "sslKeyPath lost on round-trip — bug #3 regression")
        XCTAssertEqual(restored?.sslCertPath, "/Users/me/.teleport/cert.pem",
                       "sslCertPath lost on round-trip — bug #3 regression")
        XCTAssertEqual(restored?.sslCACertPath, "/Users/me/.teleport/ca.pem",
                       "sslCACertPath lost on round-trip — bug #3 regression")
    }

    func test_update_preservesAllCertPaths() async throws {
        let id = UUID()
        let initial = ConnectionConfig(
            id: id, name: "first", databaseType: .postgresql,
            host: "h", username: "u", sslEnabled: true, sslMode: .preferred
        )
        try await repository.save(initial)

        // Edit: switch to mTLS by adding cert paths.
        let edited = ConnectionConfig(
            id: id, name: "edited-mtls", databaseType: .postgresql,
            host: "h", username: "u", sslEnabled: true, sslMode: .verifyCA,
            sslKeyPath: "/etc/pg/key.pem",
            sslCertPath: "/etc/pg/cert.pem",
            sslCACertPath: "/etc/pg/root.crt"
        )
        try await repository.update(edited)

        let restored = try await repository.fetchByID(id)
        XCTAssertEqual(restored?.name, "edited-mtls")
        XCTAssertEqual(restored?.sslMode, .verifyCA)
        XCTAssertEqual(restored?.sslKeyPath, "/etc/pg/key.pem",
                       "update() dropped sslKeyPath")
        XCTAssertEqual(restored?.sslCertPath, "/etc/pg/cert.pem",
                       "update() dropped sslCertPath")
        XCTAssertEqual(restored?.sslCACertPath, "/etc/pg/root.crt",
                       "update() dropped sslCACertPath")
    }

    // MARK: - Bug #2 — sslMode persistence

    func test_save_then_fetch_preservesAllFiveSSLModes() async throws {
        let modes: [SSLMode] = [.disabled, .preferred, .required, .verifyCA, .verifyIdentity]
        for mode in modes {
            let id = UUID()
            let cfg = ConnectionConfig(
                id: id, name: mode.rawValue, databaseType: .postgresql,
                host: "h", username: "u",
                sslEnabled: mode != .disabled, sslMode: mode
            )
            try await repository.save(cfg)
            let restored = try await repository.fetchByID(id)
            XCTAssertEqual(restored?.sslMode, mode,
                           "sslMode=\(mode) was not persisted correctly")
            XCTAssertEqual(restored?.effectiveSSLMode, mode,
                           "effectiveSSLMode mismatch for \(mode)")
        }
    }

    // MARK: - Backward-compat — legacy row (no sslMode column written)

    func test_legacyRow_withoutSSLMode_fallsBackToPreferred() async throws {
        // Simulate a row saved by pre-PR Gridex: sslEnabled set, sslMode field nil.
        // The repository writes whatever the config has; pre-PR config wouldn't
        // have set sslMode at all → entity sslMode column stays nil.
        let id = UUID()
        let legacy = ConnectionConfig(
            id: id, name: "legacy", databaseType: .postgresql,
            host: "h", username: "u",
            sslEnabled: true, sslMode: nil  // pre-PR shape
        )
        try await repository.save(legacy)
        let restored = try await repository.fetchByID(id)

        XCTAssertNil(restored?.sslMode,
                     "legacy row must keep sslMode=nil, not silently upgrade")
        XCTAssertTrue(restored?.sslEnabled == true)
        XCTAssertEqual(restored?.effectiveSSLMode, .preferred,
                       "legacy sslEnabled=true must read back as .preferred (matches pre-PR adapter)")
    }
}
