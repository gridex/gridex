#pragma once
//
// MCPPermissionResult.h
// Gridex
//
// Tagged union returned by MCPPermissionEngine::checkPermission.
// Mirrors macos enum MCPPermissionResult { .allowed, .requiresApproval, .denied(String) }.

#include <string>

namespace DBModels
{
    struct MCPPermissionResult
    {
        enum class Kind { Allowed, RequiresApproval, Denied };
        Kind kind = Kind::Allowed;
        std::string message; // populated only when kind == Denied

        static MCPPermissionResult allowed()
        {
            return { Kind::Allowed, {} };
        }
        static MCPPermissionResult requiresApproval()
        {
            return { Kind::RequiresApproval, {} };
        }
        static MCPPermissionResult denied(std::string msg)
        {
            return { Kind::Denied, std::move(msg) };
        }

        bool isAllowed() const         { return kind == Kind::Allowed; }
        bool requiresUserApproval() const { return kind == Kind::RequiresApproval; }
        // Returns nullptr when no error message (Allowed or RequiresApproval).
        const std::string* errorMessage() const
        {
            return kind == Kind::Denied ? &message : nullptr;
        }
    };
}
