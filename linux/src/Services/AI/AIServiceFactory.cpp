#include "Services/AI/AIServiceFactory.h"

#include <stdexcept>

#include "Services/AI/Providers/AnthropicProvider.h"
#include "Services/AI/Providers/ChatGPTProvider.h"
#include "Services/AI/Providers/GeminiProvider.h"
#include "Services/AI/Providers/OllamaProvider.h"
#include "Services/AI/Providers/OpenAIProvider.h"

namespace gridex {

std::unique_ptr<ILLMService>
AIServiceFactory::createAIService(const std::string& providerName,
                                   const std::string& apiKey,
                                   const std::string& baseUrl) {
    if (providerName == "Anthropic") {
        return std::make_unique<AnthropicProvider>(apiKey, baseUrl);
    }
    if (providerName == "OpenAI") {
        return std::make_unique<OpenAIProvider>(apiKey, baseUrl);
    }
    if (providerName == "Ollama") {
        // apiKey unused; Ollama is local.
        return baseUrl.empty()
            ? std::make_unique<OllamaProvider>()
            : std::make_unique<OllamaProvider>(baseUrl);
    }
    if (providerName == "Gemini") {
        return std::make_unique<GeminiProvider>(apiKey, baseUrl);
    }
    if (providerName == "ChatGPT") {
        // apiKey is unused — credentials live in libsecret as an OAuth bundle
        // populated by the Sign in with ChatGPT flow.
        return std::make_unique<ChatGPTProvider>(apiKey, baseUrl);
    }
    throw std::invalid_argument("AIServiceFactory: unknown provider '" + providerName + "'");
}

}  // namespace gridex
