#pragma once
//
// MCPApprovalGate.h
// Gridex
//
// Presents a ContentDialog on the MainWindow XAML root and returns
// the user's verdict via std::future<bool>. Mirrors
// macos/Services/MCP/Security/MCPApprovalGate.swift but uses Win32
// threading primitives instead of Swift actors.
//
// The tool layer calls `requestApproval(...).get()` on a worker
// thread (HTTP/stdio request handler). The dialog is marshalled
// onto the UI thread via DispatcherQueue::TryEnqueue.

#include <string>
#include <future>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <utility>

// Forward-declare winrt types so this header stays usable in
// non-winrt TUs. Full types pulled in by the .cpp.
namespace winrt::Microsoft::UI::Dispatching { struct DispatcherQueue; }
namespace winrt::Microsoft::UI::Xaml { struct XamlRoot; }

namespace DBModels
{
    struct MCPApprovalRequest
    {
        std::wstring tool;           // tool name
        std::wstring description;    // "Insert 3 rows into 'users'"
        std::wstring details;        // multi-line body — SQL + row count
        std::wstring connectionId;
        std::wstring clientName;     // shown as "<client> wants to:"
        int timeoutSeconds = 60;
    };

    class MCPApprovalGate
    {
    public:
        MCPApprovalGate() = default;

        // Set once from MainWindow after the window is activated.
        // Approvals requested before this is set auto-deny with a
        // logged warning (the server may be running headless via
        // --mcp-stdio CLI mode, in which case Tier 3 tools are
        // expected to be unavailable — documented in Setup tab).
        void setUIContext(void* dispatcherQueue, void* xamlRoot);

        std::future<bool> requestApproval(const MCPApprovalRequest& req);

        void revokeSessionApproval(const std::wstring& connectionId);
        void revokeAllSessionApprovals();

    private:
        // Session cache key = (connectionId, tool). 30-minute TTL
        // matches macOS.
        using SessionKey = std::pair<std::wstring, std::wstring>;
        struct SessionKeyHash
        {
            size_t operator()(const SessionKey& k) const noexcept
            {
                const auto h1 = std::hash<std::wstring>{}(k.first);
                const auto h2 = std::hash<std::wstring>{}(k.second);
                return h1 ^ (h2 << 1);
            }
        };

        mutable std::mutex mtx_;
        std::unordered_map<SessionKey,
                           std::chrono::system_clock::time_point,
                           SessionKeyHash> sessionApprovals_;

        // UI thread hook — opaque void* here, winrt::com_ptr<...>
        // on the impl side to avoid dragging winrt headers into
        // every consumer.
        void* dispatcher_ = nullptr;
        void* xamlRoot_   = nullptr;

        static constexpr std::chrono::seconds kSessionTTL{30 * 60};
    };
}
