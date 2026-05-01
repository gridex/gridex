#pragma once
// PKCE.h
// RFC 7636 PKCE primitives for the ChatGPT OAuth flow. Pure functions, no I/O.
// Uses OpenSSL EVP_Digest (libcrypto already linked via vcpkg).

#include <string>

namespace ChatGPT
{
    // 32 random bytes -> base64url-no-pad (43 chars). Suitable as code_verifier.
    std::string PKCEMakeVerifier();

    // SHA256(verifier) -> base64url-no-pad. Used as code_challenge with S256.
    std::string PKCEChallenge(const std::string& verifier);

    // 32 random bytes -> base64url-no-pad. Used as OAuth state param (CSRF guard).
    std::string PKCEMakeState();
}
