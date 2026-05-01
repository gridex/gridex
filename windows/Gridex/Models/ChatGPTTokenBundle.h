#pragma once
// ChatGPTTokenBundle.h
// OAuth token bundle persisted via DPAPI-encrypted file at
// %APPDATA%\Gridex\chatgpt-tokens.bin for the ChatGPT subscription provider.
// Stored as JSON (single blob) so refresh writes are atomic.

#include <string>
#include <chrono>
#include <nlohmann/json.hpp>

namespace DBModels
{
    struct ChatGPTTokenBundle
    {
        // JWT — short-lived, used as Authorization: Bearer against chatgpt.com/backend-api/codex.
        std::string accessToken;

        // JWT — long-lived, used to mint new access tokens. Server may rotate on refresh.
        std::string refreshToken;

        // JWT — carries claims (email, chatgpt_account_id, chatgpt_plan_type).
        // Persisted so we can re-derive identity without re-querying the server.
        std::string idToken;

        // Mirrors chatgpt_account_id from idToken. Sent as ChatGPT-Account-ID header.
        std::string accountId;

        // Mirrors email claim. UI-only: "Signed in as ...".
        std::string email;

        // Mirrors chatgpt_plan_type claim (e.g. "plus", "pro"). UI-only.
        std::string planType;

        // Unix timestamp (seconds) when bundle was minted (sign-in or last refresh).
        int64_t obtainedAtUnix = 0;

        bool empty() const { return accessToken.empty(); }

        // Serialize to JSON string for DPAPI storage.
        std::string toJson() const
        {
            nlohmann::json j;
            j["accessToken"]  = accessToken;
            j["refreshToken"] = refreshToken;
            j["idToken"]      = idToken;
            j["accountId"]    = accountId;
            j["email"]        = email;
            j["planType"]     = planType;
            j["obtainedAt"]   = obtainedAtUnix;
            j["schemaVersion"] = 1;
            return j.dump();
        }

        // Deserialize from JSON string. Returns empty bundle on parse error.
        static ChatGPTTokenBundle fromJson(const std::string& json)
        {
            try
            {
                auto j = nlohmann::json::parse(json);
                ChatGPTTokenBundle b;
                b.accessToken  = j.value("accessToken",  "");
                b.refreshToken = j.value("refreshToken", "");
                b.idToken      = j.value("idToken",      "");
                b.accountId    = j.value("accountId",    "");
                b.email        = j.value("email",        "");
                b.planType     = j.value("planType",     "");
                b.obtainedAtUnix = j.value("obtainedAt", int64_t(0));
                return b;
            }
            catch (...) { return {}; }
        }
    };
}
