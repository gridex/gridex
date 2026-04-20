#pragma once
//
// MCPTool.h
// Gridex
//
// Tool interface + context bundle passed at execute() time. The
// MCPServer builds one MCPToolContext per request and hands it to
// the tool. Tools throw MCPToolError on failure; the server folds
// them into MCPToolResult::error for the client.

#include <string>
#include <memory>
#include <nlohmann/json.hpp>
#include "../../../Models/MCP/MCPToolDefinition.h"
#include "../../../Models/MCP/MCPPermissionTier.h"
#include "../../../Models/MCP/MCPToolError.h"
#include "../../../Models/DatabaseAdapter.h"
#include "../../../Models/ConnectionConfig.h"
#include "../Security/MCPPermissionEngine.h"
#include "../Security/MCPRateLimiter.h"
#include "../Security/MCPApprovalGate.h"
#include "../Audit/MCPAuditLogger.h"
#include "../MCPConnectionPool.h"

namespace DBModels
{
    // Forward-declared to break circularity with ConnectionStore.h.
    // (Audit client is plain data.)
    struct MCPAuditClient;

    // Bundle of shared services made available to every tool.
    // Members are non-owning references — the MCPServer owns them.
    struct MCPToolContext
    {
        MCPConnectionPool& pool;
        MCPPermissionEngine& permissionEngine;
        MCPRateLimiter& rateLimiter;
        MCPApprovalGate& approvalGate;
        MCPAuditLogger& auditLogger;
        MCPAuditClient client; // by-value; copied per-request

        // Helper: looks up `connection_id` in JSON params, finds
        // the matching ConnectionConfig, opens the adapter via the
        // pool, returns (adapter, config).
        // Throws MCPToolError::connectionNotFound if id is unknown.
        std::pair<std::shared_ptr<DatabaseAdapter>, ConnectionConfig>
            getAdapter(const std::wstring& connectionId) const;

        MCPPermissionResult checkPermission(MCPPermissionTier tier,
                                            const std::wstring& id) const
        {
            return permissionEngine.checkPermission(tier, id);
        }

        // Convenience — request approval and BLOCK the caller on
        // the resulting future. Tools are invoked from worker
        // threads, so blocking here is safe.
        bool requestApproval(const std::wstring& tool,
                             const std::wstring& description,
                             const std::wstring& details,
                             const std::wstring& connectionId,
                             const std::wstring& clientName,
                             int timeoutSeconds = 60) const;
    };

    // Pure-virtual tool interface. All tool implementations inherit
    // this + define name/description/tier/inputSchema + execute.
    class MCPTool
    {
    public:
        virtual ~MCPTool() = default;
        virtual std::string name() const = 0;
        virtual std::string description() const = 0;
        virtual MCPPermissionTier tier() const = 0;
        virtual nlohmann::json inputSchema() const = 0;
        virtual MCPToolResult execute(const nlohmann::json& params,
                                      MCPToolContext& context) = 0;

        MCPToolDefinition definition() const
        {
            return { name(), description(), inputSchema() };
        }

        // Helper: extract `connection_id` from JSON params and
        // return it as wstring (the store's native id type).
        static std::wstring extractConnectionId(const nlohmann::json& params);
    };
}
