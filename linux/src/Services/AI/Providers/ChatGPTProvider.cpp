#include "Services/AI/Providers/ChatGPTProvider.h"

#include <chrono>
#include <stdexcept>

#include <QByteArray>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QString>
#include <QUrl>
#include <QUrlQuery>

#include <nlohmann/json.hpp>

#include "Core/Errors/GridexError.h"
#include "Core/Models/AI/ChatGPTTokenBundle.h"
#include "Data/Keychain/SecretStore.h"
#include "Services/AI/Auth/ChatGPTOAuthConstants.h"
#include "Services/AI/Auth/JwtDecoder.h"

namespace gridex {

namespace {

constexpr const char* kProviderKey = "chatgpt";

std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n/");
    if (b == std::string::npos) return "";
    auto e = s.find_last_not_of(" \t\r\n/");
    return s.substr(b, e - b + 1);
}

QByteArray httpPost(const QUrl& url,
                    const QByteArray& body,
                    const std::vector<std::pair<QByteArray, QByteArray>>& headers,
                    int* outStatus,
                    QString* outError) {
    QNetworkAccessManager nam;
    QNetworkRequest req(url);
    for (const auto& [k, v] : headers) req.setRawHeader(k, v);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);

    QEventLoop loop;
    QNetworkReply* reply = nam.post(req, body);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    if (outStatus) *outStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (outError) *outError = reply->errorString();
    QByteArray data = reply->readAll();
    reply->deleteLater();
    return data;
}

QByteArray httpGet(const QUrl& url,
                   const std::vector<std::pair<QByteArray, QByteArray>>& headers,
                   int* outStatus,
                   QString* outError) {
    QNetworkAccessManager nam;
    QNetworkRequest req(url);
    for (const auto& [k, v] : headers) req.setRawHeader(k, v);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);

    QEventLoop loop;
    QNetworkReply* reply = nam.get(req);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    if (outStatus) *outStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (outError) *outError = reply->errorString();
    QByteArray data = reply->readAll();
    reply->deleteLater();
    return data;
}

// Loads the bundle, refreshes the access_token in-process if it's within
// kRefreshSkewSeconds of expiring, persists the rotated bundle, and returns it.
ChatGPTTokenBundle loadAndMaybeRefresh() {
    SecretStore store;
    auto raw = store.loadChatGPTTokens(kProviderKey);
    if (!raw) throw AuthenticationError("ChatGPT: not signed in");
    auto j = nlohmann::json::parse(*raw);
    auto bundleOpt = ChatGPTTokenBundle::fromJson(j);
    if (!bundleOpt) throw AuthenticationError("ChatGPT: stored token bundle is malformed");
    ChatGPTTokenBundle bundle = *bundleOpt;

    auto exp = oauth::JwtDecoder::expiration(QString::fromStdString(bundle.accessToken));
    bool stale = !exp || (std::chrono::system_clock::now() +
                          std::chrono::seconds(oauth::kRefreshSkewSeconds) >= *exp);
    if (!stale) return bundle;

    // Refresh — POST application/json {client_id, grant_type, refresh_token}
    QUrl tokenUrl(QString::fromUtf8(oauth::kIssuer.data(),
                                    static_cast<int>(oauth::kIssuer.size())) +
                  QString::fromUtf8(oauth::kTokenPath.data(),
                                    static_cast<int>(oauth::kTokenPath.size())));
    nlohmann::json body = {
        {"client_id",     std::string(oauth::kClientId)},
        {"grant_type",    "refresh_token"},
        {"refresh_token", bundle.refreshToken},
    };
    int status = 0;
    QString err;
    QByteArray data = httpPost(tokenUrl, QByteArray::fromStdString(body.dump()),
                               {{"Content-Type", "application/json"}}, &status, &err);
    if (status < 200 || status >= 300) {
        // Refresh-token-rejected family — wipe the bundle and surface
        // AuthenticationError so the UI prompts re-sign-in.
        try {
            auto e = nlohmann::json::parse(data.toStdString());
            std::string detail = e.value("error", std::string{});
            if (detail == "refresh_token_expired" ||
                detail == "refresh_token_reused"  ||
                detail == "refresh_token_invalidated") {
                store.deleteChatGPTTokens(kProviderKey);
            }
        } catch (...) {}
        throw AuthenticationError("ChatGPT refresh failed (HTTP " +
                                  std::to_string(status) + "): " +
                                  std::string(data.constData(), data.size()));
    }
    auto rj = nlohmann::json::parse(data.toStdString());
    bundle.accessToken = rj.value("access_token", bundle.accessToken);
    if (rj.contains("refresh_token") && rj["refresh_token"].is_string()) {
        std::string rt = rj["refresh_token"].get<std::string>();
        if (!rt.empty()) bundle.refreshToken = rt;
    }
    if (rj.contains("id_token") && rj["id_token"].is_string()) {
        std::string idt = rj["id_token"].get<std::string>();
        if (!idt.empty()) bundle.idToken = idt;
    }
    bundle.obtainedAt = std::chrono::system_clock::now();
    store.saveChatGPTTokens(kProviderKey, bundle.toJson().dump());
    return bundle;
}

std::string roleToRpc(LLMMessage::Role role) {
    switch (role) {
        case LLMMessage::Role::System:    return "system";
        case LLMMessage::Role::User:      return "user";
        case LLMMessage::Role::Assistant: return "assistant";
    }
    return "user";
}

// Walk SSE byte stream and concatenate `response.output_text.delta` payloads.
std::string parseResponsesSse(const QByteArray& body) {
    std::string out;
    QList<QByteArray> lines = body.split('\n');
    QByteArray currentEvent;
    for (auto& raw : lines) {
        QByteArray line = raw.endsWith('\r') ? raw.left(raw.size() - 1) : raw;
        if (line.isEmpty()) { currentEvent.clear(); continue; }
        if (line.startsWith(":")) continue;       // SSE comment
        if (line.startsWith("event: ")) {
            currentEvent = line.mid(7);
            continue;
        }
        if (!line.startsWith("data: ")) continue;
        QByteArray payload = line.mid(6);
        if (payload == "[DONE]") break;
        if (currentEvent == "response.output_text.delta") {
            try {
                auto j = nlohmann::json::parse(payload.toStdString());
                if (j.contains("delta") && j["delta"].is_string()) {
                    out += j["delta"].get<std::string>();
                }
            } catch (...) {}
        } else if (currentEvent == "response.error") {
            try {
                auto j = nlohmann::json::parse(payload.toStdString());
                std::string m;
                if (j.contains("error") && j["error"].is_object() &&
                    j["error"].contains("message"))
                    m = j["error"]["message"].get<std::string>();
                else if (j.contains("message"))
                    m = j["message"].get<std::string>();
                else
                    m = payload.toStdString();
                throw QueryError("ChatGPT response.error: " + m);
            } catch (const QueryError&) {
                throw;
            } catch (...) {}
        } else if (currentEvent == "response.completed") {
            return out;
        }
    }
    return out;
}

}  // namespace

ChatGPTProvider::ChatGPTProvider(const std::string& /*apiKey*/, const std::string& baseUrl) {
    std::string base = trim(baseUrl);
    baseUrl_ = base.empty() ? std::string(oauth::kChatGPTBackend) : base;
    while (!baseUrl_.empty() && baseUrl_.back() == '/') baseUrl_.pop_back();
}

std::string ChatGPTProvider::sendMessage(const std::vector<LLMMessage>& messages,
                                         const std::string& systemPrompt,
                                         const std::string& model,
                                         int /*maxTokens*/, double /*temperature*/) {
    auto bundle = loadAndMaybeRefresh();

    nlohmann::json input = nlohmann::json::array();
    for (const auto& m : messages) {
        if (m.role == LLMMessage::Role::System) continue;  // → instructions field
        std::string partType = (m.role == LLMMessage::Role::Assistant) ? "output_text" : "input_text";
        input.push_back({
            {"type", "message"},
            {"role", roleToRpc(m.role)},
            {"content", nlohmann::json::array({{ {"type", partType}, {"text", m.content} }})},
        });
    }
    nlohmann::json body = {
        {"model",        model},
        {"instructions", systemPrompt},
        {"input",        input},
        {"stream",       true},
        {"store",        false},
    };

    QUrl url(QString::fromStdString(baseUrl_ + "/responses"));
    std::vector<std::pair<QByteArray, QByteArray>> headers = {
        {"Content-Type",  "application/json"},
        {"Authorization", QByteArrayLiteral("Bearer ") + QByteArray::fromStdString(bundle.accessToken)},
        {"OpenAI-Beta",   "responses=v1"},
    };
    if (bundle.accountId) {
        headers.push_back({"ChatGPT-Account-ID", QByteArray::fromStdString(*bundle.accountId)});
    }

    int status = 0;
    QString err;
    QByteArray data = httpPost(url, QByteArray::fromStdString(body.dump()), headers, &status, &err);
    if (status == 401 || status == 403) {
        SecretStore store;
        store.deleteChatGPTTokens(kProviderKey);
        throw AuthenticationError("ChatGPT sign-in expired — sign in again");
    }
    if (status < 200 || status >= 300) {
        std::string snippet(data.constData(), std::min<int>(400, data.size()));
        throw QueryError("ChatGPT /responses HTTP " + std::to_string(status) + ": " + snippet);
    }
    return parseResponsesSse(data);
}

std::vector<LLMModel> ChatGPTProvider::availableModels() {
    auto bundle = loadAndMaybeRefresh();
    QUrl url(QString::fromStdString(baseUrl_ + "/models"));
    QUrlQuery q;
    q.addQueryItem("client_version", "1.0.0");
    url.setQuery(q);

    std::vector<std::pair<QByteArray, QByteArray>> headers = {
        {"Authorization", QByteArrayLiteral("Bearer ") + QByteArray::fromStdString(bundle.accessToken)},
    };
    if (bundle.accountId) {
        headers.push_back({"ChatGPT-Account-ID", QByteArray::fromStdString(*bundle.accountId)});
    }

    int status = 0;
    QString err;
    QByteArray data = httpGet(url, headers, &status, &err);
    if (status < 200 || status >= 300) return {};

    std::vector<LLMModel> out;
    try {
        auto j = nlohmann::json::parse(data.toStdString());
        if (!j.contains("models") || !j["models"].is_array()) return out;
        for (const auto& m : j["models"]) {
            if (!m.contains("slug") || !m["slug"].is_string()) continue;
            bool supported  = m.value("supported_in_api", true);
            std::string vis = m.value("visibility", std::string("list"));
            if (!supported || vis != "list") continue;
            LLMModel mm;
            mm.id           = m["slug"].get<std::string>();
            mm.name         = m.value("name", mm.id);
            mm.provider     = "ChatGPT";
            mm.contextWindow = m.value("context_window", 128000);
            out.push_back(std::move(mm));
        }
    } catch (...) {}
    return out;
}

bool ChatGPTProvider::validateAPIKey() {
    SecretStore store;
    return store.loadChatGPTTokens(kProviderKey).has_value();
}

}  // namespace gridex
