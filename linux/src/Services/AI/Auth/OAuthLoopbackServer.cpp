#include "Services/AI/Auth/OAuthLoopbackServer.h"

#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>

#include "Services/AI/Auth/ChatGPTOAuthConstants.h"

namespace gridex::oauth {

namespace {

constexpr const char* kSuccessPage = R"(<!doctype html><html><head><meta charset="utf-8">
<title>Gridex sign-in</title>
<style>body{font:14px system-ui;color:#cdd6f4;background:#1e1e2e;text-align:center;padding:80px}
h2{color:#a6e3a1}</style></head><body>
<h2>Sign-in complete</h2>
<p>You can close this tab and return to Gridex.</p>
<script>setTimeout(()=>window.close(),500)</script></body></html>)";

constexpr const char* kErrorPage = R"(<!doctype html><html><head><meta charset="utf-8">
<title>Gridex sign-in</title>
<style>body{font:14px system-ui;color:#cdd6f4;background:#1e1e2e;text-align:center;padding:80px}
h2{color:#f38ba8}</style></head><body>
<h2>Sign-in failed</h2>
<p>Return to Gridex for details. You can close this tab.</p></body></html>)";

}  // namespace

OAuthLoopbackServer::OAuthLoopbackServer(QObject* parent)
    : QObject(parent) {
    timeout_.setSingleShot(true);
    connect(&timeout_, &QTimer::timeout, this, [this] {
        resolveError(tr("OAuth callback timed out"));
    });
}

OAuthLoopbackServer::~OAuthLoopbackServer() { stop(); }

std::uint16_t OAuthLoopbackServer::start() {
    server_ = new QTcpServer(this);
    connect(server_, &QTcpServer::newConnection, this,
            &OAuthLoopbackServer::handleNewConnection);

    for (auto p : kPreferredPorts) {
        if (server_->listen(QHostAddress::LocalHost, p)) {
            port_ = p;
            return p;
        }
    }
    // Last resort: kernel-assigned ephemeral port.
    if (server_->listen(QHostAddress::LocalHost, 0)) {
        port_ = static_cast<std::uint16_t>(server_->serverPort());
        return port_;
    }
    delete server_;
    server_ = nullptr;
    return 0;
}

void OAuthLoopbackServer::awaitCallback(int timeoutSeconds,
                                        SuccessFn onSuccess, ErrorFn onError) {
    onSuccess_ = std::move(onSuccess);
    onError_   = std::move(onError);
    timeout_.start(timeoutSeconds * 1000);
}

void OAuthLoopbackServer::stop() {
    timeout_.stop();
    if (server_) {
        server_->close();
        server_->deleteLater();
        server_ = nullptr;
    }
}

void OAuthLoopbackServer::handleNewConnection() {
    while (auto* sock = server_->nextPendingConnection()) {
        connect(sock, &QTcpSocket::readyRead, this, [this, sock] {
            handleClientReadyRead(sock);
        });
        connect(sock, &QTcpSocket::disconnected, sock, &QTcpSocket::deleteLater);
    }
}

void OAuthLoopbackServer::handleClientReadyRead(QTcpSocket* sock) {
    // Buffer the read on the socket itself — Qt keeps appended bytes
    // available via `peek/readAll`. Stop reading once we have the
    // request line (CRLF or LF).
    QByteArray buf = sock->peek(8192);
    int eol = buf.indexOf("\r\n");
    if (eol < 0) eol = buf.indexOf('\n');
    if (eol < 0) {
        if (buf.size() > 16 * 1024) {
            sendHtml(sock, 400, kErrorPage);
            resolveError(tr("OAuth callback: headers exceed 16KB"));
        }
        return;  // need more bytes
    }
    QString line = QString::fromUtf8(buf.left(eol));
    sock->read(eol + 1);  // consume request line — we don't read further
    processRequestLine(sock, line);
}

void OAuthLoopbackServer::processRequestLine(QTcpSocket* sock, const QString& line) {
    auto parts = line.split(' ');
    if (parts.size() < 2) {
        sendHtml(sock, 400, kErrorPage);
        resolveError(tr("OAuth callback: malformed request line"));
        return;
    }
    QString pathAndQuery = parts[1];
    QUrl url(QStringLiteral("http://localhost") + pathAndQuery);
    if (!url.isValid()) {
        sendHtml(sock, 400, kErrorPage);
        resolveError(tr("OAuth callback: could not parse path"));
        return;
    }

    // /favicon.ico and stray probes — just send the error page and keep listening.
    if (url.path() != QString::fromUtf8(kCallbackPath.data(),
                                        static_cast<int>(kCallbackPath.size()))) {
        sendHtml(sock, 404, kErrorPage);
        return;
    }

    QUrlQuery q(url);
    QString code    = q.queryItemValue("code");
    QString state   = q.queryItemValue("state");
    QString errCode = q.queryItemValue("error");
    QString errDesc = q.queryItemValue("error_description");

    if (!errCode.isEmpty()) {
        sendHtml(sock, 400, kErrorPage);
        resolveError(QString("Authorization server returned '%1': %2").arg(errCode, errDesc));
        return;
    }
    if (code.isEmpty() || state.isEmpty()) {
        sendHtml(sock, 400, kErrorPage);
        resolveError(tr("OAuth callback: missing code or state"));
        return;
    }

    sendHtml(sock, 200, kSuccessPage);
    OAuthCallback cb{std::move(code), std::move(state)};
    resolveSuccess(std::move(cb));
}

void OAuthLoopbackServer::sendHtml(QTcpSocket* sock, int status, const QString& body) {
    QByteArray bytes = body.toUtf8();
    QString head = QString(
        "HTTP/1.1 %1 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %2\r\n"
        "Connection: close\r\n\r\n").arg(status).arg(bytes.size());
    sock->write(head.toUtf8());
    sock->write(bytes);
    sock->flush();
    sock->disconnectFromHost();
}

void OAuthLoopbackServer::resolveSuccess(OAuthCallback cb) {
    bool expected = false;
    if (!resolved_.compare_exchange_strong(expected, true)) return;
    timeout_.stop();
    if (onSuccess_) onSuccess_(std::move(cb));
    onSuccess_ = nullptr;
    onError_   = nullptr;
    QTimer::singleShot(0, this, &OAuthLoopbackServer::stop);
}

void OAuthLoopbackServer::resolveError(QString message) {
    bool expected = false;
    if (!resolved_.compare_exchange_strong(expected, true)) return;
    timeout_.stop();
    if (onError_) onError_(std::move(message));
    onSuccess_ = nullptr;
    onError_   = nullptr;
    QTimer::singleShot(0, this, &OAuthLoopbackServer::stop);
}

}  // namespace gridex::oauth
