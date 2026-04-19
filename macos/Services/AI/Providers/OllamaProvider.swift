// OllamaProvider.swift
// Gridex
//
// Local Ollama LLM integration. Uses the native /api/chat (NDJSON streaming)
// and /api/tags endpoints — not the OpenAI-compat shim — so no API key needed.

import Foundation

final class OllamaProvider: LLMService, @unchecked Sendable {
    let providerName = "Ollama"
    private let baseURL: URL

    init(baseURL: URL = URL(string: "http://localhost:11434")!) {
        // Ollama's native API lives at the root — strip any /v1 suffix users might
        // add when thinking "OpenAI-compatible".
        var s = baseURL.absoluteString
        if s.hasSuffix("/v1") { s.removeLast(3) }
        if s.hasSuffix("/")    { s.removeLast()  }
        self.baseURL = URL(string: s) ?? baseURL
    }

    convenience init(baseURLString: String) {
        self.init(baseURL: URL(string: baseURLString) ?? URL(string: "http://localhost:11434")!)
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
                    var request = URLRequest(url: baseURL.appendingPathComponent("/api/chat"))
                    request.httpMethod = "POST"
                    request.setValue("application/json", forHTTPHeaderField: "content-type")

                    var msgs: [[String: Any]] = [["role": "system", "content": systemPrompt]]
                    msgs += messages.map { ["role": $0.role.rawValue, "content": $0.content] }

                    let body: [String: Any] = [
                        "model": model,
                        "messages": msgs,
                        "stream": true,
                        "options": [
                            "num_predict": maxTokens,
                            "temperature": temperature,
                        ],
                    ]
                    request.httpBody = try JSONSerialization.data(withJSONObject: body)

                    let (stream, response) = try await URLSession.shared.bytes(for: request)
                    guard let http = response as? HTTPURLResponse, http.statusCode == 200 else {
                        let code = (response as? HTTPURLResponse)?.statusCode ?? 0
                        continuation.finish(throwing: GridexError.aiProviderError("Ollama HTTP \(code)"))
                        return
                    }

                    // NDJSON: one JSON object per line
                    for try await line in stream.lines {
                        guard !line.isEmpty,
                              let data = line.data(using: .utf8),
                              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
                            continue
                        }
                        if let msg = obj["message"] as? [String: Any],
                           let content = msg["content"] as? String,
                           !content.isEmpty {
                            continuation.yield(content)
                        }
                        if obj["done"] as? Bool == true { break }
                    }
                    continuation.finish()
                } catch {
                    continuation.finish(throwing: error)
                }
            }
        }
    }

    func availableModels() async throws -> [LLMModel] {
        var request = URLRequest(url: baseURL.appendingPathComponent("/api/tags"))
        request.timeoutInterval = 5
        let (data, http) = try await LLMRetry.perform(request, policy: .none)
        guard http.statusCode == 200,
              let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let arr = json["models"] as? [[String: Any]] else {
            return []
        }

        return arr.compactMap { m -> LLMModel? in
            guard let id = m["name"] as? String else { return nil }
            // Build a richer display name from details: "family paramSize quantLevel"
            // e.g. "llama 8.0B Q4_K_M". Mirrors goclaw's approach.
            let details = m["details"] as? [String: Any]
            var parts: [String] = []
            if let v = details?["family"] as? String, !v.isEmpty { parts.append(v) }
            if let v = details?["parameter_size"] as? String, !v.isEmpty { parts.append(v) }
            if let v = details?["quantization_level"] as? String, !v.isEmpty { parts.append(v) }
            let name = parts.isEmpty ? id : parts.joined(separator: " ")
            let ctx = details?["context_length"] as? Int ?? 8192
            return LLMModel(id: id, name: name, provider: providerName, contextWindow: ctx, supportsStreaming: true)
        }
    }

    func validateAPIKey() async throws -> Bool {
        var request = URLRequest(url: baseURL.appendingPathComponent("/api/tags"))
        request.timeoutInterval = 5
        do {
            let (_, http) = try await LLMRetry.perform(request, policy: .none)
            return http.statusCode == 200
        } catch {
            return false
        }
    }
}
