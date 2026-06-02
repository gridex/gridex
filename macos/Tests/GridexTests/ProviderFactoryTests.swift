// ProviderFactoryTests.swift
//
// Pin the dispatch behaviour of ProviderFactory.make so a future contributor
// can't silently break the .chatGPT case (e.g. by flipping a switch label,
// or by reaching for the legacy `make(type:apiKey:baseURL:)` overload that
// can't thread an OAuth service through).

import XCTest
@testable import Gridex

final class ProviderFactoryTests: XCTestCase {

    // MARK: - .chatGPT dispatch

    func test_make_returnsChatGPTProvider_whenOAuthServiceProvided() {
        let oauth = ChatGPTOAuthService(
            keychainService: KeychainService(),
            urlSession: .shared
        )
        let config = ProviderConfig(name: "ChatGPT", type: .chatGPT)

        let service = ProviderFactory.make(
            config: config,
            apiKey: "",
            chatGPTOAuthService: oauth
        )

        XCTAssertTrue(service is ChatGPTProvider,
            "with OAuth service supplied, factory must return a real ChatGPTProvider")
    }

    func test_make_returnsMisconfiguredStub_whenOAuthServiceMissing() {
        // Reachable from `make(type:apiKey:baseURL:)` (legacy / DependencyContainer
        // path) which never threads chatGPTOAuthService through.
        let config = ProviderConfig(name: "ChatGPT", type: .chatGPT)
        let service = ProviderFactory.make(config: config, apiKey: "")

        // Stub is private; verify by behaviour: every call must throw GridexError.
        // (Pre-fix this code-path was preconditionFailure → process crash.)
        let stream = service.stream(
            messages: [],
            systemPrompt: "",
            model: "gpt-5-codex",
            maxTokens: 100,
            temperature: 1.0
        )

        let exp = expectation(description: "stream throws aiProviderError")
        Task {
            do {
                for try await _ in stream {}
                XCTFail("misconfigured stub must not yield values")
            } catch let GridexError.aiProviderError(msg) {
                XCTAssertTrue(msg.contains("misconfigured"), "got: \(msg)")
                exp.fulfill()
            } catch {
                XCTFail("expected GridexError.aiProviderError, got \(error)")
            }
        }
        wait(for: [exp], timeout: 1.0)
    }

    func test_make_misconfiguredStub_availableModelsThrows() async {
        let config = ProviderConfig(name: "ChatGPT", type: .chatGPT)
        let service = ProviderFactory.make(config: config, apiKey: "")

        do {
            _ = try await service.availableModels()
            XCTFail("availableModels must throw when ChatGPT factory is misconfigured")
        } catch let GridexError.aiProviderError(msg) {
            XCTAssertTrue(msg.contains("misconfigured"))
        } catch {
            XCTFail("expected GridexError.aiProviderError, got \(error)")
        }
    }

    // MARK: - Other providers route correctly

    func test_make_anthropic_returnsAnthropicProvider() {
        let config = ProviderConfig(name: "Anthropic", type: .anthropic)
        let service = ProviderFactory.make(config: config, apiKey: "sk-test")
        XCTAssertTrue(service is AnthropicProvider)
    }

    func test_make_openAI_returnsOpenAIProvider() {
        let config = ProviderConfig(name: "OpenAI", type: .openAI)
        let service = ProviderFactory.make(config: config, apiKey: "sk-test")
        XCTAssertTrue(service is OpenAIProvider)
    }

    // MARK: - Default model for .chatGPT

    func test_defaultModel_chatGPT_isNonEmpty() {
        // The authoritative slug list comes from `/backend-api/codex/models`
        // after sign-in. The default just needs to be non-empty so the form
        // has something to send before the live picker resolves.
        XCTAssertFalse(ProviderType.chatGPT.defaultModel.isEmpty)
    }
}
