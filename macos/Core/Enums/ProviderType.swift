// ProviderType.swift
// Gridex

import Foundation

/// Supported LLM providers. Most entries dispatch to `OpenAIProvider` with a
/// preset `baseURL` — they all speak the OpenAI `/chat/completions` SSE shape.
enum ProviderType: String, Codable, Sendable, CaseIterable, Identifiable {
    // Native formats (own provider class)
    case anthropic
    case gemini
    case ollama

    // Subscription-based access via OAuth (own provider class)
    case chatGPT          = "chatgpt"

    // OpenAI-compatible family (all use OpenAIProvider)
    case openAI           = "openai"
    case azureOpenAI      = "azure-openai"
    case groq
    case deepseek
    case mistral
    case xAI              = "xai"
    case perplexity
    case openRouter       = "openrouter"
    case together
    case fireworks
    case dashscope
    case dashscopeCoding  = "dashscope-coding"
    case openAICompatible = "openai-compatible"

    var id: String { rawValue }

    // MARK: - Metadata

    var displayName: String {
        switch self {
        case .anthropic:         return "Anthropic (Claude)"
        case .gemini:            return "Google Gemini"
        case .ollama:            return "Ollama (Local)"
        case .chatGPT:           return "OpenAI (Sign in)"
        case .openAI:            return "OpenAI"
        case .azureOpenAI:       return "Azure OpenAI"
        case .groq:              return "Groq"
        case .deepseek:          return "DeepSeek"
        case .mistral:           return "Mistral"
        case .xAI:               return "xAI (Grok)"
        case .perplexity:        return "Perplexity"
        case .openRouter:        return "OpenRouter"
        case .together:          return "Together AI"
        case .fireworks:         return "Fireworks AI"
        case .dashscope:         return "DashScope (Qwen)"
        case .dashscopeCoding:   return "DashScope Bailian (Coding)"
        case .openAICompatible:  return "OpenAI-Compatible (Custom)"
        }
    }

    /// Group used in the type picker UI.
    var family: Family {
        switch self {
        case .anthropic: return .anthropic
        case .gemini:    return .gemini
        case .ollama:    return .local
        case .chatGPT:   return .subscription
        case .openAI, .azureOpenAI, .groq, .deepseek, .mistral, .xAI,
             .perplexity, .openRouter, .together, .fireworks, .dashscope,
             .dashscopeCoding, .openAICompatible:
            return .openAICompat
        }
    }

    enum Family: String, CaseIterable {
        case anthropic     = "Anthropic"
        case gemini        = "Google"
        case openAICompat  = "OpenAI-Compatible"
        case subscription  = "Subscription / OAuth"
        case local         = "Local"
    }

    var defaultBaseURL: String {
        switch self {
        case .anthropic:         return "https://api.anthropic.com/v1"
        case .gemini:            return "https://generativelanguage.googleapis.com/v1beta/openai"
        case .ollama:            return "http://localhost:11434"
        case .chatGPT:           return "https://chatgpt.com/backend-api/codex"
        case .openAI:            return "https://api.openai.com/v1"
        case .azureOpenAI:       return "https://YOUR-RESOURCE.openai.azure.com/openai"
        case .groq:              return "https://api.groq.com/openai/v1"
        case .deepseek:          return "https://api.deepseek.com/v1"
        case .mistral:           return "https://api.mistral.ai/v1"
        case .xAI:               return "https://api.x.ai/v1"
        case .perplexity:        return "https://api.perplexity.ai"
        case .openRouter:        return "https://openrouter.ai/api/v1"
        case .together:          return "https://api.together.xyz/v1"
        case .fireworks:         return "https://api.fireworks.ai/inference/v1"
        case .dashscope:         return "https://dashscope.aliyuncs.com/compatible-mode/v1"
        case .dashscopeCoding:   return "https://coding-intl.dashscope.aliyuncs.com/v1"
        case .openAICompatible:  return ""
        }
    }

    var defaultModel: String {
        switch self {
        case .anthropic:         return "claude-sonnet-4-6"
        case .gemini:            return "gemini-2.5-flash"
        case .ollama:            return "llama3"
        case .chatGPT:           return "gpt-5.4"
        case .openAI:            return "gpt-4o"
        case .azureOpenAI:       return "gpt-4o"
        case .groq:              return "llama-3.3-70b-versatile"
        case .deepseek:          return "deepseek-chat"
        case .mistral:           return "mistral-large-latest"
        case .xAI:               return "grok-2-latest"
        case .perplexity:        return "llama-3.1-sonar-large-128k-online"
        case .openRouter:        return "anthropic/claude-sonnet-4"
        case .together:          return "meta-llama/Llama-3.3-70B-Instruct-Turbo"
        case .fireworks:         return "accounts/fireworks/models/llama-v3p3-70b-instruct"
        case .dashscope:         return "qwen-plus"
        case .dashscopeCoding:   return "qwen3-coder-plus"
        case .openAICompatible:  return ""
        }
    }

    var requiresAPIKey: Bool {
        // .chatGPT uses OAuth tokens (Keychain blob), not a long-lived API key.
        // .ollama is local and unauthenticated.
        self != .ollama && self != .chatGPT
    }

    var allowsPrivateHosts: Bool {
        self == .ollama || self == .openAICompatible
    }

    /// True when the provider has no live `/models` listing endpoint — the UI should
    /// use the built-in catalog (`fallbackModelIDs`) directly instead of a network call.
    /// Mirrors goclaw's hardcoded-catalog providers (DashScope, Bailian, MiniMax).
    var hasHardcodedCatalog: Bool {
        switch self {
        case .dashscope, .dashscopeCoding: return true
        default: return false
        }
    }

    var iconName: String {
        switch self {
        case .anthropic:                     return "a.circle.fill"
        case .gemini:                        return "g.circle.fill"
        case .ollama:                        return "desktopcomputer"
        case .chatGPT:                       return "person.crop.circle.badge.checkmark"
        case .openAI:                        return "bubble.left.and.text.bubble.right.fill"
        case .azureOpenAI:                   return "cloud.fill"
        case .groq:                          return "bolt.circle.fill"
        case .deepseek:                      return "magnifyingglass.circle.fill"
        case .mistral:                       return "wind"
        case .xAI:                           return "x.circle.fill"
        case .perplexity:                    return "questionmark.circle.fill"
        case .openRouter:                    return "arrow.triangle.branch"
        case .together:                      return "person.2.fill"
        case .fireworks:                     return "sparkles"
        case .dashscope:                     return "cpu.fill"
        case .dashscopeCoding:               return "chevron.left.forwardslash.chevron.right"
        case .openAICompatible:              return "slider.horizontal.3"
        }
    }

    var fallbackModelIDs: [String] {
        switch self {
        case .anthropic:         return ["claude-sonnet-4-6", "claude-haiku-4-5-20251001", "claude-opus-4-6"]
        case .gemini:            return ["gemini-2.5-flash", "gemini-2.5-pro", "gemini-2.0-flash"]
        case .ollama:            return ["llama3", "codellama", "mistral"]
        // ChatGPT model slugs are owned by the Responses API and the only
        // authoritative list comes from `/backend-api/codex/models`. Hard-coding
        // names here would invent IDs that don't exist server-side; if the live
        // fetch fails the user must type a slug manually.
        case .chatGPT:           return []
        case .openAI:            return ["gpt-4o", "gpt-4o-mini", "o1", "o1-mini"]
        case .azureOpenAI:       return ["gpt-4o", "gpt-4o-mini"]
        case .groq:              return ["llama-3.3-70b-versatile", "llama-3.1-8b-instant", "mixtral-8x7b-32768"]
        case .deepseek:          return ["deepseek-chat", "deepseek-reasoner"]
        case .mistral:           return ["mistral-large-latest", "mistral-small-latest", "codestral-latest"]
        case .xAI:               return ["grok-2-latest", "grok-2-vision-latest"]
        case .perplexity:        return ["llama-3.1-sonar-large-128k-online", "llama-3.1-sonar-small-128k-online"]
        case .openRouter:        return ["anthropic/claude-sonnet-4", "openai/gpt-4o", "meta-llama/llama-3.3-70b"]
        case .together:          return ["meta-llama/Llama-3.3-70B-Instruct-Turbo", "mistralai/Mixtral-8x7B-Instruct-v0.1"]
        case .fireworks:         return ["accounts/fireworks/models/llama-v3p3-70b-instruct"]
        case .dashscope:
            // DashScope doesn't expose /v1/models — catalog mirrors goclaw's dashScopeModels()
            return [
                "qwen3.6-plus",
                "qwen3.5-plus",
                "qwen3.5-flash",
                "qwen3.5-turbo",
                "qwen3-max",
                "qwen3-plus",
                "qwen3-turbo",
                "wan2.6-image",
                "wan2.1-image",
                "wan2.6-video",
            ]
        case .dashscopeCoding:
            // Bailian Coding catalog — mirrors goclaw's bailianModels()
            return [
                "qwen3.6-plus",
                "qwen3.5-plus",
                "kimi-k2.5",
                "GLM-5",
                "MiniMax-M2.5",
                "qwen3-max-2026-01-23",
                "qwen3-coder-next",
                "qwen3-coder-plus",
                "glm-4.7",
            ]
        case .openAICompatible:  return []
        }
    }
}
