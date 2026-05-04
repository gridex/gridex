#include "Services/AI/Auth/PKCE.h"

#include <QCryptographicHash>
#include <QRandomGenerator>

namespace gridex::oauth {

namespace {

QByteArray randomBytes(int count) {
    // QRandomGenerator::system() wraps a CSPRNG (getrandom on Linux).
    QByteArray out(count, '\0');
    QRandomGenerator::system()->generate(reinterpret_cast<quint32*>(out.data()),
                                         reinterpret_cast<quint32*>(out.data() + count));
    return out;
}

}  // namespace

QString PKCE::base64UrlNoPad(const QByteArray& data) {
    QByteArray b64 = data.toBase64();
    b64.replace('+', '-');
    b64.replace('/', '_');
    while (b64.endsWith('=')) b64.chop(1);
    return QString::fromLatin1(b64);
}

QByteArray PKCE::base64UrlDecode(const QString& s) {
    QByteArray b = s.toLatin1();
    b.replace('-', '+');
    b.replace('_', '/');
    int pad = b.size() % 4;
    if (pad) b.append(4 - pad, '=');
    return QByteArray::fromBase64(b);
}

QString PKCE::makeVerifier() {
    return base64UrlNoPad(randomBytes(32));
}

QString PKCE::challengeFor(const QString& verifier) {
    QByteArray hash = QCryptographicHash::hash(verifier.toUtf8(), QCryptographicHash::Sha256);
    return base64UrlNoPad(hash);
}

QString PKCE::makeState() {
    return base64UrlNoPad(randomBytes(32));
}

}  // namespace gridex::oauth
