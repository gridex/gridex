#pragma once
//
// MCPErrorCode.h
// Gridex
//
// Gridex-specific JSON-RPC error codes. The integer values are part
// of the MCP client contract and MUST match macOS
// (macos/Core/Models/MCP/MCPProtocol.swift : MCPErrorCode) exactly.
// Claude Desktop, Cursor, etc. surface these codes to the user, so
// renumbering would silently break downstream clients.

namespace DBModels
{
    enum class MCPErrorCode : int
    {
        PermissionDenied   = -32001,
        ApprovalTimeout    = -32002,
        ApprovalDenied     = -32003,
        ConnectionError    = -32004,
        SyntaxError        = -32005,
        NotFound           = -32006,
        RateLimitExceeded  = -32007,
        ScopeDenied        = -32008,
        QueryTimeout       = -32009
    };
}
