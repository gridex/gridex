#include "Services/MCP/MCPServer.h"

#include <chrono>
#include <random>
#include <regex>
#include <sstream>

namespace gridex::mcp {

namespace {

std::string newUuid() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<uint64_t> dist;
    uint64_t a = dist(rng), b = dist(rng);
    char buf[40];
    std::snprintf(buf, sizeof(buf),
        "%08x-%04x-%04x-%04x-%012llx",
        static_cast<uint32_t>(a >> 32),
        static_cast<uint16_t>(a >> 16) & 0xFFFF,
        (static_cast<uint16_t>(a) & 0x0FFF) | 0x4000,
        (static_cast<uint16_t>(b >> 48) & 0x3FFF) | 0x8000,
        static_cast<unsigned long long>(b & 0xFFFFFFFFFFFFULL));
    return buf;
}

std::string nowIso8601() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

}  // namespace

MCPServer::MCPServer(std::shared_ptr<IMCPConnectionProvider> provider,
                     std::string serverVersion,
                     MCPTransportMode mode)
    : connectionProvider_(std::move(provider)),
      serverVersion_(std::move(serverVersion)),
      mode_(mode),
      toolRegistry_(std::make_unique<ToolRegistry>()),
      permissionEngine_(std::make_unique<PermissionEngine>()),
      auditLogger_(std::make_unique<AuditLogger>()),
      rateLimiter_(std::make_unique<RateLimiter>()),
      approvalGate_(std::make_unique<ApprovalGate>()) {
    toolRegistry_->registerBuiltins();
    if (mode_ == MCPTransportMode::Stdio) {
        transport_ = std::make_unique<StdioTransport>();
    }
}

MCPServer::~MCPServer() { stop(); }

void MCPServer::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;
    startTime_ = std::chrono::system_clock::now();
    if (transport_) {
        transport_->setHandler([this](const JSONRPCRequest& req) {
            // JSON-RPC 2.0: notifications have no `id` and MUST NOT receive a
            // response. Claude Code drops the connection if it gets an
            // unsolicited response (zod validation rejects id=null).
            const bool isNotification = req.id.is_null();
            auto resp = handleRequest(req);
            if (!isNotification) transport_->send(resp);
        });
        transport_->start();
    }
}

void MCPServer::stop() {
    if (!running_.exchange(false)) return;
    if (transport_) transport_->stop();
    auditLogger_->close();
}

void MCPServer::setConnectionMode(const std::string& id, MCPConnectionMode mode) {
    permissionEngine_->setMode(id, mode);
}

MCPConnectionMode MCPServer::getConnectionMode(const std::string& id) const {
    return permissionEngine_->getMode(id);
}

JSONRPCResponse MCPServer::handleRequest(const JSONRPCRequest& req) {
    if (req.method == "initialize")  return handleInitialize(req);
    if (req.method == "initialized") return JSONRPCResponse::ok(req.id, nullptr);  // notification
    if (req.method == "tools/list")  return handleToolsList(req);
    if (req.method == "tools/call")  return handleToolCall(req);
    if (req.method == "ping")        return JSONRPCResponse::ok(req.id, json{{"pong", true}});
    if (req.method == "shutdown") {
        auto r = JSONRPCResponse::ok(req.id, nullptr);
        stop();
        return r;
    }
    return JSONRPCResponse::err(req.id, JSONRPCError::methodNotFound());
}

JSONRPCResponse MCPServer::handleInitialize(const JSONRPCRequest& req) {
    if (req.params.is_object() && req.params.contains("clientInfo")) {
        const auto& ci = req.params["clientInfo"];
        std::lock_guard lk(stateMu_);
        clientInfo_.name    = ci.value("name", "unknown");
        clientInfo_.version = ci.value("version", "0.0.0");
    }
    json result = {
        {"serverInfo", {{"name", "gridex"}, {"version", serverVersion_}}},
        {"protocolVersion", "2024-11-05"},
        {"capabilities", {
            {"tools", {{"listChanged", true}}},
            {"resources", {{"subscribe", true}, {"listChanged", true}}},
            {"prompts", {{"listChanged", true}}},
            {"logging", json::object()},
        }},
    };
    return JSONRPCResponse::ok(req.id, result);
}

JSONRPCResponse MCPServer::handleToolsList(const JSONRPCRequest& req) {
    json arr = json::array();
    for (const auto& def : toolRegistry_->definitions()) arr.push_back(def.toJson());
    return JSONRPCResponse::ok(req.id, json{{"tools", arr}});
}

JSONRPCResponse MCPServer::handleToolCall(const JSONRPCRequest& req) {
    if (!req.params.is_object() || !req.params.contains("name") || !req.params["name"].is_string()) {
        return JSONRPCResponse::err(req.id, JSONRPCError::invalidParams());
    }
    const std::string toolName = req.params["name"].get<std::string>();
    json args = req.params.value("arguments", json::object());

    auto tool = toolRegistry_->get(toolName);
    if (!tool) {
        JSONRPCError e{static_cast<int>(MCPErrorCode::NotFound),
                       "Tool '" + toolName + "' not found", std::nullopt};
        return JSONRPCResponse::err(req.id, e);
    }

    auto start = std::chrono::steady_clock::now();

    MCPClientInfo info;
    {
        std::lock_guard lk(stateMu_);
        info = clientInfo_;
    }
    MCPAuditClient clientAudit;
    clientAudit.name      = info.name;
    clientAudit.version   = info.version;
    clientAudit.transport = (mode_ == MCPTransportMode::Stdio) ? "stdio" : "in-process";

    std::optional<std::string> connIdOpt;
    std::optional<std::string> connTypeOpt;
    if (args.contains("connection_id") && args["connection_id"].is_string()) {
        std::string id = args["connection_id"].get<std::string>();
        connIdOpt = id;
        try {
            if (connectionProvider_ && connectionProvider_->hasConnection(id)) {
                for (const auto& c : connectionProvider_->listConnections()) {
                    if (c.id == id) { connTypeOpt = std::string(rawValue(c.databaseType)); break; }
                }
            }
        } catch (...) {}
    }

    MCPToolContext ctx;
    ctx.connectionProvider = connectionProvider_.get();
    ctx.permissionEngine   = permissionEngine_.get();
    ctx.auditLogger        = auditLogger_.get();
    ctx.rateLimiter        = rateLimiter_.get();
    ctx.approvalGate       = approvalGate_.get();
    ctx.client             = clientAudit;

    auto buildAudit = [&](MCPAuditStatus status, int durationMs, const std::optional<std::string>& err) {
        MCPAuditEntry e;
        e.timestampIso8601 = nowIso8601();
        e.eventId          = newUuid();
        e.client           = clientAudit;
        e.tool             = toolName;
        e.tier             = tierRawValue(tool->tier());
        e.connectionId     = connIdOpt;
        e.connectionType   = connTypeOpt;
        std::optional<std::string> sqlOpt;
        std::optional<int> paramsCountOpt;
        if (args.contains("sql") && args["sql"].is_string()) sqlOpt = args["sql"].get<std::string>();
        if (args.contains("params") && args["params"].is_array()) paramsCountOpt = static_cast<int>(args["params"].size());
        e.input            = MCPAuditInput::fromSql(sqlOpt, paramsCountOpt);
        e.result            = {status, std::nullopt, std::nullopt, durationMs, std::nullopt};
        e.security.mode     = connIdOpt ? permissionEngine_->getMode(*connIdOpt) : MCPConnectionMode::Locked;
        e.error            = err;
        return e;
    };

    try {
        auto toolResult = tool->execute(args, ctx);
        int durationMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count());
        auditLogger_->log(buildAudit(
            toolResult.isError ? MCPAuditStatus::Error : MCPAuditStatus::Success,
            durationMs, std::nullopt));
        return JSONRPCResponse::ok(req.id, toolResult.toJson());
    } catch (const std::exception& e) {
        int durationMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count());
        std::string msg = sanitizeError(e.what());
        auditLogger_->log(buildAudit(MCPAuditStatus::Error, durationMs, msg));
        return JSONRPCResponse::ok(req.id, MCPToolResult::errorResult(msg).toJson());
    }
}

std::string MCPServer::sanitizeError(const std::string& msg) {
    static const std::regex kHome(R"(/home/[^/\s]+)");
    static const std::regex kUsers(R"(/Users/[^/\s]+)");
    static const std::regex kPg(R"(postgres://[^\s]+)");
    static const std::regex kMy(R"(mysql://[^\s]+)");
    static const std::regex kMongo(R"(mongodb://[^\s]+)");
    std::string out = std::regex_replace(msg, kHome, "[path]");
    out = std::regex_replace(out, kUsers, "[path]");
    out = std::regex_replace(out, kPg, "[connection]");
    out = std::regex_replace(out, kMy, "[connection]");
    out = std::regex_replace(out, kMongo, "[connection]");
    return out;
}

}  // namespace gridex::mcp
