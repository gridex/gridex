#pragma once
//
// StdioTransport.h
// Gridex
//
// Newline-delimited JSON-RPC over stdin/stdout for the
// `--mcp-stdio` CLI mode. One request per line. Mirrors
// macos/Services/MCP/Transport/StdioTransport.swift.
//
// Reads on a dedicated background thread and dispatches parsed
// requests via a callback. Writes are serialized on a separate
// mutex — call send() from any thread.

#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <functional>
#include "../../../Models/MCP/MCPProtocol.h"

namespace DBModels
{
    class StdioTransport
    {
    public:
        // Callback invoked for each valid JSON-RPC request. Runs on
        // the reader thread; the implementation is expected to
        // handle its own synchronization (or dispatch to a worker).
        using RequestHandler = std::function<void(const JSONRPCRequest&)>;

        StdioTransport() = default;
        ~StdioTransport();

        void setRequestHandler(RequestHandler h) { handler_ = std::move(h); }

        // Starts the reader thread. Returns immediately.
        void start();

        // Signals the reader to stop. The process exits naturally
        // when stdin closes (EOF) so this is mainly for shutdown.
        void stop();

        // Thread-safe. Writes one JSON-RPC response + trailing
        // newline to stdout and flushes.
        void send(const JSONRPCResponse& response);

        // Push a JSON-RPC notification (id=null) — e.g. progress.
        void sendNotification(const std::string& method,
                              const nlohmann::json& params);

    private:
        std::atomic<bool> running_{false};
        std::thread reader_;
        std::mutex writeMtx_;
        RequestHandler handler_;

        void readLoop();
    };
}
