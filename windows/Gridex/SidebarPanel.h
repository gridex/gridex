#pragma once

#include "SidebarPanel.g.h"
#include "Models/SidebarItem.h"
#include "Models/DatabaseType.h"
#include <functional>
#include <vector>

namespace winrt::Gridex::implementation
{
    struct SidebarPanel : SidebarPanelT<SidebarPanel>
    {
        SidebarPanel();

        void SetItems(const std::vector<DBModels::SidebarItem>& items);

        // Inform the sidebar of the active connection's DB type so it can
        // render type-specific context menus (e.g. Redis Browse Keys vs
        // SQL Open/Export/Import).
        void SetDatabaseType(DBModels::DatabaseType type) { currentDbType_ = type; }

        // Callback invoked when user clicks a table/view/function
        std::function<void(const std::wstring& name, const std::wstring& schema)> OnItemSelected;

        // Callback invoked when schema picker changes
        std::function<void(const std::wstring& schema)> OnSchemaChanged;

        // Callbacks for add/delete table buttons
        std::function<void()> OnAddTable;
        std::function<void(const std::wstring& tableName, const std::wstring& schema)> OnDeleteTable;
        // Truncate — same signature as OnDeleteTable. Host confirms
        // + emits TRUNCATE TABLE (or DELETE FROM on SQLite).
        std::function<void(const std::wstring& tableName, const std::wstring& schema)> OnTruncateTable;

        // Callback for export table (tableName, schema, format: "csv"/"json"/"sql")
        std::function<void(const std::wstring& tableName, const std::wstring& schema,
                           const std::wstring& format)> OnExportTable;

        // Callback for import data into a specific table (right-click target)
        std::function<void(const std::wstring& tableName,
                           const std::wstring& schema)> OnImportTable;

        // Callback for "Show ER Diagram" on a Database/Schema group
        std::function<void(const std::wstring& schema)> OnShowERDiagram;

        // Callback for Redis-only "Flush Database" — adapter issues FLUSHDB
        std::function<void()> OnFlushRedisDb;

        // Callback for Redis-only "Refresh Keys" — reload sidebar listing
        std::function<void()> OnRefreshSidebar;

        // Callback for Redis-only "Browse Keys" — host shows pattern input
        // dialog and triggers a filtered fetchRows
        std::function<void()> OnBrowseRedisKeys;

        // Get currently selected sidebar item name (for delete)
        std::wstring GetSelectedItemName() const { return selectedItemName_; }
        std::wstring GetSelectedItemSchema() const { return selectedItemSchema_; }

        // Set query history entries for History tab
        void SetHistory(const std::vector<std::pair<std::wstring, std::wstring>>& entries);

        // Callback when history item clicked (sql)
        std::function<void(const std::wstring& sql)> OnHistoryItemClicked;

        // Populate schema picker ComboBox from adapter.listSchemas()
        void SetSchemas(const std::vector<std::wstring>& schemas);

        // Tab clicks
        void ItemsTab_Click(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void QueriesTab_Click(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void HistoryTab_Click(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void SearchBox_TextChanged(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);

    private:
        std::vector<DBModels::SidebarItem> items_;
        std::wstring searchQuery_;
        bool suppressSchemaEvent_ = false;
        std::wstring selectedItemName_;
        std::wstring selectedItemSchema_;
        DBModels::DatabaseType currentDbType_ = DBModels::DatabaseType::PostgreSQL;

        void RenderTree();
        void RenderItem(
            winrt::Microsoft::UI::Xaml::Controls::StackPanel const& container,
            const DBModels::SidebarItem& item,
            int depth);
        void RenderGroupHeader(
            winrt::Microsoft::UI::Xaml::Controls::StackPanel const& container,
            DBModels::SidebarItem& item,
            int depth);
        void HandleItemClick(const DBModels::SidebarItem& item);
        void ToggleGroup(const std::wstring& groupId);
        bool MatchesSearch(const DBModels::SidebarItem& item) const;
    };
}

namespace winrt::Gridex::factory_implementation
{
    struct SidebarPanel : SidebarPanelT<SidebarPanel, implementation::SidebarPanel>
    {
    };
}
