#pragma once

// LLMService implementation backed by the ChatGPT subscription via the Codex
// CLI OAuth flow. Talks to chatgpt.com/backend-api/codex/responses (the
// Responses API), NOT api.openai.com/v1/chat/completions.
//
// Token refresh happens lazily inside each request so a refresh in one call
// is visible to the next: load bundle from keychain → if access_token is
// close to expiring, POST /oauth/token with refresh_token → write the
// rotated bundle back → use access_token in the request.

#include <string>
#include <vector>

#include "Core/Protocols/AI/ILLMService.h"

namespace gridex {

class ChatGPTProvider : public ILLMService {
public:
    // baseUrl defaults to https://chatgpt.com/backend-api/codex when empty.
    // apiKey is unused; the OAuth bundle in libsecret is the credential.
    ChatGPTProvider(const std::string& apiKey, const std::string& baseUrl);

    std::string providerName() const override { return "ChatGPT"; }

    std::string sendMessage(const std::vector<LLMMessage>& messages,
                            const std::string& systemPrompt,
                            const std::string& model,
                            int maxTokens   = 4096,
                            double temperature = 0.7) override;

    std::vector<LLMModel> availableModels() override;

    bool validateAPIKey() override;

private:
    std::string baseUrl_;
};

}  // namespace gridex
