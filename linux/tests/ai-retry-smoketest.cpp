// AI provider HTTP retry smoke: postWithRetry against a local scripted server.

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

#include <QByteArray>
#include <QCoreApplication>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>

#include "Core/Errors/GridexError.h"
#include "Services/AI/Providers/ProviderHttp.h"

using namespace gridex;

// Answers each request with the next scripted (status, extra headers, body).
// Runs on the test thread: postWithRetry's local QEventLoop drives it.
struct ScriptedServer {
    struct Reply { int status; QByteArray headers; QByteArray body; };

    QTcpServer         server;
    std::vector<Reply> script;
    int                requests = 0;

    explicit ScriptedServer(std::vector<Reply> s) : script(std::move(s)) {
        bool ok = server.listen(QHostAddress::LocalHost, 0);
        assert(ok);
        (void)ok;
        QObject::connect(&server, &QTcpServer::newConnection, [this] {
            while (QTcpSocket* sock = server.nextPendingConnection()) {
                auto* buf = new QByteArray;
                QObject::connect(sock, &QTcpSocket::readyRead, [this, sock, buf] {
                    buf->append(sock->readAll());
                    const int end = buf->indexOf("\r\n\r\n");
                    if (end < 0) return;
                    const int cl = buf->toLower().indexOf("content-length:");
                    const int len = cl < 0 ? 0
                        : buf->mid(cl + 15, buf->indexOf("\r\n", cl) - cl - 15).trimmed().toInt();
                    if (buf->size() < end + 4 + len) return;
                    const Reply r = script.at(static_cast<size_t>(requests++));
                    QByteArray out = "HTTP/1.1 " + QByteArray::number(r.status) + " X\r\n"
                        + "Content-Type: application/json\r\n"
                        + "Content-Length: " + QByteArray::number(r.body.size()) + "\r\n"
                        + r.headers + "Connection: close\r\n\r\n" + r.body;
                    sock->write(out);
                    sock->disconnectFromHost();
                    delete buf;
                });
            }
        });
    }

    QString url() const {
        return QString("http://127.0.0.1:%1/v1/messages").arg(server.serverPort());
    }
};

static QByteArray post(const ScriptedServer& s) {
    return postWithRetry(s.url(), {{"Content-Type", "application/json"}}, "{}", "Test");
}

static void testRecoversFrom503() {
    ScriptedServer s({{503, "", "busy"}, {503, "", "busy"}, {200, "", "{\"ok\":1}"}});
    assert(post(s) == "{\"ok\":1}");
    assert(s.requests == 3);
    std::printf("  503, 503, 200 -> ok after 3 requests\n");
}

static void testHonorsShortRetryAfter() {
    ScriptedServer s({{429, "Retry-After: 0\r\n", "slow down"}, {200, "", "{\"ok\":2}"}});
    assert(post(s) == "{\"ok\":2}");
    assert(s.requests == 2);
    std::printf("  429 + Retry-After: 0, 200 -> ok after 2 requests\n");
}

static void testClientErrorFailsAtOnce() {
    ScriptedServer s({{401, "", "{\"error\":\"bad key\"}"}});
    try {
        post(s);
        assert(false && "401 must throw");
    } catch (const NetworkError&) {
        assert(false && "an HTTP status is not a NetworkError");
    } catch (const GridexError& e) {
        assert(e.category() == ErrorCategory::Network);
        assert(std::string(e.what()).rfind("Test HTTP 401: ", 0) == 0);
    }
    assert(s.requests == 1);
    std::printf("  401 -> GridexError after 1 request\n");
}

static void testLongRetryAfterFailsAtOnce() {
    ScriptedServer s({{429, "Retry-After: 120\r\n", "come back later"}});
    try {
        post(s);
        assert(false && "429 with a long Retry-After must throw");
    } catch (const GridexError& e) {
        assert(std::string(e.what()).rfind("Test HTTP 429: ", 0) == 0);
    }
    assert(s.requests == 1);
    std::printf("  429 + Retry-After: 120 -> GridexError after 1 request\n");
}

static void testGivesUpAfterThree() {
    ScriptedServer s({{529, "", "overloaded"}, {529, "", "overloaded"}, {529, "", "overloaded"}});
    try {
        post(s);
        assert(false && "three 529s must throw");
    } catch (const GridexError& e) {
        assert(std::string(e.what()) == "Test HTTP 529: overloaded");
    }
    assert(s.requests == 3);
    std::printf("  529 x3 -> GridexError after 3 requests\n");
}

static void testRefusedConnectionIsNetworkError() {
    QTcpServer probe;
    probe.listen(QHostAddress::LocalHost, 0);
    const quint16 port = probe.serverPort();
    probe.close();  // nothing listens on this port now
    try {
        postWithRetry(QString("http://127.0.0.1:%1/").arg(port), {}, "{}", "Test");
        assert(false && "refused connection must throw");
    } catch (const NetworkError& e) {
        assert(std::string(e.what()).rfind("Network error: ", 0) == 0);
    }
    std::printf("  connection refused -> NetworkError after retries\n");
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    std::printf("ai-retry-smoketest\n");
    testRecoversFrom503();
    testHonorsShortRetryAfter();
    testClientErrorFailsAtOnce();
    testLongRetryAfterFailsAtOnce();
    testGivesUpAfterThree();
    testRefusedConnectionIsNetworkError();
    std::printf("OK\n");
    return 0;
}
