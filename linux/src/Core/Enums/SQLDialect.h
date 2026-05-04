#pragma once

#include <string>
#include <string_view>

namespace gridex {

enum class SQLDialect {
    PostgreSQL,
    MySQL,
    SQLite,
    Redis,
    MongoDB,
    MSSQL,
    ClickHouse,
};

inline std::string quoteIdentifier(SQLDialect dialect, std::string_view identifier) {
    std::string out;
    out.reserve(identifier.size() + 2);

    auto escapeWith = [&](char quote) {
        out.push_back(quote);
        for (char c : identifier) {
            if (c == quote) out.push_back(quote);
            out.push_back(c);
        }
        out.push_back(quote);
    };

    switch (dialect) {
        case SQLDialect::PostgreSQL:
        case SQLDialect::SQLite:
            escapeWith('"');
            return out;
        case SQLDialect::MySQL:
        case SQLDialect::ClickHouse:
            // ClickHouse accepts both backticks and double quotes; backticks
            // are unambiguous since CH treats double-quoted strings as
            // string literals when ANSI mode is off.
            escapeWith('`');
            return out;
        case SQLDialect::MSSQL: {
            out.push_back('[');
            for (char c : identifier) {
                if (c == ']') out.push_back(']');
                out.push_back(c);
            }
            out.push_back(']');
            return out;
        }
        case SQLDialect::Redis:
        case SQLDialect::MongoDB:
            return std::string(identifier);
    }
    return std::string(identifier);
}

inline bool sqlDialectSupportsDoubleQuoted(SQLDialect dialect) {
    return dialect == SQLDialect::PostgreSQL || dialect == SQLDialect::SQLite;
}

inline std::string parameterPlaceholder(SQLDialect dialect, int index) {
    switch (dialect) {
        case SQLDialect::PostgreSQL:
            return "$" + std::to_string(index);
        case SQLDialect::MSSQL:
            return "@p" + std::to_string(index);
        case SQLDialect::MySQL:
        case SQLDialect::SQLite:
        case SQLDialect::Redis:
        case SQLDialect::MongoDB:
        case SQLDialect::ClickHouse:
            return "?";
    }
    return "?";
}

inline constexpr std::string_view limitClause(SQLDialect) { return "LIMIT"; }
inline constexpr std::string_view offsetClause(SQLDialect) { return "OFFSET"; }

}
