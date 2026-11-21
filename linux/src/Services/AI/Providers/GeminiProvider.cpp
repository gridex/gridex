#include "Services/AI/Providers/GeminiProvider.h"

#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <nlohmann/json.hpp>

#include "Core/Errors/GridexError.h"
#include "Services/AI/Providers/ProviderHttp.h"

namespace gridex {

namespace {

QByteArray qtGet(const QString& url) {
    QNetworkAccessManager nam;
    QNetworkRequest       req{QUrl{url}};
    QNetworkReply* reply = nam.get(req);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    if (reply->error() != QNetworkReply::NoError) {
        const std::string msg = reply->errorString().toStdString();
        reply->deleteLater();
        throw NetworkError("Network error: " + msg);
    }
    const int statusCode =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray data = reply->readAll();
    reply->deleteLater();
    if (statusCode < 200 || statusCode >= 300) {
        throw GridexError(ErrorCategory::Network,
            "Gemini HTTP " + std::to_string(statusCode) + ": "
            + data.left(200).toStdString());
    }
    return data;
}

}  // namespace

GeminiProvider::GeminiProvider(std::string apiKey, std::string baseUrl)
    : apiKey_(std::move(apiKey)), baseUrl_(std::move(baseUrl)) {
    while (!baseUrl_.empty() && baseUrl_.back() == '/') baseUrl_.pop_back();
    if (baseUrl_.empty()) baseUrl_ = "https://generativelanguage.googleapis.com/v1beta";
}

std::string GeminiProvider::sendMessage(
    const std::vector<LLMMessage>& messages,
    const std::string&             systemPrompt,
    const std::string&             model,
    int                            maxTokens,
    double                         temperature)
{
    using json = nlohmann::json;

    // Gemini uses "user"/"model" roles, not "user"/"assistant".
    json contents = json::array();
    for (const auto& m : messages) {
        const std::string geminiRole =
            (m.role == LLMMessage::Role::Assistant) ? "model" : "user";
        contents.push_back({
            {"role",  geminiRole},
            {"parts", json::array({{{"text", m.content}}})}
        });
    }

    json body;
    body["contents"] = contents;
    if (!systemPrompt.empty()) {
        body["systemInstruction"] = {
            {"parts", json::array({{{"text", systemPrompt}}})}
        };
    }
    body["generationConfig"] = {
        {"maxOutputTokens", maxTokens},
        {"temperature",     temperature}
    };

    const QString url = QString::fromStdString(baseUrl_ + "/models/" + model)
        + ":generateContent?key="
        + QString::fromStdString(apiKey_);

    const QByteArray raw = postWithRetry(
        url,
        {{"Content-Type", "application/json"}},
        QByteArray::fromStdString(body.dump()),
        "Gemini");

    // {"candidates":[{"content":{"parts":[{"text":"..."}]}}]}
    const auto resp = json::parse(raw.toStdString());
    if (resp.contains("candidates") && resp["candidates"].is_array()
        && !resp["candidates"].empty()) {
        const auto& cand = resp["candidates"][0];
        if (cand.contains("content")
            && cand["content"].contains("parts")
            && cand["content"]["parts"].is_array()
            && !cand["content"]["parts"].empty()) {
            return cand["content"]["parts"][0]["text"].get<std::string>();
        }
    }
    throw GridexError(ErrorCategory::Internal,
        "Gemini: unexpected response: " + raw.left(200).toStdString());
}

std::vector<LLMModel> GeminiProvider::availableModels() {
    if (apiKey_.empty()) return {};
    try {
        const QString url = QString::fromStdString(baseUrl_ + "/models?key=")
            + QString::fromStdString(apiKey_);
        const QByteArray raw = qtGet(url);
        const auto j = nlohmann::json::parse(raw.toStdString());
        std::vector<LLMModel> out;
        if (j.contains("models") && j["models"].is_array()) {
            for (const auto& m : j["models"]) {
                // Only generateContent-capable models.
                bool supports = false;
                if (m.contains("supportedGenerationMethods")) {
                    for (const auto& meth : m["supportedGenerationMethods"]) {
                        if (meth.get<std::string>() == "generateContent") { supports = true; break; }
                    }
                }
                if (!supports) continue;
                std::string name = m.value("name", std::string{});
                if (name.rfind("models/", 0) == 0) name = name.substr(7);
                if (name.empty()) continue;
                const std::string display = m.value("displayName", name);
                out.push_back({name, display, "Gemini", 1000000, true});
            }
        }
        return out;
    } catch (...) {
        return {};
    }
}

bool GeminiProvider::validateAPIKey() { return !apiKey_.empty(); }

}  // namespace gridex
