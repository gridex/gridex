//
// HttpTransport.cpp
//
// cpp-httplib based JSON-RPC server. The WinSock/OpenSSL headers
// it drags in are massive so we hide them behind a pimpl.

#include "HttpTransport.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>

#pragma warning(push)
#pragma warning(disable: 4996)
#include <httplib.h>
#pragma warning(pop)

namespace DBModels
{
    struct HttpTransport::Impl
    {
        httplib::Server server;
    };

    HttpTransport::HttpTransport() = default;
    HttpTransport::~HttpTransport() { stop(); }

    bool HttpTransport::start(const std::string& host, int port)
    {
        if (running_.exchange(true)) return true;

        impl_ = std::make_unique<Impl>();

        // MCP clients POST JSON-RPC requests to `/` (convention
        // used by the reference Python/Node servers). We accept
        // both `/` and `/mcp` for tool compatibility.
        auto handlePost = [this](const httplib::Request& req, httplib::Response& res)
        {
            res.set_header("Access-Control-Allow-Origin", "*");
            JSONRPCResponse resp;
            try
            {
                auto j = nlohmann::json::parse(req.body);
                JSONRPCRequest rpc;
                from_json(j, rpc);
                if (handler_)
                    resp = handler_(rpc);
                else
                    resp = JSONRPCResponse::fail(rpc.id, JSONRPCError::internalError());
            }
            catch (const std::exception&)
            {
                resp = JSONRPCResponse::fail(nullptr, JSONRPCError::parseError());
            }
            nlohmann::json rj;
            to_json(rj, resp);
            res.set_content(rj.dump(), "application/json");
        };

        auto handleOptions = [](const httplib::Request&, httplib::Response& res)
        {
            // CORS preflight for browser-based clients (dev aid).
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_header("Access-Control-Allow-Methods", "POST, OPTIONS");
            res.set_header("Access-Control-Allow-Headers", "Content-Type");
            res.status = 204;
        };

        impl_->server.Post("/", handlePost);
        impl_->server.Post("/mcp", handlePost);
        impl_->server.Options("/", handleOptions);
        impl_->server.Options("/mcp", handleOptions);

        // GET /health — lightweight check so ops can ping without
        // sending a full JSON-RPC.
        impl_->server.Get("/health", [](const httplib::Request&, httplib::Response& r)
        {
            r.set_content(R"({"status":"ok","server":"gridex-mcp"})", "application/json");
        });

        server_ = std::thread([this, host, port]()
        {
            impl_->server.listen(host.c_str(), port);
            running_.store(false);
        });

        return true;
    }

    void HttpTransport::stop()
    {
        if (!running_.exchange(false)) return;
        if (impl_) impl_->server.stop();
        if (server_.joinable()) server_.join();
        impl_.reset();
    }
}
