#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "Winhttp.lib")
#include "Models/AiService.h"
#include "Services/ChatGPTOAuth/ChatGPTOAuthService.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <sstream>
#include <functional>

// cpp-httplib for HTTP client — suppress deprecated SSL API warnings
#pragma warning(push)
#pragma warning(disable: 4996)
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>
#pragma warning(pop)

namespace {
    // Native-Win32 HTTPS GET helper. Replaces httplib::SSLClient for
    // ChatGPT calls — vcpkg httplib 0.40.0 vs OpenSSL 3.x triggers a
    // SSL_shutdown crash on the worker thread (`s = 0x2`). WinHTTP
    // sidesteps OpenSSL entirely.
    struct WinHttpResult { int status = 0; std::string body; };

    WinHttpResult WinHttpsRequest(const std::wstring& host,
                                  const std::wstring& method,
                                  const std::wstring& path,
                                  const std::vector<std::wstring>& headers,
                                  const std::string& body,
                                  const std::function<bool(const char*, size_t)>* chunkCb = nullptr)
    {
        WinHttpResult out;
        HINTERNET hSession = ::WinHttpOpen(
            L"Gridex/AiService",
            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) return out;
        HINTERNET hConnect = ::WinHttpConnect(
            hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!hConnect) { ::WinHttpCloseHandle(hSession); return out; }
        HINTERNET hReq = ::WinHttpOpenRequest(
            hConnect, method.c_str(), path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
            WINHTTP_FLAG_SECURE);
        if (!hReq) { ::WinHttpCloseHandle(hConnect); ::WinHttpCloseHandle(hSession); return out; }

        // Long timeout for streaming responses.
        ::WinHttpSetTimeouts(hReq, 30000, 30000, 30000, 120000);

        std::wstring hdrBlob;
        for (auto& h : headers) { hdrBlob += h; hdrBlob += L"\r\n"; }

        BOOL ok = ::WinHttpSendRequest(hReq,
            hdrBlob.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdrBlob.c_str(),
            hdrBlob.empty() ? 0 : (DWORD)-1L,
            body.empty() ? nullptr : (LPVOID)body.data(),
            (DWORD)body.size(), (DWORD)body.size(), 0);
        if (ok) ok = ::WinHttpReceiveResponse(hReq, nullptr);

        if (ok)
        {
            DWORD statusCode = 0, statusLen = sizeof(statusCode);
            ::WinHttpQueryHeaders(hReq,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusLen,
                WINHTTP_NO_HEADER_INDEX);
            out.status = (int)statusCode;

            DWORD avail = 0;
            while (::WinHttpQueryDataAvailable(hReq, &avail) && avail > 0)
            {
                std::string chunk(avail, '\0');
                DWORD read = 0;
                if (!::WinHttpReadData(hReq, chunk.data(), avail, &read)) break;
                chunk.resize(read);
                if (chunkCb)
                {
                    if (!(*chunkCb)(chunk.data(), chunk.size())) break;
                }
                else
                {
                    out.body.append(chunk);
                }
            }
        }
        ::WinHttpCloseHandle(hReq);
        ::WinHttpCloseHandle(hConnect);
        ::WinHttpCloseHandle(hSession);
        return out;
    }
}

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
        case AiProvider::ChatGPT:    return CallChatGPT(messages, systemPrompt);
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

    // ── ChatGPT Subscription (Codex Responses API) ─────
    //
    // Uses OAuth bearer token from ChatGPT::OAuthService.
    // POST https://chatgpt.com/backend-api/codex/responses with SSE streaming.
    // Body shape mirrors Codex CLI's /responses wire format — see
    // macos/Services/AI/Providers/ChatGPTProvider.swift for the reference impl.
    std::wstring AiService::CallChatGPT(
        const std::vector<ChatMessage>& messages,
        const std::wstring& systemPrompt)
    {
        auto bearer = ChatGPT::OAuthService::Instance().BearerToken();
        if (bearer.empty())
            return L"Error: Not signed in to ChatGPT. Go to Settings and click Sign in.";

        auto model = trimWs(config_.model);
        if (model.empty()) model = L"gpt-4o";

        // Build input messages — system role goes to top-level "instructions".
        // Each content item uses typed parts (input_text / output_text) as
        // required by the Responses API. Do NOT send temperature or
        // max_output_tokens — GPT-5 family rejects them (returns HTTP 400).
        nlohmann::json inputMsgs = nlohmann::json::array();
        for (auto& m : messages)
        {
            if (m.role == L"system") continue; // hoisted to "instructions"
            std::string partType = (m.role == L"assistant") ? "output_text" : "input_text";
            nlohmann::json msg;
            msg["type"] = "message";
            msg["role"] = toUtf8(m.role);
            msg["content"] = nlohmann::json::array({
                nlohmann::json{{"type", partType}, {"text", toUtf8(m.content)}}
            });
            inputMsgs.push_back(msg);
        }

        nlohmann::json body;
        body["model"]        = toUtf8(model);
        body["instructions"] = toUtf8(systemPrompt);
        body["input"]        = inputMsgs;
        body["stream"]       = true;
        body["store"]        = false;

        std::vector<std::wstring> headers = {
            L"Authorization: Bearer " + fromUtf8(bearer),
            L"Content-Type: application/json",
            L"OpenAI-Beta: responses=v1",
        };
        // ChatGPT-Account-ID is required for /responses on accounts
        // that have multiple workspaces. Mac sends it whenever the
        // bundle has the claim — mirror that.
        auto accountId = ChatGPT::OAuthService::Instance().AccountId();
        if (!accountId.empty())
            headers.push_back(L"ChatGPT-Account-ID: " + fromUtf8(accountId));

        // Accumulate SSE deltas into full response text.
        std::string accumulated;
        bool streamError = false;
        std::string streamErrorMsg;

        // WinHTTP streaming POST: chunkCb fires per WinHttpReadData read.
        std::function<bool(const char*, size_t)> chunkCb =
            [&](const char* data, size_t len) -> bool
            {
                // Parse SSE lines from this chunk
                std::string chunk(data, len);
                std::istringstream ss(chunk);
                std::string line;
                std::string currentEvent;

                while (std::getline(ss, line))
                {
                    // Strip trailing \r
                    if (!line.empty() && line.back() == '\r')
                        line.pop_back();

                    if (line.empty()) { currentEvent.clear(); continue; }

                    if (line.rfind("event: ", 0) == 0)
                    {
                        currentEvent = line.substr(7);
                        continue;
                    }
                    if (line.rfind(": ", 0) == 0) continue; // SSE comment
                    if (line.rfind("data: ", 0) != 0) continue;

                    std::string payload = line.substr(6);
                    if (payload == "[DONE]") return true;

                    if (currentEvent == "response.output_text.delta")
                    {
                        try
                        {
                            auto j = nlohmann::json::parse(payload);
                            if (j.contains("delta") && j["delta"].is_string())
                                accumulated += j["delta"].get<std::string>();
                        }
                        catch (...) {}
                    }
                    else if (currentEvent == "response.error")
                    {
                        streamError = true;
                        streamErrorMsg = payload;
                        return false; // abort stream
                    }
                    else if (currentEvent == "response.completed")
                    {
                        return true;
                    }
                }
                return true; // continue receiving
            };

        auto res = WinHttpsRequest(L"chatgpt.com", L"POST",
            L"/backend-api/codex/responses", headers, body.dump(), &chunkCb);

        if (res.status == 0)
            return L"Error: Failed to connect to ChatGPT backend";

        if (res.status == 401 || res.status == 403)
        {
            // Token rejected — wipe and tell user to re-authenticate
            ChatGPT::OAuthService::Instance().SignOut();
            return L"Error: Session expired. Go to Settings and sign in to ChatGPT again.";
        }

        if (res.status != 200)
            return fromUtf8("Error " + std::to_string(res.status) +
                            " from ChatGPT: " + res.body.substr(0, 500));

        if (streamError)
            return fromUtf8("ChatGPT stream error: " + streamErrorMsg);

        if (accumulated.empty())
            return L"No response content";

        return fromUtf8(accumulated);
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

            case AiProvider::ChatGPT:
            {
                // GET https://chatgpt.com/backend-api/codex/models with bearer token.
                // Response: {"models": [{"slug": "...", "name": "...", ...}, ...]}
                auto bearer = ChatGPT::OAuthService::Instance().BearerToken();
                if (bearer.empty())
                {
                    r.errorMessage = L"Not signed in to ChatGPT. Sign in via Settings.";
                    return r;
                }
                std::vector<std::wstring> hdrs = {
                    L"Authorization: Bearer " + fromUtf8(bearer),
                };
                // /backend-api/codex/models rejects the request with
                // HTTP 400 ({"type":"missing","msg":"Field required"})
                // when client_version is absent. Mac sends "1.0.0";
                // mirror that. ChatGPT-Account-ID is required by some
                // endpoints — pass through when we have it.
                auto accountId = ChatGPT::OAuthService::Instance().AccountId();
                if (!accountId.empty())
                    hdrs.push_back(L"ChatGPT-Account-ID: " + fromUtf8(accountId));
                auto res = WinHttpsRequest(L"chatgpt.com", L"GET",
                    L"/backend-api/codex/models?client_version=1.0.0",
                    hdrs, "");
                if (res.status == 0)
                {
                    r.errorMessage = L"ChatGPT models request failed (no response).";
                    return r;
                }
                if (res.status == 401 || res.status == 403)
                {
                    ChatGPT::OAuthService::Instance().SignOut();
                    r.errorMessage = L"Session expired — sign in to ChatGPT again.";
                    return r;
                }
                if (res.status != 200)
                {
                    r.errorMessage = fromUtf8("HTTP " + std::to_string(res.status) + ": " + res.body);
                    return r;
                }
                auto json = nlohmann::json::parse(res.body);
                if (json.contains("models") && json["models"].is_array())
                {
                    for (auto& m : json["models"])
                    {
                        if (!m.contains("slug")) continue;
                        // Filter to user-listable models only
                        bool supported  = m.value("supported_in_api", true);
                        std::string vis = m.value("visibility", "list");
                        if (!supported || vis != "list") continue;
                        r.models.push_back(fromUtf8(m["slug"].get<std::string>()));
                    }
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
