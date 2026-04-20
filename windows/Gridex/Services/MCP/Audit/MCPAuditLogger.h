#pragma once
//
// MCPAuditLogger.h
// Gridex
//
// Append-only JSONL audit log at %APPDATA%\Gridex\mcp-audit.jsonl.
// Mirrors macos/Services/MCP/Audit/MCPAuditLogger.swift format 1:1
// so `jq` over the file works cross-platform.
//
// Rotation: when the file reaches `maxSizeMB` the current file is
// renamed to `mcp-audit-<ISO-timestamp>.jsonl` and a fresh file
// is started. Retention is enforced at startup (deleting backups
// older than `retentionDays`; 0 means forever).

#include <string>
#include <vector>
#include <mutex>
#include "../../../Models/MCP/MCPAuditEntry.h"

namespace DBModels
{
    class MCPAuditLogger
    {
    public:
        // Sizes in MB, retention in days. 0 retention = keep forever.
        MCPAuditLogger(int maxSizeMB = 100, int retentionDays = 90);

        // Thread-safe append. Swallows exceptions — audit must
        // never crash the app.
        void log(const MCPAuditEntry& entry);

        // Close the underlying handle (called on server shutdown).
        void close();

        // Tail the last `limit` entries (most recent first). Returns
        // empty vector on error.
        std::vector<MCPAuditEntry> recentEntries(int limit = 100);

        // Delete the current log (testing / UI "Clear Log" button).
        void clearAll();

        // Absolute path of the current log file.
        std::wstring logFilePath() const;

    private:
        mutable std::mutex mtx_;
        std::wstring path_;
        int maxSizeBytes_;
        int retentionDays_;

        void rotateIfNeeded();
        void purgeOldBackups();
        static std::wstring resolveAppDataPath();
        static std::string isoTimestampForFilename();
    };
}
