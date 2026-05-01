#pragma once
// ChatGPTOAuthService.h
// Owns the full ChatGPT OAuth lifecycle for the Windows app:
//   - SignIn(callback)   — PKCE flow, opens browser, loopback listener, stores tokens
//   - SignOut()          — deletes the DPAPI-encrypted token file
//   - BearerToken()      — returns current access_token, refreshing if within kRefreshSkewSeconds
//   - CurrentEmail()     — UI helper, returns stored email or empty string
//
// Token storage: DPAPI-encrypted JSON at %APPDATA%\Gridex\chatgpt-tokens.bin
// Concurrent refresh coalesced via std::mutex + std::shared_future.

#include <string>
#include <functional>
#include <mutex>
#include <future>
#include "../../Models/ChatGPTTokenBundle.h"

namespace ChatGPT
{
    class OAuthService
    {
    public:
        // Callback type: called on the worker thread after sign-in completes.
        // success=true: email holds the signed-in address.
        // success=false: error holds a user-visible message.
        using SignInCallback = std::function<void(bool success, std::wstring emailOrError)>;

        // Launch the PKCE browser flow asynchronously. Callback fires on a
        // background thread — marshal to UI thread before touching UI.
        void SignIn(SignInCallback callback);

        // Wipe the stored token bundle. Safe to call even if not signed in.
        void SignOut();

        // Returns the current access_token, refreshing it if needed.
        // Returns empty string if not signed in or refresh failed.
        std::string BearerToken();

        // Returns the stored email claim, or empty string if signed out.
        std::wstring CurrentEmail();
        // ChatGPT-Account-ID claim value (UTF-8). Some /backend-api/codex
        // endpoints require this as a header; empty when not signed in.
        std::string AccountId();

        // True if a valid token bundle exists on disk.
        bool IsSignedIn();

        // Singleton accessor — no DI complexity needed for Windows port.
        static OAuthService& Instance();

    private:
        OAuthService() = default;

        // DPAPI-encrypted token file path: %APPDATA%\Gridex\chatgpt-tokens.bin
        static std::wstring TokenFilePath();

        // Load/save/delete token bundle via DPAPI.
        static bool SaveBundle(const DBModels::ChatGPTTokenBundle& bundle);
        static DBModels::ChatGPTTokenBundle LoadBundle();
        static void DeleteBundle();

        // Perform token refresh via POST /oauth/token with refresh_token grant.
        // Returns empty bundle on failure.
        static DBModels::ChatGPTTokenBundle RefreshBundle(
            const DBModels::ChatGPTTokenBundle& current);

        // Apply claims from id_token into the bundle fields.
        static void ApplyClaims(DBModels::ChatGPTTokenBundle& bundle);

        // Coalesce concurrent refresh calls.
        std::mutex               refreshMutex_;
        std::shared_future<std::string> refreshFuture_;
    };
}
