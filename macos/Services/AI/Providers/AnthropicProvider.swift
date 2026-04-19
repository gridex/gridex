// AnthropicProvider.swift
// Gridex
//
// Anthropic Claude API integration with streaming support.

import Foundation

final class AnthropicProvider: LLMService, @unchecked Sendable {
    let providerName = "Anthropic"
    private let apiKey: String
    private let baseURL: String
    private let anthropicVersion = "2023-06-01"

    init(apiKey: String, baseURL: String = "https://api.anthropic.com/v1") {
        self.apiKey = apiKey.trimmingCharacters(in: .whitespacesAndNewlines)
        let trimmedURL = baseURL.trimmingCharacters(in: .whitespacesAndNewlines)
        self.baseURL = trimmedURL.hasSuffix("/") ? String(trimmedURL.dropLast()) : trimmedURL
    }

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
                    var request = URLRequest(url: URL(string: "\(baseURL)/messages")!)
                    request.httpMethod = "POST"
                    request.setValue("application/json", forHTTPHeaderField: "content-type")
                    request.setValue(apiKey, forHTTPHeaderField: "x-api-key")
                    request.setValue(anthropicVersion, forHTTPHeaderField: "anthropic-version")

                    let body: [String: Any] = [
                        "model": model,
                        "max_tokens": maxTokens,
                        "temperature": temperature,
                        "stream": true,
                        "system": systemPrompt,
                        "messages": messages.map { ["role": $0.role.rawValue, "content": $0.content] },
                    ]
                    request.httpBody = try JSONSerialization.data(withJSONObject: body)

                    let (stream, response) = try await URLSession.shared.bytes(for: request)
                    guard let http = response as? HTTPURLResponse, http.statusCode == 200 else {
                        let code = (response as? HTTPURLResponse)?.statusCode ?? 0
                        continuation.finish(throwing: GridexError.aiProviderError("Anthropic HTTP \(code)"))
                        return
                    }

                    for try await line in stream.lines {
                        guard line.hasPrefix("data: ") else { continue }
                        let data = String(line.dropFirst(6))
                        guard data != "[DONE]" else { break }

                        if let json = try? JSONSerialization.jsonObject(with: Data(data.utf8)) as? [String: Any],
                           let delta = (json["delta"] as? [String: Any])?["text"] as? String {
                            continuation.yield(delta)
                        }
                    }
                    continuation.finish()
                } catch {
                    continuation.finish(throwing: error)
                }
            }
        }
    }

    func availableModels() async throws -> [LLMModel] {
        // Use Anthropic's official models endpoint. Auth is x-api-key (NOT Bearer).
        // Response: {"data":[{"id":"...","display_name":"..."}]}
        var request = URLRequest(url: URL(string: "\(baseURL)/models")!)
        request.setValue(apiKey, forHTTPHeaderField: "x-api-key")
        request.setValue(anthropicVersion, forHTTPHeaderField: "anthropic-version")

        let (data, http) = try await LLMRetry.perform(request)
        if http.statusCode == 401 || http.statusCode == 403 {
            throw GridexError.aiProviderError("HTTP \(http.statusCode): check API key")
        }
        guard http.statusCode == 200 else {
            throw GridexError.aiProviderError("HTTP \(http.statusCode)")
        }

        guard let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let arr = json["data"] as? [[String: Any]] else {
            return []
        }
        return arr.compactMap { m -> LLMModel? in
            guard let id = m["id"] as? String else { return nil }
            let display = (m["display_name"] as? String) ?? id
            return LLMModel(id: id, name: display, provider: providerName, contextWindow: 200000, supportsStreaming: true)
        }
    }

    func validateAPIKey() async throws -> Bool {
        // Minimal, cheap probe: 1-token message. 200 → valid, 401/403 → invalid.
        var request = URLRequest(url: URL(string: "\(baseURL)/messages")!)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "content-type")
        request.setValue(apiKey, forHTTPHeaderField: "x-api-key")
        request.setValue(anthropicVersion, forHTTPHeaderField: "anthropic-version")
        let body: [String: Any] = [
            "model": "claude-haiku-4-5-20251001",
            "max_tokens": 1,
            "messages": [["role": "user", "content": "."]],
        ]
        request.httpBody = try JSONSerialization.data(withJSONObject: body)

        let (_, http) = try await LLMRetry.perform(request, policy: .none)
        return http.statusCode == 200
    }
}
