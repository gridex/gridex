// ChatGPTOAuthServiceTests.swift
// Gridex
//
// Mocked-network tests for ChatGPTOAuthService.tokenBundle(...) refresh path:
//   1. Concurrent calls with an expired access_token coalesce into ONE
//      auth.openai.com round-trip (per-providerId in-flight de-dup).
//   2. `error: refresh_token_expired` from the server clears the keychain
//      blob and surfaces aiAPIKeyMissing for the caller to prompt re-login.
//   3. A 200 response without `refresh_token` keeps the previously-stored one
//      (server may rotate, may not — we mustn't drop it).
//
// We do not exercise the interactive `signIn(...)` path here — that opens
// NSWorkspace + the loopback server, which is end-to-end manual territory.

import XCTest
@testable import Gridex

final class ChatGPTOAuthServiceTests: XCTestCase {

    override func setUp() {
        super.setUp()
        MockURLProtocol.reset()
    }

    override func tearDown() {
        MockURLProtocol.reset()
        super.tearDown()
    }

    // MARK: - 1. Concurrent refresh de-duplication

    func test_concurrentTokenBundle_coalescesToSingleRefresh() async throws {
        let providerId = UUID()
        let keychain = MockKeychain()
        keychain.bundle = Self.makeExpiredBundle()

        // Server takes 200ms to respond, giving every concurrent caller a
        // chance to race into refreshInFlight before the first task finishes.
        MockURLProtocol.responseDelay = 0.2
        MockURLProtocol.responder = { _ in
            let body = Self.tokenJSON(
                accessToken: Self.freshAccessJWT(),
                refreshToken: "rotated-refresh",
                idToken: Self.idJWTFixture()
            )
            return (Self.okResponse(), body)
        }

        let service = ChatGPTOAuthService(
            keychainService: keychain,
            urlSession: Self.mockedSession()
        )

        // Fan out 5 concurrent calls. All 5 must succeed; the server must
        // see exactly 1 hit (the actor coalesces the rest onto the same Task).
        try await withThrowingTaskGroup(of: ChatGPTTokenBundle.self) { group in
            for _ in 0..<5 {
                group.addTask { try await service.tokenBundle(providerId: providerId) }
            }
            var results: [ChatGPTTokenBundle] = []
            for try await b in group { results.append(b) }
            XCTAssertEqual(results.count, 5)
            for b in results {
                XCTAssertEqual(b.refreshToken, "rotated-refresh", "all callers see the rotated token")
            }
        }

        XCTAssertEqual(MockURLProtocol.requestCount, 1,
                       "5 concurrent calls must collapse into a single refresh request")
    }

    // MARK: - 2. Refresh-token-expired path

    func test_refreshTokenExpired_clearsKeychainAndThrowsAPIKeyMissing() async throws {
        let providerId = UUID()
        let keychain = MockKeychain()
        keychain.bundle = Self.makeExpiredBundle()

        MockURLProtocol.responder = { _ in
            let body = Data(#"{"error":"refresh_token_expired","error_description":"jwt is expired"}"#.utf8)
            return (Self.errorResponse(status: 400), body)
        }

        let service = ChatGPTOAuthService(
            keychainService: keychain,
            urlSession: Self.mockedSession()
        )

        do {
            _ = try await service.tokenBundle(providerId: providerId)
            XCTFail("expected aiAPIKeyMissing to be thrown")
        } catch GridexError.aiAPIKeyMissing {
            // expected
        } catch {
            XCTFail("expected aiAPIKeyMissing, got \(error)")
        }

        XCTAssertNil(keychain.bundle, "expired refresh_token must purge the keychain blob")
    }

    // MARK: - 3. Optional refresh_token preservation

    func test_refreshSuccessWithoutRotation_keepsOldRefreshToken() async throws {
        let providerId = UUID()
        let keychain = MockKeychain()
        let original = Self.makeExpiredBundle(refreshToken: "original-refresh")
        keychain.bundle = original

        MockURLProtocol.responder = { _ in
            // Server rotates access_token + id_token but omits refresh_token.
            let body = Self.tokenJSON(
                accessToken: Self.freshAccessJWT(),
                refreshToken: nil,
                idToken: Self.idJWTFixture()
            )
            return (Self.okResponse(), body)
        }

        let service = ChatGPTOAuthService(
            keychainService: keychain,
            urlSession: Self.mockedSession()
        )

        let updated = try await service.tokenBundle(providerId: providerId)
        XCTAssertEqual(updated.refreshToken, "original-refresh",
                       "missing refresh_token in response must keep the old one")
        XCTAssertNotEqual(updated.accessToken, original.accessToken,
                          "access_token should still be rotated")
    }

    // MARK: - 4. Provider auth rejection cleanup

    func test_streamUnauthorized_clearsKeychainAndThrowsAPIKeyMissing() async throws {
        let providerId = UUID()
        let keychain = MockKeychain()
        keychain.bundle = Self.makeFreshBundle()

        MockURLProtocol.responder = { _ in
            let body = Data(#"{"error":{"message":"unauthorized"}}"#.utf8)
            return (Self.errorResponse(status: 401), body)
        }

        let service = ChatGPTOAuthService(
            keychainService: keychain,
            urlSession: Self.mockedSession()
        )
        let provider = ChatGPTProvider(
            providerId: providerId,
            oauthService: service,
            urlSession: Self.mockedSession()
        )

        do {
            var iterator = provider.stream(
                messages: [LLMMessage(role: .user, content: "hi")],
                systemPrompt: "",
                model: "gpt-test",
                maxTokens: 1,
                temperature: 0
            ).makeAsyncIterator()
            _ = try await iterator.next()
            XCTFail("expected aiAPIKeyMissing")
        } catch GridexError.aiAPIKeyMissing {
            // expected
        } catch {
            XCTFail("expected aiAPIKeyMissing, got \(error)")
        }

        XCTAssertNil(keychain.bundle, "401 from /responses must clear the stale bundle")
    }

    func test_availableModelsForbidden_clearsKeychainAndThrowsAPIKeyMissing() async throws {
        let providerId = UUID()
        let keychain = MockKeychain()
        keychain.bundle = Self.makeFreshBundle()

        MockURLProtocol.responder = { _ in
            let body = Data(#"{"error":{"message":"forbidden"}}"#.utf8)
            return (Self.errorResponse(status: 403), body)
        }

        let service = ChatGPTOAuthService(
            keychainService: keychain,
            urlSession: Self.mockedSession()
        )
        let provider = ChatGPTProvider(
            providerId: providerId,
            oauthService: service,
            urlSession: Self.mockedSession()
        )

        do {
            _ = try await provider.availableModels()
            XCTFail("expected aiAPIKeyMissing")
        } catch GridexError.aiAPIKeyMissing {
            // expected
        } catch {
            XCTFail("expected aiAPIKeyMissing, got \(error)")
        }

        XCTAssertNil(keychain.bundle, "403 from /models must clear the stale bundle")
    }

    // MARK: - 5. tokenBundle short-circuit + missing keychain

    /// A non-expired access_token must NOT trigger a refresh round-trip — every
    /// production request hits this path. Regression here means we'd hammer
    /// auth.openai.com on every call and risk being rate-limited.
    func test_tokenBundle_freshToken_skipsNetworkRefresh() async throws {
        let providerId = UUID()
        let keychain = MockKeychain()
        keychain.bundle = Self.makeFreshBundle(refreshToken: "kept-refresh")

        MockURLProtocol.responder = { _ in
            XCTFail("fresh token must not hit auth.openai.com")
            return (Self.errorResponse(status: 500), Data())
        }

        let service = ChatGPTOAuthService(
            keychainService: keychain,
            urlSession: Self.mockedSession()
        )

        let bundle = try await service.tokenBundle(providerId: providerId)
        XCTAssertEqual(bundle.refreshToken, "kept-refresh")
        XCTAssertEqual(MockURLProtocol.requestCount, 0,
                       "non-expired token must not trigger refresh")
    }

    /// When the user added a ChatGPT provider row but never completed sign-in,
    /// `tokenBundle()` must surface `aiAPIKeyMissing` so the UI can prompt for
    /// re-login. DependencyContainer.bootstrap relies on this contract.
    func test_tokenBundle_missingKeychain_throwsAPIKeyMissing() async throws {
        let providerId = UUID()
        let keychain = MockKeychain()

        let service = ChatGPTOAuthService(
            keychainService: keychain,
            urlSession: Self.mockedSession()
        )

        do {
            _ = try await service.tokenBundle(providerId: providerId)
            XCTFail("expected aiAPIKeyMissing")
        } catch GridexError.aiAPIKeyMissing {
            // expected
        } catch {
            XCTFail("expected aiAPIKeyMissing, got \(error)")
        }

        XCTAssertEqual(MockURLProtocol.requestCount, 0,
                       "must not call auth.openai.com when no bundle exists")
    }

    // MARK: - 6. ChatGPTProvider SSE streaming

    /// Locks the SSE happy path — accumulating deltas and finishing on
    /// `response.completed`. This is ChatGPTProvider's core contract; without
    /// this test a regression in `parseSSE` would silently drop tokens or
    /// hang forever, and only end-to-end testing would catch it.
    func test_streamHappyPath_yieldsDeltasAndFinishes() async throws {
        let providerId = UUID()
        let keychain = MockKeychain()
        keychain.bundle = Self.makeFreshBundle()

        // SSE wire format: `event: …\ndata: …` followed by a blank line that
        // terminates the event. The two trailing blank lines below are
        // load-bearing — the second one terminates `response.completed`.
        let sse = """
        event: response.output_text.delta
        data: {"delta":"Hello"}

        event: response.output_text.delta
        data: {"delta":", "}

        event: response.output_text.delta
        data: {"delta":"world"}

        event: response.completed
        data: {}


        """

        MockURLProtocol.responder = { _ in
            (Self.sseResponse(), Data(sse.utf8))
        }

        let service = ChatGPTOAuthService(
            keychainService: keychain,
            urlSession: Self.mockedSession()
        )
        let provider = ChatGPTProvider(
            providerId: providerId,
            oauthService: service,
            urlSession: Self.mockedSession()
        )

        var collected = ""
        for try await chunk in provider.stream(
            messages: [LLMMessage(role: .user, content: "hi")],
            systemPrompt: "",
            model: "gpt-test",
            maxTokens: 1,
            temperature: 0
        ) {
            collected += chunk
        }
        XCTAssertEqual(collected, "Hello, world",
                       "deltas must concatenate in arrival order")
    }

    /// `response.error` events must surface as `aiStreamingError` with the
    /// server's message. The chat UI depends on this to render rate-limit /
    /// content-policy failures inline rather than as a generic crash.
    func test_streamErrorEvent_throwsStreamingError() async throws {
        let providerId = UUID()
        let keychain = MockKeychain()
        keychain.bundle = Self.makeFreshBundle()

        // Trailing blank line terminates the `response.error` event.
        let sse = """
        event: response.error
        data: {"message":"rate limited"}


        """

        MockURLProtocol.responder = { _ in
            (Self.sseResponse(), Data(sse.utf8))
        }

        let service = ChatGPTOAuthService(
            keychainService: keychain,
            urlSession: Self.mockedSession()
        )
        let provider = ChatGPTProvider(
            providerId: providerId,
            oauthService: service,
            urlSession: Self.mockedSession()
        )

        do {
            for try await _ in provider.stream(
                messages: [LLMMessage(role: .user, content: "hi")],
                systemPrompt: "",
                model: "gpt-test",
                maxTokens: 1,
                temperature: 0
            ) {}
            XCTFail("expected aiStreamingError")
        } catch let GridexError.aiStreamingError(message) {
            XCTAssertEqual(message, "rate limited",
                           "server's error message must propagate to caller")
        } catch {
            XCTFail("expected aiStreamingError, got \(error)")
        }
    }

    // MARK: - 7. availableModels filtering

    /// Locks the list-filter contract — `supported_in_api == false` and
    /// `visibility != "list"` rows must be dropped. Server returns hidden /
    /// deprecated entries; without this filter the UI's model picker would
    /// surface unusable slugs that 400 on /responses.
    func test_availableModels_filtersHiddenAndUnsupported() async throws {
        let providerId = UUID()
        let keychain = MockKeychain()
        keychain.bundle = Self.makeFreshBundle()

        let body: [String: Any] = [
            "models": [
                ["slug": "gpt-5", "name": "GPT-5",
                 "supported_in_api": true, "visibility": "list",
                 "context_window": 200_000],
                ["slug": "gpt-internal", "name": "Internal",
                 "supported_in_api": true, "visibility": "hidden"],
                ["slug": "gpt-deprecated", "name": "Old",
                 "supported_in_api": false, "visibility": "list"],
                ["slug": "gpt-mini", "name": "GPT-mini",
                 "supported_in_api": true, "visibility": "list"],
            ],
        ]
        let data = try JSONSerialization.data(withJSONObject: body)

        MockURLProtocol.responder = { _ in
            (Self.modelsResponse(), data)
        }

        let service = ChatGPTOAuthService(
            keychainService: keychain,
            urlSession: Self.mockedSession()
        )
        let provider = ChatGPTProvider(
            providerId: providerId,
            oauthService: service,
            urlSession: Self.mockedSession()
        )

        let models = try await provider.availableModels()
        XCTAssertEqual(Set(models.map(\.id)), Set(["gpt-5", "gpt-mini"]),
                       "only supported + visible='list' models survive")
        XCTAssertEqual(models.first(where: { $0.id == "gpt-5" })?.contextWindow,
                       200_000,
                       "context_window from response must propagate")
    }

    // MARK: - 8. Loopback callback ordering

    func test_loopbackCallbackBeforeAwait_isBuffered() async throws {
        let server = ChatGPTOAuthLoopbackServer()
        let port = try await server.start(preferredPorts: [])
        defer { server.stop() }

        let url = URL(string: "http://127.0.0.1:\(port)\(ChatGPTOAuthConstants.callbackPath)?code=early-code&state=early-state")!
        _ = try await URLSession.shared.data(from: url)

        let callback = try await server.awaitCallback(timeout: 1)
        XCTAssertEqual(callback.code, "early-code")
        XCTAssertEqual(callback.state, "early-state")
    }

    // MARK: - Fixtures

    /// In-memory `ChatGPTTokenBundle` JWT fixtures encode the same exp claim
    /// the service inspects via JWTDecoder. Header is `{"alg":"none"}`.
    private static func makeJWT(payload: String) -> String {
        let header = #"{"alg":"none","typ":"JWT"}"#
        let h = PKCE.base64URL(Data(header.utf8))
        let p = PKCE.base64URL(Data(payload.utf8))
        return "\(h).\(p)."
    }

    /// access_token whose exp is 60s in the past — guaranteed to trigger refresh.
    private static func expiredAccessJWT() -> String {
        let exp = Int(Date().addingTimeInterval(-60).timeIntervalSince1970)
        return makeJWT(payload: #"{"exp":\#(exp),"sub":"u"}"#)
    }

    /// access_token with exp 1 hour in the future. Keeps tests deterministic.
    private static func freshAccessJWT() -> String {
        let exp = Int(Date().addingTimeInterval(3600).timeIntervalSince1970)
        return makeJWT(payload: #"{"exp":\#(exp),"sub":"u"}"#)
    }

    private static func idJWTFixture() -> String {
        makeJWT(payload: #"{"email":"u@example.com","chatgpt_account_id":"acc","chatgpt_plan_type":"plus"}"#)
    }

    private static func makeExpiredBundle(refreshToken: String = "old-refresh") -> ChatGPTTokenBundle {
        ChatGPTTokenBundle(
            accessToken: expiredAccessJWT(),
            refreshToken: refreshToken,
            idToken: idJWTFixture(),
            accountId: "acc",
            email: "u@example.com",
            planType: "plus",
            obtainedAt: Date(timeIntervalSince1970: 1_700_000_000)
        )
    }

    private static func makeFreshBundle(refreshToken: String = "old-refresh") -> ChatGPTTokenBundle {
        ChatGPTTokenBundle(
            accessToken: freshAccessJWT(),
            refreshToken: refreshToken,
            idToken: idJWTFixture(),
            accountId: "acc",
            email: "u@example.com",
            planType: "plus",
            obtainedAt: Date(timeIntervalSince1970: 1_700_000_000)
        )
    }

    private static func tokenJSON(accessToken: String, refreshToken: String?, idToken: String) -> Data {
        var dict: [String: Any] = ["access_token": accessToken, "id_token": idToken]
        if let refreshToken { dict["refresh_token"] = refreshToken }
        return try! JSONSerialization.data(withJSONObject: dict)
    }

    private static func okResponse() -> HTTPURLResponse {
        HTTPURLResponse(
            url: URL(string: "https://auth.openai.com/oauth/token")!,
            statusCode: 200,
            httpVersion: "HTTP/1.1",
            headerFields: ["Content-Type": "application/json"]
        )!
    }

    private static func errorResponse(status: Int) -> HTTPURLResponse {
        HTTPURLResponse(
            url: URL(string: "https://auth.openai.com/oauth/token")!,
            statusCode: status,
            httpVersion: "HTTP/1.1",
            headerFields: ["Content-Type": "application/json"]
        )!
    }

    private static func sseResponse() -> HTTPURLResponse {
        HTTPURLResponse(
            url: URL(string: "https://chatgpt.com/backend-api/codex/responses")!,
            statusCode: 200,
            httpVersion: "HTTP/1.1",
            headerFields: ["Content-Type": "text/event-stream"]
        )!
    }

    private static func modelsResponse() -> HTTPURLResponse {
        HTTPURLResponse(
            url: URL(string: "https://chatgpt.com/backend-api/codex/models")!,
            statusCode: 200,
            httpVersion: "HTTP/1.1",
            headerFields: ["Content-Type": "application/json"]
        )!
    }

    private static func mockedSession() -> URLSession {
        let cfg = URLSessionConfiguration.ephemeral
        cfg.protocolClasses = [MockURLProtocol.self]
        return URLSession(configuration: cfg)
    }
}

// MARK: - Mock keychain (in-memory, single-bundle)

private final class MockKeychain: KeychainServiceProtocol, @unchecked Sendable {
    private let lock = NSLock()
    private var _bundle: ChatGPTTokenBundle?

    var bundle: ChatGPTTokenBundle? {
        get { lock.lock(); defer { lock.unlock() }; return _bundle }
        set { lock.lock(); defer { lock.unlock() }; _bundle = newValue }
    }

    func save(key: String, value: String) throws { fatalError("unused in these tests") }
    func load(key: String) throws -> String? { fatalError("unused in these tests") }
    func delete(key: String) throws { fatalError("unused in these tests") }
    func update(key: String, value: String) throws { fatalError("unused in these tests") }

    func saveChatGPTTokens(providerId: UUID, bundle: ChatGPTTokenBundle) throws {
        self.bundle = bundle
    }

    func loadChatGPTTokens(providerId: UUID) throws -> ChatGPTTokenBundle? {
        self.bundle
    }

    func deleteChatGPTTokens(providerId: UUID) throws {
        self.bundle = nil
    }
}

// MARK: - Mock URLProtocol

/// URLSession passes every request through this protocol when attached to the
/// session config. We track call count + emit canned responses so the service
/// thinks it's talking to auth.openai.com.
private final class MockURLProtocol: URLProtocol, @unchecked Sendable {
    nonisolated(unsafe) static var requestCount: Int = 0
    nonisolated(unsafe) static var responseDelay: TimeInterval = 0
    nonisolated(unsafe) static var responder: ((URLRequest) -> (HTTPURLResponse, Data))?
    private static let lock = NSLock()

    static func reset() {
        lock.lock(); defer { lock.unlock() }
        requestCount = 0
        responseDelay = 0
        responder = nil
    }

    override class func canInit(with request: URLRequest) -> Bool { true }
    override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }

    override func startLoading() {
        Self.lock.lock()
        Self.requestCount += 1
        let responder = Self.responder
        let delay = Self.responseDelay
        Self.lock.unlock()

        let work: @Sendable () -> Void = { [weak self] in
            guard let self else { return }
            guard let responder else {
                let err = NSError(domain: "MockURLProtocolNoResponder", code: -1)
                self.client?.urlProtocol(self, didFailWithError: err)
                return
            }
            let (response, data) = responder(self.request)
            self.client?.urlProtocol(self, didReceive: response, cacheStoragePolicy: .notAllowed)
            self.client?.urlProtocol(self, didLoad: data)
            self.client?.urlProtocolDidFinishLoading(self)
        }

        if delay > 0 {
            DispatchQueue.global().asyncAfter(deadline: .now() + delay, execute: work)
        } else {
            DispatchQueue.global().async(execute: work)
        }
    }

    override func stopLoading() {}
}
