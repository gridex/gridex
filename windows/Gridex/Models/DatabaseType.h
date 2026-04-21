#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace DBModels
{
    enum class DatabaseType
    {
        PostgreSQL,
        MySQL,
        SQLite,
        Redis,
        MongoDB,
        MSSQLServer,
        ClickHouse
    };

    inline std::wstring DatabaseTypeDisplayName(DatabaseType type)
    {
        switch (type)
        {
        case DatabaseType::PostgreSQL: return L"PostgreSQL";
        case DatabaseType::MySQL:      return L"MySQL";
        case DatabaseType::SQLite:     return L"SQLite";
        case DatabaseType::Redis:      return L"Redis";
        case DatabaseType::MongoDB:      return L"MongoDB";
        case DatabaseType::MSSQLServer:  return L"SQL Server";
        case DatabaseType::ClickHouse:   return L"ClickHouse";
        default:                         return L"Unknown";
        }
    }

    inline uint16_t DatabaseTypeDefaultPort(DatabaseType type)
    {
        switch (type)
        {
        case DatabaseType::PostgreSQL: return 5432;
        case DatabaseType::MySQL:      return 3306;
        case DatabaseType::Redis:      return 6379;
        case DatabaseType::MongoDB:      return 27017;
        case DatabaseType::MSSQLServer:  return 1433;
        case DatabaseType::ClickHouse:   return 8123;
        default:                         return 0;
        }
    }

    // Common column types per database engine
    inline std::vector<std::wstring> ColumnTypesForDB(DatabaseType type)
    {
        switch (type)
        {
        case DatabaseType::PostgreSQL:
            return {
                L"bigint", L"bigserial", L"boolean", L"bytea", L"char",
                L"cidr", L"date", L"decimal", L"double precision", L"inet",
                L"integer", L"interval", L"json", L"jsonb", L"macaddr",
                L"money", L"numeric", L"real", L"serial", L"smallint",
                L"smallserial", L"text", L"time", L"time with time zone",
                L"timestamp", L"timestamp with time zone", L"uuid",
                L"varchar", L"varchar(255)", L"xml"
            };
        case DatabaseType::MySQL:
            return {
                L"bigint", L"binary", L"bit", L"blob", L"boolean", L"char",
                L"date", L"datetime", L"decimal", L"double", L"enum",
                L"float", L"int", L"json", L"longblob", L"longtext",
                L"mediumint", L"mediumtext", L"set", L"smallint", L"text",
                L"time", L"timestamp", L"tinyint", L"tinytext",
                L"varbinary", L"varchar(255)"
            };
        case DatabaseType::SQLite:
            return {
                L"BLOB", L"INTEGER", L"NUMERIC", L"REAL", L"TEXT"
            };
        case DatabaseType::Redis:
            // Redis virtual columns — types are fixed for the Keys table
            return {
                L"string", L"list", L"hash", L"set", L"zset", L"stream"
            };
        case DatabaseType::MongoDB:
            return {
                L"String", L"Int32", L"Int64", L"Double", L"Boolean",
                L"Date", L"ObjectId", L"Array", L"Object", L"Binary"
            };
        case DatabaseType::ClickHouse:
            return {
                L"Array", L"Date", L"Date32", L"DateTime", L"DateTime64",
                L"Decimal", L"Enum8", L"Enum16",
                L"Float32", L"Float64",
                L"Int8", L"Int16", L"Int32", L"Int64",
                L"UInt8", L"UInt16", L"UInt32", L"UInt64",
                L"IPv4", L"IPv6", L"JSON",
                L"LowCardinality(String)", L"Map", L"Nullable(String)",
                L"String", L"Tuple", L"UUID"
            };
        case DatabaseType::MSSQLServer:
            return {
                L"bigint", L"bit", L"char", L"date", L"datetime",
                L"datetime2", L"decimal", L"float", L"int", L"money",
                L"nchar", L"ntext", L"numeric", L"nvarchar(255)",
                L"nvarchar(MAX)", L"real", L"smalldatetime", L"smallint",
                L"smallmoney", L"text", L"time", L"tinyint",
                L"uniqueidentifier", L"varbinary", L"varchar(255)",
                L"varchar(MAX)", L"xml"
            };
        default:
            return { L"text", L"integer", L"boolean" };
        }
    }

    // Glyph codes for Segoe MDL2 Assets font
    inline std::wstring DatabaseTypeGlyph(DatabaseType type)
    {
        switch (type)
        {
        case DatabaseType::PostgreSQL: return L"\xE968";
        case DatabaseType::MySQL:      return L"\xE968";
        case DatabaseType::SQLite:     return L"\xE8E5";
        case DatabaseType::Redis:      return L"\xE8D7";  // Permissions/Key
        case DatabaseType::MongoDB:      return L"\xE943";  // Globe/Document DB
        case DatabaseType::MSSQLServer:  return L"\xE968";  // Database
        case DatabaseType::ClickHouse:   return L"\xE968";  // Database (same glyph)
        default:                         return L"\xE774";
        }
    }
}
