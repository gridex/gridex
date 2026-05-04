#pragma once

// RFC 7636 PKCE primitives + base64url helpers. Pure functions.

#include <QByteArray>
#include <QString>

namespace gridex::oauth {

class PKCE {
public:
    // 32 random bytes → base64url-no-pad (43 chars). Suitable as code_verifier.
    static QString makeVerifier();

    // SHA256(verifier) → base64url-no-pad. Used as code_challenge with S256.
    static QString challengeFor(const QString& verifier);

    // 32 random bytes → base64url-no-pad. Used as the OAuth `state` param.
    static QString makeState();

    // Standard base64 → URL-safe variant (no padding).
    static QString base64UrlNoPad(const QByteArray& data);

    // base64url → bytes. Re-pads to a multiple of 4 with '=' and reverses
    // URL-safe substitutions before decoding.
    static QByteArray base64UrlDecode(const QString& s);
};

}  // namespace gridex::oauth
