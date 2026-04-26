#pragma once

#include "WorkspacePage.g.h"
#include "Models/WorkspaceState.h"
#include "Models/ConnectionManager.h"
#include "Models/ChangeTracker.h"
#include "Models/DumpRestoreService.h"
#include "Models/ERDiagramService.h"
#include <mutex>
#include <atomic>
#include <memory>

namespace winrt::Gridex::implementation
{
    struct WorkspacePage : WorkspacePageT<WorkspacePage>
    {
        WorkspacePage();

        // Called by HomePage after navigation
        void SetConnection(const DBModels::ConnectionConfig& config, const std::wstring& password);

        // Old XAML-bound handlers (kept for backward compat with existing Click= attrs)
        void ToggleSidebar_Click(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void ToggleDetails_Click(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void DataToggle_Click(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void StructureToggle_Click(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void ToggleFilter_Click(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void ToggleAI_Click(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);

    private:
        DBModels::WorkspaceState state_;
        DBModels::ConnectionManager connMgr_;
        DBModels::ChangeTracker changeTracker_;
        std::wstring currentSchema_ = L"public";
        std::wstring pendingPassword_;
        bool hasConnection_ = false;
        bool isLoaded_ = false;

        // Monotonic counter incremented on every LoadTableDataFromDB call.
        // Background worker captures the value at dispatch time; when the
        // worker re-enters the UI thread it checks that this counter has
        // not advanced — if the user clicked another table while the
        // query was running, the stale result is dropped instead of
        // overwriting the fresh one. UI-thread-only, no atomic needed.
        uint64_t loadTableRequestCounter_ = 0;

        // Process-wide cache of the last active workspace connection.
        // HomePage → WorkspacePage instances are destroyed when the user
        // navigates to SettingsPage (Frame.Navigate drops the previous
        // page), so the Back button creates a fresh WorkspacePage with
        // no connection state. Restoring from these statics inside the
        // Loaded handler keeps the DB session visible instead of falling
        // through to LoadDemoSidebarData.
        static DBModels::ConnectionConfig sLastConfig_;
        static std::wstring sLastPassword_;
        static bool sHasLastConnection_;
        bool buttonsWired_ = false;
        bool sidebarVisible_ = true;
        bool detailsVisible_ = true;
        bool filterVisible_ = false;
        bool showingData_ = true;
        bool erFullscreen_ = false;
        bool webMessageWired_ = false;
        double prevSidebarWidth_ = 280.0;
        double prevDetailsWidth_ = 260.0;

        // Target path set when the "Export PNG" button opens the save
        // picker. Cleared once WebMessageReceived receives the rendered
        // PNG payload and writes it to disk.
        std::wstring pendingPngPath_;

        // Decode a "data:image/png;base64,..." URL coming back from the
        // ER renderer and write it to pendingPngPath_.
        void WritePngFromDataUrl(const std::wstring& dataUrl);

        // ── Drag-to-resize state for left/right sidebars + query log ──
        bool   leftResizing_         = false;
        double leftResizeStartX_     = 0.0;
        double leftResizeStartWidth_ = 0.0;
        bool   rightResizing_        = false;
        double rightResizeStartX_    = 0.0;
        double rightResizeStartWidth_= 0.0;
        bool   logResizing_          = false;
        double logResizeStartY_      = 0.0;
        double logResizeStartHeight_ = 0.0;

        // ── Initialization ──────────────────────────
        void InitializeConnection();
        void WireAllButtons();
        void WireResizeGrips();

        // ── Sidebar / Data Loading ──────────────────
        void LoadSidebarFromDB();
        void LoadTableDataFromDB(const std::wstring& tableName);
        void LoadDemoSidebarData();
        void LoadDemoTableData(const std::wstring& tableName);

        // ── Content Navigation ──────────────────────
        void UpdateBreadcrumb();
        void SwitchContentView();
        void OnTableSelected(const std::wstring& tableName, const std::wstring& schema);
        void OpenNewQueryTab();
        // Inspect a failed query's error text for the typical
        // "syntax error at or near 'X'" pattern. If X is a known
        // reserved keyword for the active dialect AND it appears
        // unquoted in the SQL, return a short user-facing hint
        // suggesting the right quote char. Empty string = no hint.
        static std::wstring ReservedKeywordHint(
            const std::wstring& sql,
            const std::wstring& error,
            DBModels::DatabaseType dbType);

        // ── CRUD Operations ─────────────────────────
        void DeleteSelectedRow();
        void AddNewRow();
        void DuplicateSelectedRow();
        void CommitChanges();
        void DiscardChanges();
        DBModels::TableRow ExtractPrimaryKey(int rowIndex);

        // ── Pagination ──────────────────────────────
        void PrevPage();
        void NextPage();
        void LoadCurrentTablePage(int page);
        void UpdatePaginationUI();

        // ── Database/Schema Picker ──────────────────
        void LoadDatabasePicker();
        void SwitchDatabase(const std::wstring& dbName);

        // ── Filter ──────────────────────────────────
        void ApplyFilter(const std::wstring& column, const std::wstring& op, const std::wstring& value);

        // ── Query Log ───────────────────────────────
        // Log one executed query into the console log panel. When the
        // query also produced a UI render (table load, filter apply,
        // query editor run), pass the render wall-clock so the log
        // entry shows a proper "Exec + Render" split. Metadata queries
        // that never touch the data grid leave the default 0 — the log
        // then omits the "· Render" suffix.
        void LogQuery(const DBModels::QueryResult& result, double renderTimeMs = 0.0);
        void RefreshQueryLog();

        // ── Pending Changes UI ──────────────────────
        void UpdatePendingUI();

        // ── Tab Cache ────────────────────────────────
        void SaveCurrentTabCache();

        // ── Export (async) ───────────────────────────
        winrt::fire_and_forget ExportTableAsync(
            std::wstring tableName, std::wstring schema, std::wstring format);

        // ── Import (async) — into specific table from sidebar context menu
        winrt::fire_and_forget ImportDataAsync(
            std::wstring targetTable, std::wstring targetSchema);

        // ── Dump / Restore (async) ──────────────────
        winrt::fire_and_forget DumpDatabaseAsync();
        winrt::fire_and_forget RestoreDatabaseAsync();

        // ── ER Diagram (async) ──────────────────────
        winrt::fire_and_forget ShowERDiagramAsync(std::wstring schema);
        void LoadERDiagramIntoView(const DBModels::ContentTab& tab);
        void ApplyERDiagramResult(
            const std::wstring& tabId, const DBModels::ERDiagramResult& result);
        winrt::fire_and_forget EnsureWebViewAndNavigate(winrt::hstring html);
        void ToggleERFullscreen();

        // ── Redis-specific actions ────────────────
        winrt::fire_and_forget FlushRedisDbAsync();
        winrt::fire_and_forget BrowseRedisKeysAsync();

#ifdef GRIDEX_ENTERPRISE
        // ── EE Mock Data Generator ────────────────
        // Opens config dialog, spawns the service, shows progress, and
        // reloads the grid on success. Gated because it pulls EE-only
        // headers; the callback itself is never wired in OSS so the
        // menu entry is hidden there.
        winrt::fire_and_forget GenerateMockDataAsync(
            std::wstring tableName, std::wstring schema);

        // ── EE Realtime Connection Monitor ─────────
        // Mounts the WebView2-hosted monitor as a full-page overlay,
        // mirroring EERowRelationshipDialog. Postgres-only entry — the
        // sidebar button is hidden on other DB types.
        void OpenConnectionMonitor();

        // ── EE Visual Query Builder ────────────────
        // WebView2 overlay hosting a drag-drop query canvas. Same
        // mount pattern as the monitor. Postgres + MySQL only.
        void OpenVisualQueryBuilder();
#endif

        // Shared progress state for dump/restore background jobs
        struct DumpRestoreJobState
        {
            std::mutex mtx;
            std::wstring log;
            std::atomic<bool> done{ false };
            DBModels::DumpResult dumpResult;
            DBModels::RestoreResult restoreResult;
        };

        // Show modal progress dialog with live log; returns when state->done is true
        winrt::Windows::Foundation::IAsyncAction ShowProgressDialogAsync(
            std::shared_ptr<DumpRestoreJobState> state,
            std::wstring title,
            std::wstring subtitle);

        // ── AI settings sync ────────────────────────
        void ApplyAiSettings();

        // ── Reload ──────────────────────────────────
        void ReloadCurrentTable();
    };
}

namespace winrt::Gridex::factory_implementation
{
    struct WorkspacePage : WorkspacePageT<WorkspacePage, implementation::WorkspacePage>
    {
    };
}
