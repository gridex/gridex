#include "pch.h"
#include "xaml-includes.h"
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Microsoft.UI.Xaml.Interop.h>
#include <algorithm>
#include "HomePage.h"
#include "Models/AppSettings.h"
#if __has_include("HomePage.g.cpp")
#include "HomePage.g.cpp"
#endif

#include "ConnectionCard.h"
#include "DatabaseTypePickerDialog.h"
#include "ConnectionFormDialog.h"
#include "WorkspacePage.h"
#include "App.xaml.h"
#include "Models/ConnectionStore.h"
#include "Models/ConnectionManager.h"
#include "Services/Import/DBeaverImporter.h"
#include "Services/Import/DataGripImporter.h"
#include "Services/Import/NavicatImporter.h"
#include "Services/Import/TablePlusImporter.h"
#include <winrt/Microsoft.Windows.Storage.Pickers.h>
#include <random>

namespace winrt::Gridex::implementation
{
    namespace mux  = winrt::Microsoft::UI::Xaml;
    namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
    namespace muxm = winrt::Microsoft::UI::Xaml::Media;

    HomePage::HomePage()
    {
        InitializeComponent();

        // Load saved connections from disk
        allConnections_ = DBModels::ConnectionStore::Load();
        RefreshList();
    }

    void HomePage::DeleteConnection(const std::wstring& id)
    {
        // Remove from memory
        allConnections_.erase(
            std::remove_if(allConnections_.begin(), allConnections_.end(),
                [&id](const DBModels::ConnectionConfig& c) { return c.id == id; }),
            allConnections_.end());

        // Remove from SQLite
        DBModels::ConnectionStore::Delete(id);

        // Refresh UI
        RefreshList();
    }

    // Test a connection synchronously on the UI thread. Uses Win32 MessageBox
    // for the result — a ContentDialog cannot be shown while the parent edit
    // form ContentDialog is still open (WinUI 3 allows only one at a time).
    void HomePage::TestConnectionAsync(
        const DBModels::ConnectionConfig& config, const std::wstring& password)
    {
        bool success = false;
        std::wstring errorMsg;
        try
        {
            DBModels::ConnectionManager mgr;
            success = mgr.testConnection(config, password);
            if (!success) errorMsg = L"Connection test returned false.";
        }
        catch (const DBModels::DatabaseError& ex)
        {
            auto what = ex.what();
            int sz = MultiByteToWideChar(CP_UTF8, 0, what, -1, nullptr, 0);
            if (sz > 0)
            {
                errorMsg.resize(sz);
                MultiByteToWideChar(CP_UTF8, 0, what, -1, &errorMsg[0], sz);
                if (!errorMsg.empty() && errorMsg.back() == L'\0') errorMsg.pop_back();
            }
        }
        catch (...) { errorMsg = L"Unknown error"; }

        std::wstring title = success ? L"Connection Successful" : L"Connection Failed";
        std::wstring text  = success
            ? (L"Successfully connected to " + config.host)
            : errorMsg;

        HWND owner = winrt::Gridex::implementation::App::MainHwnd;
        MessageBoxW(owner, text.c_str(), title.c_str(),
            MB_OK | (success ? MB_ICONINFORMATION : MB_ICONERROR));
    }

    void HomePage::EditConnection(const std::wstring& id)
    {
        // Find the connection in memory
        auto it = std::find_if(allConnections_.begin(), allConnections_.end(),
            [&id](const DBModels::ConnectionConfig& c) { return c.id == id; });
        if (it == allConnections_.end()) return;

        const auto existing = *it;

        // Show ConnectionFormDialog pre-filled with existing values
        muxc::ContentDialog formDialog;
        formDialog.Title(winrt::box_value(winrt::hstring(
            L"Edit " + DBModels::DatabaseTypeDisplayName(existing.databaseType) +
            L" Connection")));
        auto form = winrt::make<ConnectionFormDialog>();
        auto formImpl = form.as<ConnectionFormDialog>();
        formImpl->SetDatabaseType(existing.databaseType);
        formImpl->SetConnectionConfig(existing);
        formDialog.Content(form);
        formDialog.CloseButtonText(L"Cancel");
        formDialog.XamlRoot(this->XamlRoot());
        formDialog.Resources().Insert(
            winrt::box_value(L"ContentDialogMaxWidth"),
            winrt::box_value(800.0));
        formDialog.Resources().Insert(
            winrt::box_value(L"ContentDialogMinWidth"),
            winrt::box_value(720.0));

        // Save — keep original id, overwrite existing record
        formImpl->OnSave = [this, formImpl, formDialog, id]() mutable
        {
            auto config = formImpl->GetConnectionConfig();
            config.id = id;  // preserve original id

            // Password comes from form (pre-filled on open). If somehow empty,
            // fall back to stored value to avoid breaking the connection.
            config.password = formImpl->GetPassword();
            if (config.password.empty())
            {
                auto existingIt = std::find_if(
                    allConnections_.begin(), allConnections_.end(),
                    [&id](const DBModels::ConnectionConfig& c) { return c.id == id; });
                if (existingIt != allConnections_.end())
                    config.password = existingIt->password;
            }

            // Update in-memory list
            for (auto& c : allConnections_)
                if (c.id == id) { c = config; break; }

            DBModels::ConnectionStore::Save(config);
            RefreshList();
            formDialog.Hide();
        };

        // Test — validate current form values without saving
        formImpl->OnTest = [this, formImpl, id]()
        {
            auto config = formImpl->GetConnectionConfig();
            config.id = id;
            auto pwd = formImpl->GetPassword();
            if (pwd.empty())
            {
                // Fall back to stored password if user didn't change it
                auto it = std::find_if(allConnections_.begin(), allConnections_.end(),
                    [&id](const DBModels::ConnectionConfig& c) { return c.id == id; });
                if (it != allConnections_.end()) pwd = it->password;
            }
            TestConnectionAsync(config, pwd);
        };

        formDialog.ShowAsync();
    }

    void HomePage::AddConnectionCard(const DBModels::ConnectionConfig& config)
    {
        auto card = winrt::make<ConnectionCard>();
        auto cardImpl = card.as<ConnectionCard>();
        cardImpl->SetConnection(config);
        cardImpl->OnDelete = [this](const std::wstring& id)
        {
            DeleteConnection(id);
        };
        cardImpl->OnEdit = [this](const std::wstring& id)
        {
            EditConnection(id);
        };
        card.Tag(winrt::box_value(winrt::hstring(config.id)));
        ConnectionsListView().Items().Append(card);
    }

    void HomePage::RefreshList(const std::wstring& searchQuery)
    {
        // Always re-read from the store so imports / edits / deletes
        // don't need a page-reload to show. Cheap: ConnectionStore::Load
        // reads one SQLite table and decrypts passwords — sub-ms for
        // typical <200 connection counts.
        allConnections_ = DBModels::ConnectionStore::Load();
        ConnectionsListView().Items().Clear();

        auto matches = [&](const DBModels::ConnectionConfig& conn) -> bool
        {
            if (searchQuery.empty()) return true;
            auto toLower = [](std::wstring s)
            {
                std::transform(s.begin(), s.end(), s.begin(), ::towlower);
                return s;
            };
            auto q = toLower(searchQuery);
            return toLower(conn.name).find(q) != std::wstring::npos
                || toLower(conn.host).find(q) != std::wstring::npos
                || toLower(conn.database).find(q) != std::wstring::npos;
        };

        // Collect all group names (from settings + connection memberships)
        auto s = DBModels::AppSettings::Load();
        std::vector<std::wstring> groupNames = s.connectionGroups;
        for (const auto& c : allConnections_)
        {
            if (c.group.empty()) continue;
            auto it = std::find(groupNames.begin(), groupNames.end(), c.group);
            if (it == groupNames.end()) groupNames.push_back(c.group);
        }

        // Render each group section
        for (const auto& groupName : groupNames)
        {
            // Count connections in this group that match search
            int count = 0;
            for (const auto& conn : allConnections_)
                if (conn.group == groupName && matches(conn)) count++;

            // Show group header even if empty (so user sees the group they created)
            AddGroupHeader(groupName, count);
            for (const auto& conn : allConnections_)
                if (conn.group == groupName && matches(conn))
                    AddConnectionCard(conn);
        }

        // Ungrouped connections
        bool hasUngrouped = false;
        for (const auto& conn : allConnections_)
            if (conn.group.empty() && matches(conn)) { hasUngrouped = true; break; }

        if (hasUngrouped)
        {
            if (!groupNames.empty())
            {
                int ungroupedCount = 0;
                for (const auto& conn : allConnections_)
                    if (conn.group.empty() && matches(conn)) ungroupedCount++;
                AddGroupHeader(L"Ungrouped", ungroupedCount);
            }
            for (const auto& conn : allConnections_)
                if (conn.group.empty() && matches(conn))
                    AddConnectionCard(conn);
        }

        UpdateEmptyState();
    }

    // Add a non-clickable group header row to the ListView
    void HomePage::AddGroupHeader(const std::wstring& name, int count)
    {
        muxc::StackPanel header;
        header.Orientation(muxc::Orientation::Horizontal);
        header.Spacing(8.0);
        header.Padding(mux::ThicknessHelper::FromLengths(8, 12, 8, 4));

        muxc::TextBlock folderIcon;
        folderIcon.Text(L"\xE8B7");
        folderIcon.FontFamily(muxm::FontFamily(L"Segoe Fluent Icons,Segoe MDL2 Assets"));
        folderIcon.FontSize(12.0);
        folderIcon.Opacity(0.6);
        folderIcon.VerticalAlignment(mux::VerticalAlignment::Center);
        header.Children().Append(folderIcon);

        muxc::TextBlock nameText;
        nameText.Text(winrt::hstring(name));
        nameText.FontSize(12.0);
        nameText.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        nameText.Opacity(0.75);
        nameText.VerticalAlignment(mux::VerticalAlignment::Center);
        header.Children().Append(nameText);

        muxc::TextBlock countText;
        countText.Text(winrt::hstring(L"(" + std::to_wstring(count) + L")"));
        countText.FontSize(11.0);
        countText.Opacity(0.4);
        countText.VerticalAlignment(mux::VerticalAlignment::Center);
        header.Children().Append(countText);

        // No Tag set → ConnectionItem_Click ignores it
        ConnectionsListView().Items().Append(header);
    }

    void HomePage::UpdateEmptyState()
    {
        bool empty = (ConnectionsListView().Items().Size() == 0);
        EmptyState().Visibility(empty ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        ConnectionsListView().Visibility(empty ? mux::Visibility::Collapsed : mux::Visibility::Visible);
    }

    void HomePage::NewConnection_Click(
        winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
    {
        ShowNewConnectionFlow();
    }

    void HomePage::ShowNewConnectionFlow()
    {
        muxc::ContentDialog typeDialog;
        typeDialog.Title(winrt::box_value(winrt::hstring(L"New Connection")));
        auto picker = winrt::make<DatabaseTypePickerDialog>();
        typeDialog.Content(picker);
        typeDialog.CloseButtonText(L"Cancel");
        typeDialog.XamlRoot(this->XamlRoot());

        auto pickerImpl = picker.as<DatabaseTypePickerDialog>();
        pickerImpl->OnTypeSelected = [this, typeDialog](DBModels::DatabaseType type) mutable
        {
            typeDialog.Hide();

            muxc::ContentDialog formDialog;
            formDialog.Title(winrt::box_value(winrt::hstring(
                DBModels::DatabaseTypeDisplayName(type) + L" Connection")));
            auto form = winrt::make<ConnectionFormDialog>();
            auto formImpl = form.as<ConnectionFormDialog>();
            formImpl->SetDatabaseType(type);
            formDialog.Content(form);
            formDialog.CloseButtonText(L"Cancel");
            formDialog.XamlRoot(this->XamlRoot());
            // Ensure dialog is wide enough for the form
            formDialog.Resources().Insert(
                winrt::box_value(L"ContentDialogMaxWidth"),
                winrt::box_value(800.0));
            formDialog.Resources().Insert(
                winrt::box_value(L"ContentDialogMinWidth"),
                winrt::box_value(720.0));

            // Wire Save callback — save config + password, persist to disk
            formImpl->OnSave = [this, formImpl, formDialog]() mutable
            {
                auto config = formImpl->GetConnectionConfig();
                config.password = formImpl->GetPassword();
                GUID guid;
                CoCreateGuid(&guid);
                wchar_t guidStr[40];
                StringFromGUID2(guid, guidStr, 40);
                config.id = guidStr;
                allConnections_.push_back(config);
                DBModels::ConnectionStore::Save(config);
                RefreshList();
                formDialog.Hide();
            };

            // Wire Test callback — validate credentials without saving
            formImpl->OnTest = [this, formImpl]()
            {
                auto config = formImpl->GetConnectionConfig();
                auto pwd = formImpl->GetPassword();
                TestConnectionAsync(config, pwd);
            };

            // Wire Connect callback — save + navigate to workspace
            formImpl->OnConnect = [this, formImpl, formDialog]() mutable
            {
                auto config = formImpl->GetConnectionConfig();
                config.password = formImpl->GetPassword();
                GUID guid;
                CoCreateGuid(&guid);
                wchar_t guidStr[40];
                StringFromGUID2(guid, guidStr, 40);
                config.id = guidStr;
                allConnections_.push_back(config);
                DBModels::ConnectionStore::Save(config);
                RefreshList();
                formDialog.Hide();

                // Test connection before navigating to workspace
                auto frame = this->Frame();
                if (frame)
                {
                    try
                    {
                        DBModels::ConnectionManager mgr;
                        std::wstring err;
                        if (!mgr.testConnectionWithError(config, config.password, err))
                        {
                            std::wstring msg = L"Could not connect to " + config.name + L".";
                            if (!err.empty()) msg += L"\n\n" + err;
                            else msg += L" Check host, port, credentials, and ensure the server is running.";
                            muxc::ContentDialog errDlg;
                            errDlg.Title(winrt::box_value(winrt::hstring(L"Connection Failed")));
                            errDlg.Content(winrt::box_value(winrt::hstring(msg)));
                            errDlg.CloseButtonText(L"OK");
                            errDlg.XamlRoot(this->XamlRoot());
                            errDlg.ShowAsync();
                            return;
                        }
                    }
                    catch (...) {}

                    auto workspace = winrt::make<WorkspacePage>();
                    workspace.as<WorkspacePage>()->SetConnection(config, config.password);
                    frame.Content(workspace);
                }
            };

            formDialog.ShowAsync();
        };

        typeDialog.ShowAsync();
    }

    void HomePage::NewGroup_Click(
        winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
    {
        ShowNewGroupDialogAsync();
    }

    winrt::fire_and_forget HomePage::ShowNewGroupDialogAsync()
    {
        muxc::ContentDialog dialog;
        dialog.Title(winrt::box_value(winrt::hstring(L"New Group")));
        muxc::TextBox input;
        input.PlaceholderText(L"Group name");
        dialog.Content(input);
        dialog.PrimaryButtonText(L"Create");
        dialog.CloseButtonText(L"Cancel");
        dialog.DefaultButton(muxc::ContentDialogButton::Primary);
        dialog.XamlRoot(this->XamlRoot());

        auto result = co_await dialog.ShowAsync();
        if (result != muxc::ContentDialogResult::Primary) co_return;

        std::wstring name(input.Text());
        // Trim whitespace
        while (!name.empty() && (name.back() == L' ' || name.back() == L'\t'))
            name.pop_back();
        size_t start = name.find_first_not_of(L" \t");
        if (start != std::wstring::npos) name = name.substr(start);
        if (name.empty()) co_return;

        // Persist to AppSettings — avoid duplicates (case-insensitive)
        auto s = DBModels::AppSettings::Load();
        auto exists = std::any_of(s.connectionGroups.begin(), s.connectionGroups.end(),
            [&name](const std::wstring& g)
            {
                if (g.size() != name.size()) return false;
                for (size_t i = 0; i < g.size(); i++)
                    if (towlower(g[i]) != towlower(name[i])) return false;
                return true;
            });
        if (!exists)
        {
            s.connectionGroups.push_back(name);
            s.Save();
        }

        RefreshList();
    }

    void HomePage::Settings_Click(
        winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
    {
        muxc::Frame frame{ nullptr };
        auto fe = this->try_as<mux::FrameworkElement>();
        while (fe)
        {
            if (auto f = fe.try_as<muxc::Frame>()) { frame = f; break; }
            fe = fe.Parent().try_as<mux::FrameworkElement>();
        }
        if (!frame) frame = this->Frame();
        if (!frame) return;

        // Remember this page so Settings' Back button returns here
        auto s = DBModels::AppSettings::Load();
        s.lastPageBeforeSettings = L"Gridex.HomePage";
        s.Save();

        winrt::Windows::UI::Xaml::Interop::TypeName pageType;
        pageType.Name = L"Gridex.SettingsPage";
        pageType.Kind = winrt::Windows::UI::Xaml::Interop::TypeKind::Metadata;
        frame.Navigate(pageType);
    }

    void HomePage::OpenMcp_Click(
        winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
    {
        muxc::Frame frame{ nullptr };
        auto fe = this->try_as<mux::FrameworkElement>();
        while (fe)
        {
            if (auto f = fe.try_as<muxc::Frame>()) { frame = f; break; }
            fe = fe.Parent().try_as<mux::FrameworkElement>();
        }
        if (!frame) frame = this->Frame();
        if (!frame) return;

        auto s = DBModels::AppSettings::Load();
        s.lastPageBeforeSettings = L"Gridex.HomePage";
        s.Save();

        winrt::Windows::UI::Xaml::Interop::TypeName pageType;
        pageType.Name = L"Gridex.MCPPage";
        pageType.Kind = winrt::Windows::UI::Xaml::Interop::TypeKind::Metadata;
        frame.Navigate(pageType);
    }

    void HomePage::ConnectionItem_Click(
        winrt::Windows::Foundation::IInspectable const&, muxc::ItemClickEventArgs const& e)
    {
        if (!e.ClickedItem()) return;

        // ClickedItem returns the ConnectionCard UserControl
        // Read Tag to find connection id
        winrt::hstring connId;
        if (auto fe = e.ClickedItem().try_as<winrt::Microsoft::UI::Xaml::FrameworkElement>())
        {
            if (auto tag = fe.Tag())
                connId = winrt::unbox_value<winrt::hstring>(tag);
        }

        if (connId.empty()) return;

        // Find the connection config
        DBModels::ConnectionConfig selectedConn;
        bool found = false;
        for (const auto& conn : allConnections_)
        {
            if (conn.id == std::wstring(connId))
            {
                selectedConn = conn;
                found = true;
                break;
            }
        }
        if (!found) return;

        // Test connection BEFORE navigating to workspace. If the test
        // fails, show error on HomePage so the user doesn't land on a
        // blank workspace with an error dialog they have to dismiss.
        auto frame = this->Frame();
        if (!frame) return;

        try
        {
            DBModels::ConnectionManager mgr;
            std::wstring err;
            if (!mgr.testConnectionWithError(selectedConn, selectedConn.password, err))
            {
                std::wstring msg = L"Could not connect to " + selectedConn.name + L".";
                if (!err.empty()) msg += L"\n\n" + err;
                else msg += L" Check host, port, credentials, and ensure the server is running.";
                muxc::ContentDialog errDlg;
                errDlg.Title(winrt::box_value(winrt::hstring(L"Connection Failed")));
                errDlg.Content(winrt::box_value(winrt::hstring(msg)));
                errDlg.CloseButtonText(L"OK");
                errDlg.XamlRoot(this->XamlRoot());
                errDlg.ShowAsync();
                return;
            }
        }
        catch (...) {}

        auto workspace = winrt::make<WorkspacePage>();
        workspace.as<WorkspacePage>()->SetConnection(selectedConn, selectedConn.password);
        frame.Content(workspace);
    }

    void HomePage::SearchBox_TextChanged(
        muxc::AutoSuggestBox const& sender, muxc::AutoSuggestBoxTextChangedEventArgs const&)
    {
        RefreshList(std::wstring(sender.Text()));
    }

    // ── Import Connections wizard ──────────────────────────────
    // Builds a ContentDialog with a source switcher + list of
    // ImportedConnections with per-row CheckBoxes. Import button
    // writes selected rows into ConnectionStore and refreshes the
    // sidebar list. Navicat branches through an .ncx file picker.

    namespace
    {
        std::wstring newConnectionId()
        {
            std::random_device rd;
            std::mt19937_64 gen(rd());
            std::uniform_int_distribution<uint64_t> dist;
            wchar_t buf[33];
            swprintf_s(buf, 33, L"%016llx%016llx",
                (unsigned long long)dist(gen), (unsigned long long)dist(gen));
            return buf;
        }

        // Shared builder for a single importer-result row. The
        // CheckBox instance is stored in `row.first` so the Import
        // button can poll IsChecked when the user clicks confirm.
        std::pair<muxc::CheckBox, winrt::Microsoft::UI::Xaml::Controls::Border>
        buildImportedRow(const DBModels::ImportedConnection& c)
        {
            using namespace winrt::Microsoft::UI::Xaml;
            using namespace winrt::Microsoft::UI::Xaml::Controls;

            muxc::Grid row;
            row.Padding(Thickness{ 8, 6, 8, 6 });
            row.ColumnSpacing(10);

            ColumnDefinition c0, c1, c2;
            c0.Width(GridLengthHelper::FromPixels(36));
            c1.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
            c2.Width(GridLengthHelper::FromPixels(90));
            row.ColumnDefinitions().Append(c0);
            row.ColumnDefinitions().Append(c1);
            row.ColumnDefinitions().Append(c2);

            muxc::CheckBox chk; chk.IsChecked(true);
            muxc::Grid::SetColumn(chk, 0); row.Children().Append(chk);

            muxc::StackPanel sp; sp.Spacing(2);
            muxc::TextBlock name; name.Text(winrt::hstring(c.name));
            name.FontSize(13); name.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            sp.Children().Append(name);
            std::wstring sub = !c.host.empty()
                ? c.host + (c.port.has_value() ? L":" + std::to_wstring(*c.port) : L"")
                : c.filePath;
            if (!c.database.empty()) sub += L" / " + c.database;
            if (sub.empty()) sub = L"(no host)";
            muxc::TextBlock subt; subt.Text(winrt::hstring(sub));
            subt.FontSize(11);
            subt.Foreground(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(
                winrt::Windows::UI::ColorHelper::FromArgb(255, 150, 150, 150)));
            sp.Children().Append(subt);
            muxc::Grid::SetColumn(sp, 1); row.Children().Append(sp);

            muxc::TextBlock type; type.Text(winrt::hstring(
                DBModels::DatabaseTypeDisplayName(c.databaseType)));
            type.FontSize(11); type.VerticalAlignment(VerticalAlignment::Center);
            type.Foreground(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(
                winrt::Windows::UI::ColorHelper::FromArgb(255, 150, 150, 150)));
            muxc::Grid::SetColumn(type, 2); row.Children().Append(type);

            muxc::Border wrap;
            wrap.Child(row);
            wrap.BorderBrush(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(
                winrt::Windows::UI::ColorHelper::FromArgb(30, 128, 128, 128)));
            wrap.BorderThickness(Thickness{ 0, 0, 0, 1 });
            return { chk, wrap };
        }
    }

    winrt::fire_and_forget HomePage::ImportConnections_Click(
        winrt::Windows::Foundation::IInspectable const&,
        mux::RoutedEventArgs const&)
    {
        auto self = get_strong();

        // Dialog + source ComboBox + rows container.
        muxc::ContentDialog dlg;
        dlg.Title(winrt::box_value(winrt::hstring(L"Import Connections")));
        dlg.PrimaryButtonText(L"Import Selected");
        dlg.CloseButtonText(L"Cancel");
        dlg.DefaultButton(muxc::ContentDialogButton::Primary);
        dlg.XamlRoot(this->XamlRoot());
        dlg.Resources().Insert(winrt::box_value(L"ContentDialogMaxWidth"), winrt::box_value(720.0));

        muxc::StackPanel content; content.Spacing(10); content.MinWidth(520);

        muxc::TextBlock intro;
        intro.Text(L"Pick a source, review detected connections, then Import.");
        intro.FontSize(12);
        intro.Foreground(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(
            winrt::Windows::UI::ColorHelper::FromArgb(255, 150, 150, 150)));
        content.Children().Append(intro);

        muxc::ComboBox sourceCombo; sourceCombo.MinWidth(240);
        auto addSource = [&](const wchar_t* label, bool installed)
        {
            muxc::ComboBoxItem item;
            item.Content(winrt::box_value(winrt::hstring(
                installed ? label : std::wstring(label) + L"  (not detected)")));
            sourceCombo.Items().Append(item);
        };
        // Kept the list conservative: no .ncx file-picker branch
        // — that needed an inner coroutine + cross-frame captures
        // that proved crash-prone. Users who need .ncx can import
        // via CLI or a future dedicated entry.
        addSource(L"DBeaver",   DBModels::DBeaverImporter::isInstalled());
        addSource(L"DataGrip",  DBModels::DataGripImporter::isInstalled());
        addSource(L"Navicat",   DBModels::NavicatImporter::isInstalled());
        addSource(L"TablePlus", DBModels::TablePlusImporter::isInstalled());
        content.Children().Append(sourceCombo);

        muxc::TextBlock status; status.FontSize(11);
        status.Foreground(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(
            winrt::Windows::UI::ColorHelper::FromArgb(255, 180, 180, 180)));
        content.Children().Append(status);

        muxc::ScrollViewer sv;
        sv.HorizontalScrollMode(muxc::ScrollMode::Disabled);
        sv.VerticalScrollBarVisibility(muxc::ScrollBarVisibility::Auto);
        sv.MaxHeight(360);
        muxc::StackPanel rows; rows.Spacing(0);
        sv.Content(rows);
        content.Children().Append(sv);

        dlg.Content(content);

        // State shared with the SelectionChanged lambda. shared_ptr
        // so the lambda's copy of the handles stays valid even if
        // the outer coroutine is suspended in ShowAsync.
        auto imported = std::make_shared<std::vector<DBModels::ImportedConnection>>();
        auto checks   = std::make_shared<std::vector<muxc::CheckBox>>();

        // Synchronous scan — no coroutine, no reference captures to
        // locals. Everything the lambda needs is captured by value.
        auto loadSource = [imported, checks, rows, status](int idx)
        {
            try
            {
                imported->clear();
                checks->clear();
                rows.Children().Clear();
                status.Text(L"Scanning...");

                // Indices in addSource() order: 0 DBeaver, 1 DataGrip,
                // 2 Navicat (registry), 3 TablePlus.
                if      (idx == 0) *imported = DBModels::DBeaverImporter::importConnections();
                else if (idx == 1) *imported = DBModels::DataGripImporter::importConnections();
                else if (idx == 2) *imported = DBModels::NavicatImporter::importConnections();
                else if (idx == 3) *imported = DBModels::TablePlusImporter::importConnections();

                if (imported->empty())
                {
                    status.Text(L"No connections detected.");
                    return;
                }
                status.Text(winrt::hstring(
                    std::to_wstring(imported->size()) +
                    L" connection(s) detected. Untick any you want to skip."));
                for (const auto& c : *imported)
                {
                    auto [chk, wrap] = buildImportedRow(c);
                    checks->push_back(chk);
                    rows.Children().Append(wrap);
                }
            }
            catch (const std::exception& e)
            {
                std::string msg = e.what();
                status.Text(winrt::hstring(L"Error: " +
                    std::wstring(msg.begin(), msg.end())));
            }
            catch (...) { status.Text(L"Scan failed."); }
        };

        // Attach handler THEN seed the selection so the handler
        // fires exactly once during visible-state setup instead of
        // racing with a separate manual scan.
        sourceCombo.SelectionChanged(
            [loadSource](auto&& s, auto&&)
            {
                if (auto cb = s.try_as<muxc::ComboBox>())
                    loadSource(cb.SelectedIndex());
            });
        sourceCombo.SelectedIndex(0);

        // Show dialog; on Primary, snapshot selections immediately
        // (before the dialog tears its controls down) and then write
        // to the store + refresh the sidebar.
        muxc::ContentDialogResult result{ muxc::ContentDialogResult::None };
        try { result = co_await dlg.ShowAsync(); }
        catch (...) { co_return; }
        if (result != muxc::ContentDialogResult::Primary) co_return;

        std::vector<size_t> selected;
        for (size_t i = 0; i < imported->size() && i < checks->size(); ++i)
        {
            try
            {
                auto ib = (*checks)[i].IsChecked();
                if (ib && ib.Value()) selected.push_back(i);
            }
            catch (...) { /* control detached */ }
        }

        int added = 0;
        for (size_t i : selected)
        {
            try
            {
                auto cfg = (*imported)[i].toConnectionConfig();
                if (cfg.id.empty()) cfg.id = newConnectionId();
                DBModels::ConnectionStore::Save(cfg);
                ++added;
            }
            catch (...) { /* skip one bad row, keep the batch */ }
        }

        try { RefreshList(); } catch (...) {}
        (void)added; // feedback is the sidebar update
    }
}
