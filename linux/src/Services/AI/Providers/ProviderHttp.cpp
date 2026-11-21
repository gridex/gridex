#include "Services/AI/Providers/ProviderHttp.h"

#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#define LLM_RETRY_IMPLEMENTATION
#include <llm/llm_retry.hpp>

#include "Core/Errors/GridexError.h"

namespace gridex {

namespace {

constexpr double kMaxRetryAfterSeconds = 30.0;

bool isTransientNetworkError(QNetworkReply::NetworkError e) {
    switch (e) {
    case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::RemoteHostClosedError:
    case QNetworkReply::HostNotFoundError:
    case QNetworkReply::TimeoutError:
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::NetworkSessionFailedError:
    case QNetworkReply::UnknownNetworkError:
        return true;
    default:
        return false;
    }
}

// Retry-After in delta-seconds form; -1 when absent or an HTTP date.
double retryAfterSeconds(const QNetworkReply* reply) {
    bool ok = false;
    const double s = reply->rawHeader("Retry-After").trimmed().toDouble(&ok);
    return ok && s >= 0 ? s : -1.0;
}

// How the last attempt failed, so the final error keeps its old type and text.
struct LastFailure {
    bool        network = false;
    int         status  = 0;
    std::string text;
};

}  // namespace

QByteArray postWithRetry(const QString&                       url,
                         const QList<QPair<QString, QString>>& headers,
                         const QByteArray&                    body,
                         const std::string&                   label) {
    LastFailure last;

    auto attempt = [&]() -> QByteArray {
        QNetworkAccessManager nam;
        QNetworkRequest       req{QUrl{url}};
        for (const auto& [k, v] : headers) req.setRawHeader(k.toUtf8(), v.toUtf8());

        QNetworkReply* reply = nam.post(req, body);
        QEventLoop loop;
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();

        const QVariant statusAttr =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        if (!statusAttr.isValid()) {
            // No HTTP response at all: DNS, refused, reset, timeout, TLS.
            const auto err = reply->error();
            last = {true, 0, "Network error: " + reply->errorString().toStdString()};
            reply->deleteLater();
            throw llm::LLMError{0, last.text, isTransientNetworkError(err)};
        }

        const int        status     = statusAttr.toInt();
        const double     retryAfter = retryAfterSeconds(reply);
        const QByteArray data       = reply->readAll();
        reply->deleteLater();

        if (status >= 200 && status < 300) return data;

        last = {false, status,
                label + " HTTP " + std::to_string(status) + ": "
                    + data.left(200).toStdString()};
        // with_retry only retries the codes in cfg.retry_on_http_codes. A server
        // asking us to wait longer than a user will sit through gets its error
        // shown now instead of a frozen-looking request.
        llm::LLMError e{status, last.text, retryAfter <= kMaxRetryAfterSeconds};
        e.retry_after_seconds = retryAfter;
        throw e;
    };

    llm::RetryConfig cfg;  // same policy as macos/Core/Types/LLMRetry.swift
    cfg.max_attempts        = 3;
    cfg.base_delay_ms       = 500.0;
    cfg.max_delay_ms        = 8000.0;
    cfg.jitter_factor       = 0.25;
    cfg.retry_on_http_codes = {408, 425, 429};
    for (int s = 500; s <= 599; ++s) cfg.retry_on_http_codes.push_back(s);

    try {
        return llm::with_retry<QByteArray>(attempt, cfg).value;
    } catch (const llm::LLMError&) {
        if (last.network) throw NetworkError(last.text);
        throw GridexError(ErrorCategory::Network, last.text);
    }
}

}  // namespace gridex
