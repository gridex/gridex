#pragma once

#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>
#include <string>

namespace gridex {

// Blocking POST for the AI providers, retried on transient failures.
//
// Mirrors macos/Core/Types/LLMRetry.swift: up to 3 attempts with jittered
// exponential backoff (0.5 s, 1 s, capped at 8 s) on 408, 425, 429, 5xx and on
// transient network errors (refused, reset, timeout, DNS). A Retry-After of up
// to 30 s is honored. Any other status (400, 401, 403, 404, ...) fails at once.
//
// After the last attempt the same errors as before are thrown: NetworkError for
// a network failure, GridexError(Network, "<label> HTTP <code>: <body>") for an
// HTTP status. Must run off the GUI thread (QtConcurrent worker), like before.
QByteArray postWithRetry(const QString&                       url,
                         const QList<QPair<QString, QString>>& headers,
                         const QByteArray&                    body,
                         const std::string&                   label);

}  // namespace gridex
