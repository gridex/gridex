#pragma once

// ChatGPTOAuthConstants — endpoints + parameter constants for the ChatGPT
// OAuth flow. Mirrors the macOS / Windows ports verbatim so a single
// origination signature is used across platforms.
//
// Reusing OpenAI's public Codex CLI client_id is a deliberate trade-off: the
// authorize endpoint only accepts loopback redirects that were registered
// for that client. OpenAI may revoke it at any time.

#include <array>
#include <cstdint>
#include <string_view>

namespace gridex::oauth {

// Public Codex CLI client_id. Same value used by macOS + Windows ports.
constexpr std::string_view kClientId = "app_EMoamEEZ73f0CkXaXp7hrann";

constexpr std::string_view kIssuer        = "https://auth.openai.com";
constexpr std::string_view kAuthorizePath = "/oauth/authorize";
constexpr std::string_view kTokenPath     = "/oauth/token";

// Backend used by signed-in callers. Distinct from api.openai.com/v1.
constexpr std::string_view kChatGPTBackend = "https://chatgpt.com/backend-api/codex";

// `offline_access` returns refresh_token; `api.connectors.*` lets the
// access_token reach the chatgpt.com backend.
constexpr std::string_view kScopes =
    "openid profile email offline_access api.connectors.read api.connectors.invoke";

// Sent as `originator` query param; informational, the auth server does not
// gate behaviour on it.
constexpr std::string_view kOriginator = "gridex_linux";

// 3 min — long enough for password manager + 2FA, short enough to bound
// the UI hang.
constexpr int kCallbackTimeoutSeconds = 180;

// Refresh slightly before access_token's `exp` so a request in flight at
// `exp` doesn't get a 401.
constexpr int kRefreshSkewSeconds = 30;

// Loopback ports tried in order. Codex CLI default is 1455.
constexpr std::array<std::uint16_t, 4> kPreferredPorts = {1455, 1456, 1457, 1458};

constexpr std::string_view kCallbackPath = "/auth/callback";

}  // namespace gridex::oauth
