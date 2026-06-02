#include "Data/Adapters/AdapterFactory.h"

#include <string>

#include "Core/Errors/GridexError.h"
#include "Data/Adapters/ClickHouse/ClickhouseAdapter.h"
#include "Data/Adapters/MSSQL/MssqlAdapter.h"
#include "Data/Adapters/MongoDB/MongodbAdapter.h"
#include "Data/Adapters/MySQL/MysqlAdapter.h"
#include "Data/Adapters/PostgreSQL/PostgresAdapter.h"
#include "Data/Adapters/Redis/RedisAdapter.h"
#include "Data/Adapters/SQLite/SqliteAdapter.h"

namespace gridex {

std::unique_ptr<IDatabaseAdapter> createAdapter(DatabaseType type) {
    switch (type) {
        case DatabaseType::SQLite:
            return std::make_unique<SqliteAdapter>();
        case DatabaseType::PostgreSQL:
            return std::make_unique<PostgresAdapter>();
        case DatabaseType::MySQL:
            return std::make_unique<MysqlAdapter>();
        case DatabaseType::MSSQL:
            return std::make_unique<MssqlAdapter>();
        case DatabaseType::Redis:
            return std::make_unique<RedisAdapter>();
        case DatabaseType::MongoDB:
            return std::make_unique<MongodbAdapter>();
        case DatabaseType::ClickHouse:
            return std::make_unique<ClickhouseAdapter>();
    }
    throw ConfigurationError("Unknown DatabaseType");
}

}
