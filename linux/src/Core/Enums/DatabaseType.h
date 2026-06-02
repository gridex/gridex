#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>

#include "Core/Enums/SQLDialect.h"

namespace gridex {

enum class DatabaseType {
    PostgreSQL,
    MySQL,
    SQLite,
    Redis,
    MongoDB,
    MSSQL,
    ClickHouse,
};

inline constexpr std::array<DatabaseType, 7> kAllDatabaseTypes = {
    DatabaseType::PostgreSQL, DatabaseType::MySQL,    DatabaseType::SQLite,
    DatabaseType::Redis,      DatabaseType::MongoDB,  DatabaseType::MSSQL,
    DatabaseType::ClickHouse,
};

inline std::string_view rawValue(DatabaseType t) {
    switch (t) {
        case DatabaseType::PostgreSQL: return "postgresql";
        case DatabaseType::MySQL:      return "mysql";
        case DatabaseType::SQLite:     return "sqlite";
        case DatabaseType::Redis:      return "redis";
        case DatabaseType::MongoDB:    return "mongodb";
        case DatabaseType::MSSQL:      return "mssql";
        case DatabaseType::ClickHouse: return "clickhouse";
    }
    return "";
}

inline std::optional<DatabaseType> databaseTypeFromRaw(std::string_view raw) {
    if (raw == "clickhouse") return DatabaseType::ClickHouse;
    if (raw == "postgresql") return DatabaseType::PostgreSQL;
    if (raw == "mysql")      return DatabaseType::MySQL;
    if (raw == "sqlite")     return DatabaseType::SQLite;
    if (raw == "redis")      return DatabaseType::Redis;
    if (raw == "mongodb")    return DatabaseType::MongoDB;
    if (raw == "mssql")      return DatabaseType::MSSQL;
    return std::nullopt;
}

inline std::string_view displayName(DatabaseType t) {
    switch (t) {
        case DatabaseType::PostgreSQL: return "PostgreSQL";
        case DatabaseType::MySQL:      return "MySQL";
        case DatabaseType::SQLite:     return "SQLite";
        case DatabaseType::Redis:      return "Redis";
        case DatabaseType::MongoDB:    return "MongoDB";
        case DatabaseType::MSSQL:      return "SQL Server";
        case DatabaseType::ClickHouse: return "ClickHouse";
    }
    return "";
}

inline constexpr int defaultPort(DatabaseType t) {
    switch (t) {
        case DatabaseType::PostgreSQL: return 5432;
        case DatabaseType::MySQL:      return 3306;
        case DatabaseType::SQLite:     return 0;
        case DatabaseType::Redis:      return 6379;
        case DatabaseType::MongoDB:    return 27017;
        case DatabaseType::MSSQL:      return 1433;
        case DatabaseType::ClickHouse: return 8123;  // 8443 when sslEnabled
    }
    return 0;
}

inline SQLDialect sqlDialect(DatabaseType t) {
    switch (t) {
        case DatabaseType::PostgreSQL: return SQLDialect::PostgreSQL;
        case DatabaseType::MySQL:      return SQLDialect::MySQL;
        case DatabaseType::SQLite:     return SQLDialect::SQLite;
        case DatabaseType::Redis:      return SQLDialect::Redis;
        case DatabaseType::MongoDB:    return SQLDialect::MongoDB;
        case DatabaseType::MSSQL:      return SQLDialect::MSSQL;
        case DatabaseType::ClickHouse: return SQLDialect::ClickHouse;
    }
    return SQLDialect::SQLite;
}

inline constexpr bool isSQL(DatabaseType t) {
    switch (t) {
        case DatabaseType::PostgreSQL:
        case DatabaseType::MySQL:
        case DatabaseType::SQLite:
        case DatabaseType::MSSQL:
        case DatabaseType::ClickHouse:
            return true;
        case DatabaseType::Redis:
        case DatabaseType::MongoDB:
            return false;
    }
    return false;
}

inline constexpr bool supportsSchemas(DatabaseType t) {
    switch (t) {
        case DatabaseType::PostgreSQL:
        case DatabaseType::MSSQL:
            return true;
        case DatabaseType::MySQL:
        case DatabaseType::SQLite:
        case DatabaseType::Redis:
        case DatabaseType::MongoDB:
        case DatabaseType::ClickHouse:
            return false;
    }
    return false;
}

}
