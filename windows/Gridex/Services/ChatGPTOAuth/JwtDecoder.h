#pragma once
// JwtDecoder.h
// Minimal JWT payload decoder. No signature verification — we trust OpenAI's TLS.
// Only needed for reading claims: exp, email, chatgpt_account_id, chatgpt_plan_type.

#include <string>
#include <nlohmann/json.hpp>

namespace ChatGPT
{
    // Decode the payload segment of a JWT into a JSON object.
    // Throws std::runtime_error if the token is malformed.
    nlohmann::json JwtDecodePayload(const std::string& jwt);

    // Extract the `exp` Unix timestamp from the JWT payload.
    // Returns 0 if missing or parse error — caller should treat as expired.
    int64_t JwtExpiration(const std::string& jwt);
}
