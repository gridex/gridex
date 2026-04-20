#include "pch.h"
#include "xaml-includes.h"
#include <winrt/Windows.UI.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include "SidebarPanel.h"
#if __has_include("SidebarPanel.g.cpp")
#include "SidebarPanel.g.cpp"
#endif

#include <algorithm>

namespace winrt::Gridex::implementation
{
    namespace mux  = winrt::Microsoft::UI::Xaml;
    namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
    namespace muxm = winrt::Microsoft::UI::Xaml::Media;

    SidebarPanel::SidebarPanel()
    {
        InitializeComponent();

        // Wire schema picker in code-behind (no IDL needed)
        this->Loaded([this](winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
        {
            // Wire Add/Delete buttons
            AddItemBtn().Click(
                [this](winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
                {
                    if (OnAddTable) OnAddTable();
                });
            DeleteItemBtn().Click(
                [this](winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
                {
                    if (!selectedItemName_.empty() && OnDeleteTable)
                        OnDeleteTable(selectedItemName_, selectedItemSchema_);
                });

            SchemaPicker().SelectionChanged(
                [this](winrt::Windows::Foundation::IInspectable const&, muxc::SelectionChangedEventArgs const&)
                {
                    if (suppressSchemaEvent_) return; // Guard during SetSchemas
                    auto selected = SchemaPicker().SelectedItem();
                    if (!selected) return;
                    auto item = selected.try_as<muxc::ComboBoxItem>();
                    if (!item) return;
                    auto content = item.Content();
                    if (!content) return;
                    auto schema = winrt::unbox_value<winrt::hstring>(content);
                    if (OnSchemaChanged)
                        OnSchemaChanged(std::wstring(schema));
                });
        });
    }

    void SidebarPanel::SetSchemas(const std::vector<std::wstring>& schemas)
    {
        suppressSchemaEvent_ = true; // Don't fire OnSchemaChanged during population
        SchemaPicker().Items().Clear();
        for (auto& s : schemas)
        {
            muxc::ComboBoxItem item;
            item.Content(winrt::box_value(winrt::hstring(s)));
            SchemaPicker().Items().Append(item);
        }
        if (!schemas.empty())
            SchemaPicker().SelectedIndex(0);
        suppressSchemaEvent_ = false;
    }

    void SidebarPanel::SetItems(const std::vector<DBModels::SidebarItem>& items)
    {
        items_ = items;
        RenderTree();
    }

    // ── Tab clicks ──────────────────────────────────────
    void SidebarPanel::ItemsTab_Click(
        winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
    {
        ItemsTab().IsChecked(true);
        QueriesTab().IsChecked(false);
        HistoryTab().IsChecked(false);
        ItemsContent().Visibility(mux::Visibility::Visible);
        QueriesContent().Visibility(mux::Visibility::Collapsed);
        HistoryContent().Visibility(mux::Visibility::Collapsed);
        SearchBox().PlaceholderText(L"Search for item...");
    }
    void SidebarPanel::QueriesTab_Click(
        winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
    {
        ItemsTab().IsChecked(false);
        QueriesTab().IsChecked(true);
        HistoryTab().IsChecked(false);
        ItemsContent().Visibility(mux::Visibility::Collapsed);
        QueriesContent().Visibility(mux::Visibility::Visible);
        HistoryContent().Visibility(mux::Visibility::Collapsed);
        SearchBox().PlaceholderText(L"Search queries...");

        // Show placeholder if empty
        if (QueriesContainer().Children().Size() == 0)
        {
            muxc::TextBlock placeholder;
            placeholder.Text(L"No saved queries yet");
            placeholder.FontSize(12.0);
            placeholder.Opacity(0.4);
            placeholder.HorizontalAlignment(mux::HorizontalAlignment::Center);
            placeholder.Margin(mux::Thickness{ 0, 40, 0, 0 });
            QueriesContainer().Children().Append(placeholder);
        }
    }
    void SidebarPanel::HistoryTab_Click(
        winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
    {
        ItemsTab().IsChecked(false);
        QueriesTab().IsChecked(false);
        HistoryTab().IsChecked(true);
        ItemsContent().Visibility(mux::Visibility::Collapsed);
        QueriesContent().Visibility(mux::Visibility::Collapsed);
        HistoryContent().Visibility(mux::Visibility::Visible);
        SearchBox().PlaceholderText(L"Search history...");
    }

    void SidebarPanel::SetHistory(const std::vector<std::pair<std::wstring, std::wstring>>& entries)
    {
        HistoryContainer().Children().Clear();

        if (entries.empty())
        {
            muxc::TextBlock placeholder;
            placeholder.Text(L"No query history");
            placeholder.FontSize(12.0);
            placeholder.Opacity(0.4);
            placeholder.HorizontalAlignment(mux::HorizontalAlignment::Center);
            placeholder.Margin(mux::Thickness{ 0, 40, 0, 0 });
            HistoryContainer().Children().Append(placeholder);
            return;
        }

        // Show newest first (reverse order)
        for (int i = static_cast<int>(entries.size()) - 1; i >= 0; i--)
        {
            auto& [timestamp, sql] = entries[i];

            muxc::Button item;
            item.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
            item.HorizontalContentAlignment(mux::HorizontalAlignment::Left);
            item.Padding(mux::Thickness{ 12, 6, 12, 6 });
            item.Background(muxm::SolidColorBrush(winrt::Windows::UI::Colors::Transparent()));
            item.BorderThickness(mux::Thickness{ 0, 0, 0, 0 });

            muxc::StackPanel sp;
            sp.Spacing(2.0);

            // Timestamp
            muxc::TextBlock timeTb;
            timeTb.Text(winrt::hstring(timestamp));
            timeTb.FontSize(10.0);
            timeTb.Opacity(0.4);
            sp.Children().Append(timeTb);

            // SQL (truncated)
            muxc::TextBlock sqlTb;
            std::wstring sqlDisplay = sql;
            if (sqlDisplay.size() > 80)
                sqlDisplay = sqlDisplay.substr(0, 80) + L"...";
            // Replace newlines
            for (auto& c : sqlDisplay)
                if (c == L'\n' || c == L'\r') c = L' ';
            sqlTb.Text(winrt::hstring(sqlDisplay));
            sqlTb.FontSize(11.0);
            sqlTb.FontFamily(mux::Media::FontFamily(L"Cascadia Code,Consolas,monospace"));
            sqlTb.TextTrimming(mux::TextTrimming::CharacterEllipsis);
            sqlTb.MaxLines(2);
            sp.Children().Append(sqlTb);

            item.Content(sp);

            // Click → fill query editor
            std::wstring capturedSql = sql;
            item.Click([this, capturedSql](auto&&, auto&&)
            {
                if (OnHistoryItemClicked)
                    OnHistoryItemClicked(capturedSql);
            });

            HistoryContainer().Children().Append(item);

            // Separator
            muxc::Border sep;
            sep.Height(1.0);
            sep.Background(muxm::SolidColorBrush(
                winrt::Windows::UI::ColorHelper::FromArgb(15, 128, 128, 128)));
            HistoryContainer().Children().Append(sep);
        }
    }

    // ── Search ──────────────────────────────────────────
    void SidebarPanel::SearchBox_TextChanged(
        winrt::Windows::Foundation::IInspectable const& sender, mux::RoutedEventArgs const&)
    {
        auto tb = sender.as<muxc::TextBox>();
        searchQuery_ = std::wstring(tb.Text());
        std::transform(searchQuery_.begin(), searchQuery_.end(), searchQuery_.begin(), ::towlower);
        RenderTree();
    }

    bool SidebarPanel::MatchesSearch(const DBModels::SidebarItem& item) const
    {
        if (searchQuery_.empty()) return true;
        std::wstring titleLower = item.title;
        std::transform(titleLower.begin(), titleLower.end(), titleLower.begin(), ::towlower);
        if (titleLower.find(searchQuery_) != std::wstring::npos) return true;
        for (auto& child : item.children)
            if (MatchesSearch(child)) return true;
        return false;
    }

    // ── Expand/Collapse ─────────────────────────────────
    void SidebarPanel::ToggleGroup(const std::wstring& groupId)
    {
        // Recursively find and toggle
        std::function<bool(std::vector<DBModels::SidebarItem>&)> toggle =
            [&](std::vector<DBModels::SidebarItem>& items) -> bool
        {
            for (auto& item : items)
            {
                if (item.id == groupId)
                {
                    item.isExpanded = !item.isExpanded;
                    return true;
                }
                if (toggle(item.children)) return true;
            }
            return false;
        };
        toggle(items_);
        RenderTree();
    }

    // ── Render ───────────────────────────────────────────
    void SidebarPanel::RenderTree()
    {
        if (!TreeContainer()) return;
        TreeContainer().Children().Clear();
        for (auto& item : items_)
        {
            if (MatchesSearch(item))
                RenderItem(TreeContainer(), item, 0);
        }
    }

    void SidebarPanel::RenderItem(
        muxc::StackPanel const& container,
        const DBModels::SidebarItem& item,
        int depth)
    {
        bool isGroup = (item.type == DBModels::SidebarItemType::Group ||
                        item.type == DBModels::SidebarItemType::Schema ||
                        item.type == DBModels::SidebarItemType::Database);

        if (isGroup)
        {
            // Mutable copy for group header (toggle needs mutable)
            auto mutableItem = item;
            RenderGroupHeader(container, mutableItem, depth);

            // Render children if expanded
            if (item.isExpanded)
            {
                for (auto& child : item.children)
                {
                    if (MatchesSearch(child))
                        RenderItem(container, child, depth + 1);
                }
            }
            return;
        }

        // ── Leaf item row ──
        muxc::Button btn;
        btn.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        btn.HorizontalContentAlignment(mux::HorizontalAlignment::Left);
        btn.Background(muxm::SolidColorBrush(winrt::Windows::UI::Colors::Transparent()));
        btn.BorderThickness(mux::Thickness{ 0,0,0,0 });
        btn.Padding(mux::Thickness{
            static_cast<double>(12 + depth * 16), 4.0, 8.0, 4.0 });
        btn.MinHeight(28.0);

        muxc::StackPanel row;
        row.Orientation(muxc::Orientation::Horizontal);
        row.Spacing(8.0);

        // Type icon with color
        muxc::FontIcon icon;
        icon.FontSize(12.0);
        icon.Glyph(winrt::hstring(DBModels::SidebarItemIcon(item.type)));

        // Color by type (matching macOS)
        if (item.type == DBModels::SidebarItemType::Table)
            icon.Foreground(muxm::SolidColorBrush(
                winrt::Windows::UI::ColorHelper::FromArgb(255, 204, 128, 51)));  // orange/brown
        else if (item.type == DBModels::SidebarItemType::View ||
                 item.type == DBModels::SidebarItemType::MaterializedView)
            icon.Foreground(muxm::SolidColorBrush(
                winrt::Windows::UI::ColorHelper::FromArgb(255, 149, 97, 226)));   // purple
        else if (item.type == DBModels::SidebarItemType::Function)
            icon.Foreground(muxm::SolidColorBrush(
                winrt::Windows::UI::ColorHelper::FromArgb(255, 56, 132, 244)));   // blue
        row.Children().Append(icon);

        // Name
        muxc::TextBlock label;
        label.Text(winrt::hstring(item.title));
        label.FontSize(12.0);
        label.VerticalAlignment(mux::VerticalAlignment::Center);
        row.Children().Append(label);

        btn.Content(row);

        DBModels::SidebarItem capturedItem = item;
        btn.Click([this, capturedItem](auto&&, auto&&)
        {
            HandleItemClick(capturedItem);
        });

        // Right-click context menu for tables/views
        if (item.type == DBModels::SidebarItemType::Table ||
            item.type == DBModels::SidebarItemType::View ||
            item.type == DBModels::SidebarItemType::MaterializedView)
        {
            muxc::MenuFlyout contextMenu;
            std::wstring tblName = item.title;

            if (currentDbType_ == DBModels::DatabaseType::Redis)
            {
                // ── Redis-specific menu (matches macOS layout) ──
                // Browse Keys - opens pattern input dialog (Ctrl+F style),
                // then loads Keys table filtered by SCAN MATCH pattern
                muxc::MenuFlyoutItem browseItem;
                browseItem.Text(L"Browse Keys...");
                browseItem.Icon(muxc::FontIcon());
                browseItem.Icon().as<muxc::FontIcon>().Glyph(L"\xE721");  // Search
                browseItem.Click([this](auto&&, auto&&)
                { if (OnBrowseRedisKeys) OnBrowseRedisKeys(); });
                contextMenu.Items().Append(browseItem);

                // Copy Name (still useful — table name = "Keys")
                muxc::MenuFlyoutItem copyItem;
                copyItem.Text(L"Copy Name");
                copyItem.Icon(muxc::FontIcon());
                copyItem.Icon().as<muxc::FontIcon>().Glyph(L"\xE8C8");
                copyItem.Click([tblName](auto&&, auto&&)
                {
                    winrt::Windows::ApplicationModel::DataTransfer::DataPackage dp;
                    dp.SetText(winrt::hstring(tblName));
                    winrt::Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(dp);
                });
                contextMenu.Items().Append(copyItem);

                contextMenu.Items().Append(muxc::MenuFlyoutSeparator());

                // Refresh — re-list keys from server
                muxc::MenuFlyoutItem refreshItem;
                refreshItem.Text(L"Refresh");
                refreshItem.Icon(muxc::FontIcon());
                refreshItem.Icon().as<muxc::FontIcon>().Glyph(L"\xE72C");
                refreshItem.Click([this](auto&&, auto&&)
                { if (OnRefreshSidebar) OnRefreshSidebar(); });
                contextMenu.Items().Append(refreshItem);

                contextMenu.Items().Append(muxc::MenuFlyoutSeparator());

                // Flush Database - destructive, host shows confirmation
                muxc::MenuFlyoutItem flushItem;
                flushItem.Text(L"Flush Database...");
                flushItem.Icon(muxc::FontIcon());
                flushItem.Icon().as<muxc::FontIcon>().Glyph(L"\xE74D");
                flushItem.Click([this](auto&&, auto&&)
                { if (OnFlushRedisDb) OnFlushRedisDb(); });
                contextMenu.Items().Append(flushItem);

                btn.ContextFlyout(contextMenu);
            }
            else
            {
                // ── SQL DB menu (Postgres / MySQL / SQLite) ──
                // Open
                muxc::MenuFlyoutItem openItem;
                openItem.Text(L"Open");
                openItem.Icon(muxc::FontIcon());
                openItem.Icon().as<muxc::FontIcon>().Glyph(L"\xE8A7");
                openItem.Click([this, capturedItem](auto&&, auto&&)
                { HandleItemClick(capturedItem); });
                contextMenu.Items().Append(openItem);

                // Copy Name
                muxc::MenuFlyoutItem copyItem;
                copyItem.Text(L"Copy Name");
                copyItem.Icon(muxc::FontIcon());
                copyItem.Icon().as<muxc::FontIcon>().Glyph(L"\xE8C8");
                copyItem.Click([tblName](auto&&, auto&&)
                {
                    winrt::Windows::ApplicationModel::DataTransfer::DataPackage dp;
                    dp.SetText(winrt::hstring(tblName));
                    winrt::Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(dp);
                });
                contextMenu.Items().Append(copyItem);

                contextMenu.Items().Append(muxc::MenuFlyoutSeparator());

                // Export submenu
                muxc::MenuFlyoutSubItem exportSub;
                exportSub.Text(L"Export");
                exportSub.Icon(muxc::FontIcon());
                exportSub.Icon().as<muxc::FontIcon>().Glyph(L"\xEDE1");

                std::wstring exportName = item.title;
                std::wstring exportSchema = item.schema;

                muxc::MenuFlyoutItem csvItem;
                csvItem.Text(L"CSV");
                csvItem.Click([this, exportName, exportSchema](auto&&, auto&&)
                { if (OnExportTable) OnExportTable(exportName, exportSchema, L"csv"); });
                exportSub.Items().Append(csvItem);

                muxc::MenuFlyoutItem jsonItem;
                jsonItem.Text(L"JSON");
                jsonItem.Click([this, exportName, exportSchema](auto&&, auto&&)
                { if (OnExportTable) OnExportTable(exportName, exportSchema, L"json"); });
                exportSub.Items().Append(jsonItem);

                muxc::MenuFlyoutItem sqlItem;
                sqlItem.Text(L"SQL (INSERT)");
                sqlItem.Click([this, exportName, exportSchema](auto&&, auto&&)
                { if (OnExportTable) OnExportTable(exportName, exportSchema, L"sql"); });
                exportSub.Items().Append(sqlItem);

                contextMenu.Items().Append(exportSub);

                // Import — load CSV/JSON/SQL into THIS specific table
                muxc::MenuFlyoutItem importItem;
                importItem.Text(L"Import...");
                importItem.Icon(muxc::FontIcon());
                importItem.Icon().as<muxc::FontIcon>().Glyph(L"\xE8B5");
                importItem.Click([this, exportName, exportSchema](auto&&, auto&&)
                { if (OnImportTable) OnImportTable(exportName, exportSchema); });
                contextMenu.Items().Append(importItem);

                // Truncate + Delete — destructive. Only offered for
                // real tables (not views) since views can't be truncated
                // and DROP VIEW needs a different code path. Host shows
                // a confirmation dialog before running the statement.
                if (item.type == DBModels::SidebarItemType::Table)
                {
                    contextMenu.Items().Append(muxc::MenuFlyoutSeparator());

                    muxc::MenuFlyoutItem truncateItem;
                    truncateItem.Text(L"Truncate Table...");
                    truncateItem.Icon(muxc::FontIcon());
                    truncateItem.Icon().as<muxc::FontIcon>().Glyph(L"\xE74D"); // eraser-ish; trash is close enough
                    truncateItem.Click([this, exportName, exportSchema](auto&&, auto&&)
                    { if (OnTruncateTable) OnTruncateTable(exportName, exportSchema); });
                    contextMenu.Items().Append(truncateItem);

                    muxc::MenuFlyoutItem deleteItem;
                    deleteItem.Text(L"Delete Table...");
                    deleteItem.Icon(muxc::FontIcon());
                    deleteItem.Icon().as<muxc::FontIcon>().Glyph(L"\xE74D"); // trash
                    deleteItem.Click([this, exportName, exportSchema](auto&&, auto&&)
                    { if (OnDeleteTable) OnDeleteTable(exportName, exportSchema); });
                    contextMenu.Items().Append(deleteItem);
                }

                btn.ContextFlyout(contextMenu);
            }
        }

        container.Children().Append(btn);
    }

    void SidebarPanel::RenderGroupHeader(
        muxc::StackPanel const& container,
        DBModels::SidebarItem& item,
        int depth)
    {
        muxc::Button btn;
        btn.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        btn.HorizontalContentAlignment(mux::HorizontalAlignment::Left);
        btn.Background(muxm::SolidColorBrush(winrt::Windows::UI::Colors::Transparent()));
        btn.BorderThickness(mux::Thickness{ 0,0,0,0 });
        btn.Padding(mux::Thickness{
            static_cast<double>(8 + depth * 16), 5.0, 8.0, 5.0 });
        btn.MinHeight(30.0);

        muxc::Grid row;
        muxc::ColumnDefinition c1;
        c1.Width(mux::GridLengthHelper::FromValueAndType(1, mux::GridUnitType::Star));
        muxc::ColumnDefinition c2;
        c2.Width(mux::GridLengthHelper::FromPixels(30));
        row.ColumnDefinitions().Append(c1);
        row.ColumnDefinitions().Append(c2);

        // Left: disclosure arrow + icon + title
        muxc::StackPanel left;
        left.Orientation(muxc::Orientation::Horizontal);
        left.Spacing(6.0);
        left.VerticalAlignment(mux::VerticalAlignment::Center);

        // Disclosure triangle
        muxc::FontIcon arrow;
        arrow.FontSize(8.0);
        arrow.Glyph(item.isExpanded ? L"\xE70D" : L"\xE76C");  // ChevronDown : ChevronRight
        arrow.Opacity(0.5);
        arrow.VerticalAlignment(mux::VerticalAlignment::Center);
        left.Children().Append(arrow);

        // Folder icon
        muxc::FontIcon folderIcon;
        folderIcon.FontSize(12.0);
        folderIcon.Glyph(L"\xE8B7");  // Folder
        folderIcon.Opacity(0.6);
        left.Children().Append(folderIcon);

        // Title
        muxc::TextBlock title;
        title.Text(winrt::hstring(item.title));
        title.FontSize(12.0);
        title.VerticalAlignment(mux::VerticalAlignment::Center);
        left.Children().Append(title);

        muxc::Grid::SetColumn(left, 0);
        row.Children().Append(left);

        // Right: count badge
        if (item.count > 0)
        {
            muxc::TextBlock badge;
            badge.Text(winrt::hstring(std::to_wstring(item.count)));
            badge.FontSize(10.0);
            badge.Opacity(0.4);
            badge.VerticalAlignment(mux::VerticalAlignment::Center);
            badge.HorizontalAlignment(mux::HorizontalAlignment::Right);
            muxc::Grid::SetColumn(badge, 1);
            row.Children().Append(badge);
        }

        btn.Content(row);

        // Click toggles expand/collapse
        std::wstring groupId = item.id;
        btn.Click([this, groupId](auto&&, auto&&)
        {
            ToggleGroup(groupId);
        });

        // Right-click context menu on Database/Schema groups -> Show ER Diagram
        if (item.type == DBModels::SidebarItemType::Database ||
            item.type == DBModels::SidebarItemType::Schema)
        {
            muxc::MenuFlyout groupMenu;
            muxc::MenuFlyoutItem erItem;
            erItem.Text(L"Show ER Diagram");
            erItem.Icon(muxc::FontIcon());
            erItem.Icon().as<muxc::FontIcon>().Glyph(L"\xF169");
            std::wstring groupSchema = item.title;  // schema/db name
            erItem.Click([this, groupSchema](auto&&, auto&&)
            {
                if (OnShowERDiagram) OnShowERDiagram(groupSchema);
            });
            groupMenu.Items().Append(erItem);
            btn.ContextFlyout(groupMenu);
        }

        container.Children().Append(btn);
    }

    void SidebarPanel::HandleItemClick(const DBModels::SidebarItem& item)
    {
        // Track selection for delete button
        selectedItemName_ = item.title;
        selectedItemSchema_ = item.schema;

        if (item.type == DBModels::SidebarItemType::Table ||
            item.type == DBModels::SidebarItemType::View  ||
            item.type == DBModels::SidebarItemType::MaterializedView ||
            item.type == DBModels::SidebarItemType::Function)
        {
            if (OnItemSelected)
                OnItemSelected(item.title, item.schema);
        }
    }
}
