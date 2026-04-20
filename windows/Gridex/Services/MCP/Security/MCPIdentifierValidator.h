#pragma once
//
// MCPIdentifierValidator.h
// Gridex
//
// Allowlist check for SQL identifiers (table / column / schema
// names) received from MCP clients. Defense-in-depth: even when
// the identifier later hits `quoteSqlIdentifier`, anything outside
// a strict ASCII shape is rejected before reaching a driver.
//
// Port of macos/Services/MCP/Security/MCPIdentifierValidator.swift.

#include <string>
#include <optional>
#include <nlohmann/json.hpp>

namespace DBModels
{
    namespace MCPIdentifierValidator
    {
        constexpr int kMaxLength = 128;

        // ASCII: first char [A-Za-z_], rest [A-Za-z0-9_], length 1..128.
        bool isValid(const std::string& identifier);

        // Throws MCPToolError::InvalidParameters on failure.
        void validate(const std::string& identifier, const std::string& name = "identifier");

        // Shared extraction used by the write tools. Pulls
        // `table_name` (required) and `schema` (optional) from the
        // JSON params, validating both. Returns (table, schema_or_null).
        struct TableSchema
        {
            std::string table;
            std::optional<std::string> schema;
        };
        TableSchema extractTableAndSchema(const nlohmann::json& params);
    }
}
