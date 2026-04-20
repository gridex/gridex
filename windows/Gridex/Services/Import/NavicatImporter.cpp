//
// NavicatImporter.cpp
//
// Parses .ncx export files. Same XML shape as the mac importer —
// <Connection ConnType="..."> blocks with <Host>, <Port>, <UserName>,
// <Password>, <SSL>, SSH_Host/Port/User fields. Passwords are
// Blowfish-encrypted (key "3DC5CA39"); on Windows we skip decryption
// because Blowfish isn't in BCrypt and OpenSSL 3.0 keeps it behind
// the legacy provider — user re-enters after import.

#include "NavicatImporter.h"

#include <windows.h>
#include <fstream>
#include <sstream>
#include <string>
#include <algorithm>

namespace DBModels { namespace NavicatImporter {

namespace {
    std::wstring fromUtf8(const std::string& s)
    {
        if (s.empty()) return {};
        int sz = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
        std::wstring out(sz, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], sz);
        return out;
    }

    // Map Navicat's internal connType codes (seen in both older XML
    // string form and the newer integer form).
    DatabaseType mapConnType(const std::string& connType)
    {
        std::string lower; lower.reserve(connType.size());
        for (char c : connType) lower.push_back((char)std::tolower((unsigned char)c));

        if (lower.find("postgres") != std::string::npos) return DatabaseType::PostgreSQL;
        if (lower.find("mysql") != std::string::npos || lower.find("maria") != std::string::npos)
            return DatabaseType::MySQL;
        if (lower.find("sqlite") != std::string::npos) return DatabaseType::SQLite;
        if (lower.find("redis") != std::string::npos)  return DatabaseType::Redis;
        if (lower.find("mongo") != std::string::npos)  return DatabaseType::MongoDB;
        if (lower.find("sqlserver") != std::string::npos || lower.find("mssql") != std::string::npos)
            return DatabaseType::MSSQLServer;
        // Legacy numeric codes.
        if (lower == "1") return DatabaseType::MySQL;
        if (lower == "2") return DatabaseType::PostgreSQL;
        if (lower == "3") return DatabaseType::SQLite;
        if (lower == "7") return DatabaseType::MSSQLServer;
        if (lower == "8") return DatabaseType::MongoDB;
        return DatabaseType::PostgreSQL;
    }

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

    // Inside a single <Connection>...</Connection> body, read the
    // text content of a child element by name. Returns empty when
    // the tag is absent. Handles both open/close (<Foo>bar</Foo>)
    // and self-closing empty tags (<Foo/>).
    std::string readChild(const std::string& body, const std::string& tag)
    {
        const std::string open  = "<" + tag + ">";
        const std::string close = "</" + tag + ">";
        auto o = body.find(open);
        if (o == std::string::npos) return {};
        o += open.size();
        auto c = body.find(close, o);
        if (c == std::string::npos) return {};
        std::string v = body.substr(o, c - o);
        // Trim whitespace.
        while (!v.empty() && std::isspace((unsigned char)v.front())) v.erase(v.begin());
        while (!v.empty() && std::isspace((unsigned char)v.back()))  v.pop_back();
        return v;
    }

    std::vector<ImportedConnection> parseXml(const std::string& xml)
    {
        std::vector<ImportedConnection> out;
        size_t pos = 0;
        while (true)
        {
            auto start = xml.find("<Connection", pos);
            if (start == std::string::npos) break;
            auto headerEnd = xml.find('>', start);
            if (headerEnd == std::string::npos) break;
            auto blockEnd = xml.find("</Connection>", headerEnd);
            if (blockEnd == std::string::npos) break;

            const std::string header = xml.substr(start, headerEnd - start + 1);
            const std::string body = xml.substr(headerEnd + 1, blockEnd - (headerEnd + 1));

            std::string name = readChild(body, "ConnectionName");
            if (name.empty()) name = readChild(body, "Name");
            if (name.empty()) { pos = blockEnd + 13; continue; }

            std::string connType = extractAttr(header, "ConnType");
            if (connType.empty()) connType = readChild(body, "ConnType");

            ImportedConnection c;
            c.source = ImportSource::Navicat;
            c.name = fromUtf8(name);
            c.databaseType = mapConnType(connType);
            c.host = fromUtf8(readChild(body, "Host"));
            const auto portStr = readChild(body, "Port");
            if (!portStr.empty())
            { try { c.port = (uint16_t)std::stoi(portStr); } catch (...) {} }
            auto db = readChild(body, "DatabaseName");
            if (db.empty()) db = readChild(body, "InitialDatabase");
            c.database = fromUtf8(db);
            c.username = fromUtf8(readChild(body, "UserName"));
            c.filePath = fromUtf8(readChild(body, "DatabaseFile"));

            const auto sslStr = readChild(body, "SSL");
            const auto useSslStr = readChild(body, "UseSSL");
            c.sslEnabled = (sslStr == "true") || (useSslStr == "1");

            c.sshHost = fromUtf8(readChild(body, "SSH_Host"));
            const auto sshPortStr = readChild(body, "SSH_Port");
            if (!sshPortStr.empty())
            { try { c.sshPort = (uint16_t)std::stoi(sshPortStr); } catch (...) {} }
            c.sshUser = fromUtf8(readChild(body, "SSH_UserName"));

            // Password field is Blowfish-encrypted; skipped on Windows
            // (see header). Leave c.password empty.

            out.push_back(std::move(c));
            pos = blockEnd + std::string("</Connection>").size();
        }
        return out;
    }
}

bool isInstalled() { return true; }

std::vector<ImportedConnection> importFromNCX(const std::wstring& ncxPath)
{
    std::ifstream in(ncxPath, std::ios::binary);
    if (!in.is_open()) return {};
    std::stringstream ss; ss << in.rdbuf();
    return parseXml(ss.str());
}

}} // namespace
