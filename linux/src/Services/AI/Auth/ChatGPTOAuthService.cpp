#include "Services/AI/Auth/ChatGPTOAuthService.h"

#include <QByteArray>
#include <QNetworkAccessManager>
#include <QProcess>
#include <QStringList>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>

#include <nlohmann/json.hpp>

#include "Data/Keychain/SecretStore.h"
#include "Services/AI/Auth/ChatGPTOAuthConstants.h"
#include "Services/AI/Auth/JwtDecoder.h"
#include "Services/AI/Auth/OAuthLoopbackServer.h"
#include "Services/AI/Auth/PKCE.h"

namespace gridex {

namespace {

QString sv(std::string_view s) {
    return QString::fromUtf8(s.data(), static_cast<int>(s.size()));
}

QString parseErrorBody(const QByteArray& data) {
    try {
        auto j = nlohmann::json::parse(data.toStdString());
        if (j.contains("error")) {
            QString err = QString::fromStdString(j["error"].get<std::string>());
            if (j.contains("error_description")) {
                return err + ": " + QString::fromStdString(j["error_description"].get<std::string>());
            }
            return err;
        }
        if (j.contains("message")) {
            return QString::fromStdString(j["message"].get<std::string>());
        }
    } catch (...) {}
    return QString::fromUtf8(data).trimmed();
}

}  // namespace

ChatGPTOAuthService::ChatGPTOAuthService(SecretStore* secretStore, QObject* parent)
    : QObject(parent),
      secretStore_(secretStore),
      nam_(std::make_unique<QNetworkAccessManager>()) {}

ChatGPTOAuthService::~ChatGPTOAuthService() = default;

void ChatGPTOAuthService::signIn(const QString& providerKey) {
    using namespace oauth;

    // Tear down any previous attempt's listener.
    server_.reset(new OAuthLoopbackServer(this));
    std::uint16_t port = server_->start();
    if (port == 0) {
        emit signInFailed(providerKey, tr("Could not bind any loopback port for OAuth callback"));
        return;
    }

    QString verifier  = PKCE::makeVerifier();
    QString challenge = PKCE::challengeFor(verifier);
    QString state     = PKCE::makeState();
    QString redirectUri =
        QString("http://localhost:%1%2").arg(port).arg(sv(kCallbackPath));

    QUrl authorize(sv(kIssuer) + sv(kAuthorizePath));
    QUrlQuery q;
    q.addQueryItem("response_type",                 "code");
    q.addQueryItem("client_id",                     sv(kClientId));
    q.addQueryItem("redirect_uri",                  redirectUri);
    q.addQueryItem("scope",                         sv(kScopes));
    q.addQueryItem("code_challenge",                challenge);
    q.addQueryItem("code_challenge_method",         "S256");
    q.addQueryItem("state",                         state);
    q.addQueryItem("id_token_add_organizations",    "true");
    q.addQueryItem("codex_cli_simplified_flow",     "true");
    q.addQueryItem("originator",                    sv(kOriginator));
    authorize.setQuery(q);

    // Open the user's default browser via xdg-open. Avoids the Qt6::Gui
    // dependency on this service library (services link Qt6::Network only).
    if (!QProcess::startDetached("xdg-open", {authorize.toString(QUrl::FullyEncoded)})) {
        emit signInFailed(providerKey, tr("Could not open browser (xdg-open failed)"));
        server_.reset();
        return;
    }

    server_->awaitCallback(
        kCallbackTimeoutSeconds,
        [this, providerKey, state, verifier, redirectUri](OAuthCallback cb) {
            onCallbackSuccess(providerKey, state, verifier, redirectUri, cb.code, cb.state);
        },
        [this, providerKey](QString msg) {
            onCallbackError(providerKey, msg);
        });
}

void ChatGPTOAuthService::onCallbackSuccess(const QString& providerKey,
                                            const QString& expectedState,
                                            const QString& verifier,
                                            const QString& redirectUri,
                                            const QString& code,
                                            const QString& callbackState) {
    if (callbackState != expectedState) {
        emit signInFailed(providerKey,
            tr("OAuth state mismatch — sign-in aborted (possible CSRF)"));
        return;
    }
    exchangeCodeForTokens(providerKey, code, verifier, redirectUri);
}

void ChatGPTOAuthService::onCallbackError(const QString& providerKey, const QString& message) {
    emit signInFailed(providerKey, message);
}

void ChatGPTOAuthService::exchangeCodeForTokens(const QString& providerKey,
                                                 const QString& code,
                                                 const QString& verifier,
                                                 const QString& redirectUri) {
    using namespace oauth;
    QUrl tokenUrl(sv(kIssuer) + sv(kTokenPath));
    QNetworkRequest req(tokenUrl);
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  "application/x-www-form-urlencoded");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);

    QUrlQuery body;
    body.addQueryItem("grant_type",    "authorization_code");
    body.addQueryItem("code",          code);
    body.addQueryItem("redirect_uri",  redirectUri);
    body.addQueryItem("client_id",     sv(kClientId));
    body.addQueryItem("code_verifier", verifier);
    QByteArray payload = body.toString(QUrl::FullyEncoded).toUtf8();

    QNetworkReply* reply = nam_->post(req, payload);
    connect(reply, &QNetworkReply::finished, this, [this, reply, providerKey]() {
        reply->deleteLater();
        int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QByteArray data = reply->readAll();
        if (status < 200 || status >= 300) {
            emit signInFailed(providerKey,
                tr("Token exchange failed (HTTP %1): %2")
                    .arg(status).arg(parseErrorBody(data)));
            return;
        }
        try {
            auto j = nlohmann::json::parse(data.toStdString());
            ChatGPTTokenBundle bundle;
            bundle.accessToken  = j.value("access_token", "");
            bundle.refreshToken = j.value("refresh_token", "");
            bundle.idToken      = j.value("id_token", "");
            if (bundle.accessToken.empty() || bundle.refreshToken.empty()) {
                emit signInFailed(providerKey,
                    tr("Token exchange returned no access_token / refresh_token"));
                return;
            }
            applyClaims(bundle);
            persistBundle(providerKey, bundle);
            emit signInCompleted(providerKey, bundle);
        } catch (const std::exception& e) {
            emit signInFailed(providerKey,
                tr("Token exchange returned an unexpected payload: ") + e.what());
        }
    });
}

void ChatGPTOAuthService::requestFreshToken(const QString& providerKey) {
    auto bundle = currentBundle(providerKey);
    if (!bundle) {
        emit signInFailed(providerKey, tr("Not signed in"));
        return;
    }
    auto exp = oauth::JwtDecoder::expiration(QString::fromStdString(bundle->accessToken));
    bool stale = !exp || (std::chrono::system_clock::now() +
                          std::chrono::seconds(oauth::kRefreshSkewSeconds) >= *exp);
    if (!stale) {
        emit tokenRefreshed(providerKey, *bundle);
        return;
    }
    {
        std::lock_guard lk(mu_);
        if (refreshInFlight_) return;  // coalesce; the in-flight call will emit
        refreshInFlight_ = true;
    }
    performRefresh(providerKey, *bundle);
}

void ChatGPTOAuthService::performRefresh(const QString& providerKey,
                                         ChatGPTTokenBundle bundle) {
    using namespace oauth;
    QUrl tokenUrl(sv(kIssuer) + sv(kTokenPath));
    QNetworkRequest req(tokenUrl);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    nlohmann::json body = {
        {"client_id",     std::string(kClientId)},
        {"grant_type",    "refresh_token"},
        {"refresh_token", bundle.refreshToken},
    };
    QByteArray payload = QByteArray::fromStdString(body.dump());

    QNetworkReply* reply = nam_->post(req, payload);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, providerKey, bundle]() mutable {
        reply->deleteLater();
        {
            std::lock_guard lk(mu_);
            refreshInFlight_ = false;
        }
        int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QByteArray data = reply->readAll();

        if (status >= 200 && status < 300) {
            try {
                auto j = nlohmann::json::parse(data.toStdString());
                bundle.accessToken = j.value("access_token", bundle.accessToken);
                if (j.contains("refresh_token") && j["refresh_token"].is_string()) {
                    auto rt = j["refresh_token"].get<std::string>();
                    if (!rt.empty()) bundle.refreshToken = rt;
                }
                if (j.contains("id_token") && j["id_token"].is_string()) {
                    auto idt = j["id_token"].get<std::string>();
                    if (!idt.empty()) {
                        bundle.idToken = idt;
                        applyClaims(bundle);
                    }
                }
                bundle.obtainedAt = std::chrono::system_clock::now();
                persistBundle(providerKey, bundle);
                emit tokenRefreshed(providerKey, bundle);
            } catch (...) {
                emit signInFailed(providerKey,
                    tr("Refresh returned an unexpected payload"));
            }
            return;
        }

        // Refresh-token-rejected family: clear the bundle, prompt re-sign-in.
        QString detail = parseErrorBody(data).toLower();
        const QStringList dead = {"refresh_token_expired",
                                  "refresh_token_reused",
                                  "refresh_token_invalidated"};
        for (const auto& d : dead) {
            if (detail.contains(d)) {
                if (secretStore_) secretStore_->deleteChatGPTTokens(providerKey.toStdString());
                emit signInFailed(providerKey,
                    tr("Sign-in expired — please sign in with ChatGPT again"));
                return;
            }
        }
        emit signInFailed(providerKey,
            tr("Refresh failed (HTTP %1): %2").arg(status).arg(parseErrorBody(data)));
    });
}

void ChatGPTOAuthService::persistBundle(const QString& providerKey,
                                        const ChatGPTTokenBundle& bundle) {
    if (!secretStore_) return;
    std::string raw = bundle.toJson().dump();
    secretStore_->saveChatGPTTokens(providerKey.toStdString(), raw);
}

void ChatGPTOAuthService::applyClaims(ChatGPTTokenBundle& bundle) {
    if (bundle.idToken.empty()) return;
    try {
        auto claims = oauth::JwtDecoder::payload(QString::fromStdString(bundle.idToken));
        if (claims.contains("email") && claims["email"].is_string()) {
            bundle.email = claims["email"].get<std::string>();
        }
        if (claims.contains("chatgpt_account_id") && claims["chatgpt_account_id"].is_string()) {
            bundle.accountId = claims["chatgpt_account_id"].get<std::string>();
        }
        if (claims.contains("chatgpt_plan_type") && claims["chatgpt_plan_type"].is_string()) {
            bundle.planType = claims["chatgpt_plan_type"].get<std::string>();
        }
    } catch (...) {}
}

std::optional<ChatGPTTokenBundle>
ChatGPTOAuthService::currentBundle(const QString& providerKey) const {
    if (!secretStore_) return std::nullopt;
    auto raw = secretStore_->loadChatGPTTokens(providerKey.toStdString());
    if (!raw) return std::nullopt;
    try {
        auto j = nlohmann::json::parse(*raw);
        return ChatGPTTokenBundle::fromJson(j);
    } catch (...) {
        return std::nullopt;
    }
}

void ChatGPTOAuthService::signOut(const QString& providerKey) {
    if (secretStore_) secretStore_->deleteChatGPTTokens(providerKey.toStdString());
}

ChatGPTSignInStatus ChatGPTOAuthService::status(const QString& providerKey) const {
    ChatGPTSignInStatus s;
    auto b = currentBundle(providerKey);
    if (!b) return s;
    s.signedIn = true;
    if (b->email)    s.email    = QString::fromStdString(*b->email);
    if (b->planType) s.planType = QString::fromStdString(*b->planType);
    return s;
}

}  // namespace gridex
