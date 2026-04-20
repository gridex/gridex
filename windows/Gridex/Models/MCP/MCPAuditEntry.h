#pragma once
//
// MCPAuditEntry.h
// Gridex
//
// Audit log entry for MCP tool invocations. One of these is
// serialized per line in mcp-audit.jsonl. Mirrors
// macos/Core/Models/MCP/MCPAuditEntry.swift.
//
// IMPORTANT: this JSONL is cross-platform — the mac and Windows
// builds write to the same format so `jq` and future sync work
// uniformly. DO NOT rename or retype fields.

#include <string>
#include <optional>
#include <vector>
#include <chrono>
#include <cstdint>
#include <nlohmann/json.hpp>
#include "MCPPermissionTier.h"
#include "MCPConnectionMode.h"

namespace DBModels
{
    // Raw status values match Swift MCPAuditStatus.
    enum class MCPAuditStatus { Success, Error, Denied, Timeout };

    inline std::string mcpAuditStatusRaw(MCPAuditStatus s)
    {
        switch (s)
        {
            case MCPAuditStatus::Success: return "success";
            case MCPAuditStatus::Error:   return "error";
            case MCPAuditStatus::Denied:  return "denied";
            case MCPAuditStatus::Timeout: return "timeout";
        }
        return "success";
    }

    inline MCPAuditStatus mcpAuditStatusFromRaw(const std::string& raw)
    {
        if (raw == "error")   return MCPAuditStatus::Error;
        if (raw == "denied")  return MCPAuditStatus::Denied;
        if (raw == "timeout") return MCPAuditStatus::Timeout;
        return MCPAuditStatus::Success;
    }

    struct MCPAuditClient
    {
        std::string name = "unknown";
        std::string version = "0.0.0";
        std::string transport = "stdio"; // "stdio" | "http"
    };

    // `sqlPreview` is truncated to 200 chars at construction so
    // secrets can never leak through oversized logs. `inputHash`
    // is a non-cryptographic correlation aid (std::hash), NOT a
    // security primitive — documented explicitly for auditors.
    struct MCPAuditInput
    {
        std::optional<std::string> sqlPreview;
        std::optional<int> paramsCount;
        std::optional<std::string> inputHash;

        static MCPAuditInput fromSQL(const std::string& sql, std::optional<int> paramsCount = std::nullopt)
        {
            MCPAuditInput i;
            if (!sql.empty())
            {
                const std::string preview = sql.size() > 200 ? sql.substr(0, 200) + "..." : sql;
                i.sqlPreview = preview;
                // FNV-1a 64-bit — deterministic across processes so
                // the inputHash field stays stable for cross-session
                // correlation. Not cryptographic; display-only aid.
                uint64_t h = 14695981039346656037ULL;
                for (unsigned char c : sql)
                {
                    h ^= c;
                    h *= 1099511628211ULL;
                }
                char buf[24];
                snprintf(buf, sizeof(buf), "fnv1a:%016llx",
                         static_cast<unsigned long long>(h));
                i.inputHash = buf;
            }
            i.paramsCount = paramsCount;
            return i;
        }
    };

    struct MCPAuditResult
    {
        MCPAuditStatus status = MCPAuditStatus::Success;
        std::optional<int> rowsAffected;
        std::optional<int> rowsReturned;
        int durationMs = 0;
        std::optional<int64_t> bytesReturned;
    };

    struct MCPAuditSecurity
    {
        MCPConnectionMode permissionMode = MCPConnectionMode::Locked;
        std::optional<bool> userApproved;
        std::optional<std::string> approvalSessionId;
        std::optional<std::vector<std::string>> scopesApplied;
    };

    struct MCPAuditEntry
    {
        // ISO-8601 UTC — formatted lazily in to_json so we don't
        // depend on C++20 chrono formatters.
        std::chrono::system_clock::time_point timestamp = std::chrono::system_clock::now();
        std::string eventId;        // UUID-like string (UuidCreate → hex)
        MCPAuditClient client;
        std::string tool;
        int tier = 1;               // MCPPermissionTier::Schema
        std::optional<std::string> connectionId;
        std::optional<std::string> connectionType; // "postgres"/"mysql"/...
        MCPAuditInput input;
        MCPAuditResult result;
        MCPAuditSecurity security;
        std::optional<std::string> error;
    };

    // ── Serialization ─────────────────────────────────────────
    // These write the same field names Swift uses so mac + Windows
    // logs are indistinguishable on disk.

    inline std::string mcpFormatIso8601(std::chrono::system_clock::time_point tp)
    {
        // strftime-based path — portable across MSVC versions where
        // std::format chrono support varies. Emits "YYYY-MM-DDTHH:MM:SSZ".
        const std::time_t t = std::chrono::system_clock::to_time_t(tp);
        std::tm tm{};
#if defined(_WIN32)
        gmtime_s(&tm, &t);
#else
        gmtime_r(&t, &tm);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
        return std::string(buf);
    }

    inline void to_json(nlohmann::json& j, const MCPAuditEntry& e)
    {
        j = nlohmann::json{
            {"timestamp", mcpFormatIso8601(e.timestamp)},
            {"eventId",   e.eventId},
            {"tool",      e.tool},
            {"tier",      e.tier},
            {"client", {
                {"name",      e.client.name},
                {"version",   e.client.version},
                {"transport", e.client.transport}
            }},
            {"result", {
                {"status",     mcpAuditStatusRaw(e.result.status)},
                {"durationMs", e.result.durationMs}
            }},
            {"security", {
                {"permissionMode", mcpRawString(e.security.permissionMode)}
            }}
        };
        if (e.connectionId.has_value())   j["connectionId"] = *e.connectionId;
        if (e.connectionType.has_value()) j["connectionType"] = *e.connectionType;
        if (e.input.sqlPreview.has_value())  j["input"]["sqlPreview"] = *e.input.sqlPreview;
        if (e.input.paramsCount.has_value()) j["input"]["paramsCount"] = *e.input.paramsCount;
        if (e.input.inputHash.has_value())   j["input"]["inputHash"] = *e.input.inputHash;
        if (e.result.rowsAffected.has_value())  j["result"]["rowsAffected"]  = *e.result.rowsAffected;
        if (e.result.rowsReturned.has_value())  j["result"]["rowsReturned"]  = *e.result.rowsReturned;
        if (e.result.bytesReturned.has_value()) j["result"]["bytesReturned"] = *e.result.bytesReturned;
        if (e.security.userApproved.has_value())       j["security"]["userApproved"] = *e.security.userApproved;
        if (e.security.approvalSessionId.has_value())  j["security"]["approvalSessionId"] = *e.security.approvalSessionId;
        if (e.error.has_value()) j["error"] = *e.error;
    }
}
