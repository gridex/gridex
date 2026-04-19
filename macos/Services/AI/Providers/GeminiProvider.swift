// GeminiProvider.swift
// Gridex
//
// Google Gemini API integration via OpenAI-compatible endpoint.

import Foundation

final class GeminiProvider: LLMService, @unchecked Sendable {
    let providerName = "Gemini"
    private let apiKey: String
    private let baseURL: String

    init(apiKey: String, baseURL: String = "https://generativelanguage.googleapis.com/v1beta/openai") {
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
                    var request = URLRequest(url: URL(string: "\(baseURL)/chat/completions")!)
                    request.httpMethod = "POST"
                    request.setValue("application/json", forHTTPHeaderField: "content-type")
                    request.setValue("Bearer \(apiKey)", forHTTPHeaderField: "authorization")

                    var msgs: [[String: Any]] = [
                        ["role": "system", "content": systemPrompt]
                    ]
                    msgs += messages.map { ["role": $0.role.rawValue, "content": $0.content] }

                    let body: [String: Any] = [
                        "model": model,
                        "max_tokens": maxTokens,
                        "temperature": temperature,
                        "stream": true,
                        "messages": msgs
                    ]
                    request.httpBody = try JSONSerialization.data(withJSONObject: body)

                    let (stream, response) = try await URLSession.shared.bytes(for: request)

                    guard let httpResponse = response as? HTTPURLResponse, httpResponse.statusCode == 200 else {
                        let statusCode = (response as? HTTPURLResponse)?.statusCode ?? 0
                        continuation.finish(throwing: GridexError.aiProviderError("Gemini HTTP \(statusCode)"))
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
        // Use Google's native models endpoint — it gives the full list with display
        // names. The OpenAI-compat shim at /v1beta/openai/models is limited to a
        // subset. Auth is via ?key= query param (NOT Bearer).
        let endpoint = "https://generativelanguage.googleapis.com/v1beta/models?key=\(apiKey)"
        guard let url = URL(string: endpoint) else {
            throw GridexError.aiProviderError("Invalid Gemini URL")
        }
        let (data, http) = try await LLMRetry.perform(URLRequest(url: url))
        if http.statusCode == 401 || http.statusCode == 403 {
            throw GridexError.aiProviderError("HTTP \(http.statusCode): check API key")
        }
        guard http.statusCode == 200 else {
            throw GridexError.aiProviderError("HTTP \(http.statusCode)")
        }

        guard let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let arr = json["models"] as? [[String: Any]] else {
            return []
        }
        return arr.compactMap { m -> LLMModel? in
            // name = "models/gemini-2.5-flash" — strip the prefix for the usable ID
            guard let rawName = m["name"] as? String else { return nil }
            let id = rawName.hasPrefix("models/") ? String(rawName.dropFirst(7)) : rawName
            let display = (m["displayName"] as? String) ?? id
            return LLMModel(id: id, name: display, provider: providerName, contextWindow: 1000000, supportsStreaming: true)
        }
    }

    func validateAPIKey() async throws -> Bool {
        var request = URLRequest(url: URL(string: "\(baseURL)/models")!)
        request.setValue("Bearer \(apiKey)", forHTTPHeaderField: "authorization")
        let (_, response) = try await URLSession.shared.data(for: request)
        return (response as? HTTPURLResponse)?.statusCode == 200
    }
}
