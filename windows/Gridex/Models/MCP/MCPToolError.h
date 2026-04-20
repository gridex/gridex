#pragma once
//
// MCPToolError.h
// Gridex
//
// Typed exception thrown by MCP tools. The MCPServer catches these
// and folds them into MCPToolResult::error(...) so the AI client
// sees a structured failure message. Mirrors MCPToolError in
// macos/Services/MCP/Tools/MCPTool.swift.
//
// Tools should throw via the static factories rather than
// constructing directly — this keeps the message format consistent
// with macOS.

#include <stdexcept>
#include <string>

namespace DBModels
{
    class MCPToolError : public std::runtime_error
    {
    public:
        enum class Kind
        {
            ConnectionNotFound,
            ConnectionNotConnected,
            TableNotFound,
            InvalidParameters,
            PermissionDenied,
            QueryFailed,
            RateLimitExceeded
        };

        Kind kind;
        int retryAfterSeconds = 0; // only meaningful for RateLimitExceeded

        MCPToolError(Kind k, const std::string& msg, int retryAfter = 0)
            : std::runtime_error(msg), kind(k), retryAfterSeconds(retryAfter) {}

        // ── Factories — keep messages identical to Swift for UX parity ──

        static MCPToolError connectionNotFound(const std::string& id)
        {
            return MCPToolError(Kind::ConnectionNotFound,
                "Connection '" + id + "' not found. Use list_connections to see available connections.");
        }

        static MCPToolError connectionNotConnected(const std::string& id)
        {
            return MCPToolError(Kind::ConnectionNotConnected,
                "Connection '" + id + "' is not active. The user needs to connect first.");
        }

        static MCPToolError tableNotFound(const std::string& name)
        {
            return MCPToolError(Kind::TableNotFound,
                "Table '" + name + "' not found.");
        }

        static MCPToolError invalidParameters(const std::string& msg)
        {
            return MCPToolError(Kind::InvalidParameters,
                "Invalid parameters: " + msg);
        }

        static MCPToolError permissionDenied(const std::string& msg)
        {
            return MCPToolError(Kind::PermissionDenied, msg);
        }

        static MCPToolError queryFailed(const std::string& msg)
        {
            return MCPToolError(Kind::QueryFailed,
                "Query failed: " + msg);
        }

        static MCPToolError rateLimitExceeded(int retryAfter)
        {
            return MCPToolError(Kind::RateLimitExceeded,
                "Rate limit exceeded. Retry after " + std::to_string(retryAfter) + " seconds.",
                retryAfter);
        }
    };
}
