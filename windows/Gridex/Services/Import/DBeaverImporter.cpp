//
// DBeaverImporter.cpp
//

#include "DBeaverImporter.h"

#include <windows.h>
#include <shlobj.h>
#include <fstream>
#include <string>
#include <unordered_map>
#include <nlohmann/json.hpp>

namespace DBModels { namespace DBeaverImporter {

namespace {
    std::wstring appDataDir()
    {
        wchar_t* p = nullptr;
        if (SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p) != S_OK)
            return {};
        std::wstring out(p);
        CoTaskMemFree(p);
        return out;
    }

    std::wstring dataSourcesPath()
    {
        auto r = appDataDir();
        if (r.empty()) return {};
        return r + L"\\DBeaverData\\workspace6\\General\\.dbeaver\\data-sources.json";
    }
    std::wstring credentialsPath()
    {
        auto r = appDataDir();
        if (r.empty()) return {};
        return r + L"\\DBeaverData\\workspace6\\General\\.dbeaver\\credentials-config.json";
    }

    std::wstring fromUtf8(const std::string& s)
    {
        if (s.empty()) return {};
        int sz = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
        std::wstring out(sz, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], sz);
        return out;
    }

    DatabaseType mapProvider(const std::string& p)
    {
        std::string lower; lower.reserve(p.size());
        for (char c : p) lower.push_back(static_cast<char>(std::tolower((unsigned char)c)));
        if (lower.find("postgres") != std::string::npos) return DatabaseType::PostgreSQL;
        if (lower.find("mysql") != std::string::npos || lower.find("maria") != std::string::npos)
            return DatabaseType::MySQL;
        if (lower.find("sqlite") != std::string::npos) return DatabaseType::SQLite;
        if (lower.find("redis") != std::string::npos)  return DatabaseType::Redis;
        if (lower.find("mongo") != std::string::npos)  return DatabaseType::MongoDB;
        if (lower.find("sqlserver") != std::string::npos || lower.find("mssql") != std::string::npos)
            return DatabaseType::MSSQLServer;
        return DatabaseType::PostgreSQL;
    }

    // Base64 decode (stateless, std-only). Accepts the single-line
    // form DBeaver emits; newlines/whitespace ignored.
    std::vector<uint8_t> base64Decode(const std::string& s)
    {
        int map[256];
        for (int i = 0; i < 256; ++i) map[i] = -1;
        const char* cs = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) map[(unsigned char)cs[i]] = i;

        std::vector<uint8_t> out;
        uint32_t buf = 0; int bits = 0;
        for (unsigned char c : s)
        {
            if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
            int v = map[c];
            if (v < 0) continue;
            buf = (buf << 6) | v;
            bits += 6;
            if (bits >= 8) { bits -= 8; out.push_back(static_cast<uint8_t>((buf >> bits) & 0xFF)); }
        }
        return out;
    }

    // DBeaver ships a fixed 8-byte XOR key for its credentials file.
    // Constant is reverse-engineered from the upstream Java source.
    std::string xorDecrypt(const std::vector<uint8_t>& cipher)
    {
        static const uint8_t K[8] = { 0xBA, 0xBB, 0x4A, 0x9F, 0x7A, 0xEE, 0x8F, 0xC1 };
        std::string out(cipher.size(), '\0');
        for (size_t i = 0; i < cipher.size(); ++i)
            out[i] = static_cast<char>(cipher[i] ^ K[i % 8]);
        return out;
    }

    // key in credentials-config.json is "data-source|<connId>"; we
    // strip the prefix so lookups match the connection UUID from
    // data-sources.json.
    std::unordered_map<std::string, std::pair<std::string, std::string>> loadCredentials()
    {
        std::unordered_map<std::string, std::pair<std::string, std::string>> out;
        const auto path = credentialsPath();
        if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
            return out;
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) return out;
        std::string cipher((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
        if (cipher.empty()) return out;
        auto bytes = base64Decode(cipher);
        auto json = xorDecrypt(bytes);
        try
        {
            auto j = nlohmann::json::parse(json, nullptr, false);
            if (j.is_discarded() || !j.is_object()) return out;
            for (auto it = j.begin(); it != j.end(); ++it)
            {
                std::string key = it.key();
                const std::string prefix = "data-source|";
                if (key.rfind(prefix, 0) == 0) key.erase(0, prefix.size());
                if (!it.value().is_object()) continue;
                const auto& v = it.value();
                out[key] = {
                    v.value("user", std::string{}),
                    v.value("password", std::string{})
                };
            }
        }
        catch (...) { out.clear(); }
        return out;
    }

    // DBeaver sometimes ships host/port/db inside a bare JDBC URL
    // rather than discrete fields. Pull each piece with a tiny
    // substring scanner — std::regex would work but this avoids
    // the compile-once cost on an already-hot import path.
    struct JdbcParts { std::string host; std::optional<int> port; std::string db; };
    JdbcParts parseJdbcUrl(const std::string& url)
    {
        JdbcParts p;
        auto colSlash = url.find("://");
        if (colSlash == std::string::npos) return p;
        size_t i = colSlash + 3;
        size_t endHost = url.find_first_of(":/;?", i);
        if (endHost == std::string::npos) endHost = url.size();
        p.host = url.substr(i, endHost - i);

        if (endHost < url.size() && url[endHost] == ':')
        {
            size_t j = endHost + 1;
            std::string numStr;
            while (j < url.size() && std::isdigit((unsigned char)url[j])) numStr += url[j++];
            if (!numStr.empty()) { try { p.port = std::stoi(numStr); } catch (...) {} }
            endHost = j;
        }
        if (endHost < url.size() && url[endHost] == '/')
        {
            size_t j = endHost + 1;
            size_t endDb = url.find_first_of("?;", j);
            if (endDb == std::string::npos) endDb = url.size();
            p.db = url.substr(j, endDb - j);
        }
        return p;
    }
}

bool isInstalled()
{
    const auto p = dataSourcesPath();
    return !p.empty() && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::vector<ImportedConnection> importConnections()
{
    std::vector<ImportedConnection> out;
    const auto path = dataSourcesPath();
    if (path.empty()) return out;
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return out;

    nlohmann::json root;
    try { in >> root; } catch (...) { return out; }
    if (!root.is_object()) return out;

    const auto creds = loadCredentials();

    // Top-level object groups by folder name. Each folder has a
    // `connections` map keyed by UUID.
    for (auto fit = root.begin(); fit != root.end(); ++fit)
    {
        if (!fit.value().is_object()) continue;
        const auto& folder = fit.value();
        if (!folder.contains("connections") || !folder["connections"].is_object()) continue;

        const auto& conns = folder["connections"];
        for (auto cit = conns.begin(); cit != conns.end(); ++cit)
        {
            if (!cit.value().is_object()) continue;
            const std::string connId = cit.key();
            const auto& c = cit.value();

            std::string name     = c.value("name", std::string{});
            std::string provider = c.value("provider", std::string{});
            if (name.empty()) continue;

            const auto& conf = c.contains("configuration") && c["configuration"].is_object()
                ? c["configuration"] : nlohmann::json::object();

            std::string host     = conf.value("host", std::string{});
            std::string portStr  = conf.value("port", std::string{});
            std::string database = conf.value("database", std::string{});
            std::string url      = conf.value("url", std::string{});
            std::string sslStr   = conf.value("ssl", std::string{});
            std::string authModel= conf.value("auth-model", std::string{});

            ImportedConnection r;
            r.id = fromUtf8(connId);
            r.source = ImportSource::DBeaver;
            r.name = fromUtf8(name);
            r.databaseType = mapProvider(provider);
            r.sslEnabled = (sslStr == "true");

            if (!host.empty()) r.host = fromUtf8(host);
            if (!portStr.empty()) { try { r.port = static_cast<uint16_t>(std::stoi(portStr)); } catch (...) {} }
            if (!database.empty()) r.database = fromUtf8(database);

            // Fallback: scrape host/port/db from the bare JDBC URL
            // when DBeaver doesn't surface the discrete fields.
            if ((r.host.empty() || !r.port.has_value() || r.database.empty()) && !url.empty())
            {
                const auto p = parseJdbcUrl(url);
                if (r.host.empty() && !p.host.empty()) r.host = fromUtf8(p.host);
                if (!r.port.has_value() && p.port.has_value())
                    r.port = static_cast<uint16_t>(*p.port);
                if (r.database.empty() && !p.db.empty()) r.database = fromUtf8(p.db);
            }

            if (authModel == "native")
            {
                auto it = creds.find(connId);
                if (it != creds.end())
                {
                    r.username = fromUtf8(it->second.first);
                    r.password = fromUtf8(it->second.second);
                }
            }

            out.push_back(std::move(r));
        }
    }
    return out;
}

}} // namespace
