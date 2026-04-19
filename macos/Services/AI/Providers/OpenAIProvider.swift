// OpenAIProvider.swift
// Gridex
//
// OpenAI GPT API integration with streaming support.
// Works for any OpenAI-compatible endpoint (set baseURL accordingly).

import Foundation

final class OpenAIProvider: LLMService, @unchecked Sendable {
    let providerName = "OpenAI"
    private let apiKey: String
    private let baseURL: String
    /// Used only for the `validateAPIKey()` probe — real requests receive the model
    /// from the caller (`stream(model:)`).
    private let probeModel: String

    init(apiKey: String, baseURL: String = "https://api.openai.com/v1", probeModel: String = "gpt-4o-mini") {
        self.apiKey = apiKey.trimmingCharacters(in: .whitespacesAndNewlines)
        let trimmedURL = baseURL.trimmingCharacters(in: .whitespacesAndNewlines)
        self.baseURL = trimmedURL.hasSuffix("/") ? String(trimmedURL.dropLast()) : trimmedURL
        self.probeModel = probeModel
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
                    var request = URLRequest(url: URL(string: "\(baseURL)/chat/completions")!)
                    request.httpMethod = "POST"
                    request.setValue("application/json", forHTTPHeaderField: "content-type")
                    request.setValue("Bearer \(apiKey)", forHTTPHeaderField: "authorization")

                    var msgs: [[String: Any]] = [["role": "system", "content": systemPrompt]]
                    msgs += messages.map { ["role": $0.role.rawValue, "content": $0.content] }

                    let body: [String: Any] = [
                        "model": model,
                        "max_tokens": maxTokens,
                        "temperature": temperature,
                        "stream": true,
                        "messages": msgs,
                    ]
                    request.httpBody = try JSONSerialization.data(withJSONObject: body)

                    let (stream, response) = try await URLSession.shared.bytes(for: request)
                    guard let http = response as? HTTPURLResponse, http.statusCode == 200 else {
                        let code = (response as? HTTPURLResponse)?.statusCode ?? 0
                        continuation.finish(throwing: GridexError.aiProviderError("OpenAI HTTP \(code)"))
                        return
                    }

                    for try await line in stream.lines {
                        guard line.hasPrefix("data: ") else { continue }
                        let data = String(line.dropFirst(6))
                        guard data != "[DONE]" else { break }

                        if let json = try? JSONSerialization.jsonObject(with: Data(data.utf8)) as? [String: Any],
                           let choices = json["choices"] as? [[String: Any]],
                           let delta = choices.first?["delta"] as? [String: Any],
                           let content = delta["content"] as? String {
                            continuation.yield(content)
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
        var request = URLRequest(url: URL(string: "\(baseURL)/models")!)
        request.setValue("Bearer \(apiKey)", forHTTPHeaderField: "authorization")

        let (data, http) = try await LLMRetry.perform(request)

        // Surface auth/endpoint problems as real errors instead of silently
        // returning a fake list — the settings UI needs truthful feedback.
        if http.statusCode == 401 || http.statusCode == 403 {
            throw GridexError.aiProviderError("HTTP \(http.statusCode): check API key")
        }
        if http.statusCode == 404 {
            throw GridexError.aiProviderError("HTTP 404: endpoint doesn't support /models listing")
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
            return LLMModel(id: id, name: id, provider: providerName, contextWindow: 128000, supportsStreaming: true)
        }
    }

    func validateAPIKey() async throws -> Bool {
        // Probe /chat/completions with a 1-token request. `/models` is not universally
        // supported (DashScope, Azure, some OpenRouter routes return 404 / different
        // auth), but `/chat/completions` is the common denominator for OpenAI-compat
        // providers. 401 → bad key, 200 → good key, other → surface an error message.
        var request = URLRequest(url: URL(string: "\(baseURL)/chat/completions")!)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "content-type")
        request.setValue("Bearer \(apiKey)", forHTTPHeaderField: "authorization")
        let body: [String: Any] = [
            "model": probeModel,
            "max_tokens": 1,
            "messages": [["role": "user", "content": "."]],
        ]
        request.httpBody = try JSONSerialization.data(withJSONObject: body)

        let (data, http) = try await LLMRetry.perform(request, policy: .none)
        if http.statusCode == 200 { return true }
        // Extract error.message from the body so the UI can surface something useful.
        if let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
           let err = json["error"] as? [String: Any],
           let msg = err["message"] as? String {
            throw GridexError.aiProviderError("HTTP \(http.statusCode): \(msg)")
        }
        throw GridexError.aiProviderError("HTTP \(http.statusCode)")
    }
}
