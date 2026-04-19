// ProviderFactory.swift
// Gridex
//
// Dispatch ProviderConfig → concrete LLMService.
// The OpenAI-compatible family all share `OpenAIProvider` with a preset baseURL,
// so adding a new vendor is usually just an enum case, not a new class.

import Foundation

enum ProviderFactory {
    static func make(config: ProviderConfig, apiKey: String) -> any LLMService {
        let base = config.resolvedBaseURL
        switch config.type {
        case .anthropic:
            return AnthropicProvider(apiKey: apiKey, baseURL: base)
        case .gemini:
            return GeminiProvider(apiKey: apiKey, baseURL: base)
        case .ollama:
            return OllamaProvider(baseURLString: base)
        case .openAI, .azureOpenAI, .groq, .deepseek, .mistral, .xAI,
             .perplexity, .openRouter, .together, .fireworks, .dashscope,
             .dashscopeCoding, .openAICompatible:
            return OpenAIProvider(apiKey: apiKey, baseURL: base, probeModel: config.model)
        }
    }

    /// Convenience for legacy callers that only know the type.
    static func make(type: ProviderType, apiKey: String, baseURL: String? = nil) -> any LLMService {
        let config = ProviderConfig(
            name: type.rawValue,
            type: type,
            apiBase: baseURL,
            model: type.defaultModel
        )
        return make(config: config, apiKey: apiKey)
    }
}
