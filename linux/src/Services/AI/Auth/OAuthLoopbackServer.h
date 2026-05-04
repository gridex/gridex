#pragma once

// Single-shot loopback HTTP listener for the ChatGPT OAuth callback.
// Binds 127.0.0.1 (not 0.0.0.0) so LAN-side hijack is impossible.
// Reuse-after-success is not supported — instantiate per sign-in attempt.

#include <atomic>
#include <cstdint>
#include <functional>

#include <QObject>
#include <QString>
#include <QTimer>

class QTcpServer;
class QTcpSocket;

namespace gridex::oauth {

struct OAuthCallback {
    QString code;
    QString state;
};

class OAuthLoopbackServer : public QObject {
    Q_OBJECT
public:
    using SuccessFn = std::function<void(OAuthCallback)>;
    using ErrorFn   = std::function<void(QString)>;

    explicit OAuthLoopbackServer(QObject* parent = nullptr);
    ~OAuthLoopbackServer() override;

    // Tries each preferred port in order, falling back to a kernel-assigned
    // ephemeral port. Returns the bound port (0 on failure).
    std::uint16_t start();

    // Registers callbacks for the next callback / failure. Exactly one of
    // them is invoked once. Calling start() again on a resolved server is
    // not supported.
    void awaitCallback(int timeoutSeconds, SuccessFn onSuccess, ErrorFn onError);

    void stop();

    std::uint16_t port() const noexcept { return port_; }

private:
    void handleNewConnection();
    void handleClientReadyRead(QTcpSocket* sock);
    void processRequestLine(QTcpSocket* sock, const QString& requestLine);
    void sendHtml(QTcpSocket* sock, int status, const QString& body);
    void resolveSuccess(OAuthCallback cb);
    void resolveError(QString message);

    QTcpServer* server_ = nullptr;
    std::uint16_t port_ = 0;
    QTimer timeout_;

    SuccessFn onSuccess_;
    ErrorFn   onError_;
    std::atomic<bool> resolved_{false};
};

}  // namespace gridex::oauth
