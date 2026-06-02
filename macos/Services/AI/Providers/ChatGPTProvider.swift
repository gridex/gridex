// ChatGPTProvider.swift
// Gridex
//
// LLMService implementation backed by the ChatGPT subscription via the
// Codex CLI OAuth flow. Talks to `chatgpt.com/backend-api/codex/responses`
// (the Responses API), NOT the standard `api.openai.com/v1/chat/completions`.
//
// Token refresh happens lazily inside each request — see
// `ChatGPTOAuthService.tokenBundle(...)`. The provider holds a reference to
// the OAuth service rather than caching a token, so a refresh in one call
// is visible to the next.

import Foundation
import os

final class ChatGPTProvider: LLMService, Sendable {

    private static let log = Logger(subsystem: "com.gridex.gridex", category: "ChatGPTProvider")

    let providerName = "ChatGPT"

    private let providerId: UUID
    private let baseURL: String
    private let oauthService: ChatGPTOAuthService
    private let urlSession: URLSession

    init(
        providerId: UUID,
        baseURL: String = ChatGPTOAuthConstants.backend.absoluteString,
        oauthService: ChatGPTOAuthService,
        urlSession: URLSession = .shared
    ) {
        self.providerId = providerId
        let trimmed = baseURL.trimmingCharacters(in: .whitespacesAndNewlines)
        self.baseURL = trimmed.hasSuffix("/") ? String(trimmed.dropLast()) : trimmed
        self.oauthService = oauthService
        self.urlSession = urlSession
    }

    // MARK: - LLMService

    func stream(
        messages: [LLMMessage],
        systemPrompt: String,
        model: String,
        maxTokens: Int,
        temperature: Double
    ) -> AsyncThrowingStream<String, Error> {
        AsyncThrowingStream { continuation in
            Task {
                do {
                    let bundle = try await self.oauthService.tokenBundle(providerId: self.providerId)
                    let request = try self.buildResponsesRequest(
                        bundle: bundle,
                        messages: messages,
                        systemPrompt: systemPrompt,
                        model: model,
                        maxTokens: maxTokens,
                        temperature: temperature
                    )

                    let (byteStream, response) = try await self.urlSession.bytes(for: request)
                    guard let http = response as? HTTPURLResponse else {
                        continuation.finish(throwing: GridexError.aiStreamingError("Non-HTTP response"))
                        return
                    }
                    if http.statusCode == 401 || http.statusCode == 403 {
                        await self.clearRejectedToken(endpoint: "/responses", statusCode: http.statusCode)
                        continuation.finish(throwing: GridexError.aiAPIKeyMissing)
                        return
                    }
                    guard http.statusCode == 200 else {
                        // Drain a bounded chunk of the body for diagnostics.
                        var bodyBytes: [UInt8] = []
                        for try await byte in byteStream {
                            bodyBytes.append(byte)
                            if bodyBytes.count > 4096 { break }
                        }
                        let bodyData = Data(bodyBytes)
                        let bodyText = String(data: bodyData, encoding: .utf8) ?? ""
                        Self.log.error("/responses HTTP \(http.statusCode) body: \(bodyText, privacy: .public)")
                        let parsed = Self.extractErrorMessage(from: bodyData)
                        let snippet = bodyText.prefix(300)
                        let msg = parsed ?? "HTTP \(http.statusCode): \(snippet)"
                        continuation.finish(throwing: GridexError.aiProviderError(msg))
                        return
                    }

                    try await self.parseSSE(stream: byteStream, into: continuation)
                    continuation.finish()
                } catch {
                    continuation.finish(throwing: error)
                }
            }
        }
    }

    func availableModels() async throws -> [LLMModel] {
        let bundle = try await oauthService.tokenBundle(providerId: providerId)
        var components = URLComponents(string: "\(baseURL)/models")!
        components.queryItems = [URLQueryItem(name: "client_version", value: "1.0.0")]
        var request = URLRequest(url: components.url!)
        request.setValue("Bearer \(bundle.accessToken)", forHTTPHeaderField: "Authorization")
        if let accountId = bundle.accountId {
            request.setValue(accountId, forHTTPHeaderField: "ChatGPT-Account-ID")
        }

        let (data, http) = try await LLMRetry.perform(request, session: urlSession)

        if http.statusCode == 401 || http.statusCode == 403 {
            await clearRejectedToken(endpoint: "/models", statusCode: http.statusCode)
            throw GridexError.aiAPIKeyMissing
        }
        guard http.statusCode == 200 else {
            throw GridexError.aiProviderError("HTTP \(http.statusCode) on /models")
        }

        guard let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            throw GridexError.aiProviderError("Unexpected /models response shape")
        }
        guard let arr = json["models"] as? [[String: Any]] else {
            throw GridexError.aiProviderError("/models response missing 'models' array")
        }

        let visible = arr.compactMap { m -> LLMModel? in
            guard let slug = m["slug"] as? String else { return nil }
            // Filter to user-listable models — the API returns hidden/deprecated
            // entries that are not actually usable through `/responses`.
            let supported = (m["supported_in_api"] as? Bool) ?? true
            let visibility = m["visibility"] as? String ?? "list"
            guard supported, visibility == "list" else { return nil }
            return LLMModel(
                id: slug,
                name: (m["name"] as? String) ?? slug,
                provider: providerName,
                contextWindow: (m["context_window"] as? Int) ?? 128_000,
                supportsStreaming: true
            )
        }
        return visible
    }

    /// Re-uses `availableModels()` as a cheap probe — a 200 there proves the
    /// access_token is currently good. ProviderEditSheet hides the Test button
    /// for ChatGPT, so this method is mostly here for protocol conformance + tests.
    func validateAPIKey() async throws -> Bool {
        _ = try await availableModels()
        return true
    }

    // MARK: - Internal — request building

    private func buildResponsesRequest(
        bundle: ChatGPTTokenBundle,
        messages: [LLMMessage],
        systemPrompt: String,
        model: String,
        maxTokens: Int,
        temperature: Double
    ) throws -> URLRequest {
        guard let url = URL(string: "\(baseURL)/responses") else {
            throw GridexError.aiProviderError("Invalid baseURL: \(baseURL)")
        }
        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.setValue("Bearer \(bundle.accessToken)", forHTTPHeaderField: "Authorization")
        if let accountId = bundle.accountId {
            request.setValue(accountId, forHTTPHeaderField: "ChatGPT-Account-ID")
        }
        // Codex CLI sends this; harmless if the server ignores it.
        request.setValue("responses=v1", forHTTPHeaderField: "OpenAI-Beta")

        // System role → top-level `instructions`. Other roles flow into `input`.
        // Wire format mirrors Codex CLI's actual /responses traffic:
        //   - each input item is a typed `message` object
        //   - content is an array of {type, text} parts (input_text / output_text)
        //   - we DO NOT send temperature or max_output_tokens — GPT-5 family
        //     rejects custom temperature, and Codex relies on server defaults
        //     for output length. Sending either field returns HTTP 400.
        let inputMessages: [[String: Any]] = messages
            .filter { $0.role != .system }
            .map { msg in
                let partType = (msg.role == .assistant) ? "output_text" : "input_text"
                return [
                    "type": "message",
                    "role": msg.role.rawValue,
                    "content": [
                        ["type": partType, "text": msg.content],
                    ],
                ]
            }

        let body: [String: Any] = [
            "model":        model,
            "instructions": systemPrompt,
            "input":        inputMessages,
            "stream":       true,
            "store":        false,
        ]
        request.httpBody = try JSONSerialization.data(withJSONObject: body)
        // Body contains the user's prompt + system instructions. Logging it
        // verbatim — even at .debug — leaks chat content to Console.app on
        // dev builds. Only emit size/message count and non-content tuning params.
        Self.log.debug("temperature=\(temperature) maxTokens=\(maxTokens) ignored for ChatGPT backend")
        Self.log.debug("/responses request: \(request.httpBody?.count ?? 0) bytes, \(inputMessages.count) message(s)")
        return request
    }

    // MARK: - Internal — SSE parsing

    /// Streams `URLSession.AsyncBytes` line-by-line and yields text deltas to
    /// the continuation. Recognises three event kinds:
    ///   - response.output_text.delta → yield `delta` field
    ///   - response.completed         → finish stream
    ///   - response.error             → finish with thrown error
    /// Other events are ignored.
    private func parseSSE(
        stream: URLSession.AsyncBytes,
        into continuation: AsyncThrowingStream<String, Error>.Continuation
    ) async throws {
        var currentEvent: String? = nil

        for try await line in stream.lines {
            if line.isEmpty {
                currentEvent = nil
                continue
            }
            if line.hasPrefix("event: ") {
                currentEvent = String(line.dropFirst("event: ".count))
                continue
            }
            if line.hasPrefix(":") {
                // SSE comment / keep-alive — ignore.
                continue
            }
            guard line.hasPrefix("data: ") else { continue }
            let payload = String(line.dropFirst("data: ".count))
            if payload == "[DONE]" { break }   // not used by Responses API but harmless

            guard let data = payload.data(using: .utf8) else { continue }

            switch currentEvent {
            case "response.output_text.delta":
                if let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                   let delta = json["delta"] as? String {
                    continuation.yield(delta)
                }
            case "response.error":
                throw GridexError.aiStreamingError(Self.extractErrorMessage(from: data) ?? payload)
            case "response.completed":
                return
            default:
                Self.log.debug("Ignoring unrecognized SSE event: \(currentEvent ?? "(none)", privacy: .public)")
                continue
            }
        }
    }

    private func clearRejectedToken(endpoint: String, statusCode: Int) async {
        Self.log.notice("\(endpoint, privacy: .public) returned \(statusCode); clearing ChatGPT token bundle")
        try? await oauthService.signOut(providerId: providerId)
    }

    /// Pull a human-readable error message out of a Responses-API JSON body,
    /// in either nested (`{"error":{"message":...}}`) or flat (`{"message":...}`)
    /// form. Returns nil when the body isn't JSON or has neither shape.
    private static func extractErrorMessage(from data: Data) -> String? {
        guard let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return nil }
        if let err = json["error"] as? [String: Any], let m = err["message"] as? String { return m }
        return json["message"] as? String
    }
}
