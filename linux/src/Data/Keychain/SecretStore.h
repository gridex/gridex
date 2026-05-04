#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace gridex {

// libsecret-backed credential store. Mirrors macos KeychainService semantics.
// Keys are scoped by service name "com.gridex.credentials". The store delegates
// to the Secret Service daemon (GNOME Keyring / KWallet). If no daemon is
// available (e.g. headless/WSL) operations throw gridex::GridexError.
class SecretStore {
public:
    SecretStore();
    explicit SecretStore(std::string serviceName);

    // Store / update a password under the given account key.
    void save(std::string_view key, std::string_view value);

    // Returns std::nullopt when the key is absent.
    [[nodiscard]] std::optional<std::string> load(std::string_view key);

    // No-op if the key is absent.
    void remove(std::string_view key);

    // Probes availability once per process. Returns false if Secret Service is
    // missing (headless/WSL). Callers can decide to degrade gracefully.
    [[nodiscard]] bool isAvailable();

    // Convenience keys matching macOS layout.
    void savePassword(std::string_view connectionId, std::string_view password) {
        save("db.password." + std::string(connectionId), password);
    }
    std::optional<std::string> loadPassword(std::string_view connectionId) {
        return load("db.password." + std::string(connectionId));
    }
    void removePassword(std::string_view connectionId) {
        remove("db.password." + std::string(connectionId));
    }

    void saveSSHPassword(std::string_view connectionId, std::string_view password) {
        save("ssh.password." + std::string(connectionId), password);
    }
    std::optional<std::string> loadSSHPassword(std::string_view connectionId) {
        return load("ssh.password." + std::string(connectionId));
    }

    void saveAPIKey(std::string_view provider, std::string_view key) {
        save("ai.apikey." + std::string(provider), key);
    }
    std::optional<std::string> loadAPIKey(std::string_view provider) {
        return load("ai.apikey." + std::string(provider));
    }

    // ChatGPT OAuth token bundle (JSON-serialized — see ChatGPTTokenBundle).
    void saveChatGPTTokens(std::string_view providerKey, std::string_view tokensJson) {
        save("ai.chatgpt.tokens." + std::string(providerKey), tokensJson);
    }
    std::optional<std::string> loadChatGPTTokens(std::string_view providerKey) {
        return load("ai.chatgpt.tokens." + std::string(providerKey));
    }
    void deleteChatGPTTokens(std::string_view providerKey) {
        remove("ai.chatgpt.tokens." + std::string(providerKey));
    }

private:
    std::string serviceName_;
};

}
