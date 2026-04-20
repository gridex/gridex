#pragma once
//
// MCPSQLSanitizer.h
// Gridex
//
// Strips SQL line comments, block comments, and string literals so
// syntactic checks (prefix check, dangerous-keyword regex) operate
// on code-only content. NEVER use the sanitized string for
// execution — it is destructive (literals turned into single space).
//
// Port of macos/Services/MCP/Security/MCPSQLSanitizer.swift.

#include <string>

namespace DBModels
{
    namespace MCPSQLSanitizer
    {
        // Returns `sql` with:
        //   -- line comments removed
        //   /* block comments */ removed
        //   'single-quoted literals' collapsed to a single space
        //
        // Handles escaped `''` inside a literal correctly.
        std::string stripCommentsAndStrings(const std::string& sql);
    }
}
