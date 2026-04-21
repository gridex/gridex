// SQLDialect.swift
// Gridex

import Foundation

enum SQLDialect: Sendable {
    case postgresql
    case mysql
    case sqlite
    case redis
    case mongodb
    case mssql
    case clickhouse

    func quoteIdentifier(_ identifier: String) -> String {
        switch self {
        case .postgresql, .sqlite:
            return "\"\(identifier.replacingOccurrences(of: "\"", with: "\"\""))\""
        case .mysql, .clickhouse:
            return "`\(identifier.replacingOccurrences(of: "`", with: "``"))`"
        case .mssql:
            return "[\(identifier.replacingOccurrences(of: "]", with: "]]"))]"
        case .redis, .mongodb:
            return identifier // No quoting for non-SQL databases
        }
    }

    func qualifiedIdentifier(_ name: String, schema: String? = nil) -> String {
        guard let schema else { return quoteIdentifier(name) }
        return "\(quoteIdentifier(schema)).\(quoteIdentifier(name))"
    }

    var limitClause: String {
        "LIMIT"
    }

    var offsetClause: String {
        "OFFSET"
    }

    var parameterPlaceholder: (Int) -> String {
        switch self {
        case .postgresql:
            return { "$\($0)" }
        case .mssql:
            return { "@p\($0)" }
        case .mysql, .sqlite, .redis, .mongodb, .clickhouse:
            return { _ in "?" }
        }
    }
}
