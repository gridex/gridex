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
#include <array>

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

// ── Registry scan (Windows Navicat) ────────────────────────
// Navicat Windows persists each connection as a registry subkey
// under HKCU\Software\PremiumSoft\Navicat<PRODUCT>\Servers\<name>.
// We walk the known product hives and emit one ImportedConnection
// per subkey. String values use the `W` / wide variant because
// Navicat stores non-ASCII names (e.g. host labels in Chinese)
// verbatim.

namespace {
    struct NavicatHive {
        const wchar_t* subKey;        // under HKCU
        DatabaseType   dbType;
    };

    const std::array<NavicatHive, 8>& hives()
    {
        // Plain `Navicat\Servers` is the legacy "Navicat for MySQL"
        // standalone hive — still used on machines that started on
        // that product before upgrading to Premium. Missing this
        // cost a user their second MySQL connection during import.
        static const std::array<NavicatHive, 8> v{{
            { L"Software\\PremiumSoft\\Navicat\\Servers",         DatabaseType::MySQL },
            { L"Software\\PremiumSoft\\NavicatPG\\Servers",       DatabaseType::PostgreSQL },
            { L"Software\\PremiumSoft\\NavicatMARIADB\\Servers",  DatabaseType::MySQL },
            { L"Software\\PremiumSoft\\NavicatMSSQL\\Servers",    DatabaseType::MSSQLServer },
            { L"Software\\PremiumSoft\\NavicatMONGODB\\Servers",  DatabaseType::MongoDB },
            { L"Software\\PremiumSoft\\NavicatREDIS\\Servers",    DatabaseType::Redis },
            { L"Software\\PremiumSoft\\NavicatSQLite\\Servers",   DatabaseType::SQLite },
            { L"Software\\PremiumSoft\\NavicatPremium\\Servers",  DatabaseType::PostgreSQL }
        }};
        return v;
    }

    // Helpers around RegQueryValueExW to unwrap REG_SZ / REG_DWORD.
    std::wstring regReadString(HKEY h, const wchar_t* name)
    {
        DWORD type = 0, cb = 0;
        if (RegQueryValueExW(h, name, nullptr, &type, nullptr, &cb) != ERROR_SUCCESS
            || type != REG_SZ || cb == 0) return {};
        std::wstring out(cb / sizeof(wchar_t), L'\0');
        if (RegQueryValueExW(h, name, nullptr, nullptr,
            reinterpret_cast<LPBYTE>(&out[0]), &cb) != ERROR_SUCCESS) return {};
        while (!out.empty() && out.back() == L'\0') out.pop_back();
        return out;
    }

    bool regReadDword(HKEY h, const wchar_t* name, DWORD& out)
    {
        DWORD type = 0, cb = sizeof(DWORD);
        return RegQueryValueExW(h, name, nullptr, &type,
            reinterpret_cast<LPBYTE>(&out), &cb) == ERROR_SUCCESS
            && type == REG_DWORD;
    }

    ImportedConnection readServer(HKEY parent, const std::wstring& name, DatabaseType fallbackType)
    {
        ImportedConnection c;
        c.source = ImportSource::Navicat;
        c.name = name;
        c.databaseType = fallbackType;

        HKEY sub = nullptr;
        if (RegOpenKeyExW(parent, name.c_str(), 0, KEY_READ, &sub) != ERROR_SUCCESS)
            return c;

        // NavicatPremium\Servers mixes products — prefer the
        // connection's self-reported type when present.
        const auto conn = regReadString(sub, L"ConnectionType");
        if (!conn.empty())
        {
            std::wstring lower; lower.reserve(conn.size());
            for (wchar_t ch : conn) lower.push_back((wchar_t)towlower(ch));
            if      (lower.find(L"postgres") != std::wstring::npos) c.databaseType = DatabaseType::PostgreSQL;
            else if (lower.find(L"mysql")    != std::wstring::npos) c.databaseType = DatabaseType::MySQL;
            else if (lower.find(L"maria")    != std::wstring::npos) c.databaseType = DatabaseType::MySQL;
            else if (lower.find(L"mssql")    != std::wstring::npos) c.databaseType = DatabaseType::MSSQLServer;
            else if (lower.find(L"mongo")    != std::wstring::npos) c.databaseType = DatabaseType::MongoDB;
            else if (lower.find(L"redis")    != std::wstring::npos) c.databaseType = DatabaseType::Redis;
            else if (lower.find(L"sqlite")   != std::wstring::npos) c.databaseType = DatabaseType::SQLite;
        }

        c.host = regReadString(sub, L"Host");
        DWORD port = 0;
        if (regReadDword(sub, L"Port", port) && port != 0)
            c.port = static_cast<uint16_t>(port);

        c.username = regReadString(sub, L"UserName");
        c.database = regReadString(sub, L"InitialDatabase");
        c.filePath = regReadString(sub, L"DatabaseFile");

        DWORD ssl = 0;
        if (regReadDword(sub, L"UseSSL", ssl) && ssl != 0) c.sslEnabled = true;

        // SSH tunnel — same keys as the .ncx schema.
        c.sshHost = regReadString(sub, L"SSH_Host");
        if (c.sshHost.empty()) c.sshHost = regReadString(sub, L"SSH_Server");
        c.sshUser = regReadString(sub, L"SSH_UserName");
        DWORD sshPort = 0;
        if (regReadDword(sub, L"SSH_Port", sshPort) && sshPort != 0)
            c.sshPort = static_cast<uint16_t>(sshPort);

        // Password is Blowfish-encrypted in Pwd_2 — skipped on
        // Windows (see header).
        RegCloseKey(sub);
        return c;
    }

    std::vector<ImportedConnection> scanHive(const NavicatHive& h)
    {
        std::vector<ImportedConnection> out;
        HKEY root = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, h.subKey, 0, KEY_READ, &root) != ERROR_SUCCESS)
            return out;

        wchar_t name[512];
        DWORD nameLen = 0;
        DWORD idx = 0;
        while (true)
        {
            nameLen = 512;
            const LONG rc = RegEnumKeyExW(root, idx, name, &nameLen,
                                           nullptr, nullptr, nullptr, nullptr);
            if (rc == ERROR_NO_MORE_ITEMS) break;
            if (rc != ERROR_SUCCESS) break;
            out.push_back(readServer(root, std::wstring(name, nameLen), h.dbType));
            ++idx;
        }
        RegCloseKey(root);
        return out;
    }
}

bool isInstalled()
{
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\PremiumSoft",
                      0, KEY_READ, &h) == ERROR_SUCCESS)
    {
        RegCloseKey(h);
        return true;
    }
    return false;
}

std::vector<ImportedConnection> importConnections()
{
    // Dedupe NavicatPremium overlap — if a server name already
    // shown from a specialized hive, skip the duplicate we'd pull
    // from Premium's mixed hive.
    std::vector<ImportedConnection> out;
    for (const auto& h : hives())
    {
        auto rows = scanHive(h);
        for (auto& r : rows)
        {
            bool dup = false;
            for (const auto& existing : out)
                if (existing.name == r.name && existing.host == r.host) { dup = true; break; }
            if (!dup) out.push_back(std::move(r));
        }
    }
    return out;
}

std::vector<ImportedConnection> importFromNCX(const std::wstring& ncxPath)
{
    std::ifstream in(ncxPath, std::ios::binary);
    if (!in.is_open()) return {};
    std::stringstream ss; ss << in.rdbuf();
    return parseXml(ss.str());
}

}} // namespace
