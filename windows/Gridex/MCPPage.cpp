#include "pch.h"
#include "xaml-includes.h"
#include "MCPPage.h"
#if __has_include("MCPPage.g.cpp")
#include "MCPPage.g.cpp"
#endif

#include "Models/AppSettings.h"
#include "Models/ConnectionStore.h"
#include "Services/MCP/MCPServerHost.h"
#include "Services/MCP/Audit/MCPAuditLogger.h"
#include "Services/MCP/Tools/MCPToolHelpers.h"
#include "Services/TrayIconService.h"
#include "GridexVersion.h"
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.UI.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Windows.UI.Text.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <shlobj.h>
#include <algorithm>

namespace winrt::Gridex::implementation
{
    namespace mux  = winrt::Microsoft::UI::Xaml;
    namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
    namespace wadt = winrt::Windows::ApplicationModel::DataTransfer;

    // GRIDEX_VERSION is a wide-string literal in GridexVersion.h — we
    // need the server version as UTF-8 for the JSON handshake.
    static std::string versionUtf8()
    {
        std::wstring w(GRIDEX_VERSION);
        std::string out;
        out.reserve(w.size());
        for (wchar_t c : w) out.push_back(static_cast<char>(c & 0x7F));
        return out;
    }

    MCPPage::MCPPage()
    {
        InitializeComponent();

        // Only RefreshUI runs on Loaded — it populates controls on
        // the Overview + Setup + Config tabs, which are all realized
        // by the initial Pivot render. Connections + Activity tabs
        // use Pivot's lazy-load, so their controls are nullptr until
        // the user selects those tabs. Tabs_SelectionChanged handles
        // the deferred refresh safely.
        this->Loaded([this](auto&&, auto&&) {
            try { RefreshUI(); }
            catch (...) { /* never block page from showing */ }

            // 1 Hz uptime tick — only repaints the Overview subtitle
            // so we don't churn Config/Connections rebuilds.
            try
            {
                uptimeTimer_ = mux::DispatcherTimer();
                uptimeTimer_.Interval(winrt::Windows::Foundation::TimeSpan{
                    std::chrono::seconds(1) });
                uptimeTimer_.Tick([this](auto&&, auto&&) {
                    try { RefreshOverviewUptime(); } catch (...) {}
                });
                uptimeTimer_.Start();
            }
            catch (...) {}
        });
        this->Unloaded([this](auto&&, auto&&) {
            if (uptimeTimer_) { uptimeTimer_.Stop(); uptimeTimer_ = nullptr; }
        });
    }

    void MCPPage::RefreshUI()
    {
        // Pivot lazy-loads PivotItem content, so any control whose
        // tab isn't realized yet will be nullptr. We null-guard each
        // group so Refresh can safely be called when only some of
        // the tabs have been materialized.
        auto settings = DBModels::AppSettings::Load();

        // ── Overview: Access counts + status + tools ────────────
        {
            int locked = 0, ro = 0, rw = 0;
            auto configs = DBModels::ConnectionStore::Load();
            for (const auto& c : configs)
            {
                switch (c.mcpMode)
                {
                    case DBModels::MCPConnectionMode::Locked:    ++locked; break;
                    case DBModels::MCPConnectionMode::ReadOnly:  ++ro;     break;
                    case DBModels::MCPConnectionMode::ReadWrite: ++rw;     break;
                }
            }
            if (LockedCount())    LockedCount().Text(winrt::hstring(std::to_wstring(locked)));
            if (ReadOnlyCount())  ReadOnlyCount().Text(winrt::hstring(std::to_wstring(ro)));
            if (ReadWriteCount()) ReadWriteCount().Text(winrt::hstring(std::to_wstring(rw)));
        }

        auto srv = DBModels::MCPServerHost::instance();
        const bool running = srv && srv->isRunning();
        ApplyStartStopButton(running);
        if (ToolsCountText())
        {
            if (running)
            {
                const auto tools = srv->toolRegistry().definitions().size();
                ToolsCountText().Text(winrt::hstring(std::to_wstring(tools) + L" tools"));
            }
            else
            {
                ToolsCountText().Text(L"0 tools (server stopped)");
            }
        }

        // Overview: uptime subtitle + recent activity list.
        RefreshOverviewUptime();

        if (auto recent = RecentActivityPanel())
        {
            recent.Children().Clear();
            std::vector<DBModels::MCPAuditEntry> last;
            try
            {
                if (srv)
                    last = srv->auditLogger().recentEntries(5);
                else
                {
                    DBModels::MCPAuditLogger tmp(
                        settings.mcpAuditMaxSizeMB, settings.mcpAuditRetentionDays);
                    last = tmp.recentEntries(5);
                }
            }
            catch (...) {}

            if (last.empty())
            {
                winrt::Microsoft::UI::Xaml::Controls::TextBlock empty;
                empty.Text(L"No activity yet");
                empty.FontSize(12);
                empty.Foreground(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(
                    winrt::Windows::UI::ColorHelper::FromArgb(255, 150, 150, 150)));
                recent.Children().Append(empty);
            }
            else
            {
                for (const auto& e : last)
                {
                    winrt::Microsoft::UI::Xaml::Controls::Grid row;
                    row.ColumnSpacing(8);
                    winrt::Microsoft::UI::Xaml::Controls::ColumnDefinition c0, c1, c2;
                    c0.Width(winrt::Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(
                        1, winrt::Microsoft::UI::Xaml::GridUnitType::Star));
                    c1.Width(winrt::Microsoft::UI::Xaml::GridLengthHelper::FromPixels(60));
                    c2.Width(winrt::Microsoft::UI::Xaml::GridLengthHelper::FromPixels(70));
                    row.ColumnDefinitions().Append(c0);
                    row.ColumnDefinitions().Append(c1);
                    row.ColumnDefinitions().Append(c2);

                    winrt::Microsoft::UI::Xaml::Controls::TextBlock tool;
                    tool.Text(winrt::hstring(DBModels::MCPToolHelpers::fromUtf8(e.tool)));
                    tool.FontSize(12);
                    tool.FontFamily(winrt::Microsoft::UI::Xaml::Media::FontFamily(L"Consolas"));
                    winrt::Microsoft::UI::Xaml::Controls::Grid::SetColumn(tool, 0);
                    row.Children().Append(tool);

                    winrt::Microsoft::UI::Xaml::Controls::TextBlock status;
                    status.FontSize(11);
                    const bool ok = e.result.status == DBModels::MCPAuditStatus::Success;
                    status.Text(ok ? L"success" : (e.result.status == DBModels::MCPAuditStatus::Error ? L"error" : L"denied"));
                    status.Foreground(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(
                        winrt::Windows::UI::ColorHelper::FromArgb(
                            255,
                            ok ? 46 : 220,
                            ok ? 139 : 53,
                            ok ? 87 : 69)));
                    winrt::Microsoft::UI::Xaml::Controls::Grid::SetColumn(status, 1);
                    row.Children().Append(status);

                    winrt::Microsoft::UI::Xaml::Controls::TextBlock dur;
                    dur.Text(winrt::hstring(std::to_wstring(e.result.durationMs) + L"ms"));
                    dur.FontSize(11);
                    dur.FontFamily(winrt::Microsoft::UI::Xaml::Media::FontFamily(L"Consolas"));
                    dur.Foreground(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(
                        winrt::Windows::UI::ColorHelper::FromArgb(255, 150, 150, 150)));
                    winrt::Microsoft::UI::Xaml::Controls::Grid::SetColumn(dur, 2);
                    row.Children().Append(dur);

                    recent.Children().Append(row);
                }
                if (auto countLbl = RecentActivityCount())
                    countLbl.Text(winrt::hstring(std::to_wstring(last.size()) + L" events"));
            }
        }

        // ── Config tab — only touch if realized ─────────────────
        if (HttpToggle())
        {
            HttpToggle().IsOn(settings.mcpHttpEnabled);
            HttpPortBox().Value(static_cast<double>(settings.mcpHttpPort));
            HttpPortRow().Visibility(settings.mcpHttpEnabled
                ? mux::Visibility::Visible : mux::Visibility::Collapsed);
            QpmBox().Value(static_cast<double>(settings.mcpQueriesPerMinute));
            QphBox().Value(static_cast<double>(settings.mcpQueriesPerHour));
            WpmBox().Value(static_cast<double>(settings.mcpWritesPerMinute));
            DdlBox().Value(static_cast<double>(settings.mcpDdlPerMinute));
            QueryTimeoutBox().Value(static_cast<double>(settings.mcpQueryTimeout));
            ApprovalTimeoutBox().Value(static_cast<double>(settings.mcpApprovalTimeout));
            ConnectionTimeoutBox().Value(static_cast<double>(settings.mcpConnectionTimeout));
            RetentionBox().Value(static_cast<double>(settings.mcpAuditRetentionDays));
            MaxSizeBox().Value(static_cast<double>(settings.mcpAuditMaxSizeMB));
            RequireApprovalToggle().IsOn(settings.mcpRequireApprovalForWrites);
            AllowRemoteHttpToggle().IsOn(settings.mcpAllowRemoteHTTP);
        }

        // ── Setup tab — only touch if realized ──────────────────
        if (SetupConfigBox())
        {
            wchar_t exePath[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            std::wstring exe(exePath);
            std::wstring escaped;
            escaped.reserve(exe.size() * 2);
            for (wchar_t c : exe) { if (c == L'\\') escaped += L"\\\\"; else escaped += c; }

            std::wstring cfg =
                L"{\n"
                L"  \"mcpServers\": {\n"
                L"    \"gridex\": {\n"
                L"      \"command\": \"" + escaped + L"\",\n"
                L"      \"args\": [\"--mcp-stdio\"]\n"
                L"    }\n"
                L"  }\n"
                L"}";
            SetupConfigBox().Text(winrt::hstring(cfg));
            if (SetupPathText())
                SetupPathText().Text(
                    L"Default Claude Desktop config: %APPDATA%\\Claude\\claude_desktop_config.json");
        }
    }

    void MCPPage::ApplyStartStopButton(bool running)
    {
        // Overview is the first Pivot tab so StartStopBtn is always
        // realized at page Loaded — but guard anyway so this helper
        // stays safe to call from any refresh path.
        if (!StartStopBtn()) return;
        StartStopBtn().Content(
            winrt::box_value(winrt::hstring(running ? L"Stop Server" : L"Start Server")));
        StatusText().Text(running ? L"Running" : L"Stopped");
        RunningTitle().Text(running
            ? L"MCP Server is running" : L"MCP Server is stopped");
        RunningSubtitle().Text(running
            ? L"AI clients can now see Gridex as an MCP server."
            : L"Enable below to let AI clients connect.");
        StatusDot().Fill(
            mux::Media::SolidColorBrush(
                winrt::Windows::UI::Colors::Gray()));
        // Green when running.
        if (running)
        {
            StatusDot().Fill(
                mux::Media::SolidColorBrush(
                    winrt::Windows::UI::ColorHelper::FromArgb(255, 76, 175, 80)));
        }
    }

    void MCPPage::Back_Click(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (auto frame = this->Frame())
        {
            if (frame.CanGoBack()) frame.GoBack();
        }
    }

    void MCPPage::StartStop_Click(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        auto settings = DBModels::AppSettings::Load();
        auto srv = DBModels::MCPServerHost::instance();

        if (srv && srv->isRunning())
        {
            // Stop.
            DBModels::MCPServerHost::stop();
            settings.mcpEnabled = false;
            settings.mcpStartTime = 0; // clear uptime anchor
            settings.Save();
        }
        else
        {
            // Start — always in HttpOnly mode from the GUI. The
            // --mcp-stdio CLI path uses a separate bootstrap.
            DBModels::MCPServerHost::ensureCreated(
                settings,
                versionUtf8(),
                DBModels::MCPTransportMode::HttpOnly);

            // Hook XamlRoot + dispatcher so approval dialogs can open.
            auto running = DBModels::MCPServerHost::instance();
            if (running)
            {
                auto content = this->XamlRoot();
                // Forward pointers as void* — MCPApprovalGate stores
                // winrt handles internally. We pass the same object
                // each time; lifetime outlives the server.
                static winrt::Microsoft::UI::Dispatching::DispatcherQueue dq{ nullptr };
                static winrt::Microsoft::UI::Xaml::XamlRoot xr{ nullptr };
                dq = this->DispatcherQueue();
                xr = content;
                running->setUIContext(&dq, &xr);
            }

            DBModels::MCPServerHost::start();
            settings.mcpEnabled = true;
            // Stamp mcpStartTime so the Overview uptime counter has
            // an anchor. Reset on stop below.
            settings.mcpStartTime = static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            settings.Save();
        }
        DBModels::TrayIconService::Refresh();
        RefreshUI();
    }

    void MCPPage::HttpToggle_Toggled(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        HttpPortRow().Visibility(HttpToggle().IsOn()
            ? mux::Visibility::Visible : mux::Visibility::Collapsed);
    }

    void MCPPage::CopyConfig_Click(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        wadt::DataPackage pkg;
        pkg.SetText(SetupConfigBox().Text());
        wadt::Clipboard::SetContent(pkg);
    }

    void MCPPage::SaveConfig_Click(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        auto s = DBModels::AppSettings::Load();
        s.mcpHttpEnabled = HttpToggle().IsOn();
        s.mcpHttpPort    = static_cast<int>(HttpPortBox().Value());
        s.mcpQueriesPerMinute = static_cast<int>(QpmBox().Value());
        s.mcpQueriesPerHour   = static_cast<int>(QphBox().Value());
        s.mcpWritesPerMinute  = static_cast<int>(WpmBox().Value());
        s.mcpDdlPerMinute     = static_cast<int>(DdlBox().Value());
        s.mcpQueryTimeout      = static_cast<int>(QueryTimeoutBox().Value());
        s.mcpApprovalTimeout   = static_cast<int>(ApprovalTimeoutBox().Value());
        s.mcpConnectionTimeout = static_cast<int>(ConnectionTimeoutBox().Value());
        s.mcpAuditRetentionDays = static_cast<int>(RetentionBox().Value());
        s.mcpAuditMaxSizeMB     = static_cast<int>(MaxSizeBox().Value());
        s.mcpRequireApprovalForWrites = RequireApprovalToggle().IsOn();
        s.mcpAllowRemoteHTTP          = AllowRemoteHttpToggle().IsOn();
        s.Save();
        SaveConfigStatusText().Text(L"Saved. Restart the server to apply rate-limit / audit changes.");
    }

    // Install for Claude Desktop — merges the Gridex entry into
    // %APPDATA%\Claude\claude_desktop_config.json, preserving any
    // existing mcpServers entries. A .bak backup is dropped next to
    // the original file before mutation so users can roll back.
    void MCPPage::InstallClaude_Click(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        wchar_t* appDataW = nullptr;
        if (SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appDataW) != S_OK)
        {
            InstallStatusText().Text(L"Cannot resolve %APPDATA%.");
            return;
        }
        const std::wstring appData(appDataW);
        CoTaskMemFree(appDataW);

        const std::wstring claudeDir  = appData + L"\\Claude";
        const std::wstring configPath = claudeDir + L"\\claude_desktop_config.json";
        CreateDirectoryW(claudeDir.c_str(), nullptr);

        // Resolve exe path for the new mcpServers.gridex entry.
        wchar_t exePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);

        // Guard rails: MSIX / AppX-deployed paths (VS "F5 Deploy")
        // aren't runnable as plain child processes — Claude Desktop
        // spawns them without the packaged activation context and
        // Gridex aborts at load. Warn the user up front so they
        // don't end up with a broken config pointing at AppX Debug.
        {
            std::wstring p(exePath);
            auto contains = [&](const wchar_t* s) {
                return p.find(s) != std::wstring::npos;
            };
            if (contains(L"\\AppX\\") || contains(L"\\Debug\\"))
            {
                InstallStatusText().Text(
                    L"Current build is Debug/AppX — Claude Desktop can't spawn an MSIX "
                    L"package directly and it will abort at load. Switch Visual Studio "
                    L"to the Release configuration (or run build-unpackaged.ps1) and "
                    L"click Install again.");
                return;
            }
        }

        nlohmann::json existing = nlohmann::json::object();
        if (GetFileAttributesW(configPath.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            // Backup first.
            const std::wstring backup = configPath + L".bak";
            CopyFileW(configPath.c_str(), backup.c_str(), FALSE);
            try
            {
                std::ifstream in(configPath, std::ios::binary);
                if (in.is_open()) existing = nlohmann::json::parse(in, nullptr, false);
                if (existing.is_discarded()) existing = nlohmann::json::object();
            }
            catch (...) { existing = nlohmann::json::object(); }
        }

        if (!existing.contains("mcpServers") || !existing["mcpServers"].is_object())
            existing["mcpServers"] = nlohmann::json::object();

        // Convert exe path to utf-8 with escaped backslashes for JSON.
        std::string exeUtf8;
        {
            std::wstring w(exePath);
            int sz = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                          nullptr, 0, nullptr, nullptr);
            exeUtf8.resize(sz);
            WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                 &exeUtf8[0], sz, nullptr, nullptr);
        }

        existing["mcpServers"]["gridex"] = {
            {"command", exeUtf8},
            {"args", nlohmann::json::array({"--mcp-stdio"})}
        };

        std::ofstream out(configPath, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
        {
            InstallStatusText().Text(L"Failed to write config. Is Claude Desktop running?");
            return;
        }
        out << existing.dump(2);
        out.close();

        InstallStatusText().Text(
            L"Installed. Restart Claude Desktop; a .bak backup of the previous config is next to the file.");
    }

    // ── Connections tab ──────────────────────────────────────
    // Mac uses a SwiftUI Table with Pickers. WinUI 3 C++/winrt
    // data-binding through IInspectable VMs is verbose, so we do
    // the same thing HomePage.cpp does for ConnectionCard: build
    // each row as a Grid at refresh time.

    namespace
    {
        using namespace winrt::Microsoft::UI::Xaml;
        using namespace winrt::Microsoft::UI::Xaml::Controls;
        using namespace winrt::Microsoft::UI::Xaml::Media;
        using namespace winrt::Windows::UI;

        // Mac MCPConnectionMode colors — red / blue / green.
        SolidColorBrush modeBrush(DBModels::MCPConnectionMode m)
        {
            switch (m)
            {
                case DBModels::MCPConnectionMode::ReadOnly:
                    return SolidColorBrush(ColorHelper::FromArgb(255, 30, 144, 255));
                case DBModels::MCPConnectionMode::ReadWrite:
                    return SolidColorBrush(ColorHelper::FromArgb(255, 46, 139, 87));
                default: // Locked
                    return SolidColorBrush(ColorHelper::FromArgb(255, 220, 53, 69));
            }
        }

        bool containsCI(const std::wstring& haystack, const std::wstring& needle)
        {
            if (needle.empty()) return true;
            auto lowerH = haystack;
            auto lowerN = needle;
            std::transform(lowerH.begin(), lowerH.end(), lowerH.begin(), ::towlower);
            std::transform(lowerN.begin(), lowerN.end(), lowerN.begin(), ::towlower);
            return lowerH.find(lowerN) != std::wstring::npos;
        }

        std::wstring displayHost(const DBModels::ConnectionConfig& c)
        {
            if (c.databaseType == DBModels::DatabaseType::SQLite)
                return c.filePath.empty() ? L"" : c.filePath;
            std::wstring h = c.host;
            if (c.port > 0) h += L":" + std::to_wstring(c.port);
            return h;
        }
    }

    void MCPPage::RefreshConnectionsTab()
    {
        auto panel = ConnRowsPanel();
        // Pivot lazy-load — tab may not be realized yet on Loaded.
        if (!panel) return;
        if (!ConnSearchBox() || !ConnFilterCombo()) return;
        panel.Children().Clear();

        // Filter state
        const std::wstring q = std::wstring(ConnSearchBox().Text());
        const int filterIdx = ConnFilterCombo().SelectedIndex();

        auto all = DBModels::ConnectionStore::Load();
        int total = static_cast<int>(all.size());
        int shown = 0;

        for (const auto& c : all)
        {
            // Mode filter: 0=All, 1=Locked, 2=ReadOnly, 3=ReadWrite
            if (filterIdx == 1 && c.mcpMode != DBModels::MCPConnectionMode::Locked) continue;
            if (filterIdx == 2 && c.mcpMode != DBModels::MCPConnectionMode::ReadOnly) continue;
            if (filterIdx == 3 && c.mcpMode != DBModels::MCPConnectionMode::ReadWrite) continue;

            // Text filter on name + host
            if (!q.empty())
            {
                if (!containsCI(c.name, q) && !containsCI(displayHost(c), q)) continue;
            }

            Grid row;
            row.Padding(Thickness{ 12, 8, 12, 8 });
            row.ColumnSpacing(12);

            ColumnDefinition col0; col0.Width(GridLengthHelper::FromPixels(20));
            ColumnDefinition col1; col1.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
            col1.MinWidth(140);
            ColumnDefinition col2; col2.Width(GridLengthHelper::FromPixels(90));
            ColumnDefinition col3; col3.Width(GridLengthHelper::FromPixels(180));
            ColumnDefinition col4; col4.Width(GridLengthHelper::FromPixels(140));
            row.ColumnDefinitions().Append(col0);
            row.ColumnDefinitions().Append(col1);
            row.ColumnDefinitions().Append(col2);
            row.ColumnDefinitions().Append(col3);
            row.ColumnDefinitions().Append(col4);

            // Status dot
            Shapes::Ellipse dot;
            dot.Width(8); dot.Height(8);
            dot.Fill(modeBrush(c.mcpMode));
            dot.VerticalAlignment(VerticalAlignment::Center);
            Grid::SetColumn(dot, 0);
            row.Children().Append(dot);

            // Name
            TextBlock name;
            name.Text(winrt::hstring(c.name));
            name.VerticalAlignment(VerticalAlignment::Center);
            Grid::SetColumn(name, 1);
            row.Children().Append(name);

            // Type
            TextBlock type;
            type.Text(winrt::hstring(DBModels::DatabaseTypeDisplayName(c.databaseType)));
            type.Foreground(SolidColorBrush(ColorHelper::FromArgb(255, 160, 160, 160)));
            type.FontSize(12);
            type.VerticalAlignment(VerticalAlignment::Center);
            Grid::SetColumn(type, 2);
            row.Children().Append(type);

            // Host
            TextBlock host;
            host.Text(winrt::hstring(displayHost(c)));
            host.Foreground(SolidColorBrush(ColorHelper::FromArgb(255, 160, 160, 160)));
            host.FontSize(12);
            host.VerticalAlignment(VerticalAlignment::Center);
            host.TextTrimming(TextTrimming::CharacterEllipsis);
            Grid::SetColumn(host, 3);
            row.Children().Append(host);

            // Access mode picker — changes persist via ConnectionStore::Save
            // AND sync to the running MCPServer's permissionEngine.
            ComboBox modePicker;
            auto addItem = [&](const wchar_t* label) {
                ComboBoxItem item;
                item.Content(winrt::box_value(winrt::hstring(label)));
                modePicker.Items().Append(item);
            };
            addItem(L"Locked");
            addItem(L"Read-only");
            addItem(L"Read-write");
            modePicker.SelectedIndex(static_cast<int>(c.mcpMode));
            modePicker.VerticalAlignment(VerticalAlignment::Center);
            modePicker.MinWidth(130);

            const std::wstring connId = c.id;
            modePicker.SelectionChanged(
                [this, connId](auto&& sender, auto&&) {
                    auto cb = sender.try_as<muxc::ComboBox>();
                    if (!cb) return;
                    const auto newMode = static_cast<DBModels::MCPConnectionMode>(cb.SelectedIndex());

                    auto configs = DBModels::ConnectionStore::Load();
                    for (auto& cfg : configs)
                    {
                        if (cfg.id == connId)
                        {
                            cfg.mcpMode = newMode;
                            DBModels::ConnectionStore::Save(cfg);
                            break;
                        }
                    }
                    if (auto srv = DBModels::MCPServerHost::instance())
                        srv->permissionEngine().setMode(connId, newMode);

                    RefreshUI(); // counts on Overview
                });

            Grid::SetColumn(modePicker, 4);
            row.Children().Append(modePicker);

            // Row separator
            Border wrap;
            wrap.Child(row);
            wrap.BorderBrush(SolidColorBrush(ColorHelper::FromArgb(30, 128, 128, 128)));
            wrap.BorderThickness(Thickness{ 0, 0, 0, 1 });
            panel.Children().Append(wrap);
            ++shown;
        }

        ConnCountText().Text(winrt::hstring(
            std::to_wstring(shown) + L" of " + std::to_wstring(total)));
        ConnEmptyState().Visibility(shown == 0 && total > 0
            ? mux::Visibility::Visible : mux::Visibility::Collapsed);
    }

    void MCPPage::ConnFilter_Changed(
        winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBox const&,
        winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBoxTextChangedEventArgs const&)
    {
        RefreshConnectionsTab();
    }

    void MCPPage::ConnFilterCombo_SelectionChanged(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
    {
        RefreshConnectionsTab();
    }

    // ── Activity tab ─────────────────────────────────────────
    // Tails the last 200 entries of mcp-audit.jsonl into a table.
    // Uses the running MCPServer's logger when available; otherwise
    // spins up a temporary MCPAuditLogger pointing at the same
    // disk path so previous-session entries are still visible.

    namespace
    {
        winrt::Microsoft::UI::Xaml::Media::SolidColorBrush statusBrush(
            DBModels::MCPAuditStatus s)
        {
            using namespace winrt::Microsoft::UI::Xaml::Media;
            using namespace winrt::Windows::UI;
            switch (s)
            {
                case DBModels::MCPAuditStatus::Success:
                    return SolidColorBrush(ColorHelper::FromArgb(255, 46, 139, 87));
                case DBModels::MCPAuditStatus::Error:
                    return SolidColorBrush(ColorHelper::FromArgb(255, 220, 53, 69));
                case DBModels::MCPAuditStatus::Denied:
                    return SolidColorBrush(ColorHelper::FromArgb(255, 255, 140, 0));
                default:
                    return SolidColorBrush(ColorHelper::FromArgb(255, 150, 150, 150));
            }
        }

        std::wstring formatTimeHM(std::chrono::system_clock::time_point tp)
        {
            const std::time_t t = std::chrono::system_clock::to_time_t(tp);
            std::tm tm{};
            localtime_s(&tm, &t);
            wchar_t buf[16];
            wcsftime(buf, 16, L"%H:%M:%S", &tm);
            return buf;
        }

        const wchar_t* statusLabel(DBModels::MCPAuditStatus s)
        {
            switch (s)
            {
                case DBModels::MCPAuditStatus::Success: return L"success";
                case DBModels::MCPAuditStatus::Error:   return L"error";
                case DBModels::MCPAuditStatus::Denied:  return L"denied";
                case DBModels::MCPAuditStatus::Timeout: return L"timeout";
            }
            return L"?";
        }
    }

    void MCPPage::RefreshActivityTab()
    {
        using namespace winrt::Microsoft::UI::Xaml;
        using namespace winrt::Microsoft::UI::Xaml::Controls;
        using namespace winrt::Microsoft::UI::Xaml::Media;
        using namespace winrt::Windows::UI;

        auto panel = ActRowsPanel();
        if (!panel) return;
        if (!ActSearchBox() || !ActStatusCombo()) return;
        panel.Children().Clear();

        wchar_t* ad = nullptr;
        if (SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &ad) == S_OK)
        {
            std::wstring dir = std::wstring(ad) + L"\\Gridex";
            CoTaskMemFree(ad);
            ActPathText().Text(winrt::hstring(L"Audit log: " + dir + L"\\mcp-audit.jsonl"));
        }

        std::vector<DBModels::MCPAuditEntry> entries;
        if (auto srv = DBModels::MCPServerHost::instance())
        {
            entries = srv->auditLogger().recentEntries(200);
        }
        else
        {
            auto s = DBModels::AppSettings::Load();
            DBModels::MCPAuditLogger tempLogger(
                s.mcpAuditMaxSizeMB, s.mcpAuditRetentionDays);
            entries = tempLogger.recentEntries(200);
        }

        const std::wstring q = std::wstring(ActSearchBox().Text());
        const int statusIdx = ActStatusCombo().SelectedIndex();

        // Build the filtered list first and cache it so click
        // handlers can look up the full record by index. We need
        // stable indices — rebuilding the cache on every refresh
        // is fine because the table is also rebuilt each time.
        cachedActivity_.clear();
        cachedActivity_.reserve(entries.size());

        int shown = 0;
        for (const auto& e : entries)
        {
            const auto status = e.result.status;
            if (statusIdx == 1 && status != DBModels::MCPAuditStatus::Success) continue;
            if (statusIdx == 2 && status != DBModels::MCPAuditStatus::Error) continue;
            if (statusIdx == 3 && status != DBModels::MCPAuditStatus::Denied) continue;

            if (!q.empty())
            {
                auto lower = [](std::wstring s) {
                    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
                    return s;
                };
                const std::wstring qL = lower(q);
                auto toolW = DBModels::MCPToolHelpers::fromUtf8(e.tool);
                auto clientW = DBModels::MCPToolHelpers::fromUtf8(e.client.name);
                std::wstring sqlW = e.input.sqlPreview.has_value()
                    ? DBModels::MCPToolHelpers::fromUtf8(*e.input.sqlPreview) : L"";
                if (lower(toolW).find(qL) == std::wstring::npos &&
                    lower(clientW).find(qL) == std::wstring::npos &&
                    lower(sqlW).find(qL) == std::wstring::npos) continue;
            }

            const int cacheIdx = static_cast<int>(cachedActivity_.size());
            cachedActivity_.push_back(e);

            Grid row;
            row.Padding(Thickness{ 12, 6, 12, 6 });
            row.ColumnSpacing(12);

            ColumnDefinition c0; c0.Width(GridLengthHelper::FromPixels(90));
            ColumnDefinition c1; c1.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
            c1.MinWidth(160);
            ColumnDefinition c2; c2.Width(GridLengthHelper::FromPixels(120));
            ColumnDefinition c3; c3.Width(GridLengthHelper::FromPixels(80));
            ColumnDefinition c4; c4.Width(GridLengthHelper::FromPixels(70));
            row.ColumnDefinitions().Append(c0);
            row.ColumnDefinitions().Append(c1);
            row.ColumnDefinitions().Append(c2);
            row.ColumnDefinitions().Append(c3);
            row.ColumnDefinitions().Append(c4);

            TextBlock t; t.Text(winrt::hstring(formatTimeHM(e.timestamp)));
            t.FontSize(12); t.FontFamily(winrt::Microsoft::UI::Xaml::Media::FontFamily(L"Consolas"));
            t.Foreground(SolidColorBrush(ColorHelper::FromArgb(255, 160, 160, 160)));
            Grid::SetColumn(t, 0); row.Children().Append(t);

            TextBlock tool; tool.Text(winrt::hstring(DBModels::MCPToolHelpers::fromUtf8(e.tool)));
            tool.FontSize(12); tool.FontFamily(winrt::Microsoft::UI::Xaml::Media::FontFamily(L"Consolas"));
            Grid::SetColumn(tool, 1); row.Children().Append(tool);

            TextBlock client; client.Text(winrt::hstring(DBModels::MCPToolHelpers::fromUtf8(e.client.name)));
            client.FontSize(12);
            client.Foreground(SolidColorBrush(ColorHelper::FromArgb(255, 160, 160, 160)));
            Grid::SetColumn(client, 2); row.Children().Append(client);

            TextBlock st; st.Text(winrt::hstring(statusLabel(status)));
            st.FontSize(12); st.Foreground(statusBrush(status));
            Grid::SetColumn(st, 3); row.Children().Append(st);

            TextBlock dur;
            dur.Text(winrt::hstring(std::to_wstring(e.result.durationMs) + L"ms"));
            dur.FontSize(12); dur.FontFamily(winrt::Microsoft::UI::Xaml::Media::FontFamily(L"Consolas"));
            dur.Foreground(SolidColorBrush(ColorHelper::FromArgb(255, 160, 160, 160)));
            Grid::SetColumn(dur, 4); row.Children().Append(dur);

            Border wrap; wrap.Child(row);
            wrap.BorderBrush(SolidColorBrush(ColorHelper::FromArgb(30, 128, 128, 128)));
            wrap.BorderThickness(Thickness{ 0, 0, 0, 1 });
            wrap.Background(SolidColorBrush(ColorHelper::FromArgb(0, 0, 0, 0)));
            // Row click → populate detail pane. Uses Tapped rather
            // than PointerPressed so touch + mouse both fire one
            // event with click semantics.
            wrap.Tapped([this, cacheIdx](auto&&, auto&&) {
                SelectActivityEntry(cacheIdx);
            });
            panel.Children().Append(wrap);
            ++shown;
        }

        ActEmptyState().Visibility(shown == 0
            ? mux::Visibility::Visible : mux::Visibility::Collapsed);

        // Auto-select the first entry so the detail pane isn't empty
        // on load. Skipped when nothing passed the filter.
        if (!cachedActivity_.empty()) SelectActivityEntry(0);
    }

    // Populate the right-side detail panel for a given cached entry.
    // Mirrors the Form sections in macOS MCPActivityView.detailPanel:
    // Event, Client, Connection, Result, SQL, Error.
    void MCPPage::SelectActivityEntry(int index)
    {
        using namespace winrt::Microsoft::UI::Xaml;
        using namespace winrt::Microsoft::UI::Xaml::Controls;
        using namespace winrt::Microsoft::UI::Xaml::Media;
        using namespace winrt::Windows::UI;

        auto panel = ActDetailPanel();
        if (!panel) return;
        if (index < 0 || index >= static_cast<int>(cachedActivity_.size())) return;
        const auto& e = cachedActivity_[index];

        selectedActivityIndex_ = index;

        // Highlight the active row on the left table. Iterate the
        // row panel's Border children and swap Background — accent
        // tint on the selected row, transparent elsewhere. Cheap
        // because we only have up to 200 rows.
        if (auto rows = ActRowsPanel())
        {
            const auto sel  = SolidColorBrush(ColorHelper::FromArgb(60, 130, 90, 220));
            const auto none = SolidColorBrush(ColorHelper::FromArgb(0, 0, 0, 0));
            auto kids = rows.Children();
            for (uint32_t i = 0; i < kids.Size(); ++i)
            {
                if (auto b = kids.GetAt(i).try_as<Border>())
                    b.Background(static_cast<int>(i) == index ? sel : none);
            }
        }

        panel.Children().Clear();

        auto addHeader = [&](winrt::hstring text) {
            TextBlock h;
            h.Text(text);
            h.FontSize(13);
            h.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            h.Margin(Thickness{ 0, 8, 0, 2 });
            panel.Children().Append(h);
        };

        auto addRow = [&](const wchar_t* label, winrt::hstring value, bool mono = false) {
            Grid g; g.ColumnSpacing(8);
            ColumnDefinition l; l.Width(GridLengthHelper::FromPixels(100));
            ColumnDefinition v; v.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
            g.ColumnDefinitions().Append(l);
            g.ColumnDefinitions().Append(v);

            TextBlock lb; lb.Text(label); lb.FontSize(11);
            lb.Foreground(SolidColorBrush(ColorHelper::FromArgb(255, 150, 150, 150)));
            Grid::SetColumn(lb, 0); g.Children().Append(lb);

            TextBlock vb; vb.Text(value); vb.FontSize(12);
            vb.TextWrapping(TextWrapping::Wrap);
            vb.IsTextSelectionEnabled(true);
            if (mono) vb.FontFamily(winrt::Microsoft::UI::Xaml::Media::FontFamily(L"Consolas"));
            Grid::SetColumn(vb, 1); g.Children().Append(vb);

            panel.Children().Append(g);
        };

        auto addBlock = [&](const wchar_t* label, winrt::hstring value, bool mono = true) {
            TextBlock lb; lb.Text(label); lb.FontSize(11);
            lb.Foreground(SolidColorBrush(ColorHelper::FromArgb(255, 150, 150, 150)));
            lb.Margin(Thickness{ 0, 4, 0, 2 });
            panel.Children().Append(lb);

            Border b;
            b.Padding(Thickness{ 8, 6, 8, 6 });
            b.CornerRadius(winrt::Microsoft::UI::Xaml::CornerRadius{ 4, 4, 4, 4 });
            b.Background(SolidColorBrush(ColorHelper::FromArgb(255, 28, 28, 30)));
            TextBlock vb; vb.Text(value); vb.FontSize(12);
            vb.TextWrapping(TextWrapping::Wrap);
            vb.IsTextSelectionEnabled(true);
            if (mono) vb.FontFamily(winrt::Microsoft::UI::Xaml::Media::FontFamily(L"Consolas"));
            b.Child(vb);
            panel.Children().Append(b);
        };

        // Format full timestamp.
        auto fullTime = [](std::chrono::system_clock::time_point tp) {
            const std::time_t t = std::chrono::system_clock::to_time_t(tp);
            std::tm tm{};
            localtime_s(&tm, &t);
            wchar_t buf[32];
            wcsftime(buf, 32, L"%Y-%m-%d %H:%M:%S", &tm);
            return std::wstring(buf);
        };

        addHeader(L"Event");
        addRow(L"Tool", winrt::hstring(DBModels::MCPToolHelpers::fromUtf8(e.tool)), true);
        addRow(L"Tier", winrt::hstring(L"Tier " + std::to_wstring(e.tier)));
        addRow(L"Time", winrt::hstring(fullTime(e.timestamp)));
        if (!e.eventId.empty())
        {
            std::string shortId = e.eventId.size() > 8 ? e.eventId.substr(0, 8) + "..." : e.eventId;
            addRow(L"Event ID", winrt::hstring(DBModels::MCPToolHelpers::fromUtf8(shortId)), true);
        }

        addHeader(L"Client");
        addRow(L"Name", winrt::hstring(DBModels::MCPToolHelpers::fromUtf8(e.client.name)));
        addRow(L"Version", winrt::hstring(DBModels::MCPToolHelpers::fromUtf8(e.client.version)));
        addRow(L"Transport", winrt::hstring(DBModels::MCPToolHelpers::fromUtf8(e.client.transport)));

        if (e.connectionId.has_value() || e.connectionType.has_value())
        {
            addHeader(L"Connection");
            if (e.connectionType.has_value())
                addRow(L"Database", winrt::hstring(DBModels::MCPToolHelpers::fromUtf8(*e.connectionType)));
            if (e.connectionId.has_value())
            {
                const auto& cid = *e.connectionId;
                std::string shortId = cid.size() > 8 ? cid.substr(0, 8) + "..." : cid;
                addRow(L"ID", winrt::hstring(DBModels::MCPToolHelpers::fromUtf8(shortId)), true);
            }
        }

        addHeader(L"Result");
        addRow(L"Status", winrt::hstring(statusLabel(e.result.status)));
        addRow(L"Duration", winrt::hstring(std::to_wstring(e.result.durationMs) + L" ms"));
        if (e.result.rowsReturned.has_value())
            addRow(L"Rows returned", winrt::hstring(std::to_wstring(*e.result.rowsReturned)));
        if (e.result.rowsAffected.has_value())
            addRow(L"Rows affected", winrt::hstring(std::to_wstring(*e.result.rowsAffected)));

        if (e.input.sqlPreview.has_value() && !e.input.sqlPreview->empty())
        {
            addBlock(L"SQL", winrt::hstring(DBModels::MCPToolHelpers::fromUtf8(*e.input.sqlPreview)));
        }

        if (e.error.has_value() && !e.error->empty())
        {
            TextBlock lb; lb.Text(L"Error"); lb.FontSize(11);
            lb.Foreground(SolidColorBrush(ColorHelper::FromArgb(255, 220, 53, 69)));
            lb.Margin(Thickness{ 0, 8, 0, 2 });
            panel.Children().Append(lb);

            Border b;
            b.Padding(Thickness{ 8, 6, 8, 6 });
            b.CornerRadius(winrt::Microsoft::UI::Xaml::CornerRadius{ 4, 4, 4, 4 });
            b.Background(SolidColorBrush(ColorHelper::FromArgb(255, 40, 20, 22)));
            TextBlock vb; vb.Text(winrt::hstring(DBModels::MCPToolHelpers::fromUtf8(*e.error)));
            vb.FontSize(12); vb.TextWrapping(TextWrapping::Wrap);
            vb.IsTextSelectionEnabled(true);
            vb.Foreground(SolidColorBrush(ColorHelper::FromArgb(255, 255, 180, 180)));
            b.Child(vb);
            panel.Children().Append(b);
        }
    }

    void MCPPage::ActFilter_Changed(
        winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBox const&,
        winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBoxTextChangedEventArgs const&)
    {
        RefreshActivityTab();
    }

    void MCPPage::ActStatusCombo_SelectionChanged(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
    {
        RefreshActivityTab();
    }

    void MCPPage::ActRefresh_Click(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        RefreshActivityTab();
    }

    // Deferred-load refresh — Pivot lazily realizes PivotItem
    // content, so controls inside non-selected tabs are nullptr at
    // page Loaded. Refresh the body of whichever tab just became
    // active; guard with try/catch so a single tab's error can't
    // kill navigation.
    void MCPPage::Tabs_SelectionChanged(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
    {
        const int idx = Tabs().SelectedIndex();
        try
        {
            if (idx == 0)      RefreshUI();
            else if (idx == 1) RefreshConnectionsTab();
            else if (idx == 2) RefreshActivityTab();
            else if (idx == 3) RefreshUI(); // Setup has JSON preview
            else if (idx == 4) RefreshUI(); // Config loads from AppSettings
        }
        catch (...) { /* don't kill navigation */ }
    }

    // Paint the "Running for Xh Ym · N of M connections exposed"
    // subtitle on the Overview card. Called on Loaded and once per
    // second from the uptime timer.
    void MCPPage::RefreshOverviewUptime()
    {
        auto sub = RunningSubtitle();
        if (!sub) return;

        auto srv = DBModels::MCPServerHost::instance();
        const bool running = srv && srv->isRunning();
        if (!running)
        {
            sub.Text(L"Enable below to let AI clients connect.");
            return;
        }

        auto settings = DBModels::AppSettings::Load();
        std::wstring uptime = L"just started";
        if (settings.mcpStartTime > 0)
        {
            const int64_t now = static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            int64_t dt = now - settings.mcpStartTime;
            if (dt < 0) dt = 0;
            const int h = static_cast<int>(dt / 3600);
            const int m = static_cast<int>((dt % 3600) / 60);
            const int s = static_cast<int>(dt % 60);
            if (h > 0)       uptime = std::to_wstring(h) + L"h " + std::to_wstring(m) + L"m";
            else if (m > 0)  uptime = std::to_wstring(m) + L"m " + std::to_wstring(s) + L"s";
            else             uptime = std::to_wstring(s) + L"s";
        }

        // Count exposed connections (non-Locked).
        int exposed = 0, total = 0;
        for (const auto& c : DBModels::ConnectionStore::Load())
        {
            ++total;
            if (c.mcpMode != DBModels::MCPConnectionMode::Locked) ++exposed;
        }

        sub.Text(winrt::hstring(
            L"Running for " + uptime + L" · " +
            std::to_wstring(exposed) + L" of " + std::to_wstring(total) +
            L" connections exposed"));
    }

    // Overview → "Manage Connections..." button: jump to the
    // Connections tab (Pivot index 1).
    void MCPPage::ManageConnections_Click(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (auto t = Tabs()) t.SelectedIndex(1);
    }
}
