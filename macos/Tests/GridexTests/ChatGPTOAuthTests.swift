// ChatGPTOAuthTests.swift — pure-logic tests for the ChatGPT OAuth foundation.
// Covers PKCE primitives, JWT payload decoding, and token bundle round-trips.
// The interactive sign-in flow + live HTTP refresh land in PR 2 with their own
// URL-protocol-mocked tests.

import XCTest
@testable import Gridex

final class ChatGPTOAuthTests: XCTestCase {

    // MARK: - Keychain fixture

    private var keychain: KeychainService!
    private var trackedProviderIds: [UUID] = []

    override func setUp() {
        super.setUp()
        keychain = KeychainService()
    }

    override func tearDown() {
        trackedProviderIds.forEach { try? keychain.deleteChatGPTTokens(providerId: $0) }
        trackedProviderIds.removeAll()
        keychain = nil
        super.tearDown()
    }

    /// Mints a fresh provider UUID and registers it for tearDown cleanup, so the
    /// test body doesn't need its own `defer { try? keychain.delete... }`.
    private func makeProviderId() -> UUID {
        let id = UUID()
        trackedProviderIds.append(id)
        return id
    }

    // MARK: - PKCE

    func test_makeVerifier_returns43CharBase64URL() {
        let v = PKCE.makeVerifier()
        // 32 bytes → 43 base64url chars (no padding)
        XCTAssertEqual(v.count, 43, "verifier should be 43 chars")
        XCTAssertFalse(v.contains("="), "no padding")
        XCTAssertFalse(v.contains("+"), "URL-safe alphabet only")
        XCTAssertFalse(v.contains("/"), "URL-safe alphabet only")
    }

    func test_makeVerifier_isUniquePerCall() {
        let a = PKCE.makeVerifier()
        let b = PKCE.makeVerifier()
        XCTAssertNotEqual(a, b, "CSPRNG should not repeat in 32 bytes of entropy")
    }

    /// RFC 7636 Appendix B test vector — locks the SHA256 + base64url contract.
    func test_challenge_matchesRFC7636Vector() {
        let verifier  = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk"
        let expected  = "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM"
        XCTAssertEqual(PKCE.challenge(for: verifier), expected)
    }

    func test_makeState_returns43CharBase64URL() {
        let s = PKCE.makeState()
        XCTAssertEqual(s.count, 43)
        XCTAssertFalse(s.contains("="))
    }

    func test_base64URL_strippsPaddingAndSubstitutes() {
        // 1 byte → "AA==" in standard base64. URL-safe variant: "AA".
        XCTAssertEqual(PKCE.base64URL(Data([0])), "AA")
        // Two bytes hitting a "+" in standard base64.
        // 0xFB 0xFF → "+/8=" → "-_8" after URL-safe + strip.
        XCTAssertEqual(PKCE.base64URL(Data([0xFB, 0xFF])), "-_8")
    }

    // MARK: - JWTDecoder

    /// A hand-crafted unsigned JWT whose payload is `{"exp":1700000000,"sub":"abc","email":"u@example.com"}`.
    /// Header `{"alg":"none","typ":"JWT"}`, signature segment empty.
    private static let fixtureJWT: String = {
        let header  = #"{"alg":"none","typ":"JWT"}"#
        let payload = #"{"exp":1700000000,"sub":"abc","email":"u@example.com"}"#
        let h = PKCE.base64URL(Data(header.utf8))
        let p = PKCE.base64URL(Data(payload.utf8))
        return "\(h).\(p)."
    }()

    func test_jwtPayload_decodesClaims() throws {
        let claims = try JWTDecoder.payload(of: Self.fixtureJWT)
        XCTAssertEqual(claims["sub"] as? String, "abc")
        XCTAssertEqual(claims["email"] as? String, "u@example.com")
        XCTAssertEqual(claims["exp"] as? Int, 1700000000)
    }

    func test_jwtExpiration_returnsCorrectDate() throws {
        let date = try XCTUnwrap(JWTDecoder.expiration(of: Self.fixtureJWT))
        XCTAssertEqual(date.timeIntervalSince1970, 1700000000, accuracy: 0.001)
    }

    func test_jwtPayload_throwsOnMalformedSegmentCount() {
        XCTAssertThrowsError(try JWTDecoder.payload(of: "only.two")) { error in
            guard case JWTDecoder.DecodeError.malformed = error else {
                return XCTFail("expected malformed error, got \(error)")
            }
        }
    }

    func test_jwtPayload_throwsOnInvalidBase64() {
        XCTAssertThrowsError(try JWTDecoder.payload(of: "header.@@@.sig"))
    }

    func test_jwtPayload_throwsWhenPayloadIsNotObject() throws {
        // Payload "[]" is valid JSON but not an object.
        let header = PKCE.base64URL(Data(#"{"alg":"none"}"#.utf8))
        let body   = PKCE.base64URL(Data("[]".utf8))
        XCTAssertThrowsError(try JWTDecoder.payload(of: "\(header).\(body)."))
    }

    func test_jwtExpiration_returnsNilWhenClaimMissing() {
        let header = PKCE.base64URL(Data(#"{"alg":"none"}"#.utf8))
        let body   = PKCE.base64URL(Data(#"{"sub":"x"}"#.utf8))
        let jwt    = "\(header).\(body)."
        XCTAssertNil(JWTDecoder.expiration(of: jwt))
    }

    // MARK: - ChatGPTTokenBundle

    func test_tokenBundle_codableRoundTrip() throws {
        let original = ChatGPTTokenBundle(
            accessToken: "access.jwt.value",
            refreshToken: "refresh.jwt.value",
            idToken: "id.jwt.value",
            accountId: "acc_123",
            email: "u@example.com",
            planType: "plus",
            obtainedAt: Date(timeIntervalSince1970: 1700000000),
            schemaVersion: 1
        )

        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .iso8601
        let data = try encoder.encode(original)

        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        let decoded = try decoder.decode(ChatGPTTokenBundle.self, from: data)

        XCTAssertEqual(decoded, original)
    }

    func test_tokenBundle_optionalFieldsRoundTrip() throws {
        let original = ChatGPTTokenBundle(
            accessToken: "a", refreshToken: "r", idToken: "i"
        )
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .iso8601
        let data = try encoder.encode(original)
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        let decoded = try decoder.decode(ChatGPTTokenBundle.self, from: data)

        XCTAssertNil(decoded.accountId)
        XCTAssertNil(decoded.email)
        XCTAssertNil(decoded.planType)
        XCTAssertEqual(decoded.schemaVersion, 1)
    }

    // MARK: - Keychain helpers

    func test_keychain_chatGPTTokens_roundTrip() throws {
        let providerId = makeProviderId()

        // Pre-condition: nothing stored yet.
        XCTAssertNil(try keychain.loadChatGPTTokens(providerId: providerId))

        let bundle = ChatGPTTokenBundle(
            accessToken: "access",
            refreshToken: "refresh",
            idToken: "id",
            accountId: "acc",
            email: "u@example.com",
            planType: "pro",
            obtainedAt: Date(timeIntervalSince1970: 1700000000)
        )
        try keychain.saveChatGPTTokens(providerId: providerId, bundle: bundle)

        let loaded = try keychain.loadChatGPTTokens(providerId: providerId)
        XCTAssertEqual(loaded, bundle)

        try keychain.deleteChatGPTTokens(providerId: providerId)
        XCTAssertNil(try keychain.loadChatGPTTokens(providerId: providerId))
    }

    /// Locks the per-provider key namespacing — saving under one UUID must not
    /// be visible under another, and deleting under an unrelated UUID must not
    /// touch the original. Guards against a future regression where someone
    /// accidentally drops the UUID from the key (sharing a single slot).
    func test_keychain_chatGPTTokens_isolatedAcrossProviders() throws {
        let providerA = makeProviderId()
        let providerB = makeProviderId()

        let bundleA = ChatGPTTokenBundle(
            accessToken: "A", refreshToken: "rA", idToken: "iA",
            obtainedAt: Date(timeIntervalSince1970: 1700000000)
        )
        try keychain.saveChatGPTTokens(providerId: providerA, bundle: bundleA)

        XCTAssertNil(
            try keychain.loadChatGPTTokens(providerId: providerB),
            "loading under providerB must not see providerA's bundle"
        )

        try keychain.deleteChatGPTTokens(providerId: providerB)
        XCTAssertEqual(
            try keychain.loadChatGPTTokens(providerId: providerA),
            bundleA,
            "delete on providerB must not touch providerA's bundle"
        )
    }

    /// Locks "second save fully replaces the first" — this is the production
    /// path used by every token refresh in `ChatGPTOAuthService.performRefresh`.
    /// A regression where save somehow merged or appended would corrupt the
    /// bundle silently; this test catches that.
    func test_keychain_chatGPTTokens_overwriteReplacesPriorValue() throws {
        let providerId = makeProviderId()

        let first = ChatGPTTokenBundle(
            accessToken: "old", refreshToken: "old-r", idToken: "old-i",
            accountId: "acc-old",
            email: "old@example.com",
            planType: "plus",
            obtainedAt: Date(timeIntervalSince1970: 1700000000)
        )
        let second = ChatGPTTokenBundle(
            accessToken: "new", refreshToken: "new-r", idToken: "new-i",
            accountId: "acc-new",
            email: "new@example.com",
            planType: "pro",
            obtainedAt: Date(timeIntervalSince1970: 1700001000)
        )

        try keychain.saveChatGPTTokens(providerId: providerId, bundle: first)
        try keychain.saveChatGPTTokens(providerId: providerId, bundle: second)

        let loaded = try keychain.loadChatGPTTokens(providerId: providerId)
        XCTAssertEqual(loaded, second, "second save must fully replace the first")
    }

    /// Locks the current `dateEncodingStrategy = .iso8601` contract:
    /// `ISO8601DateFormatter` defaults to `[.withInternetDateTime]` (no
    /// fractional seconds), so `obtainedAt` round-trips truncated to whole
    /// seconds. This is harmless today (`ChatGPTOAuthService` uses JWT `exp`
    /// for refresh timing, not `obtainedAt`, and never compares bundles for
    /// equality), but if a future change starts depending on subsecond
    /// precision, this test will fire and flag that the encoding strategy
    /// needs `.withFractionalSeconds`.
    func test_keychain_chatGPTTokens_obtainedAtRoundTripStripsSubseconds() throws {
        let providerId = makeProviderId()

        // 0.123s past a whole-second boundary — small enough that any rounding
        // mode (truncate or round-to-nearest) lands on the floor value.
        let originalTs: TimeInterval = 1700000000.123
        let bundle = ChatGPTTokenBundle(
            accessToken: "a", refreshToken: "r", idToken: "i",
            obtainedAt: Date(timeIntervalSince1970: originalTs)
        )

        try keychain.saveChatGPTTokens(providerId: providerId, bundle: bundle)
        let loaded = try XCTUnwrap(try keychain.loadChatGPTTokens(providerId: providerId))

        XCTAssertEqual(
            loaded.obtainedAt.timeIntervalSince1970,
            1700000000.0,
            accuracy: 0.0001,
            "iso8601 default options drop fractional seconds"
        )
        XCTAssertNotEqual(
            loaded.obtainedAt,
            bundle.obtainedAt,
            "Equatable should detect the subsecond loss — if this fails, the encoding has gained fractional-second precision and this test should become an equality assertion"
        )
    }
}
