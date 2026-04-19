// MCPIdentifierValidator.swift
// Gridex
//
// Allowlist check for SQL identifiers received from MCP clients. Defense-
// in-depth: even when the identifier is later quoted via
// `SQLDialect.quoteIdentifier`, this validator rejects anything outside a
// strict ASCII shape so unusual names never reach a driver.

import Foundation

enum MCPIdentifierValidator {
    static let maxLength = 128

    static func isValid(_ identifier: String) -> Bool {
        var count = 0
        for scalar in identifier.unicodeScalars {
            count += 1
            if count > maxLength { return false }
            let v = scalar.value
            let isLetter = (v >= 0x41 && v <= 0x5A) || (v >= 0x61 && v <= 0x7A) || v == 0x5F
            let isDigit = v >= 0x30 && v <= 0x39
            if count == 1 {
                if !isLetter { return false }
            } else {
                if !isLetter && !isDigit { return false }
            }
        }
        return count > 0
    }

    static func validate(_ identifier: String, as name: String = "identifier") throws {
        guard isValid(identifier) else {
            throw MCPToolError.invalidParameters(
                "\(name) '\(identifier)' contains invalid characters. Allowed: letters, digits, underscore; must start with a letter or underscore; max \(maxLength) chars."
            )
        }
    }

    /// Shared extraction for MCP write tools: reads `table_name` and optional
    /// `schema` from params and validates both. The returned schema is `nil`
    /// when the caller omitted it.
    static func extractTableAndSchema(from params: JSONValue) throws -> (table: String, schema: String?) {
        guard let table = params["table_name"]?.stringValue else {
            throw MCPToolError.invalidParameters("table_name is required")
        }
        try validate(table, as: "table_name")

        let schema = params["schema"]?.stringValue
        if let schema { try validate(schema, as: "schema") }
        return (table, schema)
    }
}
