// JwtDecoder.cpp
// Minimal JWT payload decoder — base64url-decode the middle segment, parse JSON.
// No signature verification (trusts OpenAI's TLS, matching macOS behaviour).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#pragma comment(lib, "Crypt32.lib")

#include "JwtDecoder.h"
#include <stdexcept>
#include <vector>

namespace ChatGPT
{
    // base64url -> standard base64 with padding, then decode via CryptStringToBinaryA
    // (Crypt32.lib is already linked by the project).
    static std::vector<unsigned char> base64URLDecode(std::string s)
    {
        // Reverse URL-safe substitutions
        for (char& c : s)
        {
            if (c == '-') c = '+';
            else if (c == '_') c = '/';
        }
        // Re-pad to multiple of 4
        while (s.size() % 4 != 0)
            s += '=';

        DWORD outLen = 0;
        if (!CryptStringToBinaryA(s.c_str(), static_cast<DWORD>(s.size()),
            CRYPT_STRING_BASE64, nullptr, &outLen, nullptr, nullptr))
            throw std::runtime_error("JwtDecoder: base64 size query failed");

        std::vector<unsigned char> out(outLen);
        if (!CryptStringToBinaryA(s.c_str(), static_cast<DWORD>(s.size()),
            CRYPT_STRING_BASE64, out.data(), &outLen, nullptr, nullptr))
            throw std::runtime_error("JwtDecoder: base64 decode failed");
        out.resize(outLen);
        return out;
    }

    nlohmann::json JwtDecodePayload(const std::string& jwt)
    {
        // Split "header.payload.signature" on '.'
        size_t dot1 = jwt.find('.');
        if (dot1 == std::string::npos)
            throw std::runtime_error("JwtDecoder: missing first dot");
        size_t dot2 = jwt.find('.', dot1 + 1);
        if (dot2 == std::string::npos)
            throw std::runtime_error("JwtDecoder: missing second dot");

        std::string payload = jwt.substr(dot1 + 1, dot2 - dot1 - 1);
        auto bytes = base64URLDecode(payload);
        std::string json(bytes.begin(), bytes.end());
        return nlohmann::json::parse(json);
    }

    int64_t JwtExpiration(const std::string& jwt)
    {
        try
        {
            auto claims = JwtDecodePayload(jwt);
            if (claims.contains("exp"))
            {
                if (claims["exp"].is_number_integer())
                    return claims["exp"].get<int64_t>();
                if (claims["exp"].is_number_float())
                    return static_cast<int64_t>(claims["exp"].get<double>());
            }
        }
        catch (...) {}
        return 0; // treat as expired
    }
}
