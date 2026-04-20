#pragma once
//
// MCPServerHost.h
// Gridex
//
// Process-wide singleton that owns the MCPServer instance. Created
// lazily at first start(), kept alive until app exit. Lets
// MainWindow, MCPPage, HomePage, and the CLI entry point share the
// same server without passing it through every constructor.
//
// Thread-safe: start/stop take an internal mutex.

#include <memory>
#include <mutex>
#include "MCPServer.h"

namespace DBModels { namespace MCPServerHost {

// Ensures a server exists (creating on demand with the supplied
// settings + mode) and returns a shared_ptr. Caller keeps the
// shared_ptr for as long as it needs to touch the server — avoids
// UAF races with stop() running on a different thread.
std::shared_ptr<MCPServer> ensureCreated(const AppSettings& settings,
                                         const std::string& version,
                                         MCPTransportMode mode);

// Returns the shared server, or empty shared_ptr if none exists.
// Hold onto the returned shared_ptr for the duration of the call.
std::shared_ptr<MCPServer> instance();

// Starts the shared server (idempotent).
void start();

// Stops + destroys the shared server. Called from app shutdown.
void stop();

}} // namespace
