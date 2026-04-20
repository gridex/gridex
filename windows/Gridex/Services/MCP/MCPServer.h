#pragma once
//
// MCPServer.h
// Gridex
//
// Orchestrator. Owns registry + security + audit + pool + transports.
// Handles the three MCP protocol verbs: initialize, tools/list,
// tools/call. Mirrors macos/Services/MCP/MCPServer.swift.
//
// Transport modes:
//   - Stdio:   CLI `--mcp-stdio`. Uses stdin/stdout, no UI.
//              Tier 3 tools auto-deny (no window for ContentDialog).
//   - HttpOnly: GUI mode. HTTP endpoint (Phase 4d, TBD) on 127.0.0.1:port.
//              Tier 3 tools can open the approval dialog on MainWindow.

#include <memory>
#include <atomic>
#include <string>
#include "MCPConnectionPool.h"
#include "Tools/MCPTool.h"
#include "Tools/MCPToolRegistry.h"
#include "Security/MCPPermissionEngine.h"
#include "Security/MCPRateLimiter.h"
#include "Security/MCPApprovalGate.h"
#include "Audit/MCPAuditLogger.h"
#include "Transport/StdioTransport.h"
#include "Transport/HttpTransport.h"
#include "../../Models/MCP/MCPProtocol.h"
#include "../../Models/MCP/MCPServerInfo.h"
#include "../../Models/AppSettings.h"

namespace DBModels
{
    enum class MCPTransportMode
    {
        Stdio,      // CLI mode — stdin/stdout
        HttpOnly    // GUI mode — HTTP transport only
    };

    class MCPServer
    {
    public:
        MCPServer(AppSettings settings,
                  std::string serverVersion,
                  MCPTransportMode mode);
        // Out-of-line: HttpTransport uses pimpl and requires the
        // MCPServer dtor be defined where HttpTransport::Impl is
        // complete (i.e. MCPServer.cpp, which includes HttpTransport.h
        // whose .cpp has the full Impl struct).
        ~MCPServer();

        // Start accepting requests.
        void start();

        // Graceful shutdown — closes transports, flushes audit,
        // releases pool adapters.
        void stop();

        bool isRunning() const { return running_.load(); }

        // Expose the subsystems for the UI (MCPWindow needs pool
        // stats, audit entries, permission modes).
        MCPPermissionEngine& permissionEngine() { return permissionEngine_; }
        MCPRateLimiter& rateLimiter()           { return rateLimiter_; }
        MCPAuditLogger& auditLogger()           { return auditLogger_; }
        MCPConnectionPool& pool()               { return pool_; }
        MCPApprovalGate& approvalGate()         { return approvalGate_; }
        MCPToolRegistry& toolRegistry()         { return toolRegistry_; }

        // Called by MainWindow once it is activated.
        void setUIContext(void* dispatcherQueue, void* xamlRoot);

        // Register all built-in tools. Called from start() — made
        // public so tests can wire up selectively.
        void registerBuiltinTools();

    private:
        AppSettings settings_;
        std::string serverVersion_;
        MCPTransportMode mode_;
        std::atomic<bool> running_{false};

        MCPConnectionPool pool_;
        MCPPermissionEngine permissionEngine_;
        MCPRateLimiter rateLimiter_;
        MCPApprovalGate approvalGate_;
        MCPAuditLogger auditLogger_;
        MCPToolRegistry toolRegistry_;
        StdioTransport stdio_;
        HttpTransport  http_;

        MCPClientInfo clientInfo_;  // populated on initialize

        // Request dispatch.
        void handleRequest(const JSONRPCRequest& req);
        JSONRPCResponse handleInitialize(const JSONRPCRequest& req);
        JSONRPCResponse handleToolsList(const JSONRPCRequest& req);
        JSONRPCResponse handleToolsCall(const JSONRPCRequest& req);

        // Sends back via the active transport.
        void sendResponse(const JSONRPCResponse& resp);

        // Strip filesystem paths / connection strings from error
        // messages before the client sees them. Matches mac's
        // sanitizeError.
        static std::string sanitizeError(const std::string& msg);
    };
}
