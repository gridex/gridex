#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include "Models/AiService.h"
#include <nlohmann/json.hpp>
#include <algorithm>

// cpp-httplib for HTTP client — suppress deprecated SSL API warnings
#pragma warning(push)
#pragma warning(disable: 4996)
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>
#pragma warning(pop)

namespace DBModels
{
    // Trim leading/trailing whitespace from a wide string
    static std::wstring trimWs(const std::wstring& s)
    {
        size_t start = s.find_first_not_of(L" \t\r\n");
        if (start == std::wstring::npos) return {};
        size_t end = s.find_last_not_of(L" \t\r\n");
        return s.substr(start, end - start + 1);
    }

    std::string AiService::toUtf8(const std::wstring& wstr)
    {
        if (wstr.empty()) return {};
        int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(),
            static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
        std::string result(size, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(),
            static_cast<int>(wstr.size()), &result[0], size, nullptr, nullptr);
        return result;
    }

    std::wstring AiService::fromUtf8(const std::string& str)
    {
        if (str.empty()) return {};
        int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(),
            static_cast<int>(str.size()), nullptr, 0);
        std::wstring result(size, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, str.c_str(),
            static_cast<int>(str.size()), &result[0], size);
        return result;
    }

    std::wstring AiService::SendChat(
        const std::vector<ChatMessage>& messages,
        const std::wstring& systemPrompt)
    {
        switch (config_.provider)
        {
        case AiProvider::Anthropic:  return CallAnthropic(messages, systemPrompt);
        case AiProvider::OpenAI:     return CallOpenAI(messages, systemPrompt);
        case AiProvider::Ollama:     return CallOllama(messages, systemPrompt);
        case AiProvider::Gemini:     return CallGemini(messages, systemPrompt);
        case AiProvider::OpenRouter: return CallOpenRouter(messages, systemPrompt);
        default: return L"Unsupported AI provider";
        }
    }

    std::wstring AiService::TextToSql(
        const std::wstring& naturalLanguage,
        const std::wstring& schemaDescription)
    {
        std::wstring systemPrompt =
            L"You are a SQL expert. Convert natural language to SQL queries. "
            L"Only output the SQL query, no explanations.\n\n"
            L"Database schema:\n" + schemaDescription;

        std::vector<ChatMessage> messages;
        messages.push_back({ L"user", naturalLanguage });

        return SendChat(messages, systemPrompt);
    }

    // ── Anthropic Messages API ──────────────────────
    std::wstring AiService::CallAnthropic(
        const std::vector<ChatMessage>& messages,
        const std::wstring& systemPrompt)
    {
        httplib::Client cli("https://api.anthropic.com");
        cli.set_connection_timeout(30);
        cli.set_read_timeout(60);

        // Validate API key
        auto apiKey = trimWs(config_.apiKey);
        if (apiKey.empty())
            return L"Error: Anthropic API key is missing. Set it in Settings.";

        nlohmann::json body;
        auto model = trimWs(config_.model);
        body["model"] = toUtf8(model.empty() ? L"claude-sonnet-4-20250514" : model);
        body["max_tokens"] = 2048;

        if (!systemPrompt.empty())
            body["system"] = toUtf8(systemPrompt);

        nlohmann::json msgs = nlohmann::json::array();
        for (auto& m : messages)
        {
            nlohmann::json msg;
            msg["role"] = toUtf8(m.role);
            msg["content"] = toUtf8(m.content);
            msgs.push_back(msg);
        }
        body["messages"] = msgs;

        httplib::Headers headers = {
            {"x-api-key", toUtf8(apiKey)},
            {"anthropic-version", "2023-06-01"}
        };

        auto res = cli.Post("/v1/messages", headers, body.dump(), "application/json");
        if (!res)
            return L"Error: Failed to connect to Anthropic API";

        if (res->status != 200)
            return fromUtf8("Error " + std::to_string(res->status) +
                            " (model=" + toUtf8(model) + "): " + res->body);

        try
        {
            auto json = nlohmann::json::parse(res->body);
            if (json.contains("content") && !json["content"].empty())
            {
                auto& first = json["content"][0];
                if (first.contains("text"))
                    return fromUtf8(first["text"].get<std::string>());
            }
        }
        catch (const std::exception& e)
        {
            return fromUtf8(std::string("Parse error: ") + e.what());
        }
        return L"No response content";
    }

    // ── OpenAI Chat Completions API ─────────────────
    std::wstring AiService::CallOpenAI(
        const std::vector<ChatMessage>& messages,
        const std::wstring& systemPrompt)
    {
        httplib::Client cli("https://api.openai.com");
        cli.set_connection_timeout(30);
        cli.set_read_timeout(60);

        // Validate API key
        auto apiKey = trimWs(config_.apiKey);
        if (apiKey.empty())
            return L"Error: OpenAI API key is missing. Set it in Settings.";

        nlohmann::json body;
        auto model = trimWs(config_.model);
        body["model"] = toUtf8(model.empty() ? L"gpt-4o" : model);
        body["max_tokens"] = 2048;

        nlohmann::json msgs = nlohmann::json::array();
        if (!systemPrompt.empty())
        {
            nlohmann::json sysMsg;
            sysMsg["role"] = "system";
            sysMsg["content"] = toUtf8(systemPrompt);
            msgs.push_back(sysMsg);
        }
        for (auto& m : messages)
        {
            nlohmann::json msg;
            msg["role"] = toUtf8(m.role);
            msg["content"] = toUtf8(m.content);
            msgs.push_back(msg);
        }
        body["messages"] = msgs;

        httplib::Headers headers = {
            {"Authorization", "Bearer " + toUtf8(apiKey)}
        };

        auto res = cli.Post("/v1/chat/completions", headers, body.dump(), "application/json");
        if (!res)
            return L"Error: Failed to connect to OpenAI API";

        if (res->status != 200)
            return fromUtf8("Error " + std::to_string(res->status) +
                            " (model=" + toUtf8(model) + "): " + res->body);

        try
        {
            auto json = nlohmann::json::parse(res->body);
            if (json.contains("choices") && !json["choices"].empty())
                return fromUtf8(json["choices"][0]["message"]["content"].get<std::string>());
        }
        catch (const std::exception& e)
        {
            return fromUtf8(std::string("Parse error: ") + e.what());
        }
        return L"No response content";
    }

    // ── Ollama API (local) ──────────────────────────
    std::wstring AiService::CallOllama(
        const std::vector<ChatMessage>& messages,
        const std::wstring& systemPrompt)
    {
        auto endpointW = trimWs(config_.ollamaEndpoint);
        std::string endpoint = toUtf8(
            endpointW.empty() ? L"http://localhost:11434" : endpointW);
        httplib::Client cli(endpoint);
        cli.set_connection_timeout(10);
        cli.set_read_timeout(120); // Ollama can be slow

        nlohmann::json body;
        auto model = trimWs(config_.model);
        body["model"] = toUtf8(model.empty() ? L"llama3" : model);
        body["stream"] = false;

        nlohmann::json msgs = nlohmann::json::array();
        if (!systemPrompt.empty())
        {
            nlohmann::json sysMsg;
            sysMsg["role"] = "system";
            sysMsg["content"] = toUtf8(systemPrompt);
            msgs.push_back(sysMsg);
        }
        for (auto& m : messages)
        {
            nlohmann::json msg;
            msg["role"] = toUtf8(m.role);
            msg["content"] = toUtf8(m.content);
            msgs.push_back(msg);
        }
        body["messages"] = msgs;

        auto res = cli.Post("/api/chat", body.dump(), "application/json");
        if (!res)
            return L"Error: Failed to connect to Ollama (is it running?)";

        if (res->status != 200)
            return fromUtf8("Error " + std::to_string(res->status) + ": " + res->body);

        try
        {
            auto json = nlohmann::json::parse(res->body);
            if (json.contains("message") && json["message"].contains("content"))
                return fromUtf8(json["message"]["content"].get<std::string>());
        }
        catch (const std::exception& e)
        {
            return fromUtf8(std::string("Parse error: ") + e.what());
        }
        return L"No response content";
    }

    // ── Google Gemini generateContent API ────────────
    //
    // POST https://generativelanguage.googleapis.com/v1beta/models/{model}:generateContent?key={KEY}
    //
    // Body shape differs from OpenAI/Anthropic:
    //   - Messages live under "contents" as { role, parts:[{text}] }
    //   - The assistant role is "model", not "assistant"
    //   - System prompt goes in a top-level "systemInstruction" object
    //   - Max tokens is inside "generationConfig.maxOutputTokens"
    std::wstring AiService::CallGemini(
        const std::vector<ChatMessage>& messages,
        const std::wstring& systemPrompt)
    {
        httplib::Client cli("https://generativelanguage.googleapis.com");
        cli.set_connection_timeout(30);
        cli.set_read_timeout(60);

        auto apiKey = trimWs(config_.apiKey);
        if (apiKey.empty())
            return L"Error: Gemini API key is missing. Set it in Settings.";

        auto model = trimWs(config_.model);
        if (model.empty()) model = L"gemini-2.0-flash";

        nlohmann::json body;
        body["generationConfig"]["maxOutputTokens"] = 2048;

        if (!systemPrompt.empty())
        {
            nlohmann::json sys;
            sys["parts"] = nlohmann::json::array({
                nlohmann::json{{"text", toUtf8(systemPrompt)}}
            });
            body["systemInstruction"] = sys;
        }

        nlohmann::json contents = nlohmann::json::array();
        for (auto& m : messages)
        {
            // Map roles: user→user, assistant→model. System messages are
            // promoted to systemInstruction above and skipped here.
            std::wstring role = m.role;
            if (role == L"assistant") role = L"model";
            else if (role == L"system") continue;

            nlohmann::json entry;
            entry["role"] = toUtf8(role);
            entry["parts"] = nlohmann::json::array({
                nlohmann::json{{"text", toUtf8(m.content)}}
            });
            contents.push_back(entry);
        }
        body["contents"] = contents;

        std::string path = "/v1beta/models/" + toUtf8(model)
                         + ":generateContent?key=" + toUtf8(apiKey);

        auto res = cli.Post(path, body.dump(), "application/json");
        if (!res)
            return L"Error: Failed to connect to Gemini API";

        if (res->status != 200)
            return fromUtf8("Error " + std::to_string(res->status) +
                            " (model=" + toUtf8(model) + "): " + res->body);

        try
        {
            auto json = nlohmann::json::parse(res->body);
            if (json.contains("candidates") && !json["candidates"].empty())
            {
                auto& cand = json["candidates"][0];
                if (cand.contains("content") && cand["content"].contains("parts"))
                {
                    auto& parts = cand["content"]["parts"];
                    if (!parts.empty() && parts[0].contains("text"))
                        return fromUtf8(parts[0]["text"].get<std::string>());
                }
            }
        }
        catch (const std::exception& e)
        {
            return fromUtf8(std::string("Parse error: ") + e.what());
        }
        return L"No response content";
    }

    // ── OpenRouter chat completions ────────────────
    //
    // OpenRouter is OpenAI-protocol-compatible, so the request/response
    // shapes are identical to CallOpenAI. Only the host and the optional
    // HTTP-Referer / X-Title headers (used by OpenRouter for usage
    // attribution) differ.
    std::wstring AiService::CallOpenRouter(
        const std::vector<ChatMessage>& messages,
        const std::wstring& systemPrompt)
    {
        httplib::Client cli("https://openrouter.ai");
        cli.set_connection_timeout(30);
        cli.set_read_timeout(60);

        auto apiKey = trimWs(config_.apiKey);
        if (apiKey.empty())
            return L"Error: OpenRouter API key is missing. Set it in Settings.";

        nlohmann::json body;
        auto model = trimWs(config_.model);
        // OpenRouter requires a fully-qualified model slug like
        // "openai/gpt-4o" or "anthropic/claude-3.5-sonnet". Pick a sane
        // default that's cheap and always available.
        body["model"] = toUtf8(model.empty() ? L"openai/gpt-4o-mini" : model);
        body["max_tokens"] = 2048;

        nlohmann::json msgs = nlohmann::json::array();
        if (!systemPrompt.empty())
        {
            nlohmann::json sysMsg;
            sysMsg["role"] = "system";
            sysMsg["content"] = toUtf8(systemPrompt);
            msgs.push_back(sysMsg);
        }
        for (auto& m : messages)
        {
            nlohmann::json msg;
            msg["role"] = toUtf8(m.role);
            msg["content"] = toUtf8(m.content);
            msgs.push_back(msg);
        }
        body["messages"] = msgs;

        httplib::Headers headers = {
            {"Authorization", "Bearer " + toUtf8(apiKey)},
            // Optional attribution headers recommended by OpenRouter.
            {"HTTP-Referer", "https://gridex.app"},
            {"X-Title",      "Gridex"}
        };

        auto res = cli.Post("/api/v1/chat/completions", headers,
                            body.dump(), "application/json");
        if (!res)
            return L"Error: Failed to connect to OpenRouter API";

        if (res->status != 200)
            return fromUtf8("Error " + std::to_string(res->status) +
                            " (model=" + toUtf8(model) + "): " + res->body);

        try
        {
            auto json = nlohmann::json::parse(res->body);
            if (json.contains("choices") && !json["choices"].empty())
                return fromUtf8(json["choices"][0]["message"]["content"].get<std::string>());
        }
        catch (const std::exception& e)
        {
            return fromUtf8(std::string("Parse error: ") + e.what());
        }
        return L"No response content";
    }

    // ── Fetch available models per provider ────────────
    //
    // Each provider exposes a different listing endpoint and response
    // shape. Normalise everything to a flat vector<wstring> of model IDs
    // that the Settings ComboBox can display. Keep the existing network
    // style (cpp-httplib + nlohmann::json) from the chat calls.
    ModelListResult AiService::FetchModels(const AiConfig& config)
    {
        ModelListResult r;
        auto apiKey = trimWs(config.apiKey);

        try
        {
            switch (config.provider)
            {
            case AiProvider::Anthropic:
            {
                if (apiKey.empty())
                {
                    r.errorMessage = L"Anthropic API key is missing.";
                    return r;
                }
                httplib::Client cli("https://api.anthropic.com");
                cli.set_connection_timeout(15);
                cli.set_read_timeout(30);
                httplib::Headers headers = {
                    { "x-api-key",        toUtf8(apiKey) },
                    { "anthropic-version","2023-06-01" },
                };
                auto res = cli.Get("/v1/models?limit=1000", headers);
                if (!res)
                {
                    r.errorMessage = L"Anthropic models request failed (no response).";
                    return r;
                }
                if (res->status != 200)
                {
                    r.errorMessage = fromUtf8("HTTP " + std::to_string(res->status) + ": " + res->body);
                    return r;
                }
                auto json = nlohmann::json::parse(res->body);
                if (json.contains("data") && json["data"].is_array())
                {
                    for (auto& m : json["data"])
                        if (m.contains("id"))
                            r.models.push_back(fromUtf8(m["id"].get<std::string>()));
                }
                break;
            }

            case AiProvider::OpenAI:
            {
                if (apiKey.empty())
                {
                    r.errorMessage = L"OpenAI API key is missing.";
                    return r;
                }
                httplib::Client cli("https://api.openai.com");
                cli.set_connection_timeout(15);
                cli.set_read_timeout(30);
                httplib::Headers headers = {
                    { "Authorization", "Bearer " + toUtf8(apiKey) },
                };
                auto res = cli.Get("/v1/models", headers);
                if (!res)
                {
                    r.errorMessage = L"OpenAI models request failed (no response).";
                    return r;
                }
                if (res->status != 200)
                {
                    r.errorMessage = fromUtf8("HTTP " + std::to_string(res->status) + ": " + res->body);
                    return r;
                }
                auto json = nlohmann::json::parse(res->body);
                if (json.contains("data") && json["data"].is_array())
                {
                    for (auto& m : json["data"])
                        if (m.contains("id"))
                            r.models.push_back(fromUtf8(m["id"].get<std::string>()));
                }
                break;
            }

            case AiProvider::Ollama:
            {
                // Ollama needs the endpoint, not an API key. Default
                // localhost:11434 when user hasn't customized it.
                std::wstring endpointW = trimWs(config.ollamaEndpoint);
                if (endpointW.empty()) endpointW = L"http://localhost:11434";
                auto endpoint = toUtf8(endpointW);
                httplib::Client cli(endpoint);
                cli.set_connection_timeout(10);
                cli.set_read_timeout(15);
                auto res = cli.Get("/api/tags");
                if (!res)
                {
                    r.errorMessage = L"Ollama endpoint unreachable: " + endpointW;
                    return r;
                }
                if (res->status != 200)
                {
                    r.errorMessage = fromUtf8("HTTP " + std::to_string(res->status) + ": " + res->body);
                    return r;
                }
                auto json = nlohmann::json::parse(res->body);
                if (json.contains("models") && json["models"].is_array())
                {
                    for (auto& m : json["models"])
                        if (m.contains("name"))
                            r.models.push_back(fromUtf8(m["name"].get<std::string>()));
                }
                break;
            }

            case AiProvider::Gemini:
            {
                if (apiKey.empty())
                {
                    r.errorMessage = L"Gemini API key is missing.";
                    return r;
                }
                httplib::Client cli("https://generativelanguage.googleapis.com");
                cli.set_connection_timeout(15);
                cli.set_read_timeout(30);
                std::string path = "/v1beta/models?key=" + toUtf8(apiKey);
                auto res = cli.Get(path.c_str());
                if (!res)
                {
                    r.errorMessage = L"Gemini models request failed (no response).";
                    return r;
                }
                if (res->status != 200)
                {
                    r.errorMessage = fromUtf8("HTTP " + std::to_string(res->status) + ": " + res->body);
                    return r;
                }
                auto json = nlohmann::json::parse(res->body);
                if (json.contains("models") && json["models"].is_array())
                {
                    // Gemini returns "name": "models/gemini-2.0-flash".
                    // Strip the "models/" prefix so the value matches
                    // what the chat endpoint expects in its URL path.
                    for (auto& m : json["models"])
                    {
                        if (!m.contains("name")) continue;
                        std::string id = m["name"].get<std::string>();
                        const std::string prefix = "models/";
                        if (id.rfind(prefix, 0) == 0) id = id.substr(prefix.size());
                        r.models.push_back(fromUtf8(id));
                    }
                }
                break;
            }

            case AiProvider::OpenRouter:
            {
                // OpenRouter /models endpoint is public — no API key
                // required just to list. If apiKey present we pass it
                // for completeness / rate-limit attribution.
                httplib::Client cli("https://openrouter.ai");
                cli.set_connection_timeout(15);
                cli.set_read_timeout(30);
                httplib::Headers headers;
                if (!apiKey.empty())
                    headers.emplace("Authorization", "Bearer " + toUtf8(apiKey));
                auto res = cli.Get("/api/v1/models", headers);
                if (!res)
                {
                    r.errorMessage = L"OpenRouter models request failed (no response).";
                    return r;
                }
                if (res->status != 200)
                {
                    r.errorMessage = fromUtf8("HTTP " + std::to_string(res->status) + ": " + res->body);
                    return r;
                }
                auto json = nlohmann::json::parse(res->body);
                if (json.contains("data") && json["data"].is_array())
                {
                    for (auto& m : json["data"])
                        if (m.contains("id"))
                            r.models.push_back(fromUtf8(m["id"].get<std::string>()));
                }
                break;
            }

            default:
                r.errorMessage = L"Unsupported provider.";
                return r;
            }

            // Alphabetize so the dropdown is easy to scan; OpenAI /
            // OpenRouter lists are unsorted by default.
            std::sort(r.models.begin(), r.models.end());
            r.success = true;
        }
        catch (const std::exception& e)
        {
            r.errorMessage = fromUtf8(std::string("Parse error: ") + e.what());
        }
        catch (...)
        {
            r.errorMessage = L"Unknown error fetching models.";
        }
        return r;
    }
}
