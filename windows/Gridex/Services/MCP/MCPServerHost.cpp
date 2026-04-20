//
// MCPServerHost.cpp
//

#include "MCPServerHost.h"

namespace DBModels { namespace MCPServerHost {

namespace {
    std::mutex g_mtx;
    // shared_ptr so a concurrent `stop()` cannot tear the object
    // out from under a caller that already extracted the pointer.
    std::shared_ptr<MCPServer> g_server;
}

std::shared_ptr<MCPServer> ensureCreated(const AppSettings& settings,
                                          const std::string& version,
                                          MCPTransportMode mode)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_server)
        g_server = std::make_shared<MCPServer>(settings, version, mode);
    return g_server;
}

std::shared_ptr<MCPServer> instance()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_server;
}

void start()
{
    std::shared_ptr<MCPServer> local;
    { std::lock_guard<std::mutex> lk(g_mtx); local = g_server; }
    if (local) local->start();
}

void stop()
{
    std::shared_ptr<MCPServer> local;
    { std::lock_guard<std::mutex> lk(g_mtx); local = std::move(g_server); }
    if (local) local->stop();
    // `local` drops here; any other thread still holding a
    // previously-returned shared_ptr sees isRunning()==false and
    // bails safely instead of touching freed memory.
}

}} // namespace
