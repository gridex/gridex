#pragma once
//
// MCPServerInfo.h
// Gridex
//
// Server advertisement returned in `initialize` handshake. Mirrors
// MCPServerInfo.gridex() in macos/Core/Models/MCP/MCPProtocol.swift.
// The `protocolVersion` string is dictated by the MCP spec (see
// https://spec.modelcontextprotocol.io) — do NOT bump without
// first validating that Claude Desktop / Cursor / etc still accept
// the new value.

#include <string>
#include <nlohmann/json.hpp>

namespace DBModels
{
    struct MCPServerInfo
    {
        std::string name = "gridex";
        std::string version;
        std::string protocolVersion = "2024-11-05";

        static MCPServerInfo gridex(const std::string& appVersion)
        {
            MCPServerInfo s;
            s.version = appVersion;
            return s;
        }
    };

    // Capabilities advertised in `initialize` result. We only
    // implement `tools` right now — advertising `resources` /
    // `prompts` the way macOS does would be a protocol lie since
    // we return methodNotFound on their list endpoints, and
    // Claude CLI drops the connection when the server contradicts
    // its own capability manifest. Expose tools only; prompts +
    // resources land when their handlers do.
    inline nlohmann::json mcpDefaultCapabilities()
    {
        return {
            {"tools",   {{"listChanged", true}}},
            {"logging", nlohmann::json::object()}
        };
    }
}
