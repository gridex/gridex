// SchemaSnapshot.swift
// Gridex
//
// Complete snapshot of a database schema for AI context.

import Foundation

struct SchemaSnapshot: Codable, Sendable {
    let databaseName: String
    let databaseType: DatabaseType
    let schemas: [SchemaInfo]
    let capturedAt: Date

    var allTables: [TableDescription] {
        schemas.flatMap(\.tables)
    }

    var totalTableCount: Int {
        schemas.reduce(0) { $0 + $1.tables.count }
    }
}

struct SchemaInfo: Codable, Sendable {
    let name: String
    let tables: [TableDescription]
    let views: [ViewInfo]
    let functions: [FunctionInfo]
    let enums: [EnumInfo]
}

struct TableDescription: Codable, Sendable, Equatable {
    let name: String
    let schema: String?
    let columns: [ColumnInfo]
    let indexes: [IndexInfo]
    let foreignKeys: [ForeignKeyInfo]
    let constraints: [ConstraintInfo]
    let comment: String?
    let estimatedRowCount: Int?

    var primaryKeyColumns: [ColumnInfo] {
        columns.filter(\.isPrimaryKey)
    }

    func toDDL(dialect: SQLDialect) -> String {
        let tableIdentifier: String
        switch dialect {
        case .postgresql:
            tableIdentifier = dialect.qualifiedIdentifier(name, schema: schema)
        default:
            tableIdentifier = dialect.quoteIdentifier(name)
        }

        var ddl = "CREATE TABLE \(tableIdentifier) (\n"
        ddl += columns.map { "  \($0.name) \($0.dataType)\($0.isNullable ? "" : " NOT NULL")\($0.defaultValue.map { " DEFAULT \($0)" } ?? "")" }.joined(separator: ",\n")
        ddl += "\n);"
        return ddl
    }
}

struct ColumnInfo: Codable, Sendable, Hashable, Identifiable {
    var id: String { name }
    let name: String
    let dataType: String
    let isNullable: Bool
    let defaultValue: String?
    let isPrimaryKey: Bool
    let isAutoIncrement: Bool
    let comment: String?
    let ordinalPosition: Int
    let characterMaxLength: Int?
    var checkConstraint: String?
}

struct IndexInfo: Codable, Sendable, Identifiable, Equatable {
    var id: String { name }
    let name: String
    let columns: [String]
    let isUnique: Bool
    let type: String?
    let tableName: String?
    var condition: String?
    var include: String?
    var comment: String?
}

struct ForeignKeyInfo: Codable, Sendable, Identifiable, Equatable {
    var id: String { "\(columns.joined())->\(referencedTable)" }
    let name: String?
    var columns: [String]
    let referencedTable: String
    var referencedColumns: [String]
    let onDelete: ForeignKeyAction
    let onUpdate: ForeignKeyAction

    var column: String { columns.joined(separator: ", ") }
    var referencedColumn: String { referencedColumns.joined(separator: ", ") }
}

enum ForeignKeyAction: String, Codable, Sendable {
    case cascade = "CASCADE"
    case setNull = "SET NULL"
    case setDefault = "SET DEFAULT"
    case restrict = "RESTRICT"
    case noAction = "NO ACTION"
}

struct ConstraintInfo: Codable, Sendable, Equatable {
    let name: String
    let type: ConstraintType
    let columns: [String]
    let definition: String?
}

enum ConstraintType: String, Codable, Sendable {
    case primaryKey = "PRIMARY KEY"
    case unique = "UNIQUE"
    case check = "CHECK"
    case exclusion = "EXCLUSION"
}

struct TableInfo: Codable, Sendable {
    let name: String
    let schema: String?
    let type: TableKind
    let estimatedRowCount: Int?
}

enum TableKind: String, Codable, Sendable {
    case table
    case view
    case materializedView
    case foreignTable
}

struct ViewInfo: Codable, Sendable {
    let name: String
    let schema: String?
    let definition: String?
    let isMaterialized: Bool
}

struct FunctionInfo: Codable, Sendable {
    let name: String
    let schema: String?
    let returnType: String
    let parameters: String?
    let language: String?
}

struct EnumInfo: Codable, Sendable {
    let name: String
    let schema: String?
    let values: [String]
}

struct ColumnStatistics: Codable, Sendable {
    let columnName: String
    let distinctCount: Int?
    let nullRatio: Double?
    let topValues: [String]?
    let minValue: String?
    let maxValue: String?
}

struct QueryStatisticsEntry: Codable, Sendable {
    let query: String
    let callCount: Int
    let totalTime: TimeInterval
    let meanTime: TimeInterval
    let minTime: TimeInterval
    let maxTime: TimeInterval
}
