#pragma once
//
// MCPConnectionPool.h
// Gridex
//
// Per-MCP adapter cache, separate from ConnectionManager (which
// only holds the GUI's single active connection). Tools borrow
// warm adapters from this pool keyed by connectionId — avoids
// 200-800ms reconnect latency per invocation and preserves
// Redis/Mongo session state.
//
// Thread-safe: invoked from stdio reader thread + HTTP worker
// threads concurrently.

#include <string>
#include <memory>
#include <unordered_map>
#include <mutex>
#include "../../Models/DatabaseAdapter.h"
#include "../../Models/ConnectionConfig.h"

namespace DBModels
{
    class MCPConnectionPool
    {
    public:
        // Returns an adapter for `config`, opening a new connection
        // if one isn't cached. Throws DatabaseError on connect failure.
        // `password` is already resolved by the caller via CredentialManager.
        std::shared_ptr<DatabaseAdapter> acquire(
            const ConnectionConfig& config,
            const std::wstring& password);

        // Close + drop one cached adapter (e.g. when connection is
        // deleted from ConnectionStore).
        void release(const std::wstring& connectionId);

        // Close + drop everything (server shutdown).
        void releaseAll();

        // Introspection for status UI.
        bool isCached(const std::wstring& connectionId) const;
        size_t cachedCount() const;

    private:
        mutable std::mutex mtx_;
        std::unordered_map<std::wstring, std::shared_ptr<DatabaseAdapter>> adapters_;

        // Helper: creates the adapter matching `config.databaseType`.
        // Same switch used by ConnectionManager::createAdapter — we
        // duplicate here to avoid a circular dep on ConnectionManager.cpp.
        static std::shared_ptr<DatabaseAdapter> createAdapter(DatabaseType type);
    };
}
