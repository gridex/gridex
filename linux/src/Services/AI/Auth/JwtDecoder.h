#pragma once

// Minimal JWT payload decoder. Never verifies signature — issuer is
// auth.openai.com over TLS, refresh failures are surfaced by the server.

#include <chrono>
#include <optional>

#include <QString>
#include <nlohmann/json.hpp>

namespace gridex::oauth {

class JwtDecoder {
public:
    // Decodes the second segment of a JWT into a JSON object.
    // Throws std::runtime_error if the token isn't 3 segments or payload
    // isn't a JSON object.
    static nlohmann::json payload(const QString& jwt);

    // Convenience: extract `exp` claim as system_clock time_point. Returns
    // nullopt when missing/malformed → caller should treat as expired.
    static std::optional<std::chrono::system_clock::time_point> expiration(const QString& jwt);
};

}  // namespace gridex::oauth
