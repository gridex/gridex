#pragma once

// ChatGPTOAuthService — orchestrates the PKCE-OAuth flow for OpenAI's
// auth.openai.com tenant against the public Codex CLI client_id.
//
// signIn(): spins up the loopback listener, opens the browser, waits for
// the callback (3-min timeout), exchanges the auth code for tokens,
// persists the bundle in the keychain. Emits signInCompleted /
// signInFailed when done.
//
// tokenBundle(): returns a usable bundle, refreshing via refresh_token
// when access_token is within kRefreshSkewSeconds of expiring. Coalesces
// concurrent refresh calls for the same providerKey.
//
// signOut(): forgets the persisted bundle; cheap, no server call.
//
// Status: a synchronous probe so the Settings UI can render the
// "Signed in as ..." pill without a network round-trip.

#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <QObject>
#include <QString>

#include "Core/Models/AI/ChatGPTTokenBundle.h"

class QNetworkAccessManager;

namespace gridex {

class SecretStore;

namespace oauth {
class OAuthLoopbackServer;
}

struct ChatGPTSignInStatus {
    bool signedIn = false;
    QString email;
    QString planType;
};

class ChatGPTOAuthService : public QObject {
    Q_OBJECT
public:
    explicit ChatGPTOAuthService(SecretStore* secretStore, QObject* parent = nullptr);
    ~ChatGPTOAuthService() override;

    // Kicks off the browser-based sign-in flow. providerKey identifies which
    // bundle slot in the keychain (single-account "chatgpt" by default).
    // Emits signInCompleted on success, signInFailed otherwise.
    void signIn(const QString& providerKey = QStringLiteral("chatgpt"));

    // Synchronous accessor — returns nullopt if no valid bundle is stored.
    std::optional<ChatGPTTokenBundle> currentBundle(
        const QString& providerKey = QStringLiteral("chatgpt")) const;

    // Returns a fresh access token, performing a refresh if needed.
    // Emits tokenRefreshed on success, signInFailed if the refresh token
    // was rejected (in which case the bundle is wiped — caller should
    // prompt for a new sign-in).
    void requestFreshToken(
        const QString& providerKey = QStringLiteral("chatgpt"));

    void signOut(const QString& providerKey = QStringLiteral("chatgpt"));

    ChatGPTSignInStatus status(
        const QString& providerKey = QStringLiteral("chatgpt")) const;

signals:
    void signInCompleted(const QString& providerKey, const ChatGPTTokenBundle& bundle);
    void signInFailed(const QString& providerKey, const QString& message);
    void tokenRefreshed(const QString& providerKey, const ChatGPTTokenBundle& bundle);

private:
    void onCallbackSuccess(const QString& providerKey,
                           const QString& expectedState,
                           const QString& verifier,
                           const QString& redirectUri,
                           const QString& code,
                           const QString& callbackState);
    void onCallbackError(const QString& providerKey, const QString& message);
    void exchangeCodeForTokens(const QString& providerKey,
                               const QString& code,
                               const QString& verifier,
                               const QString& redirectUri);
    void performRefresh(const QString& providerKey, ChatGPTTokenBundle bundle);
    void persistBundle(const QString& providerKey, const ChatGPTTokenBundle& bundle);
    static void applyClaims(ChatGPTTokenBundle& bundle);

    SecretStore* secretStore_;
    std::unique_ptr<QNetworkAccessManager> nam_;
    std::unique_ptr<oauth::OAuthLoopbackServer> server_;

    mutable std::mutex mu_;
    bool refreshInFlight_ = false;
};

}  // namespace gridex
