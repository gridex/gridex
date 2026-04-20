#pragma once
//
// HttpTransport.h
// Gridex
//
// HTTP JSON-RPC transport for MCP using cpp-httplib. Listens on
// localhost (or 0.0.0.0 if `allowRemoteHTTP` is set). One request
// per POST. Same RequestHandler callback contract as StdioTransport
// so the MCPServer dispatch layer stays uniform.
//
// Runs the httplib::Server on a dedicated background thread; the
// callback is invoked synchronously on that thread for each POST.
// Response is produced by a helper the handler stores via a
// thread-local shim.

#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <functional>
#include <memory>
#include "../../../Models/MCP/MCPProtocol.h"

namespace DBModels
{
    class HttpTransport
    {
    public:
        // Handler returns the response (synchronous). This is
        // simpler than the stdio model because every POST has a
        // 1:1 response.
        using RequestHandler = std::function<JSONRPCResponse(const JSONRPCRequest&)>;

        HttpTransport();
        ~HttpTransport();

        void setRequestHandler(RequestHandler h) { handler_ = std::move(h); }

        // Starts the HTTP server on `host:port`. `host` = "127.0.0.1"
        // for localhost-only, "0.0.0.0" for remote connections.
        // Returns false if bind fails (port in use).
        bool start(const std::string& host, int port);

        void stop();

        bool isRunning() const { return running_.load(); }

    private:
        std::atomic<bool> running_{false};
        std::thread server_;
        RequestHandler handler_;

        // Pimpl — keeps httplib out of the header so consumers don't
        // drag OpenSSL + WinSock transitively.
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
