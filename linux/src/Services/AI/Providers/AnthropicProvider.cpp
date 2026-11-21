#include "Services/AI/Providers/AnthropicProvider.h"

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
            "Anthropic HTTP " + std::to_string(statusCode) + ": "
            + data.left(200).toStdString());
    }
    return data;
}

}  // namespace

AnthropicProvider::AnthropicProvider(std::string apiKey, std::string baseUrl)
    : apiKey_(std::move(apiKey)), baseUrl_(std::move(baseUrl)) {
    while (!baseUrl_.empty() && baseUrl_.back() == '/') baseUrl_.pop_back();
    if (baseUrl_.empty()) baseUrl_ = "https://api.anthropic.com";
}

std::string AnthropicProvider::sendMessage(
    const std::vector<LLMMessage>& messages,
    const std::string&             systemPrompt,
    const std::string&             model,
    int                            maxTokens,
    double                         temperature)
{
    using json = nlohmann::json;

    json body;
    body["model"]       = model;
    body["max_tokens"]  = maxTokens;
    body["temperature"] = temperature;
    if (!systemPrompt.empty()) body["system"] = systemPrompt;

    json msgs = json::array();
    for (const auto& m : messages) {
        msgs.push_back({
            {"role",    LLMMessage::roleString(m.role)},
            {"content", m.content}
        });
    }
    body["messages"] = msgs;

    const QByteArray raw = postWithRetry(
        QString::fromStdString(baseUrl_ + "/v1/messages"),
        {
            {"Content-Type",      "application/json"},
            {"x-api-key",         QString::fromStdString(apiKey_)},
            {"anthropic-version", "2023-06-01"}
        },
        QByteArray::fromStdString(body.dump()),
        "Anthropic");

    const auto resp = json::parse(raw.toStdString());
    if (resp.contains("content") && resp["content"].is_array()
        && !resp["content"].empty()) {
        const auto& first = resp["content"][0];
        if (first.contains("text")) return first["text"].get<std::string>();
    }
    throw GridexError(ErrorCategory::Internal,
        "Anthropic: unexpected response: " + raw.left(200).toStdString());
}

std::vector<LLMModel> AnthropicProvider::availableModels() {
    if (apiKey_.empty()) return {};
    try {
        const QByteArray raw = qtGet(
            QString::fromStdString(baseUrl_ + "/v1/models?limit=100"),
            {{"x-api-key",         QString::fromStdString(apiKey_)},
             {"anthropic-version", QStringLiteral("2023-06-01")}});
        const auto j = nlohmann::json::parse(raw.toStdString());
        std::vector<LLMModel> out;
        if (j.contains("data") && j["data"].is_array()) {
            for (const auto& m : j["data"]) {
                const std::string id   = m.value("id", std::string{});
                const std::string name = m.value("display_name", id);
                if (id.empty()) continue;
                out.push_back({id, name, "Anthropic", 200000, true});
            }
        }
        return out;
    } catch (...) {
        return {};
    }
}

bool AnthropicProvider::validateAPIKey() {
    return !apiKey_.empty();
}

}  // namespace gridex
