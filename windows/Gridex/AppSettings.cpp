#include <windows.h>
#include <shlobj.h>
#include <wincred.h>
#pragma comment(lib, "Advapi32.lib")
#include "Models/AppSettings.h"
#include <fstream>
#include <sstream>

namespace DBModels
{
    // ── Helpers ──────────────────────────────────────────

    static std::string toUtf8(const std::wstring& wstr)
    {
        if (wstr.empty()) return {};
        int sz = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(),
            static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
        std::string out(sz, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(),
            static_cast<int>(wstr.size()), &out[0], sz, nullptr, nullptr);
        return out;
    }

    static std::wstring fromUtf8(const std::string& utf8)
    {
        if (utf8.empty()) return {};
        int sz = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
            static_cast<int>(utf8.size()), nullptr, 0);
        std::wstring out(sz, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
            static_cast<int>(utf8.size()), &out[0], sz);
        return out;
    }

    static std::wstring escapeJson(const std::wstring& s)
    {
        std::wstring out;
        out.reserve(s.size() + 2);
        for (wchar_t c : s)
        {
            if (c == L'"')       out += L"\\\"";
            else if (c == L'\\') out += L"\\\\";
            else if (c == L'\n') out += L"\\n";
            else if (c == L'\r') out += L"\\r";
            else if (c == L'\t') out += L"\\t";
            else                 out += c;
        }
        return out;
    }

    // Simple "key":"value" extractor for minimal JSON (no external deps)
    static std::wstring extractString(const std::wstring& json, const std::wstring& key)
    {
        std::wstring needle = L"\"" + key + L"\":";
        size_t pos = json.find(needle);
        if (pos == std::wstring::npos) return {};
        pos += needle.size();
        while (pos < json.size() && (json[pos] == L' ' || json[pos] == L'\t')) pos++;
        if (pos >= json.size() || json[pos] != L'"') return {};
        pos++; // skip opening quote
        std::wstring result;
        while (pos < json.size() && json[pos] != L'"')
        {
            if (json[pos] == L'\\' && pos + 1 < json.size())
            {
                wchar_t next = json[pos + 1];
                if (next == L'"')       { result += L'"';  pos += 2; }
                else if (next == L'\\') { result += L'\\'; pos += 2; }
                else if (next == L'n')  { result += L'\n'; pos += 2; }
                else if (next == L'r')  { result += L'\r'; pos += 2; }
                else if (next == L't')  { result += L'\t'; pos += 2; }
                else                    { result += json[pos]; pos++; }
            }
            else
            {
                result += json[pos++];
            }
        }
        return result;
    }

    static int extractInt(const std::wstring& json, const std::wstring& key, int defaultVal)
    {
        std::wstring needle = L"\"" + key + L"\":";
        size_t pos = json.find(needle);
        if (pos == std::wstring::npos) return defaultVal;
        pos += needle.size();
        while (pos < json.size() && (json[pos] == L' ' || json[pos] == L'\t')) pos++;
        std::wstring num;
        if (pos < json.size() && (json[pos] == L'-' || iswdigit(json[pos])))
        {
            num += json[pos++];
            while (pos < json.size() && iswdigit(json[pos])) num += json[pos++];
        }
        if (num.empty()) return defaultVal;
        try { return std::stoi(num); } catch (...) { return defaultVal; }
    }

    static bool extractBool(const std::wstring& json, const std::wstring& key, bool defaultVal)
    {
        std::wstring needle = L"\"" + key + L"\":";
        size_t pos = json.find(needle);
        if (pos == std::wstring::npos) return defaultVal;
        pos += needle.size();
        while (pos < json.size() && (json[pos] == L' ' || json[pos] == L'\t')) pos++;
        if (json.compare(pos, 4, L"true") == 0) return true;
        if (json.compare(pos, 5, L"false") == 0) return false;
        return defaultVal;
    }

    // Extract a JSON array of strings: "key": ["a", "b", "c"]
    static std::vector<std::wstring> extractStringArray(
        const std::wstring& json, const std::wstring& key)
    {
        std::vector<std::wstring> result;
        std::wstring needle = L"\"" + key + L"\":";
        size_t pos = json.find(needle);
        if (pos == std::wstring::npos) return result;
        pos += needle.size();
        while (pos < json.size() && (json[pos] == L' ' || json[pos] == L'\t' ||
               json[pos] == L'\r' || json[pos] == L'\n')) pos++;
        if (pos >= json.size() || json[pos] != L'[') return result;
        pos++; // skip '['

        while (pos < json.size())
        {
            while (pos < json.size() && (json[pos] == L' ' || json[pos] == L'\t' ||
                   json[pos] == L'\r' || json[pos] == L'\n' || json[pos] == L',')) pos++;
            if (pos >= json.size() || json[pos] == L']') break;
            if (json[pos] != L'"') break;
            pos++; // opening quote
            std::wstring item;
            while (pos < json.size() && json[pos] != L'"')
            {
                if (json[pos] == L'\\' && pos + 1 < json.size())
                {
                    wchar_t next = json[pos + 1];
                    if (next == L'"')       { item += L'"';  pos += 2; }
                    else if (next == L'\\') { item += L'\\'; pos += 2; }
                    else if (next == L'n')  { item += L'\n'; pos += 2; }
                    else                    { item += json[pos]; pos++; }
                }
                else
                {
                    item += json[pos++];
                }
            }
            if (pos < json.size()) pos++; // closing quote
            result.push_back(item);
        }
        return result;
    }

    // ── Legacy DataBridge -> Gridex migration ───────────
    //
    // App was renamed from "DataBridge" to "Gridex". Existing users have
    // settings, connection database, and credential vault entries under
    // the old name. Migrate them ONCE on first launch of the renamed app.
    //
    // Idempotent: only runs if Gridex dir is missing AND DataBridge dir exists.
    // Original DataBridge data is preserved (not deleted) so user can roll
    // back if needed.
    //
    // Note: the legacy strings below intentionally hard-code "DataBridge"
    // because this is the OLD name we are migrating FROM. Mass-rename
    // tools must skip this block.
    static constexpr const wchar_t* kLegacyAppName       = L"DataBridge";
    static constexpr const wchar_t* kLegacyCredPrefix    = L"DataBridge:";
    static constexpr const wchar_t* kLegacyCredFilter    = L"DataBridge:*";
    static constexpr const wchar_t* kLegacyDbFilename    = L"databridge.db";

    // Recursive directory copy via SHFileOperationW.
    // Source must end with double-null (SHFileOp quirk), so we build a buffer.
    static bool copyDirectory(const std::wstring& src, const std::wstring& dst)
    {
        std::vector<wchar_t> from(src.begin(), src.end());
        from.push_back(L'\0');
        from.push_back(L'\0');

        std::vector<wchar_t> to(dst.begin(), dst.end());
        to.push_back(L'\0');
        to.push_back(L'\0');

        SHFILEOPSTRUCTW op = {};
        op.wFunc = FO_COPY;
        op.pFrom = from.data();
        op.pTo   = to.data();
        op.fFlags = FOF_NO_UI | FOF_NOCONFIRMATION |
                    FOF_NOERRORUI | FOF_SILENT | FOF_NOCONFIRMMKDIR;
        return SHFileOperationW(&op) == 0;
    }

    // Migrate Windows Credential Vault entries from legacy prefix to "Gridex:".
    // Failures are silently ignored (user can re-enter passwords if needed).
    static void migrateCredentialVault()
    {
        DWORD count = 0;
        PCREDENTIALW* creds = nullptr;
        if (!CredEnumerateW(kLegacyCredFilter, 0, &count, &creds) || !creds)
            return;

        const std::wstring legacyPrefix = kLegacyCredPrefix;
        const size_t legacyPrefixLen = legacyPrefix.size();

        for (DWORD i = 0; i < count; ++i)
        {
            PCREDENTIALW c = creds[i];
            if (!c || !c->TargetName) continue;

            std::wstring oldTarget = c->TargetName;
            if (oldTarget.rfind(legacyPrefix, 0) != 0) continue;
            std::wstring newTarget = L"Gridex:" + oldTarget.substr(legacyPrefixLen);

            // Skip if already migrated
            PCREDENTIALW existing = nullptr;
            if (CredReadW(newTarget.c_str(), CRED_TYPE_GENERIC, 0, &existing))
            {
                CredFree(existing);
                continue;
            }

            CREDENTIALW dst = {};
            dst.Type = CRED_TYPE_GENERIC;
            dst.TargetName = const_cast<LPWSTR>(newTarget.c_str());
            dst.CredentialBlobSize = c->CredentialBlobSize;
            dst.CredentialBlob = c->CredentialBlob;
            dst.Persist = CRED_PERSIST_LOCAL_MACHINE;
            dst.UserName = const_cast<LPWSTR>(L"Gridex");
            CredWriteW(&dst, 0);
        }
        CredFree(creds);
    }

    // Run all migration steps. Idempotent — safe to call on every launch.
    // Static flag avoids redundant filesystem probes on repeated Load/Save calls.
    static void migrateLegacyDataIfNeeded()
    {
        static bool ran = false;
        if (ran) return;
        ran = true;

        wchar_t local[MAX_PATH] = {};
        wchar_t roaming[MAX_PATH] = {};
        SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local);
        SHGetFolderPathW(nullptr, CSIDL_APPDATA,       nullptr, 0, roaming);

        const std::wstring legacyDir = std::wstring(L"\\") + kLegacyAppName;

        // 1. %LOCALAPPDATA%\DataBridge -> %LOCALAPPDATA%\Gridex (settings.json)
        std::wstring localOld = std::wstring(local) + legacyDir;
        std::wstring localNew = std::wstring(local) + L"\\Gridex";
        if (GetFileAttributesW(localNew.c_str()) == INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(localOld.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            copyDirectory(localOld, localNew);
        }

        // 2. %APPDATA%\DataBridge -> %APPDATA%\Gridex (connection store .db)
        std::wstring roamingOld = std::wstring(roaming) + legacyDir;
        std::wstring roamingNew = std::wstring(roaming) + L"\\Gridex";
        if (GetFileAttributesW(roamingNew.c_str()) == INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(roamingOld.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            copyDirectory(roamingOld, roamingNew);
            // Rename the .db file inside the new dir if it kept the old name
            std::wstring oldDbInNew = roamingNew + L"\\" + kLegacyDbFilename;
            std::wstring newDbInNew = roamingNew + L"\\gridex.db";
            if (GetFileAttributesW(oldDbInNew.c_str()) != INVALID_FILE_ATTRIBUTES &&
                GetFileAttributesW(newDbInNew.c_str()) == INVALID_FILE_ATTRIBUTES)
            {
                MoveFileW(oldDbInNew.c_str(), newDbInNew.c_str());
            }
        }

        // 3. Credential vault: DataBridge:* -> Gridex:*
        migrateCredentialVault();
    }

    // ── Path resolution ─────────────────────────────────

    std::wstring AppSettings::GetSettingsPath()
    {
        wchar_t path[MAX_PATH] = {};
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, path)))
            return {};

        // Run legacy migration before computing path so the new dir is
        // populated by the time AppSettings::Load() reads from it.
        migrateLegacyDataIfNeeded();

        std::wstring dir = std::wstring(path) + L"\\Gridex";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir + L"\\settings.json";
    }

    // ── Load ────────────────────────────────────────────

    AppSettings AppSettings::Load()
    {
        AppSettings s;
        std::wstring path = GetSettingsPath();
        if (path.empty()) return s;

        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) return s;

        std::string bytes(
            (std::istreambuf_iterator<char>(file)),
            std::istreambuf_iterator<char>());
        file.close();
        if (bytes.empty()) return s;

        // Strip UTF-8 BOM
        if (bytes.size() >= 3 && bytes[0] == '\xEF' &&
            bytes[1] == '\xBB' && bytes[2] == '\xBF')
            bytes = bytes.substr(3);

        std::wstring json = fromUtf8(bytes);

        s.themeIndex         = extractInt(json, L"themeIndex", 0);
        s.aiProviderIndex    = extractInt(json, L"aiProviderIndex", 0);
        s.aiApiKey           = extractString(json, L"aiApiKey");
        s.aiModel            = extractString(json, L"aiModel");
        s.ollamaEndpoint     = extractString(json, L"ollamaEndpoint");
        s.editorFontSize     = extractInt(json, L"editorFontSize", 13);
        s.rowLimit           = extractInt(json, L"rowLimit", 100);
        s.lastPageBeforeSettings = extractString(json, L"lastPageBeforeSettings");
        s.connectionGroups   = extractStringArray(json, L"connectionGroups");

        // MCP — all fields optional for forward compat with 0.1.3 settings.
        s.mcpEnabled                  = extractBool(json, L"mcpEnabled", false);
        s.mcpHttpEnabled              = extractBool(json, L"mcpHttpEnabled", false);
        s.mcpHttpPort                 = extractInt(json,  L"mcpHttpPort", 3333);
        s.mcpAllowRemoteHTTP          = extractBool(json, L"mcpAllowRemoteHTTP", false);
        s.mcpRequireApprovalForWrites = extractBool(json, L"mcpRequireApprovalForWrites", true);
        s.mcpQueriesPerMinute         = extractInt(json,  L"mcpQueriesPerMinute", 60);
        s.mcpQueriesPerHour           = extractInt(json,  L"mcpQueriesPerHour", 1000);
        s.mcpWritesPerMinute          = extractInt(json,  L"mcpWritesPerMinute", 10);
        s.mcpDdlPerMinute             = extractInt(json,  L"mcpDdlPerMinute", 1);
        s.mcpQueryTimeout             = extractInt(json,  L"mcpQueryTimeout", 30);
        s.mcpApprovalTimeout          = extractInt(json,  L"mcpApprovalTimeout", 60);
        s.mcpConnectionTimeout        = extractInt(json,  L"mcpConnectionTimeout", 10);
        s.mcpAuditRetentionDays       = extractInt(json,  L"mcpAuditRetentionDays", 90);
        s.mcpAuditMaxSizeMB           = extractInt(json,  L"mcpAuditMaxSizeMB", 100);
        // 64-bit unix seconds: extractInt returns 32-bit; for mcpStartTime
        // we bounce through wide-stoll to cover dates past 2038.
        {
            std::wstring needle = L"\"mcpStartTime\":";
            size_t pos = json.find(needle);
            if (pos != std::wstring::npos)
            {
                pos += needle.size();
                while (pos < json.size() && (json[pos] == L' ' || json[pos] == L'\t')) pos++;
                std::wstring num;
                if (pos < json.size() && (json[pos] == L'-' || iswdigit(json[pos])))
                {
                    num += json[pos++];
                    while (pos < json.size() && iswdigit(json[pos])) num += json[pos++];
                }
                if (!num.empty()) { try { s.mcpStartTime = std::stoll(num); } catch (...) {} }
            }
        }

        return s;
    }

    // ── Save ────────────────────────────────────────────

    bool AppSettings::Save() const
    {
        std::wstring path = GetSettingsPath();
        if (path.empty()) return false;

        std::wstringstream ss;
        ss << L"{\n";
        ss << L"  \"themeIndex\": "          << themeIndex          << L",\n";
        ss << L"  \"aiProviderIndex\": "     << aiProviderIndex     << L",\n";
        ss << L"  \"aiApiKey\": \""          << escapeJson(aiApiKey)          << L"\",\n";
        ss << L"  \"aiModel\": \""           << escapeJson(aiModel)           << L"\",\n";
        ss << L"  \"ollamaEndpoint\": \""    << escapeJson(ollamaEndpoint)    << L"\",\n";
        ss << L"  \"editorFontSize\": "      << editorFontSize      << L",\n";
        ss << L"  \"rowLimit\": "            << rowLimit            << L",\n";
        ss << L"  \"lastPageBeforeSettings\": \"" << escapeJson(lastPageBeforeSettings) << L"\",\n";
        ss << L"  \"connectionGroups\": [";
        for (size_t i = 0; i < connectionGroups.size(); i++)
        {
            if (i > 0) ss << L", ";
            ss << L"\"" << escapeJson(connectionGroups[i]) << L"\"";
        }
        ss << L"],\n";

        // MCP block. Booleans written as literal true/false; ints
        // as plain numbers. Group at the end to minimize diff churn
        // if we add more prefs later.
        ss << L"  \"mcpEnabled\": "                  << (mcpEnabled ? L"true" : L"false")                  << L",\n";
        ss << L"  \"mcpHttpEnabled\": "              << (mcpHttpEnabled ? L"true" : L"false")              << L",\n";
        ss << L"  \"mcpHttpPort\": "                 << mcpHttpPort                                         << L",\n";
        ss << L"  \"mcpAllowRemoteHTTP\": "          << (mcpAllowRemoteHTTP ? L"true" : L"false")          << L",\n";
        ss << L"  \"mcpRequireApprovalForWrites\": " << (mcpRequireApprovalForWrites ? L"true" : L"false") << L",\n";
        ss << L"  \"mcpQueriesPerMinute\": "         << mcpQueriesPerMinute                                 << L",\n";
        ss << L"  \"mcpQueriesPerHour\": "           << mcpQueriesPerHour                                   << L",\n";
        ss << L"  \"mcpWritesPerMinute\": "          << mcpWritesPerMinute                                  << L",\n";
        ss << L"  \"mcpDdlPerMinute\": "             << mcpDdlPerMinute                                     << L",\n";
        ss << L"  \"mcpQueryTimeout\": "             << mcpQueryTimeout                                     << L",\n";
        ss << L"  \"mcpApprovalTimeout\": "          << mcpApprovalTimeout                                  << L",\n";
        ss << L"  \"mcpConnectionTimeout\": "        << mcpConnectionTimeout                                << L",\n";
        ss << L"  \"mcpAuditRetentionDays\": "       << mcpAuditRetentionDays                               << L",\n";
        ss << L"  \"mcpAuditMaxSizeMB\": "           << mcpAuditMaxSizeMB                                   << L",\n";
        ss << L"  \"mcpStartTime\": "                << mcpStartTime                                        << L"\n";
        ss << L"}\n";

        std::string utf8 = toUtf8(ss.str());
        std::ofstream file(path, std::ios::binary);
        if (!file.is_open()) return false;
        file.write(utf8.c_str(), utf8.size());
        file.close();
        return true;
    }
}
