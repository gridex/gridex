#pragma once

// Persisted ChatGPT OAuth token bundle. Lives in SecretStore as a single
// JSON blob keyed by provider id (or, in our simpler Linux model, a
// process-wide "chatgpt" key).

#include <chrono>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace gridex {

struct ChatGPTTokenBundle {
    std::string accessToken;
    std::string refreshToken;
    std::string idToken;

    // Decoded id_token claims (populated by ChatGPTOAuthService::applyClaims).
    std::optional<std::string> email;
    std::optional<std::string> accountId;
    std::optional<std::string> planType;

    // Wall-clock instant the bundle was created or refreshed.
    std::chrono::system_clock::time_point obtainedAt =
        std::chrono::system_clock::now();

    [[nodiscard]] nlohmann::json toJson() const {
        nlohmann::json j = {
            {"accessToken",  accessToken},
            {"refreshToken", refreshToken},
            {"idToken",      idToken},
            {"obtainedAt",   std::chrono::duration_cast<std::chrono::seconds>(
                                 obtainedAt.time_since_epoch()).count()},
        };
        if (email)     j["email"]     = *email;
        if (accountId) j["accountId"] = *accountId;
        if (planType)  j["planType"]  = *planType;
        return j;
    }

    static std::optional<ChatGPTTokenBundle> fromJson(const nlohmann::json& j) {
        if (!j.is_object() || !j.contains("accessToken") || !j.contains("refreshToken")) {
            return std::nullopt;
        }
        ChatGPTTokenBundle b;
        b.accessToken  = j.value("accessToken", "");
        b.refreshToken = j.value("refreshToken", "");
        b.idToken      = j.value("idToken", "");
        if (j.contains("email"))     b.email     = j["email"].get<std::string>();
        if (j.contains("accountId")) b.accountId = j["accountId"].get<std::string>();
        if (j.contains("planType"))  b.planType  = j["planType"].get<std::string>();
        if (j.contains("obtainedAt") && j["obtainedAt"].is_number_integer()) {
            b.obtainedAt = std::chrono::system_clock::time_point(
                std::chrono::seconds(j["obtainedAt"].get<std::int64_t>()));
        }
        return b;
    }
};

}  // namespace gridex
