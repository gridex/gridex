#include "pch.h"
#include "xaml-includes.h"
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.System.h>
#include "WorkspacePage.h"
#include "SidebarPanel.h"
#include "TabBarControl.h"
#include "DataGridView.h"
#include "QueryEditorView.h"
#include "StructureView.h"
#include "StatusBarControl.h"
#include "DetailsPanel.h"
#include "FilterBar.h"
#include "Models/ExportService.h"
#include "Models/ImportService.h"
#include "Models/DumpRestoreService.h"
#include "Models/ERDiagramService.h"
#include "Models/AppSettings.h"
#include "App.xaml.h"
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.Web.WebView2.Core.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Microsoft.UI.Xaml.Interop.h>
#include <winrt/Microsoft.Windows.Storage.Pickers.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <microsoft.ui.xaml.window.h>
#include <ShlObj.h>
#include <chrono>
#include <fstream>
#include <thread>
#if __has_include("WorkspacePage.g.cpp")
#include "WorkspacePage.g.cpp"
#endif

namespace winrt::Gridex::implementation
{
    namespace mux  = winrt::Microsoft::UI::Xaml;
    namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
    namespace muxm = winrt::Microsoft::UI::Xaml::Media;

    // Definitions for the cross-instance connection cache declared in
    // WorkspacePage.h. Populated by SetConnection, consumed by the
    // Loaded handler when a fresh WorkspacePage lands without one
    // (e.g. after Back from SettingsPage).
    DBModels::ConnectionConfig WorkspacePage::sLastConfig_{};
    std::wstring               WorkspacePage::sLastPassword_{};
    bool                       WorkspacePage::sHasLastConnection_ = false;

    WorkspacePage::WorkspacePage()
    {
        InitializeComponent();

        this->Loaded([this](winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
        {
            isLoaded_ = true;
            WireAllButtons();
            ApplyAiSettings();   // Load AI config from settings.json → DetailsPanel

            if (hasConnection_)
            {
                InitializeConnection();
                return;
            }

            // Fresh instance (Frame created it via default ctor after Back
            // from Settings, etc.) — restore the last active connection
            // from the process-wide cache instead of falling back to the
            // hard-coded demo sidebar.
            if (sHasLastConnection_)
            {
                SetConnection(sLastConfig_, sLastPassword_);
                return;
            }

            LoadDemoSidebarData();
        });
    }

    // ── Load AI config from persisted settings into DetailsPanel ───
    void WorkspacePage::ApplyAiSettings()
    {
        auto s = DBModels::AppSettings::Load();
        DBModels::AiConfig cfg;
        cfg.provider = static_cast<DBModels::AiProvider>(s.aiProviderIndex);
        cfg.apiKey = s.aiApiKey;
        cfg.model = s.aiModel;
        cfg.ollamaEndpoint = s.ollamaEndpoint;
        Details().as<DetailsPanel>()->SetAiConfig(cfg);
    }

    // ── Button Wiring (code-behind, no IDL needed) ──────
    void WorkspacePage::WireAllButtons()
    {
        if (buttonsWired_) return;
        buttonsWired_ = true;

        // ── Keyboard shortcuts ─────────────────────────
        // Ctrl+S → commit pending row changes (mirrors the Commit toolbar button)
        {
            auto commitAccel = mux::Input::KeyboardAccelerator();
            commitAccel.Key(winrt::Windows::System::VirtualKey::S);
            commitAccel.Modifiers(winrt::Windows::System::VirtualKeyModifiers::Control);
            commitAccel.Invoked([this](auto&&, mux::Input::KeyboardAcceleratorInvokedEventArgs const& args)
            {
                CommitChanges();
                args.Handled(true);
            });
            this->KeyboardAccelerators().Append(commitAccel);
        }

        // Ctrl+Z → discard pending row changes
        {
            auto discardAccel = mux::Input::KeyboardAccelerator();
            discardAccel.Key(winrt::Windows::System::VirtualKey::Z);
            discardAccel.Modifiers(winrt::Windows::System::VirtualKeyModifiers::Control);
            discardAccel.Invoked([this](auto&&, mux::Input::KeyboardAcceleratorInvokedEventArgs const& args)
            {
                DiscardChanges();
                args.Handled(true);
            });
            this->KeyboardAccelerators().Append(discardAccel);
        }

        // Ctrl+T → open a new SQL query tab
        {
            auto newTabAccel = mux::Input::KeyboardAccelerator();
            newTabAccel.Key(winrt::Windows::System::VirtualKey::T);
            newTabAccel.Modifiers(winrt::Windows::System::VirtualKeyModifiers::Control);
            newTabAccel.Invoked([this](auto&&, mux::Input::KeyboardAcceleratorInvokedEventArgs const& args)
            {
                OpenNewQueryTab();
                args.Handled(true);
            });
            this->KeyboardAccelerators().Append(newTabAccel);
        }

        // Ctrl+W → close the currently active tab (no-op if none)
        {
            auto closeTabAccel = mux::Input::KeyboardAccelerator();
            closeTabAccel.Key(winrt::Windows::System::VirtualKey::W);
            closeTabAccel.Modifiers(winrt::Windows::System::VirtualKeyModifiers::Control);
            closeTabAccel.Invoked([this](auto&&, mux::Input::KeyboardAcceleratorInvokedEventArgs const& args)
            {
                if (!state_.activeTabId.empty())
                {
                    // CloseTab fires OnTabClosed synchronously which mutates
                    // state_.tabs and state_.activeTabId. Safe here because
                    // we're not iterating the tabs vector.
                    TabBar().as<TabBarControl>()->CloseTab(
                        winrt::hstring(state_.activeTabId));
                }
                args.Handled(true);
            });
            this->KeyboardAccelerators().Append(closeTabAccel);
        }

        // DataGrid callbacks
        DataGrid().as<DataGridView>()->OnRowSelected = [this](int rowIndex)
        {
            if (rowIndex >= 0 && rowIndex < static_cast<int>(state_.currentData.rows.size()))
            {
                Details().as<DetailsPanel>()->ShowRow(
                    state_.currentData.columnNames,
                    state_.currentData.rows[rowIndex]);
            }
        };

        // Delete key on grid → same as Delete toolbar button (mark row
        // deleted; commit still requires explicit Commit action / Ctrl+S).
        DataGrid().as<DataGridView>()->OnDeleteRequested = [this]()
        {
            DeleteSelectedRow();
        };

        // Right-click → Refresh: re-run the active tab's query.
        DataGrid().as<DataGridView>()->OnRefreshRequested = [this]()
        {
            ReloadCurrentTable();
        };

        // FK icon click in a header → open the referenced table. Schema
        // arg is empty; reuse the current schema so cross-schema FKs in
        // pg still resolve against the active search path.
        DataGrid().as<DataGridView>()->OnForeignKeyClicked =
            [this](const std::wstring& refTable, const std::wstring&)
        {
            if (!refTable.empty())
                OnTableSelected(refTable, currentSchema_);
        };

        // Wire structure view ALTER apply
        Structure().as<StructureView>()->OnApplyAlter = [this](const std::vector<std::wstring>& sqls)
        {
            if (!connMgr_.isConnected() || sqls.empty()) return;
            auto adapter = connMgr_.getActiveAdapter();
            if (!adapter) return;

            try
            {
                for (auto& sql : sqls)
                {
                    auto result = adapter->execute(sql);
                    LogQuery(result);
                    if (!result.success)
                    {
                        muxc::ContentDialog errDlg;
                        errDlg.Title(winrt::box_value(winrt::hstring(L"ALTER Failed")));
                        errDlg.Content(winrt::box_value(winrt::hstring(
                            L"SQL: " + sql + L"\n\n" + result.error)));
                        errDlg.CloseButtonText(L"OK");
                        errDlg.XamlRoot(this->XamlRoot());
                        errDlg.ShowAsync();
                        return;
                    }
                }
                // Reload table structure
                ReloadCurrentTable();
            }
            catch (...) {}
        };

        // Wire details panel field edit → ChangeTracker + DataGrid sync
        Details().as<DetailsPanel>()->OnFieldEdited = [this](
            const std::wstring& column, const std::wstring& oldValue, const std::wstring& newValue)
        {
            int rowIndex = DataGrid().as<DataGridView>()->GetSelectedRowIndex();
            if (rowIndex < 0) return;

            // Update DataGrid data
            if (rowIndex < static_cast<int>(state_.currentData.rows.size()))
                state_.currentData.rows[rowIndex][column] = newValue;

            // Track change
            auto pk = ExtractPrimaryKey(rowIndex);
            changeTracker_.trackUpdate(rowIndex, column, oldValue, newValue, pk);
            UpdatePendingUI();

            // Refresh grid row display
            DataGrid().as<DataGridView>()->SetData(state_.currentData);
        };

        // Wire details panel "+" context picker: expose the current DB's
        // table list so the chat flyout can populate, and a structure
        // fetcher that returns a DDL-like snippet the assistant can read.
        Details().as<DetailsPanel>()->OnRequestTableList = [this]() -> std::vector<std::wstring>
        {
            std::vector<std::wstring> names;
            for (auto& group : state_.sidebarItems)
            {
                if (group.id != L"tables" && group.id != L"views") continue;
                for (auto& item : group.children) names.push_back(item.title);
            }
            return names;
        };

        Details().as<DetailsPanel>()->OnFetchTableStructure =
            [this](const std::wstring& tableName) -> std::wstring
        {
            auto adapter = connMgr_.getActiveAdapter();
            if (!adapter) return {};
            try
            {
                auto cols    = adapter->describeTable(tableName, currentSchema_);
                auto indexes = adapter->listIndexes(tableName, currentSchema_);
                auto fks     = adapter->listForeignKeys(tableName, currentSchema_);

                std::wstring out;
                out += L"Table: " + tableName + L"\n";
                out += L"Columns:\n";
                for (auto& c : cols)
                {
                    out += L"  " + c.name + L" " + c.dataType;
                    if (c.isPrimaryKey)        out += L" PRIMARY KEY";
                    if (!c.nullable)           out += L" NOT NULL";
                    if (!c.defaultValue.empty())
                        out += L" DEFAULT " + c.defaultValue;
                    out += L"\n";
                }
                if (!indexes.empty())
                {
                    out += L"Indexes:\n";
                    for (auto& i : indexes)
                    {
                        out += L"  " + i.name + L" (" + i.columns + L")";
                        if (i.isUnique) out += L" UNIQUE";
                        out += L"\n";
                    }
                }
                if (!fks.empty())
                {
                    out += L"Foreign Keys:\n";
                    for (auto& f : fks)
                    {
                        out += L"  " + f.column + L" -> "
                             + f.referencedTable + L"(" + f.referencedColumn + L")\n";
                    }
                }
                return out;
            }
            catch (...) { return {}; }
        };

        // Wire cell edit → ChangeTracker
        DataGrid().as<DataGridView>()->OnCellEdited = [this](int rowIndex,
            const std::wstring& column, const std::wstring& oldValue, const std::wstring& newValue)
        {
            auto pk = ExtractPrimaryKey(rowIndex);
            changeTracker_.trackUpdate(rowIndex, column, oldValue, newValue, pk);
            UpdatePendingUI();
        };

        DataGrid().as<DataGridView>()->OnSortRequested = [this](const std::wstring& column, bool ascending)
        {
            if (!connMgr_.isConnected()) return;
            for (auto& tab : state_.tabs)
            {
                if (tab.id == state_.activeTabId && tab.type == DBModels::TabType::DataGrid)
                {
                    auto adapter = connMgr_.getActiveAdapter();
                    if (!adapter) return;
                    try
                    {
                        state_.currentData = adapter->fetchRows(
                            tab.tableName, tab.schema,
                            state_.pageSize, (state_.currentPage) * state_.pageSize,
                            column, ascending);
                        LogQuery(state_.currentData);
                        DataGrid().as<DataGridView>()->SetData(state_.currentData);
                        UpdatePaginationUI();
                    }
                    catch (...) {}
                    break;
                }
            }
        };

        // QueryEditor callback
        QueryEditor().as<QueryEditorView>()->OnExecuteQuery = [this](const std::wstring& sql) -> DBModels::QueryResult
        {
            if (connMgr_.isConnected())
            {
                auto adapter = connMgr_.getActiveAdapter();
                if (adapter)
                {
                    try
                    {
                        auto result = adapter->execute(sql);
                        LogQuery(result);
                        return result;
                    }
                    catch (const std::exception& ex)
                    {
                        DBModels::QueryResult err;
                        err.success = false;
                        int sz = MultiByteToWideChar(CP_UTF8, 0, ex.what(), -1, nullptr, 0);
                        std::wstring msg(sz, L'\0');
                        MultiByteToWideChar(CP_UTF8, 0, ex.what(), -1, &msg[0], sz);
                        err.error = msg;
                        return err;
                    }
                }
            }
            DBModels::QueryResult demo;
            demo.success = true;
            return demo;
        };

        // Tab bar callbacks
        TabBar().as<TabBarControl>()->OnNewTab = [this]() { OpenNewQueryTab(); };

        // When a tab is closed (via X button or Ctrl+W), sync state_.tabs
        // but do NOT touch state_.activeTabId or call SwitchContentView
        // here. TabBarControl::CloseTab fires OnTabChanged(newActiveId)
        // right after OnTabClosed when the closed tab was the active
        // one, and OnTabChanged owns all the "switch to a different
        // tab" logic (save cache, restore from cache, load data, repaint).
        //
        // Previously this handler also bumped state_.activeTabId to the
        // new active tab and called SwitchContentView. That corrupted
        // the cache: the subsequent OnTabChanged → SaveCurrentTabCache
        // used the newly-bumped activeTabId to decide which tab to
        // save into, so state_.currentData (still the closed tab's
        // data) got written onto the NEW active tab's cachedData, and
        // the restore path showed the wrong data in the grid. Leaving
        // activeTabId pointing at the closed-but-erased tab is safe
        // because SaveCurrentTabCache's lookup simply finds nothing
        // and becomes a no-op, then OnTabChanged bumps activeTabId
        // cleanly before loading the new tab.
        TabBar().as<TabBarControl>()->OnTabClosed = [this](const std::wstring& id)
        {
            // Cleanup ER diagram temp SVG if applicable
            for (const auto& t : state_.tabs)
            {
                if (t.id == id && t.type == DBModels::TabType::ERDiagram &&
                    !t.erSvgPath.empty())
                {
                    DeleteFileW(t.erSvgPath.c_str());
                }
            }
            state_.tabs.erase(
                std::remove_if(state_.tabs.begin(), state_.tabs.end(),
                    [&id](const DBModels::ContentTab& t) { return t.id == id; }),
                state_.tabs.end());

            // When the closed tab was NOT the active one, TabBarControl
            // won't fire OnTabChanged -- the active tab's content is
            // still visually correct, so no repaint needed.
            // When the closed tab WAS active, OnTabChanged runs next
            // and handles everything.
            if (state_.tabs.empty())
            {
                // Last tab closed: OnTabChanged will fire with empty id
                // and SwitchContentView there will show EmptyContentState.
                // Nothing to do here.
            }
        };
        TabBar().as<TabBarControl>()->OnTabChanged = [this](const std::wstring& id)
        {
            // Save current tab data to cache before switching
            SaveCurrentTabCache();

            state_.activeTabId = id;
            bool isERDiagramTab = false;
            for (auto& tab : state_.tabs)
            {
                if (tab.id == id)
                {
                    if (tab.type == DBModels::TabType::DataGrid)
                    {
                        // Restore from cache if available, else load fresh
                        if (tab.cachedData.has_value())
                        {
                            state_.currentData = tab.cachedData.value();
                            state_.currentColumns = tab.cachedColumns;
                            state_.currentIndexes = tab.cachedIndexes;
                            state_.currentForeignKeys = tab.cachedForeignKeys;
                            state_.currentPage = tab.cachedPage;
                            DataGrid().as<DataGridView>()->SetData(state_.currentData);
                            Structure().as<StructureView>()->SetTableContext(
                                tab.tableName, tab.schema, state_.connection.databaseType);
                            Structure().as<StructureView>()->SetData(
                                state_.currentColumns, state_.currentIndexes,
                                state_.currentForeignKeys, state_.currentConstraints);
                            UpdatePaginationUI();
                        }
                        else
                        {
                            state_.currentPage = 0;
                            if (connMgr_.isConnected())
                                LoadTableDataFromDB(tab.tableName);
                            else
                                LoadDemoTableData(tab.tableName);
                        }
                    }
                    else if (tab.type == DBModels::TabType::QueryEditor)
                    {
                        // Restore this tab's SQL into the shared
                        // QueryEditorView. Without this, all query
                        // tabs display the same text (whatever was
                        // last typed in any of them).
                        QueryEditor().as<QueryEditorView>()->SetSql(tab.cachedSql);
                    }
                    else if (tab.type == DBModels::TabType::ERDiagram)
                    {
                        isERDiagramTab = true;
                        LoadERDiagramIntoView(tab);
                    }
                    break;
                }
            }
            if (!isERDiagramTab)
            {
                showingData_ = true;
                DataToggle().IsChecked(true);
                StructureToggle().IsChecked(false);
            }
            SwitchContentView();
        };

        // Sidebar schema picker
        Sidebar().as<SidebarPanel>()->OnSchemaChanged = [this](const std::wstring& schema)
        {
            currentSchema_ = schema;
            state_.statusSchema = schema;
            if (!connMgr_.isConnected()) return;

            LoadSidebarFromDB();

            // Sync the open DataGrid tab to the new schema. Without this,
            // switching Redis logical DBs (db0 -> db5) updated the sidebar
            // key list but left the table view showing stale keys from the
            // previous DB. Also rewrite tab.schema so subsequent tab
            // switches fetch from the right DB.
            for (auto& tab : state_.tabs)
            {
                if (tab.id == state_.activeTabId &&
                    tab.type == DBModels::TabType::DataGrid)
                {
                    tab.schema = schema;
                    LoadTableDataFromDB(tab.tableName);
                    break;
                }
            }
        };

        // Sidebar history item click → open query tab with SQL
        Sidebar().as<SidebarPanel>()->OnHistoryItemClicked = [this](const std::wstring& sql)
        {
            OpenNewQueryTab();
            // Set the SQL in the query editor
            QueryEditor().as<QueryEditorView>()->SetSql(sql);
        };

        // Sidebar add table — show dialog to create new table
        Sidebar().as<SidebarPanel>()->OnAddTable = [this]()
        {
            if (!connMgr_.isConnected()) return;

            // Simple dialog: table name input
            muxc::ContentDialog dlg;
            dlg.Title(winrt::box_value(winrt::hstring(L"Create Table")));

            muxc::StackPanel content;
            content.Spacing(8.0);
            muxc::TextBox nameBox;
            nameBox.PlaceholderText(L"Table name");
            nameBox.Width(300.0);
            content.Children().Append(nameBox);

            muxc::TextBox columnsBox;
            if (state_.connection.databaseType == DBModels::DatabaseType::MSSQLServer)
                columnsBox.PlaceholderText(L"Columns (e.g.: id INT IDENTITY(1,1) PRIMARY KEY, name NVARCHAR(255) NOT NULL)");
            else
                columnsBox.PlaceholderText(L"Columns (e.g.: id uuid PRIMARY KEY DEFAULT gen_random_uuid(), name text NOT NULL)");
            columnsBox.AcceptsReturn(true);
            columnsBox.TextWrapping(mux::TextWrapping::Wrap);
            columnsBox.Width(300.0);
            columnsBox.Height(120.0);
            columnsBox.FontFamily(mux::Media::FontFamily(L"Cascadia Code,Consolas,monospace"));
            columnsBox.FontSize(12.0);
            content.Children().Append(columnsBox);
            dlg.Content(content);

            dlg.PrimaryButtonText(L"Create");
            dlg.CloseButtonText(L"Cancel");
            dlg.XamlRoot(this->XamlRoot());

            auto op = dlg.ShowAsync();
            op.Completed([this, nameBox, columnsBox](auto&& asyncOp, auto&&)
            {
                if (asyncOp.GetResults() != muxc::ContentDialogResult::Primary) return;
                this->DispatcherQueue().TryEnqueue([this, nameBox, columnsBox]()
                {
                    std::wstring tblName(nameBox.Text());
                    std::wstring cols(columnsBox.Text());
                    if (tblName.empty()) return;

                    // Build CREATE TABLE with dialect-aware quoting and defaults
                    auto dbType = state_.connection.databaseType;
                    std::wstring sql;
                    if (dbType == DBModels::DatabaseType::MSSQLServer)
                    {
                        sql = L"CREATE TABLE [" + currentSchema_ + L"].[" + tblName + L"] (";
                        if (cols.empty())
                            sql += L"id INT IDENTITY(1,1) PRIMARY KEY";
                        else
                            sql += cols;
                    }
                    else if (dbType == DBModels::DatabaseType::MySQL)
                    {
                        sql = L"CREATE TABLE `" + currentSchema_ + L"`.`" + tblName + L"` (";
                        if (cols.empty())
                            sql += L"id BIGINT AUTO_INCREMENT PRIMARY KEY";
                        else
                            sql += cols;
                    }
                    else if (dbType == DBModels::DatabaseType::SQLite)
                    {
                        sql = L"CREATE TABLE \"" + tblName + L"\" (";
                        if (cols.empty())
                            sql += L"id INTEGER PRIMARY KEY AUTOINCREMENT";
                        else
                            sql += cols;
                    }
                    else
                    {
                        // PostgreSQL default
                        sql = L"CREATE TABLE \"" + currentSchema_ + L"\".\"" + tblName + L"\" (";
                        if (cols.empty())
                            sql += L"id uuid PRIMARY KEY DEFAULT gen_random_uuid()";
                        else
                            sql += cols;
                    }
                    sql += L")";

                    auto adapter = connMgr_.getActiveAdapter();
                    if (!adapter) return;
                    try
                    {
                        auto result = adapter->execute(sql);
                        LogQuery(result);
                        if (result.success)
                            LoadSidebarFromDB(); // refresh
                    }
                    catch (...) {}
                });
            });
        };

        // Sidebar delete table — confirm then DROP
        Sidebar().as<SidebarPanel>()->OnDeleteTable = [this](const std::wstring& tableName, const std::wstring& schema)
        {
            if (!connMgr_.isConnected() || tableName.empty()) return;

            muxc::ContentDialog dlg;
            dlg.Title(winrt::box_value(winrt::hstring(L"Drop Table")));
            dlg.Content(winrt::box_value(winrt::hstring(
                L"Are you sure you want to drop \"" + schema + L"\".\"" + tableName + L"\"?\n\nThis action cannot be undone.")));
            dlg.PrimaryButtonText(L"Drop");
            dlg.CloseButtonText(L"Cancel");
            dlg.DefaultButton(muxc::ContentDialogButton::Close);
            dlg.XamlRoot(this->XamlRoot());

            auto op = dlg.ShowAsync();
            op.Completed([this, tableName, schema](auto&& asyncOp, auto&&)
            {
                if (asyncOp.GetResults() != muxc::ContentDialogResult::Primary) return;
                this->DispatcherQueue().TryEnqueue([this, tableName, schema]()
                {
                    auto adapter = connMgr_.getActiveAdapter();
                    if (!adapter) return;
                    try
                    {
                        // Quote identifiers per dialect and only prepend
                        // the schema when the adapter supports schemas
                        // (PostgreSQL does, MySQL databases sort of do via
                        // `schema`.`table`, SQLite has none).
                        auto dbType = state_.connection.databaseType;
                        std::wstring qt, qs;
                        if (dbType == DBModels::DatabaseType::MySQL)
                        {
                            qt = L"`" + tableName + L"`";
                            if (!schema.empty()) qs = L"`" + schema + L"`.";
                        }
                        else if (dbType == DBModels::DatabaseType::SQLite)
                        {
                            qt = L"\"" + tableName + L"\"";
                            // SQLite has no schemas
                        }
                        else // PostgreSQL and anything else SQL-ish
                        {
                            qt = L"\"" + tableName + L"\"";
                            if (!schema.empty()) qs = L"\"" + schema + L"\".";
                        }

                        std::wstring sql = L"DROP TABLE " + qs + qt;
                        auto result = adapter->execute(sql);
                        LogQuery(result);
                        if (result.success)
                        {
                            // Close any open tab pointing at the dropped
                            // table so the UI stops showing stale rows.
                            //
                            // Double-erase trap: TabBarControl::CloseTab
                            // synchronously fires OnTabClosed, which in turn
                            // runs state_.tabs.erase(remove_if ...) in this
                            // file. If we call CloseTab while iterating
                            // state_.tabs here, the container is erased
                            // from underneath the iterator (vector assert
                            // "erase iterator outside range"). Snapshot the
                            // matching ids first, then drive CloseTab off
                            // the snapshot.
                            std::vector<std::wstring> idsToClose;
                            for (const auto& t : state_.tabs)
                            {
                                if (t.type == DBModels::TabType::DataGrid &&
                                    t.tableName == tableName &&
                                    t.schema == schema)
                                {
                                    idsToClose.push_back(t.id);
                                }
                            }
                            for (const auto& id : idsToClose)
                            {
                                TabBar().as<TabBarControl>()->CloseTab(
                                    winrt::hstring(id));
                            }
                            LoadSidebarFromDB(); // refresh
                            SwitchContentView();
                        }
                        else
                        {
                            muxc::ContentDialog errDlg;
                            errDlg.Title(winrt::box_value(winrt::hstring(L"Drop Failed")));
                            errDlg.Content(winrt::box_value(winrt::hstring(result.error)));
                            errDlg.CloseButtonText(L"OK");
                            errDlg.XamlRoot(this->XamlRoot());
                            errDlg.ShowAsync();
                        }
                    }
                    catch (...) {}
                });
            });
        };

        // Sidebar export table — right-click → Export → CSV/JSON/SQL
        Sidebar().as<SidebarPanel>()->OnExportTable = [this](
            const std::wstring& tableName, const std::wstring& schema, const std::wstring& format)
        {
            // Launch async export (fire-and-forget coroutine)
            ExportTableAsync(tableName, schema, format);
        };

        // Sidebar import — right-click table → Import (target = that table)
        Sidebar().as<SidebarPanel>()->OnImportTable = [this](
            const std::wstring& tableName, const std::wstring& schema)
        {
            ImportDataAsync(tableName, schema);
        };

        // Sidebar ER diagram — right-click Database/Schema group → Show ER Diagram
        Sidebar().as<SidebarPanel>()->OnShowERDiagram = [this](const std::wstring& schema)
        {
            ShowERDiagramAsync(schema);
        };

        // Redis-only: Refresh keys listing (sidebar context menu → Refresh)
        Sidebar().as<SidebarPanel>()->OnRefreshSidebar = [this]()
        {
            if (connMgr_.isConnected()) LoadSidebarFromDB();
        };

        // Redis-only: Flush DB — show confirmation, then issue FLUSHDB via execute()
        Sidebar().as<SidebarPanel>()->OnFlushRedisDb = [this]()
        {
            FlushRedisDbAsync();
        };

        // Redis-only: Browse Keys — show pattern input dialog, then open Keys table
        Sidebar().as<SidebarPanel>()->OnBrowseRedisKeys = [this]()
        {
            BrowseRedisKeysAsync();
        };

        // Filter bar callbacks
        FilterBarControl().as<FilterBar>()->OnApplyFilter = [this](const winrt::Gridex::implementation::FilterCondition& cond)
        {
            ApplyFilter(cond.column, cond.op, cond.value);
        };
        FilterBarControl().as<FilterBar>()->OnClearFilter = [this]()
        {
            ReloadCurrentTable();
        };

        // Toolbar buttons (all wired in code-behind)
        HomeBtn().Click([this](winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
        {
            // Walk visual tree to find hosting Frame, then navigate to HomePage
            muxc::Frame frame{ nullptr };
            auto fe = this->try_as<mux::FrameworkElement>();
            while (fe)
            {
                if (auto f = fe.try_as<muxc::Frame>()) { frame = f; break; }
                fe = fe.Parent().try_as<mux::FrameworkElement>();
            }
            if (!frame) frame = this->Frame();
            if (!frame) return;

            winrt::Windows::UI::Xaml::Interop::TypeName pageType;
            pageType.Name = L"Gridex.HomePage";
            pageType.Kind = winrt::Windows::UI::Xaml::Interop::TypeKind::Metadata;
            frame.Navigate(pageType);
        });

        SidebarToggleBtn().Click([this](winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
        { ToggleSidebar_Click({}, {}); });

        DetailsToggleBtn().Click([this](winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
        { ToggleDetails_Click({}, {}); });

        DumpBtn().Click([this](winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
        { DumpDatabaseAsync(); });

        RestoreBtn().Click([this](winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
        { RestoreDatabaseAsync(); });

        ERDiagramBtn().Click([this](winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
        { ShowERDiagramAsync(currentSchema_); });

        // ER view action buttons (act on the active ERDiagram tab)
        ERCopyD2Btn().Click([this](auto&&, auto&&)
        {
            for (const auto& t : state_.tabs)
            {
                if (t.id == state_.activeTabId && t.type == DBModels::TabType::ERDiagram)
                {
                    winrt::Windows::ApplicationModel::DataTransfer::DataPackage pkg;
                    pkg.SetText(winrt::hstring(t.erD2Text));
                    winrt::Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(pkg);
                    break;
                }
            }
        });

        ERSaveSvgBtn().Click([this](auto&&, auto&&) -> winrt::fire_and_forget
        {
            std::wstring svgPath, schema;
            for (const auto& t : state_.tabs)
            {
                if (t.id == state_.activeTabId && t.type == DBModels::TabType::ERDiagram)
                {
                    svgPath = t.erSvgPath;
                    schema = t.schema;
                    break;
                }
            }
            if (svgPath.empty()) co_return;
            try
            {
                winrt::Microsoft::UI::WindowId windowId{
                    reinterpret_cast<uint64_t>(
                        winrt::Gridex::implementation::App::MainHwnd) };
                winrt::Microsoft::Windows::Storage::Pickers::FileSavePicker picker(windowId);
                picker.SuggestedFileName(winrt::hstring(schema + L"_er"));
                auto exts = winrt::single_threaded_vector<winrt::hstring>();
                exts.Append(L".svg");
                picker.FileTypeChoices().Insert(L"SVG", exts);
                auto pickedFile = co_await picker.PickSaveFileAsync();
                if (!pickedFile) co_return;
                CopyFileW(svgPath.c_str(),
                    std::wstring(pickedFile.Path()).c_str(), FALSE);
            }
            catch (...) {}
        });

        // Floating ER controls (overlay top-right of WebView)
        auto runScript = [this](winrt::hstring js)
        {
            try { ERDiagramWebView().ExecuteScriptAsync(js); }
            catch (...) {}
        };
        ERZoomInBtn().Click([runScript](auto&&, auto&&)
            { runScript(L"window.__er&&window.__er.zoomIn()"); });
        ERZoomOutBtn().Click([runScript](auto&&, auto&&)
            { runScript(L"window.__er&&window.__er.zoomOut()"); });
        ERZoomLabelBtn().Click([runScript](auto&&, auto&&)
            { runScript(L"window.__er&&window.__er.resetZoom()"); });
        ERResetLayoutBtn().Click([runScript](auto&&, auto&&)
            { runScript(L"window.__er&&window.__er.resetLayout()"); });
        ERFullscreenBtn().Click([this](auto&&, auto&&)
            { ToggleERFullscreen(); });

        ERSaveD2Btn().Click([this](auto&&, auto&&) -> winrt::fire_and_forget
        {
            std::wstring d2Text, schema;
            for (const auto& t : state_.tabs)
            {
                if (t.id == state_.activeTabId && t.type == DBModels::TabType::ERDiagram)
                {
                    d2Text = t.erD2Text;
                    schema = t.schema;
                    break;
                }
            }
            if (d2Text.empty()) co_return;
            try
            {
                winrt::Microsoft::UI::WindowId windowId{
                    reinterpret_cast<uint64_t>(
                        winrt::Gridex::implementation::App::MainHwnd) };
                winrt::Microsoft::Windows::Storage::Pickers::FileSavePicker picker(windowId);
                picker.SuggestedFileName(winrt::hstring(schema + L"_er"));
                auto exts = winrt::single_threaded_vector<winrt::hstring>();
                exts.Append(L".d2");
                picker.FileTypeChoices().Insert(L"D2 Source", exts);
                auto pickedFile = co_await picker.PickSaveFileAsync();
                if (!pickedFile) co_return;

                std::wstring path(pickedFile.Path());
                std::ofstream f(path, std::ios::binary);
                if (!f.is_open()) co_return;
                f.write("\xEF\xBB\xBF", 3);
                int sz = WideCharToMultiByte(CP_UTF8, 0, d2Text.c_str(),
                    static_cast<int>(d2Text.size()), nullptr, 0, nullptr, nullptr);
                std::string utf8(sz, '\0');
                WideCharToMultiByte(CP_UTF8, 0, d2Text.c_str(),
                    static_cast<int>(d2Text.size()), &utf8[0], sz, nullptr, nullptr);
                f.write(utf8.c_str(), utf8.size());
            }
            catch (...) {}
        });

        SettingsBtn().Click([this](winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
        {
            // Walk up visual tree to find hosting Frame (Page.Frame can be null in WinUI 3)
            muxc::Frame frame{ nullptr };
            auto fe = this->try_as<mux::FrameworkElement>();
            while (fe)
            {
                if (auto f = fe.try_as<muxc::Frame>()) { frame = f; break; }
                fe = fe.Parent().try_as<mux::FrameworkElement>();
            }
            if (!frame) frame = this->Frame();
            if (!frame)
            {
                muxc::ContentDialog err;
                err.Title(winrt::box_value(winrt::hstring(L"Navigation Error")));
                err.Content(winrt::box_value(winrt::hstring(
                    L"Could not locate hosting Frame.\nUse Ctrl+Shift+P instead.")));
                err.CloseButtonText(L"OK");
                err.XamlRoot(this->XamlRoot());
                err.ShowAsync();
                return;
            }

            // Remember this page so Settings' Back button returns here
            auto s = DBModels::AppSettings::Load();
            s.lastPageBeforeSettings = L"Gridex.WorkspacePage";
            s.Save();

            winrt::Windows::UI::Xaml::Interop::TypeName pageType;
            pageType.Name = L"Gridex.SettingsPage";
            pageType.Kind = winrt::Windows::UI::Xaml::Interop::TypeKind::Metadata;
            frame.Navigate(pageType);
        });

        // Bottom bar buttons
        DataToggle().Click([this](winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
        { DataToggle_Click({}, {}); });

        StructureToggle().Click([this](winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
        { StructureToggle_Click({}, {}); });

        AddRowBtn().Click([this](winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
        { AddNewRow(); });

        PrevPageBtn().Click([this](winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
        { PrevPage(); });

        NextPageBtn().Click([this](winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
        { NextPage(); });

        DiscardBtn().Click([this](winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
        { DiscardChanges(); });

        CommitBottomBtn().Click([this](winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
        { CommitChanges(); });

        FilterToggleBtn().Click([this](winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
        { ToggleFilter_Click({}, {}); });

        ClearLogBtn().Click([this](winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
        {
            connMgr_.clearQueryLog();
            QueryLogEntries().Children().Clear();
        });

        WireResizeGrips();
    }

    // ── Drag-to-resize: left sidebar / right sidebar / query log ─────
    //
    // 3 transparent grip elements (LeftSidebarResizer / RightSidebarResizer
    // / QueryLogResizer) sit between panels in the XAML. Each one captures
    // the pointer on press, tracks the delta on move, and updates the
    // adjacent ColumnDefinition.Width or panel.Height live.
    //
    // Cursor uses UIElement.ProtectedCursor — the officially supported
    // WinUI 3 cursor API. cppwinrt strips ProtectedCursor from the public
    // projection of UIElement ("protected" in metadata, intended for
    // control authors), but the underlying ABI interface
    // winrt::Microsoft::UI::Xaml::IUIElementProtected IS publicly
    // declared and every UIElement implements it. Casting via
    // .as<IUIElementProtected>() gives us a consume_ wrapper that exposes
    // ProtectedCursor as a regular method call.
    //
    // Win32 SetCursor() in PointerMoved does NOT work reliably here —
    // the WinUI input pipeline issues its own SetCursor on every
    // WM_SETCURSOR message, racing with the user handler.

    void WorkspacePage::WireResizeGrips()
    {
        using winrt::Microsoft::UI::Input::InputSystemCursor;
        using winrt::Microsoft::UI::Input::InputSystemCursorShape;
        using winrt::Microsoft::UI::Xaml::IUIElementProtected;

        // Cursors are shared across all grip instances; created once on
        // first entry and reused — InputSystemCursor objects are cheap COM
        // refs, safe to statically cache.
        static auto sizeWECursor = InputSystemCursor::Create(
            InputSystemCursorShape::SizeWestEast);
        static auto sizeNSCursor = InputSystemCursor::Create(
            InputSystemCursorShape::SizeNorthSouth);

        // Set the persistent cursor on each grip via the ABI interface.
        // Safe to call here because WireResizeGrips runs from WireAllButtons
        // which is invoked after the Loaded event — the visual tree is
        // live by this point (ProtectedCursor throws otherwise).
        LeftSidebarResizer().as<IUIElementProtected>().ProtectedCursor(sizeWECursor);
        RightSidebarResizer().as<IUIElementProtected>().ProtectedCursor(sizeWECursor);
        QueryLogResizer().as<IUIElementProtected>().ProtectedCursor(sizeNSCursor);

        // ── Left sidebar resizer ────────────────────────
        LeftSidebarResizer().PointerPressed(
            [this](winrt::Windows::Foundation::IInspectable const& sender,
                   mux::Input::PointerRoutedEventArgs const& e)
            {
                leftResizing_ = true;
                auto pt = e.GetCurrentPoint(this->try_as<mux::UIElement>());
                leftResizeStartX_     = pt.Position().X;
                leftResizeStartWidth_ = SidebarColumn().ActualWidth();
                if (auto el = sender.try_as<mux::UIElement>())
                    el.CapturePointer(e.Pointer());
                e.Handled(true);
            });
        LeftSidebarResizer().PointerMoved(
            [this](winrt::Windows::Foundation::IInspectable const&,
                   mux::Input::PointerRoutedEventArgs const& e)
            {
                if (!leftResizing_) return;
                auto pt = e.GetCurrentPoint(this->try_as<mux::UIElement>());
                double delta = pt.Position().X - leftResizeStartX_;
                double w = leftResizeStartWidth_ + delta;
                if (w < 160.0) w = 160.0;
                if (w > 600.0) w = 600.0;
                SidebarColumn().Width(mux::GridLengthHelper::FromPixels(w));
                e.Handled(true);
            });
        LeftSidebarResizer().PointerReleased(
            [this](winrt::Windows::Foundation::IInspectable const& sender,
                   mux::Input::PointerRoutedEventArgs const& e)
            {
                if (leftResizing_)
                {
                    leftResizing_ = false;
                    if (auto el = sender.try_as<mux::UIElement>())
                        el.ReleasePointerCapture(e.Pointer());
                    // Sync toggle "remembered width" so collapse/expand
                    // via SidebarToggleBtn restores the user's choice.
                    prevSidebarWidth_ = SidebarColumn().ActualWidth();
                }
                e.Handled(true);
            });

        // ── Right sidebar (Details) resizer ─────────────
        RightSidebarResizer().PointerPressed(
            [this](winrt::Windows::Foundation::IInspectable const& sender,
                   mux::Input::PointerRoutedEventArgs const& e)
            {
                rightResizing_ = true;
                auto pt = e.GetCurrentPoint(this->try_as<mux::UIElement>());
                rightResizeStartX_     = pt.Position().X;
                rightResizeStartWidth_ = DetailsColumn().ActualWidth();
                if (auto el = sender.try_as<mux::UIElement>())
                    el.CapturePointer(e.Pointer());
                e.Handled(true);
            });
        RightSidebarResizer().PointerMoved(
            [this](winrt::Windows::Foundation::IInspectable const&,
                   mux::Input::PointerRoutedEventArgs const& e)
            {
                if (!rightResizing_) return;
                auto pt = e.GetCurrentPoint(this->try_as<mux::UIElement>());
                // Dragging RIGHT shrinks the right panel; LEFT grows it.
                double delta = pt.Position().X - rightResizeStartX_;
                double w = rightResizeStartWidth_ - delta;
                if (w < 180.0) w = 180.0;
                if (w > 700.0) w = 700.0;
                DetailsColumn().Width(mux::GridLengthHelper::FromPixels(w));
                e.Handled(true);
            });
        RightSidebarResizer().PointerReleased(
            [this](winrt::Windows::Foundation::IInspectable const& sender,
                   mux::Input::PointerRoutedEventArgs const& e)
            {
                if (rightResizing_)
                {
                    rightResizing_ = false;
                    if (auto el = sender.try_as<mux::UIElement>())
                        el.ReleasePointerCapture(e.Pointer());
                    prevDetailsWidth_ = DetailsColumn().ActualWidth();
                }
                e.Handled(true);
            });

        // ── Query log top resizer (vertical drag) ───────
        QueryLogResizer().PointerPressed(
            [this](winrt::Windows::Foundation::IInspectable const& sender,
                   mux::Input::PointerRoutedEventArgs const& e)
            {
                logResizing_ = true;
                auto pt = e.GetCurrentPoint(this->try_as<mux::UIElement>());
                logResizeStartY_      = pt.Position().Y;
                logResizeStartHeight_ = QueryLogPanel().ActualHeight();
                if (auto el = sender.try_as<mux::UIElement>())
                    el.CapturePointer(e.Pointer());
                e.Handled(true);
            });
        QueryLogResizer().PointerMoved(
            [this](winrt::Windows::Foundation::IInspectable const&,
                   mux::Input::PointerRoutedEventArgs const& e)
            {
                if (!logResizing_) return;
                auto pt = e.GetCurrentPoint(this->try_as<mux::UIElement>());
                // Dragging UP grows the panel (delta is negative).
                double delta = pt.Position().Y - logResizeStartY_;
                double h = logResizeStartHeight_ - delta;
                if (h < 60.0)  h = 60.0;
                if (h > 600.0) h = 600.0;
                QueryLogPanel().Height(h);
                e.Handled(true);
            });
        QueryLogResizer().PointerReleased(
            [this](winrt::Windows::Foundation::IInspectable const& sender,
                   mux::Input::PointerRoutedEventArgs const& e)
            {
                if (logResizing_)
                {
                    logResizing_ = false;
                    if (auto el = sender.try_as<mux::UIElement>())
                        el.ReleasePointerCapture(e.Pointer());
                }
                e.Handled(true);
            });
    }

    // ── Connection Setup ────────────────────────────────

    void WorkspacePage::SetConnection(const DBModels::ConnectionConfig& config, const std::wstring& password)
    {
        state_.connection = config;
        pendingPassword_ = password;
        hasConnection_ = true;

        state_.statusConnection = config.username + L"@" + config.host +
            L":" + std::to_wstring(config.port);
        state_.statusSchema = currentSchema_;

        // Remember this connection at process scope so a freshly
        // re-constructed WorkspacePage (e.g. after returning from
        // Settings) can auto-restore it instead of showing demo data.
        sLastConfig_ = config;
        sLastPassword_ = password;
        sHasLastConnection_ = true;

        if (isLoaded_)
            InitializeConnection();
    }

    void WorkspacePage::InitializeConnection()
    {
        auto& config = state_.connection;

        // Redis uses integer logical DBs (0..15) with a "db<N>" display
        // label. If the user pasted a non-numeric name like "app_db" into
        // the database field (common for people copying a SQL connection
        // config), Redis falls back to SELECT 0 -- but the stale label
        // stays visible in the top-left DbPickerText. Normalize before
        // anything else so the rest of InitializeConnection sees a
        // consistent "db<N>" identifier.
        if (config.databaseType == DBModels::DatabaseType::Redis)
        {
            int dbIdx = 0;
            if (!config.database.empty())
            {
                // Accept both "5" and "db5" forms; default to 0 otherwise.
                std::wstring d = config.database;
                if (d.size() > 2 && d[0] == L'd' && d[1] == L'b')
                    d = d.substr(2);
                try { dbIdx = std::stoi(d); } catch (...) { dbIdx = 0; }
            }
            std::wstring label = L"db" + std::to_wstring(dbIdx);
            state_.connection.database = label;
            currentSchema_ = label;
            state_.statusSchema = label;
        }
        // MongoDB: don't pre-set config.database here — the adapter
        // will extract it from the URI during connect(). Setting it
        // to "test" here pollutes config.database which the adapter
        // reads first (priority 1), bypassing the URI parse. The
        // post-connect sync block below reads adapter->currentDatabase()
        // and updates currentSchema_ + config.database correctly.
        else if (config.databaseType == DBModels::DatabaseType::MongoDB)
        {
            // Only set if user explicitly typed a database name in the form
            if (!config.database.empty())
            {
                currentSchema_ = config.database;
                state_.statusSchema = config.database;
            }
        }
        // SQL Server: default schema is "dbo", not "public"
        else if (config.databaseType == DBModels::DatabaseType::MSSQLServer)
        {
            currentSchema_ = L"dbo";
            state_.statusSchema = L"dbo";
        }
        // MySQL: schema = database name (MySQL has no separate schema concept)
        else if (config.databaseType == DBModels::DatabaseType::MySQL)
        {
            if (!config.database.empty())
            {
                currentSchema_ = config.database;
                state_.statusSchema = config.database;
            }
        }

        UpdateBreadcrumb();

        if (!config.database.empty())
            DbPickerText().Text(winrt::hstring(config.database));

        try
        {
            connMgr_.connect(config, pendingPassword_);

            // MongoDB only: re-read the actual database name from the
            // adapter. The database may have been specified inside the
            // URI while config.database was left empty. Other DB types
            // (PostgreSQL, MySQL, SQLite, Redis) use currentSchema_ for
            // schema names (e.g. "public"), NOT database names — syncing
            // here would overwrite "public" with the database name and
            // break listTables.
            if (config.databaseType == DBModels::DatabaseType::MongoDB)
            {
                auto adapter = connMgr_.getActiveAdapter();
                if (adapter)
                {
                    auto actualDb = adapter->currentDatabase();
                    if (!actualDb.empty())
                    {
                        currentSchema_ = actualDb;
                        state_.statusSchema = actualDb;
                        if (config.database.empty())
                            state_.connection.database = actualDb;
                        DbPickerText().Text(winrt::hstring(actualDb));
                    }
                }
            }

            // Tell sidebar the active DB type so context menus render correctly
            // (Redis gets Browse Keys / Flush DB; SQL gets Open / Export / Import)
            Sidebar().as<SidebarPanel>()->SetDatabaseType(config.databaseType);

            // Redis does not support SQL-style row updates -- inline edits
            // would silently overwrite hash / list / set values via SET
            // and corrupt the data. Flip the grid + details pane into
            // read-only mode for Redis connections so users can inspect
            // values freely but cannot accidentally destroy them. SQL
            // adapters stay editable as before.
            const bool isRedis =
                config.databaseType == DBModels::DatabaseType::Redis;
            DataGrid().as<DataGridView>()->SetReadOnly(isRedis);
            Details().as<DetailsPanel>()->SetReadOnly(isRedis);

            LoadSidebarFromDB();
            LoadDatabasePicker();

            auto adapter = connMgr_.getActiveAdapter();
            if (adapter)
            {
                auto ver = adapter->serverVersion();
                state_.statusConnection = config.username + L"@" + config.host +
                    L":" + std::to_wstring(config.port) + L"  (" +
                    DBModels::DatabaseTypeDisplayName(config.databaseType) + L" " + ver + L")";

                // Load schemas into sidebar picker
                try
                {
                    auto schemas = adapter->listSchemas();
                    Sidebar().as<SidebarPanel>()->SetSchemas(schemas);
                }
                catch (...) {}
            }

            StatusBar().as<StatusBarControl>()->SetStatus(
                state_.statusConnection, state_.statusSchema, 0, 0.0, 0.0);
        }
        catch (const std::exception& ex)
        {
            std::string what = ex.what();
            int sz = MultiByteToWideChar(CP_UTF8, 0, what.c_str(), (int)what.size(), nullptr, 0);
            std::wstring errMsg(sz, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, what.c_str(), (int)what.size(), &errMsg[0], sz);
            muxc::ContentDialog errDialog;
            errDialog.Title(winrt::box_value(winrt::hstring(L"Connection Failed")));
            errDialog.Content(winrt::box_value(winrt::hstring(
                L"Could not connect to " + config.name + L":\n\n" + errMsg)));
            errDialog.CloseButtonText(L"OK");
            errDialog.XamlRoot(this->XamlRoot());
            errDialog.ShowAsync();

            LoadDemoSidebarData();
            StatusBar().as<StatusBarControl>()->SetStatus(
                L"Disconnected", L"--", 0, 0.0, 0.0);
        }
    }

    // ── Sidebar Data Loading ────────────────────────────

    void WorkspacePage::LoadSidebarFromDB()
    {
        auto adapter = connMgr_.getActiveAdapter();
        if (!adapter) { LoadDemoSidebarData(); return; }

        try
        {
            // Query each independently — one failure shouldn't block others
            std::vector<std::wstring> funcs;
            std::vector<DBModels::TableInfo> tables;
            std::vector<DBModels::TableInfo> views;
            try { funcs = adapter->listFunctions(currentSchema_); } catch (...) {}
            try { tables = adapter->listTables(currentSchema_); } catch (...) {}
            try { views = adapter->listViews(currentSchema_); } catch (...) {}

            // Build sidebar groups — always show all 3 groups (like macOS version)
            state_.sidebarItems.clear();

            // Functions group (always shown). SQL Server includes stored
            // procedures here too, so label it "Routines" for clarity.
            {
                DBModels::SidebarItem g;
                g.id = L"functions";
                g.title = (state_.connection.databaseType == DBModels::DatabaseType::MSSQLServer)
                    ? L"Routines" : L"Functions";
                g.type = DBModels::SidebarItemType::Group;
                g.isExpanded = !funcs.empty();  // auto-expand if there are items
                for (auto& name : funcs)
                {
                    DBModels::SidebarItem item;
                    item.id = std::wstring(L"fn_") + name; item.title = name;
                    item.type = DBModels::SidebarItemType::Function; item.schema = currentSchema_;
                    g.children.push_back(item);
                }
                g.count = static_cast<int>(funcs.size());
                state_.sidebarItems.push_back(g);
            }

            // Tables group (always shown, expanded by default)
            {
                DBModels::SidebarItem g;
                g.id = L"tables"; g.title = L"Tables";
                g.type = DBModels::SidebarItemType::Group; g.isExpanded = true;
                for (auto& tbl : tables)
                {
                    DBModels::SidebarItem item;
                    item.id = tbl.name; item.title = tbl.name;
                    item.type = DBModels::SidebarItemType::Table; item.schema = currentSchema_;
                    g.children.push_back(item);
                }
                g.count = static_cast<int>(tables.size());
                state_.sidebarItems.push_back(g);
            }

            // Views group (always shown)
            {
                DBModels::SidebarItem g;
                g.id = L"views"; g.title = L"Views";
                g.type = DBModels::SidebarItemType::Group; g.isExpanded = false;
                for (auto& v : views)
                {
                    DBModels::SidebarItem item;
                    item.id = v.name; item.title = v.name;
                    item.type = DBModels::SidebarItemType::View; item.schema = currentSchema_;
                    g.children.push_back(item);
                }
                g.count = static_cast<int>(views.size());
                state_.sidebarItems.push_back(g);
            }

            Sidebar().as<SidebarPanel>()->SetItems(state_.sidebarItems);
            Sidebar().as<SidebarPanel>()->OnItemSelected = [this](const std::wstring& name, const std::wstring& schema)
            {
                OnTableSelected(name, schema);
            };

            // Feed table/function names to query editor autocomplete
            {
                std::vector<std::wstring> tableNames, funcNames;
                for (auto& tbl : tables) tableNames.push_back(tbl.name);
                for (auto& v : views) tableNames.push_back(v.name);
                funcNames = funcs;
                // Columns will be added per-table when table data loads
                QueryEditor().as<QueryEditorView>()->SetSchemaCompletions(
                    tableNames, {}, funcNames);
            }
        }
        catch (const std::exception&)
        {
            LoadDemoSidebarData();
        }
    }

    void WorkspacePage::LoadTableDataFromDB(const std::wstring& tableName)
    {
        // Show the loader on the UI thread, then push the actual DB work
        // onto a background std::thread. Previously the "heavy" work ran
        // inside a Low-priority TryEnqueue lambda which is STILL the UI
        // thread — the ProgressRing had no frames to animate and the
        // overlay often never even reached the screen before fetchRows
        // blocked for seconds. Moving fetchRows / describeTable /
        // listIndexes / listForeignKeys off the UI thread lets the
        // loader actually render and keeps the window responsive.
        DataLoadingOverlay().Visibility(mux::Visibility::Visible);

        auto adapter = connMgr_.getActiveAdapter();
        if (!adapter)
        {
            LoadDemoTableData(tableName);
            DataLoadingOverlay().Visibility(mux::Visibility::Collapsed);
            return;
        }

        // Snapshot the inputs on the UI thread — `state_` and `currentSchema_`
        // must not be read from the background thread.
        const std::wstring schema     = currentSchema_;
        const int          pageSize   = state_.pageSize;
        const int          offset     = state_.currentPage * state_.pageSize;
        const uint64_t     requestId  = ++loadTableRequestCounter_;

        auto dispatcher = this->DispatcherQueue();
        auto weakSelf   = get_weak();

        // Results package marshaled from worker → UI thread. shared_ptr
        // so the std::function copy inside TryEnqueue stays cheap.
        struct LoadResult
        {
            bool                                    ok = false;
            std::wstring                            error;
            DBModels::QueryResult                   data;
            std::vector<DBModels::ColumnInfo>       cols;
            std::vector<DBModels::IndexInfo>        idx;
            std::vector<DBModels::ForeignKeyInfo>   fks;
        };

        std::thread([weakSelf, dispatcher, adapter, tableName, schema,
                     pageSize, offset, requestId]() mutable
        {
            auto result = std::make_shared<LoadResult>();
            try
            {
                result->data = adapter->fetchRows(tableName, schema, pageSize, offset);
                result->cols = adapter->describeTable(tableName, schema);
                result->idx  = adapter->listIndexes(tableName, schema);
                result->fks  = adapter->listForeignKeys(tableName, schema);
                result->ok   = true;
            }
            catch (const std::exception& e)
            {
                result->ok = false;
                // std::exception::what() is narrow; widen for the log /
                // future error UI. ASCII-only paths are the common case.
                const std::string w = e.what();
                result->error.assign(w.begin(), w.end());
            }
            catch (...)
            {
                result->ok = false;
                result->error = L"Unknown error loading table";
            }

            // Marshal back to UI thread.
            dispatcher.TryEnqueue([weakSelf, tableName, requestId, result]() mutable
            {
                auto self = weakSelf.get();
                if (!self) return;

                // Drop stale results: user clicked another table while
                // this query was in flight. The latest request always
                // wins; earlier ones just vanish silently.
                if (requestId != self->loadTableRequestCounter_) return;

                auto hideOverlay = [self]()
                {
                    self->DataLoadingOverlay().Visibility(mux::Visibility::Collapsed);
                };

                if (!result->ok)
                {
                    self->LoadDemoTableData(tableName);
                    hideOverlay();
                    return;
                }

                self->state_.currentData        = std::move(result->data);
                self->state_.currentColumns     = std::move(result->cols);
                self->state_.currentIndexes     = std::move(result->idx);
                self->state_.currentForeignKeys = std::move(result->fks);
                self->state_.currentConstraints.clear();

                self->state_.statusRowCount    = self->state_.currentData.totalRows;
                self->state_.statusQueryTimeMs = self->state_.currentData.executionTimeMs;

                // Measure the UI build cost separately from the driver
                // call time already reported by the adapter. Split is
                // shown as "Exec · Render" in the status bar + query log
                // so wide-table slowness is attributed to the right
                // layer instead of lumped under a single number.
                auto renderStart = std::chrono::steady_clock::now();
                self->DataGrid().as<DataGridView>()->SetData(self->state_.currentData);
                self->DataGrid().as<DataGridView>()->SetColumnMetadata(
                    self->state_.currentColumns);
                auto renderEnd = std::chrono::steady_clock::now();
                self->state_.statusRenderTimeMs =
                    std::chrono::duration<double, std::milli>(renderEnd - renderStart).count();

                // Log after render so the entry carries both timings.
                self->LogQuery(self->state_.currentData, self->state_.statusRenderTimeMs);

                self->Structure().as<StructureView>()->SetTableContext(
                    tableName, self->currentSchema_, self->state_.connection.databaseType);
                self->Structure().as<StructureView>()->SetData(
                    self->state_.currentColumns, self->state_.currentIndexes,
                    self->state_.currentForeignKeys, self->state_.currentConstraints);
                self->StatusBar().as<StatusBarControl>()->SetStatus(
                    self->state_.statusConnection, self->state_.statusSchema,
                    self->state_.statusRowCount,
                    self->state_.statusQueryTimeMs,
                    self->state_.statusRenderTimeMs);
                self->UpdatePaginationUI();

                self->FilterBarControl().as<FilterBar>()->SetColumns(
                    self->state_.currentData.columnNames);

                // Update query editor autocomplete with column names from
                // the current table + sidebar-derived table/function lists.
                {
                    std::vector<std::wstring> tblNames, colNames, fnNames;
                    for (auto& group : self->state_.sidebarItems)
                        for (auto& item : group.children)
                        {
                            if (group.id == L"tables" || group.id == L"views")
                                tblNames.push_back(item.title);
                            else if (group.id == L"functions")
                                fnNames.push_back(item.title);
                        }
                    colNames = self->state_.currentData.columnNames;
                    self->QueryEditor().as<QueryEditorView>()->SetSchemaCompletions(
                        tblNames, colNames, fnNames);
                }

                self->SaveCurrentTabCache();
                hideOverlay();
            });
        }).detach();
    }

    // ── Content Navigation ──────────────────────────────

    void WorkspacePage::UpdateBreadcrumb()
    {
        auto& conn = state_.connection;
        ConnectionNameText().Text(winrt::hstring(conn.name));

        std::wstring info = DBModels::DatabaseTypeDisplayName(conn.databaseType);
        if (!conn.username.empty()) info += L"  :  " + conn.username;
        if (!conn.database.empty()) info += L"  :  " + conn.database;
        ConnectionInfoText().Text(winrt::hstring(info));

        if (conn.colorTag.has_value())
        {
            auto& tagInfo = DBModels::GetColorTagInfo(conn.colorTag.value());
            auto color = winrt::Windows::UI::ColorHelper::FromArgb(255, tagInfo.r, tagInfo.g, tagInfo.b);
            ConnectionDot().Fill(muxm::SolidColorBrush(color));
        }
        else
        {
            ConnectionDot().Fill(muxm::SolidColorBrush(
                winrt::Windows::UI::ColorHelper::FromArgb(255, 128, 128, 128)));
        }
    }

    void WorkspacePage::ToggleSidebar_Click(
        winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
    {
        sidebarVisible_ = !sidebarVisible_;
        SidebarColumn().Width(sidebarVisible_
            ? mux::GridLengthHelper::FromPixels(280)
            : mux::GridLengthHelper::FromPixels(0));
        Sidebar().Visibility(sidebarVisible_ ? mux::Visibility::Visible : mux::Visibility::Collapsed);
    }

    void WorkspacePage::ToggleDetails_Click(
        winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
    {
        detailsVisible_ = !detailsVisible_;
        DetailsColumn().Width(detailsVisible_
            ? mux::GridLengthHelper::FromPixels(260)
            : mux::GridLengthHelper::FromPixels(0));
        Details().Visibility(detailsVisible_ ? mux::Visibility::Visible : mux::Visibility::Collapsed);
    }

    void WorkspacePage::DataToggle_Click(
        winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
    {
        showingData_ = true;
        DataToggle().IsChecked(true);
        StructureToggle().IsChecked(false);
        SwitchContentView();
    }

    void WorkspacePage::StructureToggle_Click(
        winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
    {
        showingData_ = false;
        DataToggle().IsChecked(false);
        StructureToggle().IsChecked(true);
        SwitchContentView();
    }

    void WorkspacePage::SwitchContentView()
    {
        if (state_.tabs.empty())
        {
            EmptyContentState().Visibility(mux::Visibility::Visible);
            DataGrid().Visibility(mux::Visibility::Collapsed);
            Structure().Visibility(mux::Visibility::Collapsed);
            QueryEditor().Visibility(mux::Visibility::Collapsed);
            ERDiagramView().Visibility(mux::Visibility::Collapsed);
            return;
        }

        EmptyContentState().Visibility(mux::Visibility::Collapsed);

        DBModels::TabType activeType = DBModels::TabType::DataGrid;
        for (auto& tab : state_.tabs)
            if (tab.id == state_.activeTabId) { activeType = tab.type; break; }

        if (activeType == DBModels::TabType::ERDiagram)
        {
            DataGrid().Visibility(mux::Visibility::Collapsed);
            Structure().Visibility(mux::Visibility::Collapsed);
            QueryEditor().Visibility(mux::Visibility::Collapsed);
            ERDiagramView().Visibility(mux::Visibility::Visible);
        }
        else if (activeType == DBModels::TabType::QueryEditor)
        {
            DataGrid().Visibility(mux::Visibility::Collapsed);
            Structure().Visibility(mux::Visibility::Collapsed);
            QueryEditor().Visibility(mux::Visibility::Visible);
            ERDiagramView().Visibility(mux::Visibility::Collapsed);
        }
        else if (showingData_)
        {
            DataGrid().Visibility(mux::Visibility::Visible);
            Structure().Visibility(mux::Visibility::Collapsed);
            QueryEditor().Visibility(mux::Visibility::Collapsed);
            ERDiagramView().Visibility(mux::Visibility::Collapsed);
        }
        else
        {
            DataGrid().Visibility(mux::Visibility::Collapsed);
            Structure().Visibility(mux::Visibility::Visible);
            QueryEditor().Visibility(mux::Visibility::Collapsed);
            ERDiagramView().Visibility(mux::Visibility::Collapsed);
        }
    }

    void WorkspacePage::ToggleFilter_Click(
        winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
    {
        filterVisible_ = !filterVisible_;
        FilterBarControl().Visibility(filterVisible_ ? mux::Visibility::Visible : mux::Visibility::Collapsed);
    }

    void WorkspacePage::ToggleAI_Click(
        winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
    {
        if (!detailsVisible_)
        {
            detailsVisible_ = true;
            DetailsColumn().Width(mux::GridLengthHelper::FromPixels(260));
            Details().Visibility(mux::Visibility::Visible);
        }
        Details().as<DetailsPanel>()->AssistantTab_Click(nullptr, {});
    }

    void WorkspacePage::OnTableSelected(const std::wstring& tableName, const std::wstring& schema)
    {
        // Discard pending changes when switching tables
        if (changeTracker_.hasChanges())
        {
            changeTracker_.discardAll();
            UpdatePendingUI();
        }

        // Reuse existing tab
        for (auto& tab : state_.tabs)
        {
            if (tab.tableName == tableName && tab.schema == schema && tab.type == DBModels::TabType::DataGrid)
            {
                state_.activeTabId = tab.id;
                TabBar().as<TabBarControl>()->SetActiveTab(winrt::hstring(tab.id));
                state_.currentPage = 0;
                if (connMgr_.isConnected())
                    LoadTableDataFromDB(tableName);
                else
                    LoadDemoTableData(tableName);
                SwitchContentView();
                return;
            }
        }

        // Create new tab
        GUID guid;
        CoCreateGuid(&guid);
        wchar_t guidStr[40];
        StringFromGUID2(guid, guidStr, 40);

        DBModels::ContentTab tab;
        tab.id = guidStr;
        tab.type = DBModels::TabType::DataGrid;
        tab.title = tableName;
        tab.tableName = tableName;
        tab.schema = schema;
        tab.databaseName = state_.connection.database;

        state_.tabs.push_back(tab);
        state_.activeTabId = tab.id;
        state_.currentPage = 0;
        TabBar().as<TabBarControl>()->AddTab(winrt::hstring(tab.id), winrt::hstring(tab.title));
        TabBar().as<TabBarControl>()->SetActiveTab(winrt::hstring(tab.id));

        if (connMgr_.isConnected())
            LoadTableDataFromDB(tableName);
        else
            LoadDemoTableData(tableName);
        showingData_ = true;
        DataToggle().IsChecked(true);
        StructureToggle().IsChecked(false);
        SwitchContentView();
    }

    void WorkspacePage::OpenNewQueryTab()
    {
        // Save the outgoing tab's state (SQL text for a query tab, grid
        // data for a DataGrid tab) before we change activeTabId. Without
        // this the old tab's content is lost and the new empty tab shows
        // the previous tab's SQL.
        SaveCurrentTabCache();

        GUID guid;
        CoCreateGuid(&guid);
        wchar_t guidStr[40];
        StringFromGUID2(guid, guidStr, 40);

        DBModels::ContentTab tab;
        tab.id = guidStr;
        tab.type = DBModels::TabType::QueryEditor;
        tab.title = L"Query";
        tab.databaseName = state_.connection.database;

        state_.tabs.push_back(tab);
        state_.activeTabId = tab.id;
        TabBar().as<TabBarControl>()->AddTab(winrt::hstring(tab.id), winrt::hstring(tab.title));
        TabBar().as<TabBarControl>()->SetActiveTab(winrt::hstring(tab.id));

        // Clear the shared QueryEditorView for the fresh tab — otherwise
        // it still holds whatever text was typed in the previous tab.
        QueryEditor().as<QueryEditorView>()->SetSql(L"");

        SwitchContentView();
    }

    // ── CRUD Operations ─────────────────────────────────

    DBModels::TableRow WorkspacePage::ExtractPrimaryKey(int rowIndex)
    {
        DBModels::TableRow pk;
        if (rowIndex < 0 || rowIndex >= static_cast<int>(state_.currentData.rows.size()))
            return pk;

        auto& row = state_.currentData.rows[rowIndex];
        for (auto& col : state_.currentColumns)
        {
            if (col.isPrimaryKey)
            {
                auto it = row.find(col.name);
                if (it != row.end())
                    pk[col.name] = it->second;
            }
        }
        return pk;
    }

    void WorkspacePage::DeleteSelectedRow()
    {
        int idx = DataGrid().as<DataGridView>()->GetSelectedRowIndex();
        if (idx < 0)
        {
            // No row selected — show tip
            return;
        }

        // Check if row is already marked for deletion
        auto deleted = changeTracker_.deletedRowIndices();
        if (deleted.count(idx)) return;

        auto pk = ExtractPrimaryKey(idx);
        if (pk.empty())
        {
            muxc::ContentDialog dlg;
            dlg.Title(winrt::box_value(winrt::hstring(L"Cannot Delete")));
            dlg.Content(winrt::box_value(winrt::hstring(L"This table has no primary key. Cannot identify row for deletion.")));
            dlg.CloseButtonText(L"OK");
            dlg.XamlRoot(this->XamlRoot());
            dlg.ShowAsync();
            return;
        }

        changeTracker_.trackDelete(idx, pk);
        DataGrid().as<DataGridView>()->MarkRowDeleted(idx);
        UpdatePendingUI();
    }

    // Check if a default value is a SQL expression (not a literal)
    static bool IsSqlExpression(const std::wstring& val)
    {
        if (val.empty()) return false;
        // Functions: gen_random_uuid(), now(), nextval('seq'), uuid_generate_v4()
        if (val.find(L"(") != std::wstring::npos) return true;
        // Keywords: CURRENT_TIMESTAMP, CURRENT_DATE, CURRENT_USER
        if (val.find(L"CURRENT_") != std::wstring::npos) return true;
        if (val == L"true" || val == L"false") return false; // boolean literals are OK
        return false;
    }

    void WorkspacePage::AddNewRow()
    {
        if (state_.currentColumns.empty()) return;

        // Build empty row — skip columns with SQL expression defaults (let DB handle)
        DBModels::TableRow newRow;
        for (auto& col : state_.currentColumns)
        {
            // MSSQL IDENTITY columns: server auto-generates the value,
            // including them in INSERT causes "Cannot insert explicit
            // value for identity column" error.
            if (col.comment == L"IDENTITY")
                continue;
            if (col.isPrimaryKey && IsSqlExpression(col.defaultValue))
                continue; // PK with auto-gen default — omit from INSERT
            if (IsSqlExpression(col.defaultValue))
                continue; // DB will generate: now(), nextval(), etc.
            if (!col.defaultValue.empty() && col.defaultValue != L"NULL")
                newRow[col.name] = col.defaultValue; // static default like '0', 'active'
            else if (col.nullable)
                newRow[col.name] = L"NULL";
            else
                newRow[col.name] = L""; // required column, user must fill
        }

        // Build display row (for grid) — include auto-gen placeholders
        DBModels::TableRow displayRow = newRow;
        for (auto& col : state_.currentColumns)
        {
            if (displayRow.find(col.name) == displayRow.end())
                displayRow[col.name] = col.defaultValue; // show e.g. "gen_random_uuid()"
        }

        state_.currentData.rows.push_back(displayRow);
        int newRowIndex = static_cast<int>(state_.currentData.rows.size()) - 1;
        changeTracker_.trackInsert(newRow, newRowIndex); // INSERT only has user-editable columns
        DataGrid().as<DataGridView>()->SetData(state_.currentData);
        UpdatePendingUI();
    }

    void WorkspacePage::CommitChanges()
    {
        // Flush any in-flight inline cell edit before reading the
        // change tracker. Without this, pressing Ctrl+S while a TextBox
        // still holds the newly-typed text would ship the pre-edit
        // value (often NULL for a freshly-inserted row) because the
        // commit only fires on Enter / LostFocus and the accelerator
        // doesn't move focus away from the TextBox.
        if (auto g = DataGrid().try_as<DataGridView>()) g->FlushEdit();

        if (!changeTracker_.hasChanges()) return;
        if (!connMgr_.isConnected()) return;

        // Find active table
        std::wstring tableName, schema;
        for (auto& tab : state_.tabs)
        {
            if (tab.id == state_.activeTabId && tab.type == DBModels::TabType::DataGrid)
            {
                tableName = tab.tableName;
                schema = tab.schema;
                break;
            }
        }
        if (tableName.empty()) return;

        const bool isMongo =
            state_.connection.databaseType == DBModels::DatabaseType::MongoDB;

        // MongoDB doesn't use SQL. Build a human-readable preview
        // for SQL adapters; for MongoDB just summarize the operations.
        std::wstring preview;
        std::vector<std::wstring> statements;
        if (!isMongo)
        {
            statements = changeTracker_.generateSQL(
                tableName, schema, state_.connection.databaseType);
            if (statements.empty()) return;
            preview = std::to_wstring(statements.size()) + L" statement(s):\n\n";
            for (auto& s : statements) preview += s + L"\n";
        }
        else
        {
            auto& edits = changeTracker_.pendingEdits();
            int ins = 0, upd = 0, del = 0;
            for (auto& e : edits)
            {
                if (e.type == DBModels::EditType::Insert) ++ins;
                else if (e.type == DBModels::EditType::Update) ++upd;
                else if (e.type == DBModels::EditType::Delete) ++del;
            }
            preview = L"MongoDB operations on " + tableName + L":\n";
            if (ins) preview += L"  Insert: " + std::to_wstring(ins) + L" document(s)\n";
            if (upd) preview += L"  Update: " + std::to_wstring(upd) + L" field(s)\n";
            if (del) preview += L"  Delete: " + std::to_wstring(del) + L" document(s)\n";
        }

        // Show confirmation dialog
        muxc::ContentDialog dlg;
        dlg.Title(winrt::box_value(winrt::hstring(L"Commit Changes")));

        muxc::ScrollViewer sv;
        sv.MaxHeight(300);
        muxc::TextBlock tb;
        tb.Text(winrt::hstring(preview));
        tb.FontFamily(mux::Media::FontFamily(L"Cascadia Code,Consolas,monospace"));
        tb.FontSize(12);
        tb.IsTextSelectionEnabled(true);
        tb.TextWrapping(mux::TextWrapping::Wrap);
        sv.Content(tb);
        dlg.Content(sv);

        dlg.PrimaryButtonText(L"Execute");
        dlg.CloseButtonText(L"Cancel");
        dlg.DefaultButton(muxc::ContentDialogButton::Primary);
        dlg.XamlRoot(this->XamlRoot());

        auto op = dlg.ShowAsync();
        op.Completed([this, statements, tableName, schema, isMongo](auto&& asyncOp, auto&&)
        {
            auto result = asyncOp.GetResults();
            if (result != muxc::ContentDialogResult::Primary) return;

            this->DispatcherQueue().TryEnqueue([this, statements, tableName, schema, isMongo]()
            {
                auto adapter = connMgr_.getActiveAdapter();
                if (!adapter) return;

                try
                {
                    if (isMongo)
                    {
                        // MongoDB: call adapter CRUD methods directly
                        for (auto& edit : changeTracker_.pendingEdits())
                        {
                            if (edit.type == DBModels::EditType::Insert)
                                adapter->insertRow(tableName, schema, edit.insertValues);
                            else if (edit.type == DBModels::EditType::Update)
                            {
                                DBModels::TableRow setVals;
                                setVals[edit.column] = edit.newValue;
                                adapter->updateRow(tableName, schema, setVals, edit.primaryKey);
                            }
                            else if (edit.type == DBModels::EditType::Delete)
                                adapter->deleteRow(tableName, schema, edit.primaryKey);
                        }
                    }
                    else
                    {
                        // SQL adapters: execute generated SQL in a transaction
                        adapter->beginTransaction();
                        for (auto& sql : statements)
                        {
                            auto r = adapter->execute(sql);
                            LogQuery(r);
                            if (!r.success)
                            {
                                adapter->rollbackTransaction();
                                muxc::ContentDialog errDlg;
                                errDlg.Title(winrt::box_value(winrt::hstring(L"Commit Failed")));
                                errDlg.Content(winrt::box_value(winrt::hstring(r.error)));
                                errDlg.CloseButtonText(L"OK");
                                errDlg.XamlRoot(this->XamlRoot());
                                errDlg.ShowAsync();
                                return;
                            }
                        }
                        adapter->commitTransaction();
                    }

                    changeTracker_.discardAll();
                    ReloadCurrentTable();
                    UpdatePendingUI();
                }
                catch (const std::exception& ex)
                {
                    if (!isMongo) try { adapter->rollbackTransaction(); } catch (...) {}

                    int sz = MultiByteToWideChar(CP_UTF8, 0, ex.what(), -1, nullptr, 0);
                    std::wstring msg(sz, L'\0');
                    MultiByteToWideChar(CP_UTF8, 0, ex.what(), -1, &msg[0], sz);

                    muxc::ContentDialog errDlg;
                    errDlg.Title(winrt::box_value(winrt::hstring(L"Commit Error")));
                    errDlg.Content(winrt::box_value(winrt::hstring(msg)));
                    errDlg.CloseButtonText(L"OK");
                    errDlg.XamlRoot(this->XamlRoot());
                    errDlg.ShowAsync();
                }
            });
        });
    }

    void WorkspacePage::DiscardChanges()
    {
        changeTracker_.discardAll();
        DataGrid().as<DataGridView>()->ClearRowMarks();
        ReloadCurrentTable();
        UpdatePendingUI();
    }

    // ── Pagination ──────────────────────────────────────

    void WorkspacePage::PrevPage()
    {
        if (state_.currentPage <= 0) return;
        state_.currentPage--;
        LoadCurrentTablePage(state_.currentPage);
    }

    void WorkspacePage::NextPage()
    {
        int totalPages = state_.currentData.totalPages();
        if (state_.currentPage + 1 >= totalPages) return;
        state_.currentPage++;
        LoadCurrentTablePage(state_.currentPage);
    }

    void WorkspacePage::LoadCurrentTablePage(int page)
    {
        state_.currentPage = page;
        for (auto& tab : state_.tabs)
        {
            if (tab.id == state_.activeTabId && tab.type == DBModels::TabType::DataGrid)
            {
                if (connMgr_.isConnected())
                    LoadTableDataFromDB(tab.tableName);
                else
                    LoadDemoTableData(tab.tableName);
                break;
            }
        }
    }

    void WorkspacePage::UpdatePaginationUI()
    {
        int start = state_.currentPage * state_.pageSize + 1;
        int end = start + static_cast<int>(state_.currentData.rows.size()) - 1;
        int total = state_.currentData.totalRows;

        if (total == 0)
        {
            PaginationText().Text(L"0 rows");
        }
        else
        {
            PaginationText().Text(winrt::hstring(
                std::to_wstring(start) + L"-" + std::to_wstring(end) +
                L" of " + std::to_wstring(total) + L" rows"));
        }

        PrevPageBtn().IsEnabled(state_.currentPage > 0);
        NextPageBtn().IsEnabled(state_.currentPage + 1 < state_.currentData.totalPages());
    }

    // ── Database Picker ─────────────────────────────────

    void WorkspacePage::LoadDatabasePicker()
    {
        auto adapter = connMgr_.getActiveAdapter();
        if (!adapter) return;

        try
        {
            auto dbs = adapter->listDatabases();
            DbPickerFlyout().Items().Clear();

            for (auto& dbName : dbs)
            {
                muxc::MenuFlyoutItem item;
                item.Text(winrt::hstring(dbName));

                // Highlight current
                if (dbName == state_.connection.database)
                    item.FontWeight(winrt::Windows::UI::Text::FontWeights::Bold());

                item.Click([this, dbName](winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
                {
                    SwitchDatabase(dbName);
                });

                DbPickerFlyout().Items().Append(item);
            }
        }
        catch (...) {}
    }

    void WorkspacePage::SwitchDatabase(const std::wstring& dbName)
    {
        if (dbName == state_.connection.database) return;

        // Discard any pending changes
        if (changeTracker_.hasChanges())
        {
            changeTracker_.discardAll();
            UpdatePendingUI();
        }

        state_.connection.database = dbName;
        DbPickerText().Text(winrt::hstring(dbName));

        auto adapter = connMgr_.getActiveAdapter();
        if (!adapter) return;

        try
        {
            // Redis: logical DBs live on the same connection. No reconnect
            // needed. The adapter's fetchRows / listTables already issue a
            // SELECT <n> based on the schema parameter, so we just route
            // the new dbName through currentSchema_ and rebuild the sidebar
            // + reload the active tab. Without this path, listTables kept
            // reading currentSchema_ ("public" from the header default)
            // and the key list never actually changed when the user
            // flipped between db0 / db1 / db2 / ...
            if (state_.connection.databaseType == DBModels::DatabaseType::Redis)
            {
                currentSchema_ = dbName;
                state_.statusSchema = dbName;

                LoadSidebarFromDB();
                LoadDatabasePicker();
                UpdateBreadcrumb();

                // Reload the active DataGrid tab (if any) against the new DB
                for (auto& tab : state_.tabs)
                {
                    if (tab.id == state_.activeTabId &&
                        tab.type == DBModels::TabType::DataGrid)
                    {
                        tab.schema = dbName;
                        LoadTableDataFromDB(tab.tableName);
                        break;
                    }
                }
                return;
            }

            // MongoDB: no reconnect needed — just update the adapter's
            // internal database name and reload the sidebar. MongoDB
            // connections are server-wide (not database-scoped).
            if (state_.connection.databaseType == DBModels::DatabaseType::MongoDB)
            {
                currentSchema_ = dbName;
                state_.statusSchema = dbName;

                LoadSidebarFromDB();
                LoadDatabasePicker();
                UpdateBreadcrumb();

                // Clear tabs since collections differ per database.
                // Snapshot tab IDs first to avoid iterator invalidation
                // (CloseTab fires OnTabClosed which erases from state_.tabs).
                std::vector<std::wstring> ids;
                for (auto& t : state_.tabs) ids.push_back(t.id);
                for (auto& id : ids)
                    TabBar().as<TabBarControl>()->CloseTab(winrt::hstring(id));
                SwitchContentView();
                return;
            }

            // MySQL and SQL Server can switch with USE, PostgreSQL needs reconnect
            if (state_.connection.databaseType == DBModels::DatabaseType::MySQL)
            {
                adapter->execute(L"USE `" + dbName + L"`");
            }
            else if (state_.connection.databaseType == DBModels::DatabaseType::MSSQLServer)
            {
                adapter->execute(L"USE [" + dbName + L"]");
            }
            else
            {
                connMgr_.disconnect();
                connMgr_.connect(state_.connection, pendingPassword_);
            }

            LoadSidebarFromDB();
            LoadDatabasePicker();
            UpdateBreadcrumb();

            // Clear tabs
            state_.tabs.clear();
            state_.activeTabId.clear();
            // TabBar doesn't have a ClearAll, so rebuild later on table select
        }
        catch (const std::exception& ex)
        {
            std::string what = ex.what();
            int sz = MultiByteToWideChar(CP_UTF8, 0, what.c_str(), (int)what.size(), nullptr, 0);
            std::wstring errMsg(sz, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, what.c_str(), (int)what.size(), &errMsg[0], sz);

            muxc::ContentDialog dlg;
            dlg.Title(winrt::box_value(winrt::hstring(L"Switch Failed")));
            dlg.Content(winrt::box_value(winrt::hstring(errMsg)));
            dlg.CloseButtonText(L"OK");
            dlg.XamlRoot(this->XamlRoot());
            dlg.ShowAsync();
        }
    }

    // ── Filter ──────────────────────────────────────────

    void WorkspacePage::ApplyFilter(const std::wstring& column, const std::wstring& op, const std::wstring& value)
    {
        if (!connMgr_.isConnected()) return;

        std::wstring tableName;
        for (auto& tab : state_.tabs)
        {
            if (tab.id == state_.activeTabId && tab.type == DBModels::TabType::DataGrid)
            {
                tableName = tab.tableName;
                break;
            }
        }
        if (tableName.empty()) return;

        // Build WHERE clause
        std::wstring where;
        std::wstring qCol = L"\"" + column + L"\"";
        if (state_.connection.databaseType == DBModels::DatabaseType::MySQL)
            qCol = L"`" + column + L"`";

        if (op == L"equals")          where = qCol + L" = '" + value + L"'";
        else if (op == L"not equals") where = qCol + L" != '" + value + L"'";
        else if (op == L"contains")   where = qCol + L" LIKE '%" + value + L"%'";
        else if (op == L"starts with") where = qCol + L" LIKE '" + value + L"%'";
        else if (op == L"ends with")  where = qCol + L" LIKE '%" + value + L"'";
        else if (op == L"greater than") where = qCol + L" > '" + value + L"'";
        else if (op == L"less than")  where = qCol + L" < '" + value + L"'";
        else if (op == L"is null")    where = qCol + L" IS NULL";
        else if (op == L"is not null") where = qCol + L" IS NOT NULL";
        else where = qCol + L" = '" + value + L"'";

        // Build filtered query
        std::wstring schemaQ = (state_.connection.databaseType == DBModels::DatabaseType::MySQL)
            ? L"`" + currentSchema_ + L"`"
            : L"\"" + currentSchema_ + L"\"";
        std::wstring tableQ = (state_.connection.databaseType == DBModels::DatabaseType::MySQL)
            ? L"`" + tableName + L"`"
            : L"\"" + tableName + L"\"";

        std::wstring sql;
        if (state_.connection.databaseType == DBModels::DatabaseType::SQLite)
            sql = L"SELECT * FROM " + tableQ + L" WHERE " + where + L" LIMIT " + std::to_wstring(state_.pageSize);
        else
            sql = L"SELECT * FROM " + schemaQ + L"." + tableQ + L" WHERE " + where + L" LIMIT " + std::to_wstring(state_.pageSize);

        auto adapter = connMgr_.getActiveAdapter();
        if (!adapter) return;

        try
        {
            state_.currentData = adapter->execute(sql);

            auto renderStart = std::chrono::steady_clock::now();
            DataGrid().as<DataGridView>()->SetData(state_.currentData);
            auto renderEnd = std::chrono::steady_clock::now();
            state_.statusRenderTimeMs =
                std::chrono::duration<double, std::milli>(renderEnd - renderStart).count();
            state_.statusQueryTimeMs = state_.currentData.executionTimeMs;

            LogQuery(state_.currentData, state_.statusRenderTimeMs);

            StatusBar().as<StatusBarControl>()->SetStatus(
                state_.statusConnection, state_.statusSchema,
                state_.currentData.totalRows,
                state_.statusQueryTimeMs,
                state_.statusRenderTimeMs);
            UpdatePaginationUI();
        }
        catch (...) {}
    }

    // ── Query Log ───────────────────────────────────────

    void WorkspacePage::LogQuery(const DBModels::QueryResult& result, double renderTimeMs)
    {
        DBModels::QueryLogEntry entry;
        entry.sql = result.sql;
        entry.status = result.success ? L"success" : L"error";
        entry.rowCount = result.totalRows;
        entry.durationMs = result.executionTimeMs;
        entry.renderDurationMs = renderTimeMs;
        entry.timestamp = std::chrono::system_clock::now();
        entry.error = result.error;
        connMgr_.addQueryLog(entry);
        RefreshQueryLog();
    }

    void WorkspacePage::RefreshQueryLog()
    {
        QueryLogEntries().Children().Clear();

        auto& log = connMgr_.getQueryLog();
        // Show last 50 entries max
        int startIdx = static_cast<int>(log.size()) > 50 ? static_cast<int>(log.size()) - 50 : 0;

        for (int i = startIdx; i < static_cast<int>(log.size()); i++)
        {
            auto& entry = log[i];

            // Timestamp + duration line
            auto tt = std::chrono::system_clock::to_time_t(entry.timestamp);
            struct tm tm_buf;
            localtime_s(&tm_buf, &tt);
            wchar_t timeBuf[64];
            wcsftime(timeBuf, 64, L"%Y-%m-%d %H:%M:%S", &tm_buf);

            // Display format: "-- TS (Exec Nms · Render Nms)" when the
            // entry carries a render timing, else "-- TS (Nms)" for
            // pure metadata queries that never hit the data grid.
            std::wstring metaLine = std::wstring(L"-- ") + timeBuf + L" (";
            if (entry.renderDurationMs > 0.0)
            {
                metaLine += L"Exec "   + std::to_wstring(static_cast<int>(entry.durationMs))       + L"ms";
                metaLine += L"  \x00B7  ";
                metaLine += L"Render " + std::to_wstring(static_cast<int>(entry.renderDurationMs)) + L"ms";
            }
            else
            {
                metaLine += std::to_wstring(static_cast<int>(entry.durationMs)) + L"ms";
            }
            metaLine += L")";

            muxc::TextBlock metaTb;
            metaTb.Text(winrt::hstring(metaLine));
            metaTb.FontSize(11);
            metaTb.FontFamily(mux::Media::FontFamily(L"Cascadia Code,Consolas,monospace"));
            metaTb.IsTextSelectionEnabled(true);
            metaTb.Opacity(0.5);
            QueryLogEntries().Children().Append(metaTb);

            // SQL line
            if (!entry.sql.empty())
            {
                muxc::TextBlock sqlTb;
                sqlTb.Text(winrt::hstring(entry.sql));
                sqlTb.FontSize(12);
                sqlTb.FontFamily(mux::Media::FontFamily(L"Cascadia Code,Consolas,monospace"));
                sqlTb.IsTextSelectionEnabled(true);
                sqlTb.TextWrapping(mux::TextWrapping::Wrap);

                if (!entry.error.empty())
                    sqlTb.Foreground(muxm::SolidColorBrush(
                        winrt::Windows::UI::ColorHelper::FromArgb(255, 196, 43, 28)));

                QueryLogEntries().Children().Append(sqlTb);
            }

            // Error line
            if (!entry.error.empty())
            {
                muxc::TextBlock errTb;
                errTb.Text(winrt::hstring(L"-- ERROR: " + entry.error));
                errTb.FontSize(11);
                errTb.FontFamily(mux::Media::FontFamily(L"Cascadia Code,Consolas,monospace"));
                errTb.IsTextSelectionEnabled(true);
                errTb.Foreground(muxm::SolidColorBrush(
                    winrt::Windows::UI::ColorHelper::FromArgb(255, 196, 43, 28)));
                errTb.Opacity(0.8);
                QueryLogEntries().Children().Append(errTb);
            }
        }

        // Auto-scroll to bottom
        QueryLogScroller().ChangeView(nullptr, QueryLogScroller().ExtentHeight(), nullptr);

        // Also update sidebar History tab
        auto& logEntries = connMgr_.getQueryLog();
        std::vector<std::pair<std::wstring, std::wstring>> historyItems;
        for (auto& entry : logEntries)
        {
            if (entry.sql.empty()) continue;
            auto tt = std::chrono::system_clock::to_time_t(entry.timestamp);
            struct tm tm_buf;
            localtime_s(&tm_buf, &tt);
            wchar_t timeBuf[64];
            wcsftime(timeBuf, 64, L"%H:%M:%S", &tm_buf);
            std::wstring ts = timeBuf;
            ts += L"  (" + std::to_wstring(static_cast<int>(entry.durationMs)) + L"ms)";
            historyItems.push_back({ ts, entry.sql });
        }
        Sidebar().as<SidebarPanel>()->SetHistory(historyItems);
    }

    // ── Pending Changes UI ──────────────────────────────

    void WorkspacePage::UpdatePendingUI()
    {
        int count = changeTracker_.changeCount();
        if (count > 0)
        {
            PendingChangesPanel().Visibility(mux::Visibility::Visible);
            PendingCountText().Text(winrt::hstring(std::to_wstring(count) + L" pending"));
        }
        else
        {
            PendingChangesPanel().Visibility(mux::Visibility::Collapsed);
        }
    }

    // ── Import (async) ────────────────────────────────────

    winrt::fire_and_forget WorkspacePage::ImportDataAsync(
        std::wstring targetTable, std::wstring targetSchema)
    {
        auto showError = [this](const std::wstring& msg) -> winrt::fire_and_forget
        {
            muxc::ContentDialog d;
            d.Title(winrt::box_value(winrt::hstring(L"Import")));
            d.Content(winrt::box_value(winrt::hstring(msg)));
            d.CloseButtonText(L"OK");
            d.XamlRoot(this->XamlRoot());
            co_await d.ShowAsync();
        };

        if (!connMgr_.isConnected())
        {
            showError(L"Not connected to any database.");
            co_return;
        }
        auto adapter = connMgr_.getActiveAdapter();
        if (!adapter) co_return;

        if (targetTable.empty())
        {
            showError(L"No target table specified.");
            co_return;
        }

        // ── Pick file via new Windows App SDK Pickers (supports elevated) ──
        std::wstring filePath;
        bool pickerFailed = false;
        try
        {
            // Get WindowId from stored HWND (C interop function)
            winrt::Microsoft::UI::WindowId windowId{
                reinterpret_cast<uint64_t>(
                    winrt::Gridex::implementation::App::MainHwnd) };

            // New Microsoft.Windows.Storage.Pickers API (WinAppSDK 1.8+)
            winrt::Microsoft::Windows::Storage::Pickers::FileOpenPicker picker(windowId);
            picker.FileTypeFilter().Append(L".csv");
            picker.FileTypeFilter().Append(L".json");
            picker.FileTypeFilter().Append(L".sql");

            auto pickedFile = co_await picker.PickSingleFileAsync();
            if (!pickedFile) co_return;  // User cancelled
            filePath = std::wstring(pickedFile.Path());
        }
        catch (...)
        {
            pickerFailed = true;
        }

        // Fallback: manual path input if native picker crashed
        if (pickerFailed)
        {
            muxc::ContentDialog dlg;
            dlg.Title(winrt::box_value(winrt::hstring(
                L"Import into " + targetTable)));

            muxc::StackPanel panel;
            panel.Spacing(10.0);

            muxc::TextBlock lbl;
            lbl.Text(L"File path:");
            lbl.FontSize(12.0);
            lbl.Opacity(0.7);
            panel.Children().Append(lbl);

            muxc::TextBox pathBox;
            pathBox.PlaceholderText(L"C:\\Users\\...\\data.csv");
            pathBox.FontFamily(muxm::FontFamily(L"Cascadia Code,Consolas,monospace"));
            pathBox.FontSize(12.0);
            panel.Children().Append(pathBox);

            muxc::TextBlock hint;
            hint.Text(L"Supported: .csv, .json, .sql");
            hint.FontSize(11.0);
            hint.Opacity(0.4);
            panel.Children().Append(hint);

            dlg.Content(panel);
            dlg.PrimaryButtonText(L"Import");
            dlg.CloseButtonText(L"Cancel");
            dlg.DefaultButton(muxc::ContentDialogButton::Primary);
            dlg.XamlRoot(this->XamlRoot());

            auto r = co_await dlg.ShowAsync();
            if (r != muxc::ContentDialogResult::Primary) co_return;
            filePath = std::wstring(pathBox.Text());
            if (filePath.empty()) co_return;
        }

        // ── Read & parse file ───────────────────────
        std::wstring content = DBModels::ImportService::ReadFileAsWstring(filePath);
        if (content.empty())
        {
            muxc::ContentDialog errDlg;
            errDlg.Title(winrt::box_value(winrt::hstring(L"Import Failed")));
            errDlg.Content(winrt::box_value(winrt::hstring(
                L"Cannot read file:\n" + filePath)));
            errDlg.CloseButtonText(L"OK");
            errDlg.XamlRoot(this->XamlRoot());
            co_await errDlg.ShowAsync();
            co_return;
        }

        std::wstring fmt = DBModels::ImportService::DetectFormat(filePath);
        if (fmt.empty())
        {
            muxc::ContentDialog errDlg;
            errDlg.Title(winrt::box_value(winrt::hstring(L"Import Failed")));
            errDlg.Content(winrt::box_value(winrt::hstring(
                L"Unsupported file format. Use .csv, .json, or .sql")));
            errDlg.CloseButtonText(L"OK");
            errDlg.XamlRoot(this->XamlRoot());
            co_await errDlg.ShowAsync();
            co_return;
        }

        DBModels::ImportResult parsed;
        if (fmt == L"csv")       parsed = DBModels::ImportService::ParseCsv(content);
        else if (fmt == L"json") parsed = DBModels::ImportService::ParseJson(content);
        else if (fmt == L"sql")  parsed = DBModels::ImportService::ParseSql(content);

        if (!parsed.success)
        {
            muxc::ContentDialog errDlg;
            errDlg.Title(winrt::box_value(winrt::hstring(L"Parse Error")));
            errDlg.Content(winrt::box_value(winrt::hstring(parsed.error)));
            errDlg.CloseButtonText(L"OK");
            errDlg.XamlRoot(this->XamlRoot());
            co_await errDlg.ShowAsync();
            co_return;
        }

        // ── Confirm dialog ──────────────────────────
        std::wstring confirmMsg;
        if (fmt == L"sql")
            confirmMsg = std::to_wstring(parsed.totalParsed) +
                         L" SQL statements will be executed.";
        else
            confirmMsg = std::to_wstring(parsed.totalParsed) +
                         L" rows will be inserted into \"" + targetTable + L"\".";

        muxc::ContentDialog confirmDlg;
        confirmDlg.Title(winrt::box_value(winrt::hstring(L"Confirm Import")));
        confirmDlg.Content(winrt::box_value(winrt::hstring(confirmMsg)));
        confirmDlg.PrimaryButtonText(L"Import");
        confirmDlg.CloseButtonText(L"Cancel");
        confirmDlg.DefaultButton(muxc::ContentDialogButton::Primary);
        confirmDlg.XamlRoot(this->XamlRoot());

        auto confirmResult = co_await confirmDlg.ShowAsync();
        if (confirmResult != muxc::ContentDialogResult::Primary) co_return;

        // ── Execute import ──────────────────────────
        int successCount = 0;
        int failCount = 0;
        std::wstring lastError;

        try
        {
            adapter->beginTransaction();

            if (fmt == L"sql")
            {
                for (auto& stmt : parsed.sqlStatements)
                {
                    try
                    {
                        auto r = adapter->execute(stmt);
                        if (r.success) successCount++;
                        else { failCount++; lastError = r.error; }
                    }
                    catch (...) { failCount++; }
                }
            }
            else
            {
                // CSV/JSON: insert rows via adapter->insertRow
                for (auto& row : parsed.rows)
                {
                    try
                    {
                        auto r = adapter->insertRow(
                            targetTable, targetSchema, row);
                        if (r.success) successCount++;
                        else { failCount++; lastError = r.error; }
                    }
                    catch (...) { failCount++; }
                }
            }

            if (failCount == 0)
                adapter->commitTransaction();
            else
                adapter->rollbackTransaction();
        }
        catch (...)
        {
            try { adapter->rollbackTransaction(); } catch (...) {}
        }

        // ── Result dialog ───────────────────────────
        std::wstring resultMsg;
        if (failCount == 0)
            resultMsg = std::to_wstring(successCount) + L" rows imported successfully.";
        else
            resultMsg = std::to_wstring(successCount) + L" succeeded, " +
                        std::to_wstring(failCount) + L" failed.\n" +
                        L"Transaction rolled back.\n\nLast error: " + lastError;

        muxc::ContentDialog resultDlg;
        resultDlg.Title(winrt::box_value(winrt::hstring(
            failCount == 0 ? L"Import Complete" : L"Import Failed")));
        resultDlg.Content(winrt::box_value(winrt::hstring(resultMsg)));
        resultDlg.CloseButtonText(L"OK");
        resultDlg.XamlRoot(this->XamlRoot());
        co_await resultDlg.ShowAsync();

        // Reload table to show new data
        if (failCount == 0) ReloadCurrentTable();
    }

    // ── Dump Database (async) ─────────────────────────────

    winrt::fire_and_forget WorkspacePage::DumpDatabaseAsync()
    {
        auto showInfo = [this](const std::wstring& title, const std::wstring& msg)
        {
            HWND owner = winrt::Gridex::implementation::App::MainHwnd;
            MessageBoxW(owner, msg.c_str(), title.c_str(), MB_OK | MB_ICONINFORMATION);
        };
        auto showError = [this](const std::wstring& msg)
        {
            HWND owner = winrt::Gridex::implementation::App::MainHwnd;
            MessageBoxW(owner, msg.c_str(), L"Dump Failed", MB_OK | MB_ICONERROR);
        };

        if (!connMgr_.isConnected())
        {
            showError(L"Not connected to any database.");
            co_return;
        }
        auto adapter = connMgr_.getActiveAdapter();
        if (!adapter) co_return;

        // Use the same schema the sidebar is showing (PostgreSQL: "public",
        // MySQL: db name, SQLite: empty/main). This matches whatever
        // listTables(currentSchema_) returns elsewhere in the page.
        std::wstring schema = currentSchema_;

        // ── Options dialog: batch size ─────────────
        muxc::ContentDialog dlg;
        dlg.Title(winrt::box_value(winrt::hstring(L"Dump Database")));

        muxc::StackPanel panel;
        panel.Spacing(12.0);

        muxc::TextBlock info;
        info.Text(winrt::hstring(L"Schema: " + schema));
        info.FontSize(12.0);
        info.Opacity(0.7);
        panel.Children().Append(info);

        muxc::TextBlock batchLabel;
        batchLabel.Text(L"Rows per batch (LIMIT/OFFSET fetch size):");
        batchLabel.FontSize(12.0);
        panel.Children().Append(batchLabel);

        muxc::NumberBox batchBox;
        batchBox.Value(static_cast<double>(DBModels::DumpRestoreService::DEFAULT_BATCH_SIZE));
        batchBox.Minimum(100);
        batchBox.Maximum(50000);
        batchBox.SpinButtonPlacementMode(muxc::NumberBoxSpinButtonPlacementMode::Inline);
        batchBox.Width(160);
        batchBox.HorizontalAlignment(mux::HorizontalAlignment::Left);
        panel.Children().Append(batchBox);

        muxc::TextBlock hint;
        hint.Text(L"Larger batches = fewer round-trips but more memory.\n"
                  L"Lower for tables with very large rows (BLOB, JSON).");
        hint.FontSize(11.0);
        hint.Opacity(0.5);
        hint.TextWrapping(mux::TextWrapping::Wrap);
        panel.Children().Append(hint);

        dlg.Content(panel);
        dlg.PrimaryButtonText(L"Choose File...");
        dlg.CloseButtonText(L"Cancel");
        dlg.DefaultButton(muxc::ContentDialogButton::Primary);
        dlg.XamlRoot(this->XamlRoot());

        auto dialogResult = co_await dlg.ShowAsync();
        if (dialogResult != muxc::ContentDialogResult::Primary) co_return;

        int batchSize = static_cast<int>(batchBox.Value());
        if (batchSize <= 0) batchSize = DBModels::DumpRestoreService::DEFAULT_BATCH_SIZE;

        // ── File save picker ──────────────────────
        std::wstring filePath;
        try
        {
            winrt::Microsoft::UI::WindowId windowId{
                reinterpret_cast<uint64_t>(
                    winrt::Gridex::implementation::App::MainHwnd) };

            winrt::Microsoft::Windows::Storage::Pickers::FileSavePicker picker(windowId);
            picker.SuggestedFileName(winrt::hstring(schema + L"_dump"));
            auto exts = winrt::single_threaded_vector<winrt::hstring>();
            exts.Append(L".sql");
            picker.FileTypeChoices().Insert(L"SQL Files", exts);

            auto pickedFile = co_await picker.PickSaveFileAsync();
            if (!pickedFile) co_return;
            filePath = std::wstring(pickedFile.Path());
        }
        catch (...)
        {
            showError(L"Could not open save dialog.");
            co_return;
        }

        // ── Background job + progress dialog ──────
        auto state = std::make_shared<DumpRestoreJobState>();
        auto dbType = connMgr_.getConfig().databaseType;

        auto progress = [state](const std::wstring& msg)
        {
            std::lock_guard lock(state->mtx);
            state->log += msg + L"\n";
        };

        std::thread([state, adapter, dbType, schema, filePath, batchSize, progress]() mutable
        {
            try
            {
                state->dumpResult = DBModels::DumpRestoreService::DumpDatabase(
                    adapter, dbType, schema, filePath, batchSize, progress);
            }
            catch (...)
            {
                state->dumpResult.success = false;
                state->dumpResult.error = L"Unexpected exception";
            }
            state->done = true;
        }).detach();

        DumpBtn().IsEnabled(false);
        co_await ShowProgressDialogAsync(state, L"Dumping Database",
            L"Dumping " + schema + L" to " + filePath);
        DumpBtn().IsEnabled(true);

        const auto& res = state->dumpResult;
        if (res.success)
        {
            showInfo(L"Dump Complete",
                std::to_wstring(res.tablesProcessed) + L" tables, " +
                std::to_wstring(res.rowsExported) + L" rows exported to:\n" +
                filePath);
        }
        else
        {
            showError(L"Dump failed: " + res.error);
        }
    }

    // ── Restore Database (async) ──────────────────────────

    winrt::fire_and_forget WorkspacePage::RestoreDatabaseAsync()
    {
        auto showInfo = [this](const std::wstring& title, const std::wstring& msg)
        {
            HWND owner = winrt::Gridex::implementation::App::MainHwnd;
            MessageBoxW(owner, msg.c_str(), title.c_str(), MB_OK | MB_ICONINFORMATION);
        };
        auto showError = [this](const std::wstring& msg)
        {
            HWND owner = winrt::Gridex::implementation::App::MainHwnd;
            MessageBoxW(owner, msg.c_str(), L"Restore Failed", MB_OK | MB_ICONERROR);
        };

        if (!connMgr_.isConnected())
        {
            showError(L"Not connected to any database.");
            co_return;
        }
        auto adapter = connMgr_.getActiveAdapter();
        if (!adapter) co_return;

        // ── File open picker ──────────────────────
        std::wstring filePath;
        try
        {
            winrt::Microsoft::UI::WindowId windowId{
                reinterpret_cast<uint64_t>(
                    winrt::Gridex::implementation::App::MainHwnd) };

            winrt::Microsoft::Windows::Storage::Pickers::FileOpenPicker picker(windowId);
            picker.FileTypeFilter().Append(L".sql");

            auto pickedFile = co_await picker.PickSingleFileAsync();
            if (!pickedFile) co_return;
            filePath = std::wstring(pickedFile.Path());
        }
        catch (...)
        {
            showError(L"Could not open file dialog.");
            co_return;
        }

        // ── Confirm dialog ──────────────────────────
        muxc::ContentDialog confirm;
        confirm.Title(winrt::box_value(winrt::hstring(L"Confirm Restore")));
        confirm.Content(winrt::box_value(winrt::hstring(
            L"This will execute SQL statements from:\n\n" + filePath +
            L"\n\nExisting tables may be dropped. Continue?")));
        confirm.PrimaryButtonText(L"Restore");
        confirm.CloseButtonText(L"Cancel");
        confirm.DefaultButton(muxc::ContentDialogButton::Close);
        confirm.XamlRoot(this->XamlRoot());

        auto r = co_await confirm.ShowAsync();
        if (r != muxc::ContentDialogResult::Primary) co_return;

        // ── Background job + progress dialog ──────
        auto state = std::make_shared<DumpRestoreJobState>();

        auto progress = [state](const std::wstring& msg)
        {
            std::lock_guard lock(state->mtx);
            state->log += msg + L"\n";
        };

        std::thread([state, adapter, filePath, progress]() mutable
        {
            try
            {
                state->restoreResult = DBModels::DumpRestoreService::RestoreDatabase(
                    adapter, filePath, progress);
            }
            catch (...)
            {
                state->restoreResult.success = false;
                state->restoreResult.error = L"Unexpected exception";
            }
            state->done = true;
        }).detach();

        RestoreBtn().IsEnabled(false);
        co_await ShowProgressDialogAsync(state, L"Restoring Database",
            L"Restoring from " + filePath);
        RestoreBtn().IsEnabled(true);

        const auto& res = state->restoreResult;
        if (res.success)
        {
            showInfo(L"Restore Complete",
                std::to_wstring(res.statementsExecuted) +
                L" statements executed successfully.");
            ReloadCurrentTable();
        }
        else
        {
            showError(
                std::to_wstring(res.statementsExecuted) + L" succeeded, " +
                std::to_wstring(res.statementsFailed) + L" failed.\n" +
                L"Transaction rolled back.\n\n" + res.error);
        }
    }

    // ── ER Diagram (async) — opens as a tab, not a dialog ──

    winrt::fire_and_forget WorkspacePage::ShowERDiagramAsync(std::wstring schema)
    {
        auto showError = [this](const std::wstring& msg)
        {
            HWND owner = winrt::Gridex::implementation::App::MainHwnd;
            MessageBoxW(owner, msg.c_str(), L"ER Diagram", MB_OK | MB_ICONERROR);
        };

        if (!connMgr_.isConnected())
        {
            showError(L"Not connected to any database.");
            co_return;
        }
        auto adapter = connMgr_.getActiveAdapter();
        if (!adapter) co_return;

        if (schema.empty()) schema = currentSchema_;
        if (schema.empty()) schema = adapter->currentDatabase();

        // Reuse existing ER tab for this schema if any
        for (auto& tab : state_.tabs)
        {
            if (tab.type == DBModels::TabType::ERDiagram && tab.schema == schema)
            {
                state_.activeTabId = tab.id;
                TabBar().as<TabBarControl>()->SetActiveTab(winrt::hstring(tab.id));
                LoadERDiagramIntoView(tab);
                SwitchContentView();
                co_return;
            }
        }

        // Open new ER tab immediately with loading state, then run generation
        // on a background thread so the UI (spinner) stays responsive.
        GUID guid;
        CoCreateGuid(&guid);
        wchar_t guidStr[40];
        StringFromGUID2(guid, guidStr, 40);

        DBModels::ContentTab tab;
        tab.id = guidStr;
        tab.type = DBModels::TabType::ERDiagram;
        tab.title = L"ER: " + schema;
        tab.schema = schema;
        tab.databaseName = state_.connection.database;
        // erD2Text/erSvgPath empty for now -> loading state

        std::wstring tabId = tab.id;
        state_.tabs.push_back(tab);
        state_.activeTabId = tabId;
        TabBar().as<TabBarControl>()->AddTab(winrt::hstring(tabId), winrt::hstring(tab.title));
        TabBar().as<TabBarControl>()->SetActiveTab(winrt::hstring(tabId));

        // LoadERDiagramIntoView shows the loading panel when erSvgPath is empty
        ERDiagramBtn().IsEnabled(false);
        LoadERDiagramIntoView(state_.tabs.back());
        SwitchContentView();

        // Run generation on background thread. Capture only plain C++ data
        // (adapter shared_ptr, wstring) and the agile DispatcherQueue.
        auto dispatcher = this->DispatcherQueue();
        auto weakSelf = get_weak();

        std::thread([weakSelf, dispatcher, adapter, schema, tabId]()
        {
            DBModels::ERDiagramResult result;
            try
            {
                result = DBModels::ERDiagramService::Generate(adapter, schema);
            }
            catch (...)
            {
                result.success = false;
                result.error = L"Unknown error during generation";
            }

            // Dispatch result back to UI thread
            dispatcher.TryEnqueue([weakSelf, tabId, result]() mutable
            {
                auto self = weakSelf.get();
                if (!self) return;
                self->ApplyERDiagramResult(tabId, result);
            });
        }).detach();

        co_return;
    }

    // Apply background-generated result to the matching ER tab + view.
    void WorkspacePage::ApplyERDiagramResult(
        const std::wstring& tabId, const DBModels::ERDiagramResult& result)
    {
        ERDiagramBtn().IsEnabled(true);

        // Find the tab — it may have been closed while generation ran
        DBModels::ContentTab* tabPtr = nullptr;
        for (auto& t : state_.tabs)
            if (t.id == tabId) { tabPtr = &t; break; }
        if (!tabPtr)
        {
            // Tab was closed; clean up generated SVG
            if (!result.svgPath.empty()) DeleteFileW(result.svgPath.c_str());
            return;
        }

        if (!result.success)
        {
            ERDiagramSubtitle().Text(winrt::hstring(
                L"Generation failed: " + result.error));
            ERDiagramLoadingText().Text(winrt::hstring(L"Failed: " + result.error));
            return;
        }

        tabPtr->erD2Text = result.d2Text;
        tabPtr->erSvgPath = result.svgPath;
        tabPtr->erTableCount = result.tableCount;
        tabPtr->erRelationshipCount = result.relationshipCount;

        // Only update the view if this tab is still the active one.
        // LoadERDiagramIntoView toggles loader/webview visibility internally.
        if (state_.activeTabId == tabId)
            LoadERDiagramIntoView(*tabPtr);
    }

    // Populate the ER view (subtitle + WebView2) from a tab's cached payload.
    // WebView2 hosts the d2-generated SVG inside a minimal HTML page so the
    // browser engine handles font rendering, panning, and scrolling natively.
    void WorkspacePage::LoadERDiagramIntoView(const DBModels::ContentTab& tab)
    {
        // If still generating (no SVG yet), show the loading panel instead
        if (tab.erSvgPath.empty())
        {
            ERDiagramSubtitle().Text(winrt::hstring(
                L"Schema: " + tab.schema + L"  -  generating..."));
            ERDiagramLoadingText().Text(L"Generating ER diagram...");
            ERDiagramLoadingPanel().Visibility(mux::Visibility::Visible);
            ERDiagramWebView().Visibility(mux::Visibility::Collapsed);
            return;
        }

        // SVG ready -> hide loader, show WebView
        ERDiagramLoadingPanel().Visibility(mux::Visibility::Collapsed);
        ERDiagramWebView().Visibility(mux::Visibility::Visible);

        ERDiagramSubtitle().Text(winrt::hstring(
            L"Schema: " + tab.schema + L"  -  " +
            std::to_wstring(tab.erTableCount) + L" tables, " +
            std::to_wstring(tab.erRelationshipCount) + L" relationships"));

        // Read SVG file content
        std::ifstream svgFile(tab.erSvgPath, std::ios::binary);
        if (!svgFile.is_open()) return;
        std::string svgBytes((std::istreambuf_iterator<char>(svgFile)),
                             std::istreambuf_iterator<char>());
        svgFile.close();
        if (svgBytes.empty()) return;

        // Build minimal HTML wrapper with the SVG inlined.
        // - Body acts as the infinite white canvas (min 20000x20000)
        // - SVG overflow:visible so tables dragged outside the original
        //   diagram bounds still render (instead of getting clipped)
        // - Body bg white matches d2's white background -> seamless canvas
        std::string html =
            "<!DOCTYPE html><html><head><meta charset=\"UTF-8\">"
            "<style>"
            "html,body{margin:0;padding:0;background:#FFFFFF;"
            "font-family:'Segoe UI',sans-serif;}"
            "body{overflow:auto;padding:16px;"
            "min-width:20000px;min-height:20000px;}"
            "body>svg{display:block;overflow:visible;}"
            "body>svg svg{overflow:visible;}"
            "</style></head><body>";
        html += svgBytes;
        html +=
            "<script>(function(){"
            // Render SVG at intrinsic viewBox size. Body provides the
            // 'infinite' white canvas via min-width/min-height. Tables can
            // be dragged outside the SVG bounds because svg overflow:visible.
            "var outer=document.querySelector('body>svg');"
            "if(outer&&outer.viewBox&&outer.viewBox.baseVal){"
              "var vb=outer.viewBox.baseVal;"
              "if(vb.width>0&&vb.height>0){"
                "outer.setAttribute('width',vb.width);"
                "outer.setAttribute('height',vb.height);"
              "}"
            "}"
            // Add arrowhead marker for FK direction (FK column -> parent table)
            "var svgNS='http://www.w3.org/2000/svg';"
            "var defs=document.createElementNS(svgNS,'defs');"
            "var marker=document.createElementNS(svgNS,'marker');"
            "marker.setAttribute('id','fk-arrow');"
            "marker.setAttribute('viewBox','0 0 10 10');"
            "marker.setAttribute('refX','9');"
            "marker.setAttribute('refY','5');"
            "marker.setAttribute('markerWidth','7');"
            "marker.setAttribute('markerHeight','7');"
            "marker.setAttribute('orient','auto-start-reverse');"
            "var arrowPath=document.createElementNS(svgNS,'path');"
            "arrowPath.setAttribute('d','M 0 0 L 10 5 L 0 10 z');"
            "arrowPath.setAttribute('fill','#7AA7D9');"
            "marker.appendChild(arrowPath);"
            "defs.appendChild(marker);"
            "outer.appendChild(defs);"
            // Zoom state shared between wheel handler and toolbar buttons
            "var scale=1;"
            "var target=outer||document.body;"
            "target.style.transformOrigin='0 0';"
            "function postZoom(){"
              "if(window.chrome&&window.chrome.webview){"
                "try{window.chrome.webview.postMessage('zoom:'+Math.round(scale*100));}catch(e){}"
              "}"
            "}"
            "function applyScale(){"
              "target.style.transform='scale('+scale+')';"
              "postZoom();"
            "}"
            // Custom wheel: Ctrl=zoom (up=in), Shift=horizontal, default=vertical
            "document.addEventListener('wheel',function(e){"
              "if(e.ctrlKey){"
                "e.preventDefault();"
                "var d=e.deltaY<0?0.1:-0.1;"
                "scale=Math.max(0.25,Math.min(4,scale+d));"
                "applyScale();"
              "}else if(e.shiftKey){"
                "e.preventDefault();"
                "window.scrollBy({left:e.deltaY,behavior:'auto'});"
              "}"
            "},{passive:false});"
            // Drag-to-reposition tables. FK lines auto-follow via overlay
            // <line> elements that we draw between table edges.
            "var dragSvg=outer.querySelector('svg.d2-svg')||outer;"
            "function svgPoint(e){"
              "var pt=outer.createSVGPoint();"
              "pt.x=e.clientX;pt.y=e.clientY;"
              "return pt.matrixTransform(outer.getScreenCTM().inverse());"
            "}"
            "function parseTransform(s){"
              "var m=/translate\\(([-\\d.]+)[\\s,]+([-\\d.]+)\\)/.exec(s||'');"
              "return m?[parseFloat(m[1]),parseFloat(m[2])]:[0,0];"
            "}"
            // Decode d2 base64 class name; returns null if not base64
            "function decodeCls(c){"
              "if(!c||!/^[A-Za-z0-9+/]+=*$/.test(c))return null;"
              "try{return atob(c);}catch(e){return null;}"
            "}"
            // Get element bbox in outer SVG user-space coords
            "function vbox(el){"
              "var r=el.getBoundingClientRect();"
              "var inv=outer.getScreenCTM().inverse();"
              "var p1=outer.createSVGPoint();p1.x=r.left;p1.y=r.top;p1=p1.matrixTransform(inv);"
              "var p2=outer.createSVGPoint();p2.x=r.right;p2.y=r.bottom;p2=p2.matrixTransform(inv);"
              "return{x:p1.x,y:p1.y,width:p2.x-p1.x,height:p2.y-p1.y};"
            "}"
            // Pick the closest edge anchor (left|right|top|bottom) on each
            // table based on the relative direction between centers, then
            // build a cubic Bezier whose control points extend perpendicular
            // to that edge. Result: smooth S-curves that always enter/exit
            // tables at right angles, like Lucidchart / Apple Notes style.
            "function bezierPath(sb,db){"
              "var sc={x:sb.x+sb.width/2,y:sb.y+sb.height/2};"
              "var dc={x:db.x+db.width/2,y:db.y+db.height/2};"
              "var dx=dc.x-sc.x,dy=dc.y-sc.y;"
              "var horiz=Math.abs(dx)>=Math.abs(dy);"
              "var p1,p2,c1,c2,cd;"
              "if(horiz){"
                "if(dx>=0){"
                  "p1={x:sb.x+sb.width,y:sc.y};"
                  "p2={x:db.x,y:dc.y};"
                "}else{"
                  "p1={x:sb.x,y:sc.y};"
                  "p2={x:db.x+db.width,y:dc.y};"
                "}"
                "cd=Math.max(40,Math.abs(p2.x-p1.x)*0.5);"
                "var sx=dx>=0?1:-1;"
                "c1={x:p1.x+sx*cd,y:p1.y};"
                "c2={x:p2.x-sx*cd,y:p2.y};"
              "}else{"
                "if(dy>=0){"
                  "p1={x:sc.x,y:sb.y+sb.height};"
                  "p2={x:dc.x,y:db.y};"
                "}else{"
                  "p1={x:sc.x,y:sb.y};"
                  "p2={x:dc.x,y:db.y+db.height};"
                "}"
                "cd=Math.max(40,Math.abs(p2.y-p1.y)*0.5);"
                "var sy=dy>=0?1:-1;"
                "c1={x:p1.x,y:p1.y+sy*cd};"
                "c2={x:p2.x,y:p2.y-sy*cd};"
              "}"
              "return'M '+p1.x+' '+p1.y+' C '+c1.x+' '+c1.y+', '+c2.x+' '+c2.y+', '+p2.x+' '+p2.y;"
            "}"
            // Build table map: name -> group element
            "var tables={};"
            "var allGroups=dragSvg.querySelectorAll('g');"
            "allGroups.forEach(function(g){"
              "var n=decodeCls(g.getAttribute('class'));"
              "if(!n||n.charAt(0)==='(')return;"
              "if(!g.querySelector('.class_header'))return;"
              "tables[n]={group:g};"
            "});"
            // Build connections, hide originals, create overlay lines
            "var connections=[];"
            "allGroups.forEach(function(g){"
              "var n=decodeCls(g.getAttribute('class'));"
              "if(!n||n.charAt(0)!=='(')return;"
              "var nn=n.replace(/&gt;/g,'>').replace(/&lt;/g,'<');"
              "var m=/^\\((\\S+)\\s*->\\s*(\\S+)\\)/.exec(nn);"
              "if(!m)return;"
              "var src=tables[m[1]],dst=tables[m[2]];"
              "if(!src||!dst)return;"
              "g.setAttribute('display','none');"
              "g.style.display='none';"
              // Use <path> with cubic Bezier instead of straight <line>
              "var path=document.createElementNS(svgNS,'path');"
              "path.setAttribute('stroke','#7AA7D9');"
              "path.setAttribute('stroke-width','1.5');"
              "path.setAttribute('fill','none');"
              // Arrow at path end points to dst (the parent/referenced table)
              "path.setAttribute('marker-end','url(#fk-arrow)');"
              // Append to OUTER svg so path coords match vbox() output
              // (vbox uses outer.getScreenCTM, while inner has its own viewBox)
              "outer.appendChild(path);"
              "connections.push({src:src,dst:dst,path:path});"
            "});"
            "function updateLine(c){"
              "var sb=vbox(c.src.group);"
              "var db=vbox(c.dst.group);"
              "c.path.setAttribute('d',bezierPath(sb,db));"
            "}"
            "connections.forEach(updateLine);"
            // Drag handlers
            "var dragging=null,dragTable=null,startX=0,startY=0,baseTx=0,baseTy=0;"
            "function refreshDraggedLines(){"
              "if(!dragTable)return;"
              "connections.forEach(function(c){"
                "if(c.src===dragTable||c.dst===dragTable)updateLine(c);"
              "});"
            "}"
            "var draggableGroups=[];"
            "var headers=dragSvg.querySelectorAll('.class_header');"
            "headers.forEach(function(h){"
              "var grp=h.parentElement;"
              "while(grp&&grp.tagName.toLowerCase()==='g'){"
                "var cls=grp.getAttribute('class')||'';"
                "if(cls!=='shape'&&cls.indexOf('d2-svg')===-1&&cls.indexOf('text')===-1)break;"
                "grp=grp.parentElement;"
              "}"
              "if(!grp||grp.tagName.toLowerCase()!=='g')return;"
              // Save original transform so Reset Layout can restore it
              "grp.__origTransform=grp.getAttribute('transform')||'';"
              "draggableGroups.push(grp);"
              // Bug fix: d2 renders the table name as a separate <text>
              // sibling on top of class_header. Clicks on the text used to
              // be swallowed by the text element so drag never started.
              // Use header rect's screen bbox to find any sibling element
              // visually inside it, and disable pointer-events on those
              // so clicks fall through to the header rect underneath.
              "try{"
                "var hr=h.getBoundingClientRect();"
                "var hp=h.parentElement;"
                "if(hp){"
                  "Array.prototype.slice.call(hp.children).forEach(function(sib){"
                    "if(sib===h)return;"
                    "var sr=sib.getBoundingClientRect();"
                    "var cy=(sr.top+sr.bottom)/2;"
                    "var cx=(sr.left+sr.right)/2;"
                    "if(cy>=hr.top&&cy<=hr.bottom&&cx>=hr.left&&cx<=hr.right){"
                      "sib.style.pointerEvents='none';"
                    "}"
                  "});"
                "}"
              "}catch(err){}"
              "h.style.cursor='move';"
              "h.addEventListener('mousedown',function(e){"
                "if(e.button!==0)return;"
                "e.preventDefault();e.stopPropagation();"
                "dragging=grp;"
                // Find which table this group belongs to
                "var n=decodeCls(grp.getAttribute('class'));"
                "dragTable=n&&tables[n]?tables[n]:null;"
                "var p=svgPoint(e);"
                "var tr=parseTransform(grp.getAttribute('transform'));"
                "baseTx=tr[0];baseTy=tr[1];"
                "startX=p.x;startY=p.y;"
                "document.body.style.userSelect='none';"
              "});"
            "});"
            "document.addEventListener('mousemove',function(e){"
              "if(!dragging)return;"
              "var p=svgPoint(e);"
              "var nx=baseTx+(p.x-startX);"
              "var ny=baseTy+(p.y-startY);"
              "dragging.setAttribute('transform','translate('+nx+','+ny+')');"
              "refreshDraggedLines();"
            "});"
            "document.addEventListener('mouseup',function(){"
              "if(dragging){dragging=null;dragTable=null;document.body.style.userSelect='';}"
            "});"
            // Toolbar API exposed to host (called via ExecuteScriptAsync).
            // Wrapped in window.__er namespace to avoid global pollution.
            "window.__er={"
              "zoomIn:function(){"
                "scale=Math.min(4,scale+0.1);applyScale();"
              "},"
              "zoomOut:function(){"
                "scale=Math.max(0.25,scale-0.1);applyScale();"
              "},"
              "resetZoom:function(){"
                "scale=1;applyScale();window.scrollTo(0,0);"
              "},"
              "resetLayout:function(){"
                // Restore table positions only — preserve zoom and scroll
                "draggableGroups.forEach(function(g){"
                  "g.setAttribute('transform',g.__origTransform);"
                "});"
                "connections.forEach(updateLine);"
              "}"
            "};"
            // Send initial zoom % so the label is correct after load
            "postZoom();"
            "})();</script>"
            "</body></html>";

        // UTF-8 -> wstring for NavigateToString
        int sz = MultiByteToWideChar(CP_UTF8, 0, html.c_str(),
            static_cast<int>(html.size()), nullptr, 0);
        std::wstring wHtml(sz, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, html.c_str(),
            static_cast<int>(html.size()), &wHtml[0], sz);

        EnsureWebViewAndNavigate(winrt::hstring(wHtml));
    }

    // Initialize WebView2 (first-time async init) then navigate to HTML.
    //
    // We create our OWN CoreWebView2Environment with an explicit user data
    // folder under %LOCALAPPDATA%\Gridex\WebView2 instead of relying on the
    // default (which lands next to the exe and is often non-writable for
    // MSIX-deployed apps, OR collides with stale state from a previous build
    // — both of which throw hresult_error from EnsureCoreWebView2Async).
    winrt::fire_and_forget WorkspacePage::EnsureWebViewAndNavigate(winrt::hstring html)
    {
        try
        {
            auto wv = ERDiagramWebView();

            // Build a writable user data folder under %LOCALAPPDATA%\Gridex
            wchar_t localAppData[MAX_PATH] = {};
            SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localAppData);
            std::wstring userDataFolder = std::wstring(localAppData) + L"\\Gridex\\WebView2";
            CreateDirectoryW((std::wstring(localAppData) + L"\\Gridex").c_str(), nullptr);
            CreateDirectoryW(userDataFolder.c_str(), nullptr);

            namespace WV2C = winrt::Microsoft::Web::WebView2::Core;

            // Create environment pointing at our writable folder.
            // Empty browserExecutableFolder = use the system-installed Edge runtime.
            auto env = co_await WV2C::CoreWebView2Environment::CreateWithOptionsAsync(
                L"",                            // browserExecutableFolder
                winrt::hstring(userDataFolder), // userDataFolder
                WV2C::CoreWebView2EnvironmentOptions{});

            // First-time init — must be awaited before navigating
            co_await wv.EnsureCoreWebView2Async(env);

            // Hook WebMessageReceived once for zoom % updates from JS
            if (!webMessageWired_)
            {
                webMessageWired_ = true;
                auto core = wv.CoreWebView2();
                auto weakSelf = get_weak();
                core.WebMessageReceived(
                    [weakSelf](auto const&, auto const& args)
                {
                    auto self = weakSelf.get();
                    if (!self) return;
                    try
                    {
                        std::wstring m{ args.TryGetWebMessageAsString() };
                        if (m.rfind(L"zoom:", 0) == 0)
                        {
                            auto pct = m.substr(5);
                            self->ERZoomLabel().Text(winrt::hstring(pct + L"%"));
                        }
                    }
                    catch (...) {}
                });
            }

            wv.NavigateToString(html);
        }
        catch (...) { /* leave empty if WebView fails to init */ }
    }

    // Toggle ER diagram fullscreen: hide all surrounding chrome so the
    // WebView fills the entire window. Restoring brings everything back.
    void WorkspacePage::ToggleERFullscreen()
    {
        erFullscreen_ = !erFullscreen_;

        if (erFullscreen_)
        {
            // Save current sidebar/details widths so we can restore them
            prevSidebarWidth_ = SidebarColumn().Width().Value;
            prevDetailsWidth_ = DetailsColumn().Width().Value;

            SidebarColumn().Width(mux::GridLengthHelper::FromPixels(0));
            DetailsColumn().Width(mux::GridLengthHelper::FromPixels(0));
            TopToolbar().Visibility(mux::Visibility::Collapsed);
            TabBar().Visibility(mux::Visibility::Collapsed);
            QueryLogPanel().Visibility(mux::Visibility::Collapsed);
            BottomBar().Visibility(mux::Visibility::Collapsed);
            StatusBar().Visibility(mux::Visibility::Collapsed);
            ERFullscreenIcon().Glyph(L"\xE73F");  // BackToWindow
        }
        else
        {
            SidebarColumn().Width(
                mux::GridLengthHelper::FromPixels(prevSidebarWidth_ > 0 ? prevSidebarWidth_ : 280));
            DetailsColumn().Width(
                mux::GridLengthHelper::FromPixels(prevDetailsWidth_ > 0 ? prevDetailsWidth_ : 260));
            TopToolbar().Visibility(mux::Visibility::Visible);
            TabBar().Visibility(mux::Visibility::Visible);
            QueryLogPanel().Visibility(mux::Visibility::Visible);
            BottomBar().Visibility(mux::Visibility::Visible);
            StatusBar().Visibility(mux::Visibility::Visible);
            ERFullscreenIcon().Glyph(L"\xE740");  // FullScreen
        }
    }

    // ── Redis: Browse Keys with pattern input dialog ───
    // Mac equivalent: Ctrl+F-style search input. Pattern uses Redis glob
    // syntax: "*" all, "user:*" prefix, "?" single char, "[abc]" set.
    winrt::fire_and_forget WorkspacePage::BrowseRedisKeysAsync()
    {
        if (!connMgr_.isConnected()) co_return;
        auto adapter = connMgr_.getActiveAdapter();
        if (!adapter) co_return;
        if (state_.connection.databaseType != DBModels::DatabaseType::Redis)
            co_return;

        // Build TextBox + helper text for the dialog content
        muxc::StackPanel panel;
        panel.Spacing(8.0);
        panel.Width(380);

        muxc::TextBlock hint;
        hint.Text(L"Glob pattern (Redis SCAN MATCH). Examples:");
        hint.FontSize(12.0);
        hint.Opacity(0.7);
        panel.Children().Append(hint);

        muxc::TextBlock examples;
        examples.Text(
            L"  *           - all keys\n"
            L"  user:*      - keys starting with \"user:\"\n"
            L"  *:session   - keys ending with \":session\"\n"
            L"  cache:?:*   - one char between cache: and :*");
        examples.FontSize(11.0);
        examples.Opacity(0.55);
        examples.FontFamily(muxm::FontFamily(L"Cascadia Mono,Consolas,monospace"));
        panel.Children().Append(examples);

        muxc::TextBox input;
        input.PlaceholderText(L"*");
        input.Text(L"*");  // default = all keys
        input.AcceptsReturn(false);
        panel.Children().Append(input);

        muxc::ContentDialog dlg;
        dlg.Title(winrt::box_value(winrt::hstring(L"Browse Keys")));
        dlg.Content(panel);
        dlg.PrimaryButtonText(L"Search");
        dlg.CloseButtonText(L"Cancel");
        dlg.DefaultButton(muxc::ContentDialogButton::Primary);
        dlg.XamlRoot(this->XamlRoot());

        auto result = co_await dlg.ShowAsync();
        if (result != muxc::ContentDialogResult::Primary) co_return;

        std::wstring pattern(input.Text());
        if (pattern.empty()) pattern = L"*";

        // Apply pattern to adapter (Redis stores it; SQL adapters no-op)
        adapter->setSearchPattern(pattern);

        // Open or refresh the Keys virtual table for the current schema
        OnTableSelected(L"Keys", currentSchema_);
    }

    // ── Redis: FLUSHDB with confirmation dialog ────────
    winrt::fire_and_forget WorkspacePage::FlushRedisDbAsync()
    {
        if (!connMgr_.isConnected()) co_return;
        auto adapter = connMgr_.getActiveAdapter();
        if (!adapter) co_return;
        if (state_.connection.databaseType != DBModels::DatabaseType::Redis)
            co_return;

        // Confirmation — destructive op, require explicit user yes
        muxc::ContentDialog dlg;
        dlg.Title(winrt::box_value(winrt::hstring(L"Flush Database")));
        dlg.Content(winrt::box_value(winrt::hstring(
            L"This will DELETE ALL keys in the current Redis database.\n\n"
            L"This action cannot be undone. Continue?")));
        dlg.PrimaryButtonText(L"Flush");
        dlg.CloseButtonText(L"Cancel");
        dlg.DefaultButton(muxc::ContentDialogButton::Close);
        dlg.XamlRoot(this->XamlRoot());

        auto result = co_await dlg.ShowAsync();
        if (result != muxc::ContentDialogResult::Primary) co_return;

        try
        {
            adapter->execute(L"FLUSHDB");
            LoadSidebarFromDB();
        }
        catch (const std::exception& ex)
        {
            std::string what = ex.what();
            int sz = MultiByteToWideChar(CP_UTF8, 0, what.c_str(), (int)what.size(), nullptr, 0);
            std::wstring err(sz, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, what.c_str(), (int)what.size(), &err[0], sz);

            muxc::ContentDialog errDlg;
            errDlg.Title(winrt::box_value(winrt::hstring(L"Flush Failed")));
            errDlg.Content(winrt::box_value(winrt::hstring(err)));
            errDlg.CloseButtonText(L"OK");
            errDlg.XamlRoot(this->XamlRoot());
            errDlg.ShowAsync();
        }
    }

    // ── Progress dialog for dump/restore background jobs ─────
    winrt::Windows::Foundation::IAsyncAction WorkspacePage::ShowProgressDialogAsync(
        std::shared_ptr<DumpRestoreJobState> state,
        std::wstring title,
        std::wstring subtitle)
    {
        // Build dialog on UI thread
        muxc::ContentDialog dlg;
        dlg.Title(winrt::box_value(winrt::hstring(title)));

        muxc::StackPanel panel;
        panel.Spacing(8.0);
        panel.Width(600);

        muxc::TextBlock subtitleText;
        subtitleText.Text(winrt::hstring(subtitle));
        subtitleText.FontSize(12.0);
        subtitleText.Opacity(0.7);
        subtitleText.TextWrapping(mux::TextWrapping::Wrap);
        panel.Children().Append(subtitleText);

        muxc::ProgressRing ring;
        ring.IsActive(true);
        ring.Width(24);
        ring.Height(24);
        ring.HorizontalAlignment(mux::HorizontalAlignment::Left);
        panel.Children().Append(ring);

        muxc::ScrollViewer scroll;
        scroll.Height(280);
        scroll.HorizontalScrollBarVisibility(muxc::ScrollBarVisibility::Auto);
        scroll.VerticalScrollBarVisibility(muxc::ScrollBarVisibility::Auto);

        muxc::TextBlock logText;
        logText.FontFamily(muxm::FontFamily(L"Cascadia Code,Consolas,monospace"));
        logText.FontSize(11.0);
        logText.IsTextSelectionEnabled(true);
        logText.TextWrapping(mux::TextWrapping::NoWrap);
        scroll.Content(logText);
        panel.Children().Append(scroll);

        dlg.Content(panel);
        dlg.XamlRoot(this->XamlRoot());

        // Poll background state via DispatcherQueueTimer (runs on UI thread)
        auto dispatcher = this->DispatcherQueue();
        auto timer = dispatcher.CreateTimer();
        timer.Interval(std::chrono::milliseconds(150));

        timer.Tick([state, logText, scroll, ring, dlg, timer](auto&&, auto&&) mutable
        {
            std::wstring snapshot;
            {
                std::lock_guard lock(state->mtx);
                snapshot = state->log;
            }
            bool finished = state->done.load();

            auto currentText = std::wstring(logText.Text());
            if (snapshot != currentText)
            {
                logText.Text(winrt::hstring(snapshot));
                // Auto-scroll to bottom — ChangeView needs IReference<double>
                auto vOffset = winrt::box_value(scroll.ScrollableHeight())
                    .as<winrt::Windows::Foundation::IReference<double>>();
                scroll.ChangeView(nullptr, vOffset, nullptr);
            }

            if (finished)
            {
                timer.Stop();
                ring.IsActive(false);
                dlg.Hide();
            }
        });

        timer.Start();
        co_await dlg.ShowAsync();
        timer.Stop();
    }

    // ── Export (async with co_await FileSavePicker) ──────

    winrt::fire_and_forget WorkspacePage::ExportTableAsync(
        std::wstring tableName, std::wstring schema, std::wstring format)
    {
        if (!connMgr_.isConnected()) co_return;
        auto adapter = connMgr_.getActiveAdapter();
        if (!adapter) co_return;

        // Fetch all rows
        DBModels::QueryResult result;
        try
        {
            result = adapter->execute(
                L"SELECT * FROM \"" + schema + L"\".\"" + tableName + L"\"");
            LogQuery(result);
            if (!result.success || result.columnNames.empty()) co_return;
        }
        catch (...) { co_return; }

        // Generate content
        std::wstring content;
        std::wstring ext;
        if (format == L"csv")      { content = DBModels::ExportService::ToCsv(result); ext = L".csv"; }
        else if (format == L"json") { content = DBModels::ExportService::ToJson(result); ext = L".json"; }
        else if (format == L"sql")  { content = DBModels::ExportService::ToSqlInsert(result, tableName); ext = L".sql"; }
        else co_return;

        // Native Save dialog via Microsoft.Windows.Storage.Pickers (WinAppSDK 1.8+)
        std::wstring savePath;
        {
            winrt::Microsoft::UI::WindowId windowId{
                reinterpret_cast<uint64_t>(
                    winrt::Gridex::implementation::App::MainHwnd) };

            winrt::Microsoft::Windows::Storage::Pickers::FileSavePicker picker(windowId);
            picker.SuggestedFileName(winrt::hstring(tableName));

            std::wstring filterLabel = (ext == L".csv") ? L"CSV Files"
                                     : (ext == L".json") ? L"JSON Files"
                                     : L"SQL Files";
            auto exts = winrt::single_threaded_vector<winrt::hstring>();
            exts.Append(winrt::hstring(ext));
            picker.FileTypeChoices().Insert(winrt::hstring(filterLabel), exts);

            auto pickedFile = co_await picker.PickSaveFileAsync();
            if (!pickedFile) co_return;  // User cancelled
            savePath = std::wstring(pickedFile.Path());
        }

        // Write file
        try
        {
            std::string utf8;
            int sz2 = WideCharToMultiByte(CP_UTF8, 0, content.c_str(),
                static_cast<int>(content.size()), nullptr, 0, nullptr, nullptr);
            utf8.resize(sz2);
            WideCharToMultiByte(CP_UTF8, 0, content.c_str(),
                static_cast<int>(content.size()), &utf8[0], sz2, nullptr, nullptr);

            std::ofstream outFile(savePath, std::ios::binary);
            if (!outFile.is_open())
            {
                muxc::ContentDialog errDlg;
                errDlg.Title(winrt::box_value(winrt::hstring(L"Export Failed")));
                errDlg.Content(winrt::box_value(winrt::hstring(L"Cannot write to:\n" + savePath)));
                errDlg.CloseButtonText(L"OK");
                errDlg.XamlRoot(this->XamlRoot());
                co_await errDlg.ShowAsync();
                co_return;
            }

            if (ext == L".csv") outFile.write("\xEF\xBB\xBF", 3);
            outFile.write(utf8.c_str(), utf8.size());
            outFile.close();

            // Success dialog
            muxc::ContentDialog successDlg;
            successDlg.Title(winrt::box_value(winrt::hstring(L"Export Complete")));
            successDlg.Content(winrt::box_value(winrt::hstring(
                std::to_wstring(result.totalRows) + L" rows exported to:\n" + savePath)));
            successDlg.CloseButtonText(L"OK");
            successDlg.XamlRoot(this->XamlRoot());
            co_await successDlg.ShowAsync();
        }
        catch (...) {}
    }

    // ── Tab Cache ────────────────────────────────────────

    void WorkspacePage::SaveCurrentTabCache()
    {
        if (state_.activeTabId.empty()) return;
        for (auto& tab : state_.tabs)
        {
            if (tab.id != state_.activeTabId) continue;

            if (tab.type == DBModels::TabType::DataGrid)
            {
                tab.cachedData = state_.currentData;
                tab.cachedColumns = state_.currentColumns;
                tab.cachedIndexes = state_.currentIndexes;
                tab.cachedForeignKeys = state_.currentForeignKeys;
                tab.cachedPage = state_.currentPage;
            }
            else if (tab.type == DBModels::TabType::QueryEditor)
            {
                // Each query tab keeps its own SQL text so switching
                // tabs does not show another tab's content in the
                // shared QueryEditorView.
                tab.cachedSql = QueryEditor().as<QueryEditorView>()->GetSql();
            }
            break;
        }
    }

    // ── Reload ──────────────────────────────────────────

    void WorkspacePage::ReloadCurrentTable()
    {
        for (auto& tab : state_.tabs)
        {
            if (tab.id == state_.activeTabId && tab.type == DBModels::TabType::DataGrid)
            {
                if (connMgr_.isConnected())
                    LoadTableDataFromDB(tab.tableName);
                else
                    LoadDemoTableData(tab.tableName);
                break;
            }
        }
    }

    // ── Demo Data (fallback when not connected) ─────────

    void WorkspacePage::LoadDemoSidebarData()
    {
        DBModels::SidebarItem functionsGroup;
        functionsGroup.id = L"functions"; functionsGroup.title = L"Functions";
        functionsGroup.type = DBModels::SidebarItemType::Group; functionsGroup.isExpanded = false;
        for (auto& name : {L"calculate_discount", L"get_customer_orders", L"update_inventory", L"validate_payment"})
        {
            DBModels::SidebarItem item;
            item.id = std::wstring(L"fn_") + name; item.title = name;
            item.type = DBModels::SidebarItemType::Function; item.schema = L"public";
            functionsGroup.children.push_back(item);
        }
        functionsGroup.count = static_cast<int>(functionsGroup.children.size());

        DBModels::SidebarItem tablesGroup;
        tablesGroup.id = L"tables"; tablesGroup.title = L"Tables";
        tablesGroup.type = DBModels::SidebarItemType::Group; tablesGroup.isExpanded = true;
        for (auto& name : {L"brands", L"categories", L"customers", L"departments",
                           L"employees", L"inventory", L"order_items", L"orders",
                           L"payments", L"products", L"stores", L"suppliers"})
        {
            DBModels::SidebarItem item;
            item.id = name; item.title = name;
            item.type = DBModels::SidebarItemType::Table; item.schema = L"public";
            tablesGroup.children.push_back(item);
        }
        tablesGroup.count = static_cast<int>(tablesGroup.children.size());

        DBModels::SidebarItem viewsGroup;
        viewsGroup.id = L"views"; viewsGroup.title = L"Views";
        viewsGroup.type = DBModels::SidebarItemType::Group; viewsGroup.isExpanded = false;
        for (auto& name : {L"v_monthly_revenue", L"v_top_products", L"v_customer_analysis"})
        {
            DBModels::SidebarItem item;
            item.id = name; item.title = name;
            item.type = DBModels::SidebarItemType::View; item.schema = L"public";
            viewsGroup.children.push_back(item);
        }
        viewsGroup.count = static_cast<int>(viewsGroup.children.size());

        state_.sidebarItems = { functionsGroup, tablesGroup, viewsGroup };
        Sidebar().as<SidebarPanel>()->SetItems(state_.sidebarItems);
        Sidebar().as<SidebarPanel>()->OnItemSelected = [this](const std::wstring& name, const std::wstring& schema)
        {
            OnTableSelected(name, schema);
        };
    }

    void WorkspacePage::LoadDemoTableData(const std::wstring& tableName)
    {
        state_.currentData = DBModels::QueryResult{};
        state_.currentColumns.clear();
        state_.currentIndexes.clear();
        state_.currentForeignKeys.clear();

        state_.currentData.columnNames = {L"id", L"name", L"created_at"};
        state_.currentData.columnTypes = {L"integer", L"varchar(255)", L"timestamp"};
        for (int i = 1; i <= 10; i++)
        {
            DBModels::TableRow row;
            row[L"id"]         = std::to_wstring(i);
            row[L"name"]       = tableName + L"_item_" + std::to_wstring(i);
            row[L"created_at"] = L"2025-01-01 00:00:00";
            state_.currentData.rows.push_back(row);
        }
        state_.currentData.totalRows = 10;
        state_.currentData.success = true;
        state_.statusRowCount = 10;
        state_.statusQueryTimeMs = 0.5;

        state_.currentColumns = {
            {L"id", L"integer", false, L"", true, false, L"", L"", L"", 1},
            {L"name", L"varchar(255)", true, L"", false, false, L"", L"", L"", 2},
            {L"created_at", L"timestamp", true, L"CURRENT_TIMESTAMP", false, false, L"", L"", L"", 3}
        };

        auto renderStart = std::chrono::steady_clock::now();
        DataGrid().as<DataGridView>()->SetData(state_.currentData);
        auto renderEnd = std::chrono::steady_clock::now();
        state_.statusRenderTimeMs =
            std::chrono::duration<double, std::milli>(renderEnd - renderStart).count();

        Structure().as<StructureView>()->SetData(state_.currentColumns, state_.currentIndexes,
                            state_.currentForeignKeys, state_.currentConstraints);
        StatusBar().as<StatusBarControl>()->SetStatus(state_.statusConnection, state_.statusSchema,
                              state_.statusRowCount,
                              state_.statusQueryTimeMs,
                              state_.statusRenderTimeMs);
        UpdatePaginationUI();
    }
}
