#pragma once
#include <string>
#include <vector>

namespace DBModels
{
    // Persisted app settings — JSON file in %LOCALAPPDATA%\Gridex\settings.json
    struct AppSettings
    {
        // Appearance
        int themeIndex = 0;              // 0=System, 1=Light, 2=Dark

        // AI
        int aiProviderIndex = 0;         // 0=Anthropic, 1=OpenAI, 2=Ollama
        std::wstring aiApiKey;
        std::wstring aiModel;
        std::wstring ollamaEndpoint;

        // Editor
        int editorFontSize = 13;
        int rowLimit = 100;

        // Navigation state (for back button)
        std::wstring lastPageBeforeSettings;  // e.g. "Gridex.WorkspacePage"

        // Connection groups (user-defined labels)
        std::vector<std::wstring> connectionGroups;

        // ── MCP Server ───────────────────────────────────────
        // All MCP prefs live in the same settings.json as the rest
        // of the app (no separate file). Defaults mirror macOS so
        // first-launch behavior is identical across platforms.

        // Transport / server lifecycle
        bool mcpEnabled = false;
        bool mcpHttpEnabled = false;
        int  mcpHttpPort = 3333;                 // localhost by default
        bool mcpAllowRemoteHTTP = false;         // flips bind to 0.0.0.0
        int64_t mcpStartTime = 0;                // unix seconds; uptime tracking

        // Security
        bool mcpRequireApprovalForWrites = true;

        // Rate limits (mac defaults)
        int mcpQueriesPerMinute = 60;
        int mcpQueriesPerHour   = 1000;
        int mcpWritesPerMinute  = 10;
        int mcpDdlPerMinute     = 1;

        // Timeouts (seconds)
        int mcpQueryTimeout      = 30;
        int mcpApprovalTimeout   = 60;
        int mcpConnectionTimeout = 10;

        // Audit log rotation (JSONL at %APPDATA%\Gridex\mcp-audit.jsonl)
        int mcpAuditRetentionDays = 90;
        int mcpAuditMaxSizeMB     = 100;

        // Load from file (returns default if file missing)
        static AppSettings Load();

        // Save to file
        bool Save() const;

    private:
        static std::wstring GetSettingsPath();
    };
}
