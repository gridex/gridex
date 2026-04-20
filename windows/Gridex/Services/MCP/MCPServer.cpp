//
// MCPServer.cpp
//

#include "MCPServer.h"
#include "../../Models/ConnectionStore.h"
#include "Tools/MCPBuiltinTools.h"

#include <chrono>
#include <regex>

// For UUID generation (eventId).
#include <windows.h>
#include <rpc.h>
#pragma comment(lib, "Rpcrt4.lib")

namespace DBModels
{
    namespace
    {
        std::string generateEventId()
        {
            UUID u; UuidCreate(&u);
            char* s = nullptr;
            UuidToStringA(&u, reinterpret_cast<RPC_CSTR*>(&s));
            std::string out = s ? s : "";
            if (s) RpcStringFreeA(reinterpret_cast<RPC_CSTR*>(&s));
            return out;
        }

        std::wstring utf8ToWide(const std::string& s)
        {
            if (s.empty()) return {};
            int sz = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
            std::wstring out(sz, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], sz);
            return out;
        }

        std::string wideToUtf8(const std::wstring& s)
        {
            if (s.empty()) return {};
            int sz = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(),
                                         nullptr, 0, nullptr, nullptr);
            std::string out(sz, '\0');
            WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], sz, nullptr, nullptr);
            return out;
        }
    }

    MCPServer::~MCPServer() = default;

    MCPServer::MCPServer(AppSettings settings, std::string serverVersion, MCPTransportMode mode)
        : settings_(std::move(settings)),
          serverVersion_(std::move(serverVersion)),
          mode_(mode),
          rateLimiter_(MCPRateLimiterConfig{
              settings_.mcpQueriesPerMinute,
              settings_.mcpQueriesPerHour,
              settings_.mcpWritesPerMinute,
              settings_.mcpDdlPerMinute
          }),
          auditLogger_(settings_.mcpAuditMaxSizeMB, settings_.mcpAuditRetentionDays)
    {
    }

    void MCPServer::start()
    {
        if (running_.exchange(true)) return;

        // Load per-connection MCP modes into the permission engine.
        for (const auto& c : ConnectionStore::Load())
            permissionEngine_.setMode(c.id, c.mcpMode);

        registerBuiltinTools();

        if (mode_ == MCPTransportMode::Stdio)
        {
            stdio_.setRequestHandler([this](const JSONRPCRequest& r){ handleRequest(r); });
            stdio_.start();
        }
        else if (settings_.mcpHttpEnabled)
        {
            // HttpOnly mode + toggle is on → bind the HTTP listener.
            // Defaults: 127.0.0.1:<mcpHttpPort>. Remote binding
            // (0.0.0.0) only when allowRemoteHTTP is also on.
            const int port = settings_.mcpHttpPort > 0
                ? settings_.mcpHttpPort : 3333;
            const std::string host = settings_.mcpAllowRemoteHTTP
                ? "0.0.0.0" : "127.0.0.1";

            http_.setRequestHandler(
                [this](const JSONRPCRequest& r) -> JSONRPCResponse {
                    // HTTP is synchronous — we build the response
                    // inline instead of routing through sendResponse.
                    if (r.method == "initialize")      return handleInitialize(r);
                    if (r.method == "tools/list")      return handleToolsList(r);
                    if (r.method == "tools/call")      return handleToolsCall(r);
                    if (r.method == "prompts/list")    return JSONRPCResponse::ok(r.id, nlohmann::json{{"prompts", nlohmann::json::array()}});
                    if (r.method == "resources/list")  return JSONRPCResponse::ok(r.id, nlohmann::json{{"resources", nlohmann::json::array()}});
                    if (r.method == "resources/templates/list") return JSONRPCResponse::ok(r.id, nlohmann::json{{"resourceTemplates", nlohmann::json::array()}});
                    if (r.method == "ping")
                        return JSONRPCResponse::ok(r.id, nlohmann::json{{"pong", true}});
                    if (r.method == "shutdown")       { stop(); return JSONRPCResponse::ok(r.id, nullptr); }
                    if (r.method == "initialized" || r.method == "notifications/initialized")
                        return JSONRPCResponse::ok(r.id, nullptr);
                    return JSONRPCResponse::fail(r.id, JSONRPCError::methodNotFound());
                });
            http_.start(host, port);
        }
        // else: HttpOnly mode but HTTP toggle is off — server runs
        // "warm" (tools registered, audit log primed) without
        // binding any socket. Flip the toggle on + Start again to
        // actually listen.
    }

    void MCPServer::stop()
    {
        if (!running_.exchange(false)) return;
        stdio_.stop();
        http_.stop();
        auditLogger_.close();
        pool_.releaseAll();
    }

    void MCPServer::setUIContext(void* dq, void* xr)
    {
        approvalGate_.setUIContext(dq, xr);
    }

    // Phase 5 hooks register real tools here. Leaving empty avoids
    // a forward-include chain on every Tier1/2/3 header until Phase 5
    // lands — tools will add themselves via
    // toolRegistry().registerTool(...) from a helper TU.
    void MCPServer::registerBuiltinTools()
    {
        MCPBuiltinTools::registerAll(toolRegistry_);
    }

    // ── Request dispatch ─────────────────────────────────────

    void MCPServer::handleRequest(const JSONRPCRequest& req)
    {
        JSONRPCResponse resp = JSONRPCResponse::fail(req.id, JSONRPCError::methodNotFound());
        const std::string& m = req.method;

        // Each handler is wrapped in try/catch so a single bad
        // request (e.g. json serialization failure on a weird
        // param type) cannot tear down the stdio reader thread.
        try
        {
            if (m == "initialize")      resp = handleInitialize(req);
            // MCP notifications — client fire-and-forget, no response.
            else if (m == "initialized") return;
            else if (m == "notifications/initialized") return;
            else if (m == "tools/list") resp = handleToolsList(req);
            else if (m == "tools/call") resp = handleToolsCall(req);
            // Claude CLI probes these even when our initialize
            // doesn't advertise prompts/resources — return empty
            // lists so the client treats the server as healthy
            // instead of disconnecting on methodNotFound.
            else if (m == "prompts/list")
                resp = JSONRPCResponse::ok(req.id, nlohmann::json{{"prompts", nlohmann::json::array()}});
            else if (m == "resources/list")
                resp = JSONRPCResponse::ok(req.id, nlohmann::json{{"resources", nlohmann::json::array()}});
            else if (m == "resources/templates/list")
                resp = JSONRPCResponse::ok(req.id, nlohmann::json{{"resourceTemplates", nlohmann::json::array()}});
            else if (m == "ping")
                resp = JSONRPCResponse::ok(req.id, nlohmann::json{{"pong", true}});
            else if (m == "shutdown")
            {
                resp = JSONRPCResponse::ok(req.id, nullptr);
                sendResponse(resp);
                stop();
                return;
            }
        }
        catch (const std::exception& e)
        {
            JSONRPCError err{
                static_cast<int>(JSONRPCError::internalError().code),
                std::string("handler exception: ") + e.what()
            };
            resp = JSONRPCResponse::fail(req.id, err);
        }
        catch (...)
        {
            resp = JSONRPCResponse::fail(req.id, JSONRPCError::internalError());
        }

        sendResponse(resp);
    }

    JSONRPCResponse MCPServer::handleInitialize(const JSONRPCRequest& req)
    {
        if (req.params.is_object() && req.params.contains("clientInfo"))
        {
            const auto& ci = req.params["clientInfo"];
            clientInfo_.name = ci.value("name", std::string{"unknown"});
            clientInfo_.version = ci.value("version", std::string{"0.0.0"});
        }

        const auto info = MCPServerInfo::gridex(serverVersion_);
        const nlohmann::json result{
            {"serverInfo",
                {{"name", info.name}, {"version", info.version}}},
            {"protocolVersion", info.protocolVersion},
            {"capabilities", mcpDefaultCapabilities()}
        };
        return JSONRPCResponse::ok(req.id, result);
    }

    JSONRPCResponse MCPServer::handleToolsList(const JSONRPCRequest& req)
    {
        auto defs = toolRegistry_.definitions();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& d : defs)
        {
            nlohmann::json dj;
            to_json(dj, d);
            arr.push_back(dj);
        }
        return JSONRPCResponse::ok(req.id, nlohmann::json{{"tools", arr}});
    }

    JSONRPCResponse MCPServer::handleToolsCall(const JSONRPCRequest& req)
    {
        if (!req.params.is_object() ||
            !req.params.contains("name") || !req.params["name"].is_string())
            return JSONRPCResponse::fail(req.id, JSONRPCError::invalidParams());

        const std::string name = req.params["name"].get<std::string>();
        const nlohmann::json args = req.params.value("arguments", nlohmann::json::object());

        auto tool = toolRegistry_.get(name);
        if (!tool)
        {
            JSONRPCError e;
            e.code = static_cast<int>(MCPErrorCode::NotFound);
            e.message = "Tool '" + name + "' not found";
            return JSONRPCResponse::fail(req.id, e);
        }

        const auto startTime = std::chrono::steady_clock::now();

        MCPAuditClient auditClient{
            clientInfo_.name,
            clientInfo_.version,
            mode_ == MCPTransportMode::Stdio ? "stdio" : "http"
        };
        MCPToolContext ctx{ pool_, permissionEngine_, rateLimiter_,
                             approvalGate_, auditLogger_, auditClient };

        // Best-effort extraction for audit entry fields.
        std::optional<std::string> connIdStr;
        std::optional<std::string> connTypeStr;
        if (args.contains("connection_id") && args["connection_id"].is_string())
        {
            connIdStr = args["connection_id"].get<std::string>();
            // Try to resolve type (non-fatal).
            try
            {
                auto wid = utf8ToWide(*connIdStr);
                auto configs = ConnectionStore::Load();
                for (const auto& c : configs)
                    if (c.id == wid)
                    { connTypeStr = wideToUtf8(DatabaseTypeDisplayName(c.databaseType)); break; }
            }
            catch (...) {}
        }

        MCPAuditEntry audit;
        audit.eventId = generateEventId();
        audit.client = auditClient;
        audit.tool = name;
        audit.tier = static_cast<int>(tool->tier());
        audit.connectionId = connIdStr;
        audit.connectionType = connTypeStr;

        if (args.contains("sql") && args["sql"].is_string())
        {
            std::optional<int> pc;
            if (args.contains("params") && args["params"].is_array())
                pc = static_cast<int>(args["params"].size());
            audit.input = MCPAuditInput::fromSQL(args["sql"].get<std::string>(), pc);
        }

        try
        {
            auto result = tool->execute(args, ctx);
            const auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startTime).count();
            audit.result.durationMs = static_cast<int>(dur);
            audit.result.status = result.isError
                ? MCPAuditStatus::Error : MCPAuditStatus::Success;

            // Record mode + usage.
            if (connIdStr)
            {
                const auto wid = utf8ToWide(*connIdStr);
                audit.security.permissionMode = permissionEngine_.getMode(wid);
                rateLimiter_.recordUsage(tool->tier(), wid);
            }
            auditLogger_.log(audit);

            nlohmann::json rj;
            to_json(rj, result);
            return JSONRPCResponse::ok(req.id, rj);
        }
        catch (const MCPToolError& e)
        {
            const auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startTime).count();
            const std::string err = sanitizeError(e.what());

            audit.result.durationMs = static_cast<int>(dur);
            audit.result.status = MCPAuditStatus::Error;
            audit.error = err;
            if (connIdStr)
                audit.security.permissionMode =
                    permissionEngine_.getMode(utf8ToWide(*connIdStr));
            auditLogger_.log(audit);

            // Tool errors surface as tool result with isError=true
            // (mac behavior) rather than JSON-RPC errors, so the AI
            // client sees the message content.
            auto tr = MCPToolResult::error(err);
            nlohmann::json rj;
            to_json(rj, tr);
            return JSONRPCResponse::ok(req.id, rj);
        }
        catch (const std::exception& e)
        {
            const auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startTime).count();
            const std::string err = sanitizeError(e.what());

            audit.result.durationMs = static_cast<int>(dur);
            audit.result.status = MCPAuditStatus::Error;
            audit.error = err;
            auditLogger_.log(audit);

            auto tr = MCPToolResult::error(err);
            nlohmann::json rj;
            to_json(rj, tr);
            return JSONRPCResponse::ok(req.id, rj);
        }
    }

    void MCPServer::sendResponse(const JSONRPCResponse& resp)
    {
        if (mode_ == MCPTransportMode::Stdio)
            stdio_.send(resp);
        // HTTP transport: Phase 4d.
    }

    std::string MCPServer::sanitizeError(const std::string& msg)
    {
        static const std::regex reUsers(R"(/Users/[^/\s]+)");
        static const std::regex reHome(R"(/home/[^/\s]+)");
        static const std::regex reWinUser(R"([Cc]:\\Users\\[^\\\s]+)");
        static const std::regex rePG(R"(postgres://[^\s]+)");
        static const std::regex reMY(R"(mysql://[^\s]+)");
        static const std::regex reMG(R"(mongodb://[^\s]+)");

        std::string s = msg;
        s = std::regex_replace(s, reUsers, "[path]");
        s = std::regex_replace(s, reHome,  "[path]");
        s = std::regex_replace(s, reWinUser,"[path]");
        s = std::regex_replace(s, rePG,    "[connection]");
        s = std::regex_replace(s, reMY,    "[connection]");
        s = std::regex_replace(s, reMG,    "[connection]");
        return s;
    }
}
