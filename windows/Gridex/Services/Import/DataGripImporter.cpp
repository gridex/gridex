//
// DataGripImporter.cpp
//
// Scans %APPDATA%\JetBrains for any directory starting with
// "DataGrip" (e.g. "DataGrip2024.3"), reads options/dataSources.xml,
// and yields ImportedConnection rows. Hand-rolls the XML scan —
// DataGrip's file shape is stable (top-level <data-source name=
// uuid=>, child <driver>...</driver> + <jdbc-url>...</jdbc-url>)
// and bringing in MSXML via COM for a few tag extractions would
// be overkill.

#include "DataGripImporter.h"

#include <windows.h>
#include <shlobj.h>
#include <fstream>
#include <sstream>
#include <string>
#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;

namespace DBModels { namespace DataGripImporter {

namespace {
    std::wstring jetbrainsDir()
    {
        wchar_t* p = nullptr;
        if (SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p) != S_OK)
            return {};
        std::wstring out(p);
        CoTaskMemFree(p);
        return out + L"\\JetBrains";
    }

    // Pick the newest DataGrip* subdir (lexicographic sort works
    // because JetBrains name-versions monotonically: DataGrip2023.1,
    // DataGrip2024.2, DataGrip2025.1, ...).
    std::optional<fs::path> newestDataGripDir()
    {
        const auto jb = jetbrainsDir();
        if (jb.empty()) return std::nullopt;
        std::error_code ec;
        if (!fs::exists(jb, ec)) return std::nullopt;

        std::vector<fs::path> hits;
        for (auto it = fs::directory_iterator(jb, ec);
             !ec && it != fs::directory_iterator();
             ++it)
        {
            if (!it->is_directory()) continue;
            const auto name = it->path().filename().wstring();
            if (name.rfind(L"DataGrip", 0) == 0) hits.push_back(it->path());
        }
        if (hits.empty()) return std::nullopt;
        std::sort(hits.begin(), hits.end(),
            [](const fs::path& a, const fs::path& b) {
                return a.filename().wstring() > b.filename().wstring();
            });
        return hits.front();
    }

    std::wstring fromUtf8(const std::string& s)
    {
        if (s.empty()) return {};
        int sz = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
        std::wstring out(sz, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], sz);
        return out;
    }

    std::string toLower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
            [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    }

    DatabaseType mapDriverUrl(const std::string& driver, const std::string& url)
    {
        const std::string combined = toLower(driver + url);
        if (combined.find("postgres") != std::string::npos) return DatabaseType::PostgreSQL;
        if (combined.find("mysql") != std::string::npos || combined.find("maria") != std::string::npos)
            return DatabaseType::MySQL;
        if (combined.find("sqlite") != std::string::npos) return DatabaseType::SQLite;
        if (combined.find("redis") != std::string::npos)  return DatabaseType::Redis;
        if (combined.find("mongo") != std::string::npos)  return DatabaseType::MongoDB;
        if (combined.find("sqlserver") != std::string::npos || combined.find("mssql") != std::string::npos)
            return DatabaseType::MSSQLServer;
        return DatabaseType::PostgreSQL;
    }

    // Return substring between tag open and close — cheap and
    // good enough for the stable DataGrip schema. Returns "" if
    // either anchor is missing.
    std::string extractTag(const std::string& s, const std::string& open, const std::string& close,
                            size_t from = 0)
    {
        auto o = s.find(open, from);
        if (o == std::string::npos) return {};
        o += open.size();
        auto c = s.find(close, o);
        if (c == std::string::npos) return {};
        return s.substr(o, c - o);
    }

    // Pull an XML attribute value from a single element open tag.
    // Assumes double-quoted values (JetBrains always double-quotes).
    std::string extractAttr(const std::string& tag, const std::string& attr)
    {
        const std::string needle = attr + "=\"";
        auto p = tag.find(needle);
        if (p == std::string::npos) return {};
        p += needle.size();
        auto e = tag.find('"', p);
        if (e == std::string::npos) return {};
        return tag.substr(p, e - p);
    }

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
            std::string num;
            while (j < url.size() && std::isdigit((unsigned char)url[j])) num += url[j++];
            if (!num.empty()) { try { p.port = std::stoi(num); } catch (...) {} }
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

    // Walk every <data-source ...>...</data-source> block in the
    // XML, pull name / uuid / driver / url, and emit one row each.
    std::vector<ImportedConnection> parseXml(const std::string& xml)
    {
        std::vector<ImportedConnection> out;
        size_t pos = 0;
        while (true)
        {
            auto start = xml.find("<data-source", pos);
            if (start == std::string::npos) break;
            auto headerEnd = xml.find('>', start);
            if (headerEnd == std::string::npos) break;
            auto blockEnd = xml.find("</data-source>", headerEnd);
            if (blockEnd == std::string::npos) break;

            const std::string header = xml.substr(start, headerEnd - start + 1);
            const std::string body = xml.substr(headerEnd + 1, blockEnd - (headerEnd + 1));

            const std::string name   = extractAttr(header, "name");
            const std::string uuid   = extractAttr(header, "uuid");
            // driver may be an element (<driver-ref>pg</driver-ref>)
            // or attribute. Accept either.
            std::string driver = extractTag(body, "<driver-ref>", "</driver-ref>");
            if (driver.empty()) driver = extractTag(body, "<jdbc-driver>", "</jdbc-driver>");
            std::string url   = extractTag(body, "<jdbc-url>", "</jdbc-url>");

            if (!name.empty())
            {
                ImportedConnection c;
                c.source = ImportSource::DataGrip;
                c.id = uuid.empty() ? L"" : fromUtf8(uuid);
                c.name = fromUtf8(name);
                c.databaseType = mapDriverUrl(driver, url);
                c.sslEnabled = url.find("ssl=true") != std::string::npos;

                if (!url.empty())
                {
                    auto jp = parseJdbcUrl(url);
                    if (!jp.host.empty()) c.host = fromUtf8(jp.host);
                    if (jp.port.has_value()) c.port = static_cast<uint16_t>(*jp.port);
                    if (!jp.db.empty()) c.database = fromUtf8(jp.db);
                }

                out.push_back(std::move(c));
            }

            pos = blockEnd + strlen("</data-source>");
        }
        return out;
    }

    std::vector<ImportedConnection> parseFile(const fs::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) return {};
        std::stringstream ss; ss << in.rdbuf();
        return parseXml(ss.str());
    }
}

bool isInstalled() { return newestDataGripDir().has_value(); }

std::vector<ImportedConnection> importConnections()
{
    std::vector<ImportedConnection> out;
    auto base = newestDataGripDir();
    if (!base) return out;

    // Global options/dataSources.xml — shared across projects.
    {
        const auto p = *base / L"options" / L"dataSources.xml";
        std::error_code ec;
        if (fs::exists(p, ec))
        {
            auto rows = parseFile(p);
            out.insert(out.end(), rows.begin(), rows.end());
        }
    }
    // Per-project .idea/dataSources.xml (best effort; some users
    // keep connections in project scope rather than IDE-global).
    {
        const auto projRoot = *base / L"projects";
        std::error_code ec;
        if (fs::exists(projRoot, ec))
        {
            for (auto it = fs::directory_iterator(projRoot, ec);
                 !ec && it != fs::directory_iterator(); ++it)
            {
                if (!it->is_directory()) continue;
                const auto p = it->path() / L".idea" / L"dataSources.xml";
                if (fs::exists(p, ec))
                {
                    auto rows = parseFile(p);
                    out.insert(out.end(), rows.begin(), rows.end());
                }
            }
        }
    }
    return out;
}

}} // namespace
