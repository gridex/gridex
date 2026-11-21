#include "Services/AI/Providers/OpenAIProvider.h"

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

QByteArray qtGet(const QString&                      url,
                 const QList<QPair<QString,QString>>& headers) {
    QNetworkAccessManager nam;
    QNetworkRequest       req{QUrl{url}};
    for (const auto& [k, v] : headers) req.setRawHeader(k.toUtf8(), v.toUtf8());

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
            "OpenAI HTTP " + std::to_string(statusCode) + ": "
            + data.left(200).toStdString());
    }
    return data;
}

}  // namespace

OpenAIProvider::OpenAIProvider(std::string apiKey, std::string baseUrl)
    : apiKey_(std::move(apiKey)), baseUrl_(std::move(baseUrl)) {
    while (!baseUrl_.empty() && baseUrl_.back() == '/') baseUrl_.pop_back();
    if (baseUrl_.empty()) baseUrl_ = "https://api.openai.com/v1";
}

std::string OpenAIProvider::sendMessage(
    const std::vector<LLMMessage>& messages,
    const std::string&             systemPrompt,
    const std::string&             model,
    int                            maxTokens,
    double                         temperature)
{
    using json = nlohmann::json;

    json msgs = json::array();
    if (!systemPrompt.empty())
        msgs.push_back({{"role", "system"}, {"content", systemPrompt}});
    for (const auto& m : messages)
        msgs.push_back({{"role", LLMMessage::roleString(m.role)}, {"content", m.content}});

    json body;
    body["model"]       = model;
    body["max_tokens"]  = maxTokens;
    body["temperature"] = temperature;
    body["messages"]    = msgs;

    const QByteArray raw = postWithRetry(
        QString::fromStdString(baseUrl_ + "/chat/completions"),
        {
            {"Content-Type",  "application/json"},
            {"Authorization", "Bearer " + QString::fromStdString(apiKey_)}
        },
        QByteArray::fromStdString(body.dump()),
        "OpenAI");

    const auto resp = json::parse(raw.toStdString());
    if (resp.contains("choices") && resp["choices"].is_array()
        && !resp["choices"].empty()) {
        const auto& first = resp["choices"][0];
        if (first.contains("message") && first["message"].contains("content"))
            return first["message"]["content"].get<std::string>();
    }
    throw GridexError(ErrorCategory::Internal,
        "OpenAI: unexpected response: " + raw.left(200).toStdString());
}

std::vector<LLMModel> OpenAIProvider::availableModels() {
    // Only live /models from the API — no hardcoded fallback. Empty result
    // signals the UI to show "enter API key" / "check key" state.
    if (apiKey_.empty()) return {};
    try {
        const QByteArray raw = qtGet(
            QString::fromStdString(baseUrl_ + "/models"),
            {{"Authorization", QString::fromStdString("Bearer " + apiKey_)}});
        const auto j = nlohmann::json::parse(raw.toStdString());
        std::vector<LLMModel> out;
        if (j.contains("data") && j["data"].is_array()) {
            for (const auto& m : j["data"]) {
                const std::string id = m.value("id", std::string{});
                if (id.empty()) continue;
                out.push_back({id, id, "OpenAI", 128000, true});
            }
        }
        return out;
    } catch (...) {
        return {};
    }
}

bool OpenAIProvider::validateAPIKey() { return !apiKey_.empty(); }

}  // namespace gridex
