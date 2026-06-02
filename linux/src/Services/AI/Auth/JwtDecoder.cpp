#include "Services/AI/Auth/JwtDecoder.h"

#include <QStringList>
#include <stdexcept>

#include "Services/AI/Auth/PKCE.h"

namespace gridex::oauth {

nlohmann::json JwtDecoder::payload(const QString& jwt) {
    auto parts = jwt.split('.');
    if (parts.size() != 3) {
        throw std::runtime_error("Malformed JWT: expected 3 segments, got " +
                                 std::to_string(parts.size()));
    }
    QByteArray decoded = PKCE::base64UrlDecode(parts[1]);
    if (decoded.isEmpty()) {
        throw std::runtime_error("Malformed JWT: payload is not valid base64url");
    }
    auto j = nlohmann::json::parse(decoded.toStdString(), nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) {
        throw std::runtime_error("Malformed JWT: payload is not a JSON object");
    }
    return j;
}

std::optional<std::chrono::system_clock::time_point>
JwtDecoder::expiration(const QString& jwt) {
    try {
        auto claims = payload(jwt);
        if (!claims.contains("exp")) return std::nullopt;
        const auto& exp = claims["exp"];
        std::int64_t epochSeconds = 0;
        if (exp.is_number_integer()) epochSeconds = exp.get<std::int64_t>();
        else if (exp.is_number_float()) epochSeconds = static_cast<std::int64_t>(exp.get<double>());
        else return std::nullopt;
        return std::chrono::system_clock::time_point(std::chrono::seconds(epochSeconds));
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace gridex::oauth
