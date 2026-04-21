// DatabaseType.swift
// Gridex

import Foundation

enum DatabaseType: String, Codable, Sendable, CaseIterable, Identifiable {
    case postgresql = "postgresql"
    case mysql = "mysql"
    case sqlite = "sqlite"
    case redis = "redis"
    case mongodb = "mongodb"
    case mssql = "mssql"
    case clickhouse = "clickhouse"

    var id: String { rawValue }

    var displayName: String {
        switch self {
        case .postgresql: return "PostgreSQL"
        case .mysql: return "MySQL"
        case .sqlite: return "SQLite"
        case .redis: return "Redis"
        case .mongodb: return "MongoDB"
        case .mssql: return "SQL Server"
        case .clickhouse: return "ClickHouse"
        }
    }

    var defaultPort: Int {
        switch self {
        case .postgresql: return 5432
        case .mysql: return 3306
        case .sqlite: return 0
        case .redis: return 6379
        case .mongodb: return 27017
        case .mssql: return 1433
        case .clickhouse: return 8123
        }
    }

    var iconName: String {
        switch self {
        case .postgresql: return "server.rack"
        case .mysql: return "externaldrive.connected.to.line.below"
        case .sqlite: return "doc"
        case .redis: return "key.fill"
        case .mongodb: return "leaf.fill"
        case .mssql: return "cylinder.split.1x2.fill"
        case .clickhouse: return "bolt.horizontal.circle.fill"
        }
    }

    var sqlDialect: SQLDialect {
        switch self {
        case .postgresql: return .postgresql
        case .mysql: return .mysql
        case .sqlite: return .sqlite
        case .redis: return .redis
        case .mongodb: return .mongodb
        case .mssql: return .mssql
        case .clickhouse: return .clickhouse
        }
    }

    /// Whether this database type uses SQL as its query language.
    var isSQL: Bool {
        switch self {
        case .postgresql, .mysql, .sqlite, .mssql, .clickhouse: return true
        case .redis, .mongodb: return false
        }
    }

    var supportsSchemas: Bool {
        switch self {
        case .postgresql, .mssql: return true
        case .mysql, .sqlite, .redis, .mongodb, .clickhouse: return false
        }
    }
}
