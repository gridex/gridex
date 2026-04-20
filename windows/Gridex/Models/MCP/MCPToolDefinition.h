#pragma once
//
// MCPToolDefinition.h
// Gridex
//
// Tool metadata + execution result for the MCP tools/list and
// tools/call protocol methods. Mirrors MCPToolDefinition / MCPContent
// / MCPToolResult in macos/Core/Models/MCP/MCPProtocol.swift.

#include <string>
#include <vector>
#include <optional>
#include <nlohmann/json.hpp>

namespace DBModels
{
    // Emitted by tools/list. `inputSchema` is a JSON Schema object
    // the AI client uses to validate arguments before calling.
    struct MCPToolDefinition
    {
        std::string name;
        std::string description;
        nlohmann::json inputSchema; // JSON Schema object
    };

    inline void to_json(nlohmann::json& j, const MCPToolDefinition& t)
    {
        j = nlohmann::json{
            {"name", t.name},
            {"description", t.description},
            {"inputSchema", t.inputSchema}
        };
    }

    // Single content chunk in a tool result. Matches the MCP spec
    // encoding for type="text" | "image" | "resource" items.
    struct MCPContent
    {
        std::string type = "text"; // "text" | "image" | ...
        std::optional<std::string> text;
        std::optional<std::string> data;      // base64 for image
        std::optional<std::string> mimeType;  // for image / resource
    };

    inline void to_json(nlohmann::json& j, const MCPContent& c)
    {
        j = nlohmann::json{{"type", c.type}};
        if (c.text.has_value())     j["text"] = *c.text;
        if (c.data.has_value())     j["data"] = *c.data;
        if (c.mimeType.has_value()) j["mimeType"] = *c.mimeType;
    }

    // Full tool result — one or more content chunks + error flag.
    // `isError` makes the client surface the text as an AI-visible
    // error rather than a normal tool success output.
    struct MCPToolResult
    {
        std::vector<MCPContent> content;
        bool isError = false;

        static MCPToolResult text(const std::string& s, bool err = false)
        {
            MCPToolResult r;
            MCPContent c;
            c.type = "text";
            c.text = s;
            r.content.push_back(std::move(c));
            r.isError = err;
            return r;
        }

        static MCPToolResult error(const std::string& message)
        {
            return text(message, true);
        }
    };

    inline void to_json(nlohmann::json& j, const MCPToolResult& r)
    {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& c : r.content)
        {
            nlohmann::json cj;
            to_json(cj, c);
            arr.push_back(cj);
        }
        j = nlohmann::json{{"content", arr}};
        if (r.isError) j["isError"] = true;
    }
}
