#pragma once
//
// MCPProtocol.h
// Gridex
//
// JSON-RPC 2.0 types for MCP. Mirrors the Swift structs in
// macos/Core/Models/MCP/MCPProtocol.swift. All shapes use
// nlohmann::json for dynamic fields (params, id, result) — this
// avoids the hand-rolled JSONValue sum type on Swift side and
// keeps codec glue trivial.
//
// All types live in namespace DBModels and ship ADL-compatible
// `to_json` / `from_json` so nlohmann's generic `json(req)` and
// `j.get<JSONRPCRequest>()` round-trip losslessly.

#include <string>
#include <optional>
#include <nlohmann/json.hpp>
#include "MCPErrorCode.h"

namespace DBModels
{
    // ── JSON-RPC core ──────────────────────────────────────────

    struct JSONRPCError
    {
        int code = 0;
        std::string message;
        nlohmann::json data;  // optional arbitrary payload

        // Canonical JSON-RPC 2.0 errors (see spec).
        static JSONRPCError parseError()     { return { -32700, "Parse error", nullptr }; }
        static JSONRPCError invalidRequest() { return { -32600, "Invalid Request", nullptr }; }
        static JSONRPCError methodNotFound() { return { -32601, "Method not found", nullptr }; }
        static JSONRPCError invalidParams()  { return { -32602, "Invalid params", nullptr }; }
        static JSONRPCError internalError()  { return { -32603, "Internal error", nullptr }; }

        static JSONRPCError fromCode(MCPErrorCode c, const std::string& msg)
        {
            return { static_cast<int>(c), msg, nullptr };
        }
    };

    inline void to_json(nlohmann::json& j, const JSONRPCError& e)
    {
        j = nlohmann::json{{"code", e.code}, {"message", e.message}};
        if (!e.data.is_null()) j["data"] = e.data;
    }

    inline void from_json(const nlohmann::json& j, JSONRPCError& e)
    {
        e.code = j.value("code", 0);
        e.message = j.value("message", std::string{});
        e.data = j.value("data", nlohmann::json{});
    }

    struct JSONRPCRequest
    {
        std::string jsonrpc = "2.0";
        nlohmann::json id;          // int | string | null (notification)
        std::string method;
        nlohmann::json params;      // object | array | null
    };

    inline void to_json(nlohmann::json& j, const JSONRPCRequest& r)
    {
        j = nlohmann::json{{"jsonrpc", r.jsonrpc}, {"method", r.method}};
        if (!r.id.is_null())     j["id"] = r.id;
        if (!r.params.is_null()) j["params"] = r.params;
    }

    inline void from_json(const nlohmann::json& j, JSONRPCRequest& r)
    {
        r.jsonrpc = j.value("jsonrpc", std::string{"2.0"});
        r.id      = j.value("id", nlohmann::json{});
        r.method  = j.value("method", std::string{});
        r.params  = j.value("params", nlohmann::json{});
    }

    struct JSONRPCResponse
    {
        std::string jsonrpc = "2.0";
        nlohmann::json id;                        // echoed from request
        std::optional<nlohmann::json> result;     // success
        std::optional<JSONRPCError> error;        // failure (mutually exclusive)

        static JSONRPCResponse ok(const nlohmann::json& id, const nlohmann::json& result)
        {
            JSONRPCResponse r;
            r.id = id;
            r.result = result;
            return r;
        }

        static JSONRPCResponse fail(const nlohmann::json& id, const JSONRPCError& err)
        {
            JSONRPCResponse r;
            r.id = id;
            r.error = err;
            return r;
        }
    };

    inline void to_json(nlohmann::json& j, const JSONRPCResponse& r)
    {
        j = nlohmann::json{{"jsonrpc", r.jsonrpc}, {"id", r.id}};
        if (r.result.has_value()) j["result"] = *r.result;
        if (r.error.has_value())
        {
            nlohmann::json ej;
            to_json(ej, *r.error);
            j["error"] = ej;
        }
    }

    inline void from_json(const nlohmann::json& j, JSONRPCResponse& r)
    {
        r.jsonrpc = j.value("jsonrpc", std::string{"2.0"});
        r.id      = j.value("id", nlohmann::json{});
        if (j.contains("result")) r.result = j.at("result");
        if (j.contains("error"))
        {
            JSONRPCError e;
            from_json(j.at("error"), e);
            r.error = e;
        }
    }

    // ── Client info ────────────────────────────────────────────

    struct MCPClientInfo
    {
        std::string name = "unknown";
        std::string version = "0.0.0";
    };

    inline void to_json(nlohmann::json& j, const MCPClientInfo& c)
    {
        j = nlohmann::json{{"name", c.name}, {"version", c.version}};
    }

    inline void from_json(const nlohmann::json& j, MCPClientInfo& c)
    {
        c.name = j.value("name", std::string{"unknown"});
        c.version = j.value("version", std::string{"0.0.0"});
    }
}
