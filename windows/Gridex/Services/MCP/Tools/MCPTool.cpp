//
// MCPTool.cpp
//

#include "MCPTool.h"
#include "../../../Models/ConnectionStore.h"
#include "../../../Models/MCP/MCPAuditEntry.h"
#include <algorithm>
#include <windows.h>

namespace DBModels
{
    std::wstring MCPTool::extractConnectionId(const nlohmann::json& params)
    {
        if (!params.contains("connection_id") || !params["connection_id"].is_string())
            throw MCPToolError::invalidParameters("connection_id is required");
        const auto utf8 = params["connection_id"].get<std::string>();
        // Proper UTF-8 decode — char-by-char widening would corrupt
        // any non-ASCII id (future cloud sync may produce them).
        if (utf8.empty()) return {};
        int sz = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                      static_cast<int>(utf8.size()), nullptr, 0);
        std::wstring out(sz, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                            static_cast<int>(utf8.size()), &out[0], sz);
        return out;
    }

    std::pair<std::shared_ptr<DatabaseAdapter>, ConnectionConfig>
        MCPToolContext::getAdapter(const std::wstring& connectionId) const
    {
        // Look up the config from the connection store. Linear scan
        // is fine — typical users have <50 connections.
        auto configs = ConnectionStore::Load();
        auto it = std::find_if(configs.begin(), configs.end(),
            [&](const ConnectionConfig& c) { return c.id == connectionId; });
        if (it == configs.end())
        {
            std::string narrow(connectionId.begin(), connectionId.end());
            throw MCPToolError::connectionNotFound(narrow);
        }

        // Password comes from ConnectionStore — it DPAPI-decrypts the
        // `password_enc` column into ConnectionConfig::password during
        // Load(). CredentialManager (Windows Credential Vault) isn't
        // wired in this codebase yet; looking there would always
        // return empty and the adapter would hit
        // 'fe_sendauth: no password supplied' (observed against
        // PostgreSQL).
        auto& ncPool = const_cast<MCPConnectionPool&>(pool);
        auto adapter = ncPool.acquire(*it, it->password);
        return { adapter, *it };
    }

    bool MCPToolContext::requestApproval(
        const std::wstring& toolName,
        const std::wstring& description,
        const std::wstring& details,
        const std::wstring& connectionId,
        const std::wstring& clientName,
        int timeoutSeconds) const
    {
        MCPApprovalRequest req;
        req.tool = toolName;
        req.description = description;
        req.details = details;
        req.connectionId = connectionId;
        req.clientName = clientName;
        req.timeoutSeconds = timeoutSeconds;
        auto& nc = const_cast<MCPApprovalGate&>(approvalGate);
        auto future = nc.requestApproval(req);
        // Block the worker thread — caller is on a stdio/http worker,
        // not the UI thread, so this is safe.
        return future.get();
    }
}
