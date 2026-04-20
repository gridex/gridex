//
// TablePlusImporter.cpp
//
// Parses the TablePlus for Windows connections plist.
// Location: %LOCALAPPDATA%\com.tinyapp.TablePlus\Data\Connections.plist
// Format: standard Apple XML plist — <dict> blocks with <key> +
// typed value pairs (<string>, <integer>). We scan linearly rather
// than using MSXML — schema is stable and the file is small.
//
// Passwords are encrypted via TablePlus's .tpmaster key file; not
// attempted on Windows — user re-enters after import.

#include "TablePlusImporter.h"

#include <windows.h>
#include <shlobj.h>
#include <fstream>
#include <sstream>
#include <string>
#include <algorithm>

namespace DBModels { namespace TablePlusImporter {

namespace {
    std::wstring plistPath()
    {
        wchar_t* p = nullptr;
        if (SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p) != S_OK)
            return {};
        std::wstring out(p);
        CoTaskMemFree(p);
        return out + L"\\com.tinyapp.TablePlus\\Data\\Connections.plist";
    }

    std::wstring fromUtf8(const std::string& s)
    {
        if (s.empty()) return {};
        int sz = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
        std::wstring out(sz, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], sz);
        return out;
    }

    DatabaseType mapDriver(const std::string& driver)
    {
        std::string lower; lower.reserve(driver.size());
        for (char c : driver) lower.push_back((char)std::tolower((unsigned char)c));
        if (lower.find("postgres") != std::string::npos) return DatabaseType::PostgreSQL;
        if (lower.find("mysql") != std::string::npos || lower.find("maria") != std::string::npos)
            return DatabaseType::MySQL;
        if (lower.find("sqlite") != std::string::npos) return DatabaseType::SQLite;
        if (lower.find("redis") != std::string::npos)  return DatabaseType::Redis;
        if (lower.find("mongo") != std::string::npos)  return DatabaseType::MongoDB;
        if (lower.find("mssql") != std::string::npos || lower.find("sqlserver") != std::string::npos)
            return DatabaseType::MSSQLServer;
        return DatabaseType::PostgreSQL;
    }

    // XML plist scanner: within a `<dict>...</dict>` body, pull
    // every <key>Name</key> followed by its typed value. We only
    // need <string> and <integer>; <array>/<data>/<date> are
    // skipped (TablePlus doesn't store connection fields in them).
    std::string readStringFor(const std::string& body, const std::string& keyName,
                              size_t bodyStart = 0)
    {
        const std::string needle = "<key>" + keyName + "</key>";
        auto k = body.find(needle, bodyStart);
        if (k == std::string::npos) return {};
        auto after = k + needle.size();

        // Look for the next <string>...</string>, or self-closing
        // <string /> (meaning empty). Skip over any whitespace.
        auto openStr = body.find("<string", after);
        if (openStr == std::string::npos) return {};
        // Self-closing empty tag.
        if (body.compare(openStr, 9, "<string/>") == 0 ||
            body.compare(openStr, 10, "<string />") == 0) return {};
        auto openEnd = body.find('>', openStr);
        if (openEnd == std::string::npos) return {};
        auto close = body.find("</string>", openEnd);
        if (close == std::string::npos) return {};
        return body.substr(openEnd + 1, close - (openEnd + 1));
    }

    int readIntFor(const std::string& body, const std::string& keyName)
    {
        const std::string needle = "<key>" + keyName + "</key>";
        auto k = body.find(needle);
        if (k == std::string::npos) return 0;
        auto after = k + needle.size();
        auto openInt = body.find("<integer>", after);
        if (openInt == std::string::npos) return 0;
        auto close = body.find("</integer>", openInt);
        if (close == std::string::npos) return 0;
        const auto val = body.substr(openInt + 9, close - (openInt + 9));
        try { return std::stoi(val); } catch (...) { return 0; }
    }

    std::vector<ImportedConnection> parsePlist(const std::string& xml)
    {
        std::vector<ImportedConnection> out;
        // Each connection lives in a top-level <dict>. Walk them.
        size_t pos = 0;
        while (true)
        {
            auto start = xml.find("<dict>", pos);
            if (start == std::string::npos) break;
            // Find matching </dict> — assume TablePlus doesn't nest
            // <dict> inside a connection body (confirmed against
            // the real file; nested structures use <array>).
            auto end = xml.find("</dict>", start);
            if (end == std::string::npos) break;

            const std::string body = xml.substr(start, end + 7 - start);

            std::string name   = readStringFor(body, "ConnectionName");
            std::string driver = readStringFor(body, "Driver");
            if (name.empty() || driver.empty()) { pos = end + 7; continue; }

            ImportedConnection c;
            c.id     = fromUtf8(readStringFor(body, "ID"));
            c.source = ImportSource::TablePlus;
            c.name   = fromUtf8(name);
            c.databaseType = mapDriver(driver);

            const auto host = readStringFor(body, "DatabaseHost");
            if (!host.empty()) c.host = fromUtf8(host);

            const auto portStr = readStringFor(body, "DatabasePort");
            if (!portStr.empty())
            { try { c.port = (uint16_t)std::stoi(portStr); } catch (...) {} }

            c.username = fromUtf8(readStringFor(body, "DatabaseUser"));
            c.database = fromUtf8(readStringFor(body, "DatabaseName"));
            c.filePath = fromUtf8(readStringFor(body, "DatabasePath"));

            c.sslEnabled = readIntFor(body, "tLSMode") > 0;

            const bool overSSH = readIntFor(body, "isOverSSH") != 0;
            if (overSSH)
            {
                c.sshHost = fromUtf8(readStringFor(body, "ServerAddress"));
                const auto sp = readStringFor(body, "ServerPort");
                if (!sp.empty())
                { try { c.sshPort = (uint16_t)std::stoi(sp); } catch (...) {} }
                c.sshUser = fromUtf8(readStringFor(body, "ServerUser"));
            }

            out.push_back(std::move(c));
            pos = end + 7;
        }
        return out;
    }
}

bool isInstalled()
{
    const auto p = plistPath();
    return !p.empty() && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::vector<ImportedConnection> importConnections()
{
    const auto path = plistPath();
    if (path.empty()) return {};
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return {};
    std::stringstream ss; ss << in.rdbuf();
    return parsePlist(ss.str());
}

std::wstring windowsSupportNote()
{
    // Parsing works; passwords stay encrypted by TablePlus's
    // .tpmaster — user re-enters after import.
    return L"TablePlus import works on Windows. Passwords are encrypted by "
           L"TablePlus and will need to be re-entered after import.";
}

}} // namespace
