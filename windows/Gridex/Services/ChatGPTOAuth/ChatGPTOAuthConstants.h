#pragma once
// ChatGPTOAuthConstants.h
// Endpoint + parameter constants for the ChatGPT OAuth PKCE flow.
// All values mirror OpenAI's Codex CLI behaviour. See macos/Services/AI/Auth/ChatGPTOAuthConstants.swift.
// NOTE: Reusing the public Codex CLI client_id is a deliberate trade-off —
// OpenAI may revoke it at any time.

#include <string>

namespace ChatGPT
{
    // The public OAuth client_id baked into OpenAI's Codex CLI.
    inline constexpr const char* kClientId = "app_EMoamEEZ73f0CkXaXp7hrann";

    // OAuth issuer base URL — /oauth/authorize and /oauth/token hang off this.
    inline constexpr const char* kIssuer = "https://auth.openai.com";
    // Bare host, used when constructing httplib::SSLClient (which takes a
    // host, NOT a URL — feeding it the full kIssuer makes httplib treat
    // "https://auth.openai.com" as the literal host name and assert on a
    // null SSL socket once the handshake is attempted).
    inline constexpr const char* kIssuerHost = "auth.openai.com";

    // Scope set required by the Codex CLI flow.
    // `offline_access` gives us a refresh_token.
    inline constexpr const char* kScopes =
        "openid profile email offline_access "
        "api.connectors.read api.connectors.invoke";

    // Codex Responses API backend (distinct from api.openai.com/v1).
    inline constexpr const char* kBackend = "https://chatgpt.com";

    // Path for the loopback callback.
    inline constexpr const char* kCallbackPath = "/auth/callback";

    // Loopback port (matches macOS — port 1455 is the Codex CLI default).
    inline constexpr int kCallbackPort = 1455;

    // Refresh access_token this many seconds before it expires.
    inline constexpr int kRefreshSkewSeconds = 60;

    // How long to wait for the browser OAuth callback (seconds).
    inline constexpr int kCallbackTimeoutSeconds = 180;

    // originator query param (informational, not gated by auth server).
    inline constexpr const char* kOriginator = "gridex_win";
}
