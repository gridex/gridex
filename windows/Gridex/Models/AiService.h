#pragma once
#include <string>
#include <vector>
#include <functional>
#include <map>

namespace DBModels
{
    // Index order is persisted in settings (aiProviderIndex) and bound to
    // the SettingsPage ComboBox. NEVER reorder existing entries — only
    // append new providers at the end.
    enum class AiProvider
    {
        Anthropic  = 0,
        OpenAI     = 1,
        Ollama     = 2,
        Gemini     = 3,
        OpenRouter = 4,
        ChatGPT    = 5,  // Subscription-based via OAuth (no API key)
    };

    struct AiConfig
    {
        AiProvider provider = AiProvider::Anthropic;
        std::wstring apiKey;
        std::wstring model;          // e.g. "claude-sonnet-4-20250514", "gpt-4o", "llama3", "gemini-2.0-flash"
        std::wstring ollamaEndpoint; // e.g. "http://localhost:11434"
    };

    struct ChatMessage
    {
        std::wstring role;    // "user", "assistant", "system"
        std::wstring content;
    };

    // Result of a FetchModels() call. success=false → errorMessage holds
    // a user-visible reason (network error, bad key, unsupported
    // endpoint); models may still be empty on success for providers
    // without any models configured.
    struct ModelListResult
    {
        bool success = false;
        std::vector<std::wstring> models;
        std::wstring errorMessage;
    };

    // AI service for text-to-SQL and chat interactions
    class AiService
    {
    public:
        void SetConfig(const AiConfig& config) { config_ = config; }
        const AiConfig& GetConfig() const { return config_; }

        // Send chat completion request (blocking)
        // Returns assistant response
        std::wstring SendChat(
            const std::vector<ChatMessage>& messages,
            const std::wstring& systemPrompt = L"");

        // Convenience: text-to-SQL with schema context
        std::wstring TextToSql(
            const std::wstring& naturalLanguage,
            const std::wstring& schemaDescription);

        // List available models for the given provider config. BLOCKS on
        // the provider's HTTP models endpoint — callers on the UI thread
        // must dispatch to a worker. Used by Settings to populate an
        // editable ComboBox so users pick from a live list instead of
        // memorizing model IDs, while still allowing a free-typed name
        // for custom OpenAI-compatible domains.
        static ModelListResult FetchModels(const AiConfig& config);

    private:
        AiConfig config_;

        // Provider-specific HTTP calls
        std::wstring CallAnthropic(
            const std::vector<ChatMessage>& messages,
            const std::wstring& systemPrompt);
        std::wstring CallOpenAI(
            const std::vector<ChatMessage>& messages,
            const std::wstring& systemPrompt);
        std::wstring CallOllama(
            const std::vector<ChatMessage>& messages,
            const std::wstring& systemPrompt);
        std::wstring CallGemini(
            const std::vector<ChatMessage>& messages,
            const std::wstring& systemPrompt);
        std::wstring CallOpenRouter(
            const std::vector<ChatMessage>& messages,
            const std::wstring& systemPrompt);
        std::wstring CallChatGPT(
            const std::vector<ChatMessage>& messages,
            const std::wstring& systemPrompt);

        // UTF-8 helpers
        static std::string toUtf8(const std::wstring& wstr);
        static std::wstring fromUtf8(const std::string& str);
    };
}
