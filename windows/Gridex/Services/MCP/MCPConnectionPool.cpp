//
// MCPConnectionPool.cpp
//
// This duplicates ConnectionManager::createAdapter intentionally —
// keeping the pool decoupled avoids a circular include (Services →
// ConnectionManager → Services again once MCP hooks are added).

#include "MCPConnectionPool.h"
#include "../../Models/PostgreSQLAdapter.h"
#include "../../Models/MySQLAdapter.h"
#include "../../Models/SQLiteAdapter.h"
#include "../../Models/RedisAdapter.h"
#include "../../Models/MongoDBAdapter.h"
#include "../../Models/MSSQLAdapter.h"

namespace DBModels
{
    std::shared_ptr<DatabaseAdapter> MCPConnectionPool::createAdapter(DatabaseType t)
    {
        switch (t)
        {
            case DatabaseType::PostgreSQL: return std::make_shared<PostgreSQLAdapter>();
            case DatabaseType::MySQL:      return std::make_shared<MySQLAdapter>();
            case DatabaseType::SQLite:     return std::make_shared<SQLiteAdapter>();
            case DatabaseType::Redis:      return std::make_shared<RedisAdapter>();
            case DatabaseType::MongoDB:    return std::make_shared<MongoDBAdapter>();
            case DatabaseType::MSSQLServer:return std::make_shared<MSSQLAdapter>();
        }
        throw DatabaseError(DatabaseError::Code::ConnectionFailed,
            "Unsupported database type");
    }

    std::shared_ptr<DatabaseAdapter> MCPConnectionPool::acquire(
        const ConnectionConfig& config, const std::wstring& password)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = adapters_.find(config.id);
        if (it != adapters_.end() && it->second && it->second->isConnected())
            return it->second;

        // Either missing or stale — create fresh and connect.
        auto adapter = createAdapter(config.databaseType);
        adapter->connect(config, password);
        adapters_[config.id] = adapter;
        return adapter;
    }

    void MCPConnectionPool::release(const std::wstring& id)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = adapters_.find(id);
        if (it != adapters_.end())
        {
            try { if (it->second) it->second->disconnect(); } catch (...) {}
            adapters_.erase(it);
        }
    }

    void MCPConnectionPool::releaseAll()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& [_, a] : adapters_)
            try { if (a) a->disconnect(); } catch (...) {}
        adapters_.clear();
    }

    bool MCPConnectionPool::isCached(const std::wstring& id) const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return adapters_.count(id) != 0;
    }

    size_t MCPConnectionPool::cachedCount() const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return adapters_.size();
    }
}
