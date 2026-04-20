//
// MCPAuditLogger.cpp
//
// File I/O uses plain std::ofstream/ifstream. Path is under
// %APPDATA%\Gridex\ — the same folder ConnectionStore uses — so
// backups sit next to the connections DB.

#include "MCPAuditLogger.h"

#include <windows.h>
#include <shlobj.h>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <ctime>

namespace fs = std::filesystem;

namespace DBModels
{
    MCPAuditLogger::MCPAuditLogger(int maxSizeMB, int retentionDays)
        : maxSizeBytes_(maxSizeMB > 0 ? maxSizeMB * 1024 * 1024 : 100 * 1024 * 1024),
          retentionDays_(retentionDays)
    {
        const std::wstring dir = resolveAppDataPath();
        CreateDirectoryW(dir.c_str(), nullptr);
        path_ = dir + L"\\mcp-audit.jsonl";
        purgeOldBackups();
    }

    std::wstring MCPAuditLogger::resolveAppDataPath()
    {
        wchar_t* appData = nullptr;
        if (SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData) != S_OK)
            return L".";
        std::wstring p = std::wstring(appData) + L"\\Gridex";
        CoTaskMemFree(appData);
        return p;
    }

    std::wstring MCPAuditLogger::logFilePath() const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return path_;
    }

    std::string MCPAuditLogger::isoTimestampForFilename()
    {
        const std::time_t t = std::chrono::system_clock::to_time_t(
            std::chrono::system_clock::now());
        std::tm tm{};
        gmtime_s(&tm, &t);
        char buf[32];
        // Colons are invalid in Win32 filenames — use dashes instead.
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H-%M-%SZ", &tm);
        return std::string(buf);
    }

    void MCPAuditLogger::log(const MCPAuditEntry& entry)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        try
        {
            rotateIfNeeded();

            nlohmann::json j;
            to_json(j, entry);
            std::string line = j.dump() + "\n";

            std::ofstream out(path_, std::ios::app | std::ios::binary);
            if (!out.is_open()) return;
            out.write(line.data(), static_cast<std::streamsize>(line.size()));
        }
        catch (...) { /* never crash on audit */ }
    }

    void MCPAuditLogger::rotateIfNeeded()
    {
        std::error_code ec;
        if (!fs::exists(path_, ec)) return;
        const auto size = fs::file_size(path_, ec);
        if (ec || static_cast<int64_t>(size) < static_cast<int64_t>(maxSizeBytes_)) return;

        const std::string ts = isoTimestampForFilename();
        fs::path backup = fs::path(path_).parent_path() /
            (std::string("mcp-audit-") + ts + ".jsonl");
        fs::rename(path_, backup, ec);
    }

    void MCPAuditLogger::purgeOldBackups()
    {
        if (retentionDays_ <= 0) return;
        std::error_code ec;
        const fs::path dir = fs::path(path_).parent_path();
        if (!fs::exists(dir, ec)) return;

        const auto cutoff = std::chrono::system_clock::now()
                          - std::chrono::hours(24 * retentionDays_);

        for (auto it = fs::directory_iterator(dir, ec);
             !ec && it != fs::directory_iterator();
             ++it)
        {
            const auto& p = it->path();
            const auto name = p.filename().string();
            // Only touch our own rotated backups.
            if (name.rfind("mcp-audit-", 0) != 0) continue;
            if (p.extension() != ".jsonl") continue;

            const auto ft = fs::last_write_time(p, ec);
            if (ec) continue;
            // Convert file_time_type → system_clock::time_point
            // (portable approximation — good enough for day-level retention).
            const auto sys = std::chrono::system_clock::now()
                - (fs::file_time_type::clock::now() - ft);
            if (sys < cutoff) fs::remove(p, ec);
        }
    }

    void MCPAuditLogger::close()
    {
        // std::ofstream is opened per-log() call, nothing persistent.
    }

    void MCPAuditLogger::clearAll()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        std::error_code ec;
        fs::remove(path_, ec);
    }

    std::vector<MCPAuditEntry> MCPAuditLogger::recentEntries(int limit)
    {
        std::vector<MCPAuditEntry> out;
        std::lock_guard<std::mutex> lk(mtx_);
        std::ifstream in(path_, std::ios::binary);
        if (!in.is_open()) return out;

        // Read all lines (for human-scale logs this is fine — a full
        // 100MB log would still fit in RAM, and the UI caps at 500).
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) lines.push_back(std::move(line));
        }

        const size_t start = lines.size() > static_cast<size_t>(limit)
            ? lines.size() - static_cast<size_t>(limit) : 0;
        for (size_t i = lines.size(); i > start; --i)
        {
            try
            {
                auto j = nlohmann::json::parse(lines[i - 1]);
                MCPAuditEntry e;
                e.tool = j.value("tool", std::string{});
                e.tier = j.value("tier", 1);
                e.eventId = j.value("eventId", std::string{});
                if (j.contains("connectionId") && j["connectionId"].is_string())
                    e.connectionId = j["connectionId"].get<std::string>();
                if (j.contains("connectionType") && j["connectionType"].is_string())
                    e.connectionType = j["connectionType"].get<std::string>();
                if (j.contains("error") && j["error"].is_string())
                    e.error = j["error"].get<std::string>();

                if (j.contains("client"))
                {
                    const auto& c = j["client"];
                    e.client.name      = c.value("name", std::string{"unknown"});
                    e.client.version   = c.value("version", std::string{"0.0.0"});
                    e.client.transport = c.value("transport", std::string{"stdio"});
                }
                if (j.contains("result"))
                {
                    const auto& r = j["result"];
                    e.result.status = mcpAuditStatusFromRaw(r.value("status", std::string{"success"}));
                    e.result.durationMs = r.value("durationMs", 0);
                    if (r.contains("rowsAffected")) e.result.rowsAffected = r["rowsAffected"].get<int>();
                    if (r.contains("rowsReturned")) e.result.rowsReturned = r["rowsReturned"].get<int>();
                }
                if (j.contains("security"))
                {
                    const auto& s = j["security"];
                    e.security.permissionMode = mcpModeFromRawString(
                        s.value("permissionMode", std::string{"locked"}));
                    if (s.contains("userApproved")) e.security.userApproved = s["userApproved"].get<bool>();
                }
                if (j.contains("input"))
                {
                    const auto& in_ = j["input"];
                    if (in_.contains("sqlPreview")) e.input.sqlPreview = in_["sqlPreview"].get<std::string>();
                    if (in_.contains("paramsCount")) e.input.paramsCount = in_["paramsCount"].get<int>();
                    if (in_.contains("inputHash")) e.input.inputHash = in_["inputHash"].get<std::string>();
                }
                out.push_back(std::move(e));
            }
            catch (...) { /* skip malformed line */ }
        }
        return out;
    }
}
