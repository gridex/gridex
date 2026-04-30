#include "pch.h"
#include "xaml-includes.h"
#include "DataGridView.h"
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.System.h>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <functional>
#include <memory>
#include <algorithm>
#include <cmath>
#if __has_include("DataGridView.g.cpp")
#include "DataGridView.g.cpp"
#endif

namespace winrt::Gridex::implementation
{
    namespace mux  = winrt::Microsoft::UI::Xaml;
    namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
    namespace muxm = winrt::Microsoft::UI::Xaml::Media;

    // Forward declaration — defined later in the file but called from
    // BuildRowElement's DoubleTapped lambda above its definition site.
    static muxc::StackPanel findRowPanelByDataIndex(
        muxc::Panel const& container, int dataIdx);

    // Cap on characters fed to a row's TextBlock. Original 80-char limit
    // forced users into "right-click → Copy cell" because the cell's
    // underlying Text was the truncated string — Ctrl+C / drag-select
    // only saw 80 chars. Bumped to 4096 so the common case (paragraph-
    // sized text fields, most JSON/URLs) round-trips on a normal cell
    // copy. Above 4096 we still truncate to keep the layout measure
    // pass bounded for multi-KB HTML / log-line cells; "Copy cell"
    // menu always reads from data_.rows directly so it gets every byte.
    static constexpr size_t MAX_CELL_DISPLAY_CHARS = 4096;

    void DataGridView::EnsureCellStyles()
    {
        if (cellStyle_) return; // already cached
        auto resources = this->Resources();
        if (auto v = resources.TryLookup(winrt::box_value(L"GridCellStyle")))
            cellStyle_ = v.try_as<mux::Style>();
        if (auto v = resources.TryLookup(winrt::box_value(L"GridRowNumStyle")))
            rowNumStyle_ = v.try_as<mux::Style>();
    }

    DataGridView::DataGridView()
    {
        InitializeComponent();

        // Rows are plain StackPanels (not focusable). Without this the
        // UserControl never receives focus after a row click, so KeyDown
        // (Delete, Ctrl+C) never fires. Making the UserControl itself a
        // tab stop lets SelectRow programmatically park focus here.
        this->IsTabStop(true);
        this->UseSystemFocusVisuals(false);

        this->KeyDown([this](winrt::Windows::Foundation::IInspectable const& sender,
                             mux::Input::KeyRoutedEventArgs const& e)
        {
            Grid_KeyDown(sender, e);
        });

        // Pointer pressed anywhere in the grid → flush any in-flight
        // cell edit. Bubbles from children, so clicking a row, the empty
        // area below rows, or the scroll background all funnel here.
        // Skip flush when the press originates inside a TextBox so the
        // user can reposition the caret in the cell they're editing.
        this->AddHandler(
            mux::UIElement::PointerPressedEvent(),
            winrt::box_value(
                mux::Input::PointerEventHandler(
                    [this](winrt::Windows::Foundation::IInspectable const&,
                           mux::Input::PointerRoutedEventArgs const& e)
                    {
                        // Walk up from the hit target; bail if any
                        // ancestor is a TextBox (click landed inside
                        // the active edit, not outside it).
                        auto src = e.OriginalSource().try_as<mux::DependencyObject>();
                        while (src)
                        {
                            if (src.try_as<muxc::TextBox>()) return;
                            src = muxm::VisualTreeHelper::GetParent(src);
                        }
                        FlushEdit();
                    })),
            true /* handledEventsToo */);

        // Right-click context menu. Host wires OnRefreshRequested /
        // OnDeleteRequested in WorkspacePage; unset = no-op. Delete is
        // disabled for read-only grids (e.g. Redis projections) and when
        // no row is selected.
        {
            muxc::MenuFlyout contextMenu;

            // Copy cell — reads the FULL value out of data_.rows so the
            // user gets every byte even though the cell visual was
            // truncated at MAX_CELL_DISPLAY_CHARS.
            muxc::MenuFlyoutItem copyCellItem;
            copyCellItem.Text(L"Copy cell");
            muxc::FontIcon copyIcon;
            copyIcon.Glyph(L"\xE8C8");
            copyCellItem.Icon(copyIcon);
            copyCellItem.Click([this](auto&&, auto&&) { CopyCellToClipboard(); });
            contextMenu.Items().Append(copyCellItem);

            contextMenu.Items().Append(muxc::MenuFlyoutSeparator{});

            muxc::MenuFlyoutItem refreshItem;
            refreshItem.Text(L"Refresh");
            muxc::FontIcon refreshIcon;
            refreshIcon.Glyph(L"\xE72C");
            refreshItem.Icon(refreshIcon);
            refreshItem.Click([this](auto&&, auto&&)
            {
                if (OnRefreshRequested) OnRefreshRequested();
            });
            contextMenu.Items().Append(refreshItem);

            contextMenu.Items().Append(muxc::MenuFlyoutSeparator{});

            muxc::MenuFlyoutItem deleteItem;
            deleteItem.Text(L"Delete Row");
            muxc::FontIcon deleteIcon;
            deleteIcon.Glyph(L"\xE74D");
            deleteItem.Icon(deleteIcon);
            deleteItem.Click([this](auto&&, auto&&)
            {
                if (OnDeleteRequested) OnDeleteRequested();
            });
            contextMenu.Items().Append(deleteItem);

            // "View Relationship" is an opt-in item: stays Collapsed until a
            // host wires OnViewRelationshipsRequested. OSS builds never set
            // it, so the menu only shows Refresh + Delete there. Enterprise
            // builds wire it per connection (relational DBs only).
            auto viewRelSeparator = muxc::MenuFlyoutSeparator{};
            contextMenu.Items().Append(viewRelSeparator);

            muxc::MenuFlyoutItem viewRelItem;
            viewRelItem.Text(L"View Relationship");
            muxc::FontIcon viewRelIcon;
            viewRelIcon.Glyph(L"\xE71B"); // Link
            viewRelItem.Icon(viewRelIcon);
            viewRelItem.Click([this](auto&&, auto&&)
            {
                if (OnViewRelationshipsRequested) OnViewRelationshipsRequested();
            });
            contextMenu.Items().Append(viewRelItem);

            // Gate Delete + View Relationship + Copy cell at flyout open
            // time so the menu reflects the grid's actual state each
            // right-click.
            contextMenu.Opening([this, copyCellItem, deleteItem, viewRelItem, viewRelSeparator](auto&&, auto&&)
            {
                copyCellItem.IsEnabled(selectedRow_ >= 0 && contextCol_ >= 0);
                deleteItem.IsEnabled(!readOnly_ && selectedRow_ >= 0);
                bool showViewRel = static_cast<bool>(OnViewRelationshipsRequested);
                auto vis = showViewRel ? mux::Visibility::Visible
                                       : mux::Visibility::Collapsed;
                viewRelSeparator.Visibility(vis);
                viewRelItem.Visibility(vis);
                viewRelItem.IsEnabled(showViewRel && selectedRow_ >= 0);
            });

            this->ContextFlyout(contextMenu);
        }
        this->Loaded([this](winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
        {
            DataScroller().ViewChanged(
                [this](winrt::Windows::Foundation::IInspectable const& sender,
                       muxc::ScrollViewerViewChangedEventArgs const& e)
                {
                    DataScroller_ViewChanged(sender, e);
                });

            // Shift + mouse wheel → horizontal scroll.
            // Attached to the inner StackPanel (DataRows) rather than the
            // ScrollViewer itself: ScrollViewer installs its own internal
            // wheel handler for vertical scroll, so a handler on the child
            // fires first and can intercept Shift-wheel before the SV
            // consumes it. No Shift → leave Handled=false so the default
            // vertical-scroll path still runs.
            DataRows().PointerWheelChanged(
                [this](winrt::Windows::Foundation::IInspectable const&,
                       mux::Input::PointerRoutedEventArgs const& e)
                {
                    auto mods = e.KeyModifiers();
                    bool shiftHeld =
                        (static_cast<uint32_t>(mods) &
                         static_cast<uint32_t>(winrt::Windows::System::VirtualKeyModifiers::Shift)) != 0;
                    if (!shiftHeld) return;

                    int delta = e.GetCurrentPoint(nullptr).Properties().MouseWheelDelta();
                    if (delta == 0) return;

                    // Wheel delta is ±120 per click; subtract so wheel-up
                    // scrolls left (matches the Windows convention used by
                    // VS Code, Excel, File Explorer, etc.).
                    double newH = DataScroller().HorizontalOffset() - static_cast<double>(delta);
                    if (newH < 0.0) newH = 0.0;
                    DataScroller().ChangeView(newH, nullptr, nullptr, true);
                    e.Handled(true);
                });
        });
    }

    void DataGridView::SetColumnMetadata(const std::vector<DBModels::ColumnInfo>& meta)
    {
        columnMetadata_ = meta;
        // Re-render headers only if we already have data laid out;
        // otherwise the next SetData call will pick up the metadata.
        if (!data_.columnNames.empty())
            BuildHeaders();
    }

    void DataGridView::SetData(const DBModels::QueryResult& result)
    {
        data_        = result;
        selectedRow_ = -1;
        // New data set = prior row indices are meaningless; drop the
        // pending-delete visual bookkeeping. Host (WorkspacePage) also
        // discards its ChangeTracker on table switch, so the two stay
        // in sync.
        deletedRows_.clear();

        // EXPLAIN / EXPLAIN ANALYZE (and similar verbatim single-column
        // outputs) must not be truncated at 80 chars — each row is a
        // full plan line the user needs to read end-to-end. Detect the
        // PostgreSQL convention (single "QUERY PLAN" column) and flip
        // off the per-cell truncation for this render pass only.
        skipCellTruncation_ =
            (data_.columnNames.size() == 1 &&
             data_.columnNames[0] == L"QUERY PLAN");

        ComputeColumnWidths();
        BuildHeaders();
        BuildRows();
    }

    const DBModels::TableRow* DataGridView::GetSelectedRow() const
    {
        if (selectedRow_ >= 0 && selectedRow_ < static_cast<int>(data_.rows.size()))
            return &data_.rows[selectedRow_];
        return nullptr;
    }

    // ── Compute uniform column widths ──────────────────
    void DataGridView::ComputeColumnWidths()
    {
        size_t nCols = data_.columnNames.size();
        columnWidths_.clear();
        columnWidths_.resize(nCols, COL_DEFAULT_WIDTH);

        if (nCols == 0) return;

        // Single-column verbatim mode (EXPLAIN / EXPLAIN ANALYZE):
        // widen the column to fit the longest plan line so horizontal
        // scroll reveals the full content. TextTrimming in the cached
        // cell style would otherwise clip at the viewport width even
        // though the string itself is intact in memory.
        if (skipCellTruncation_ && nCols == 1)
        {
            size_t maxChars = data_.columnNames[0].size();
            for (const auto& row : data_.rows)
            {
                auto it = row.find(data_.columnNames[0]);
                if (it != row.end() && it->second.size() > maxChars)
                    maxChars = it->second.size();
            }
            // Rough monospace estimate: ~7px per char + 24px padding.
            // Cap below 4000px to keep scroll bounds sane on giant plans.
            double width = static_cast<double>(maxChars) * 7.0 + 24.0;
            if (width < COL_DEFAULT_WIDTH) width = COL_DEFAULT_WIDTH;
            if (width > 4000.0) width = 4000.0;
            columnWidths_[0] = width;
            return;
        }

        // Distribute available width evenly ONLY when there's leftover
        // space at default width. Never shrink below COL_DEFAULT_WIDTH —
        // 30+ skinny columns are unreadable; horizontal scroll is the
        // standard DB-tool behavior for wide tables.
        double availWidth = this->ActualWidth() - ROW_NUM_WIDTH - 20.0; // 20px scrollbar
        if (availWidth > 0 && nCols > 0)
        {
            double evenWidth = availWidth / static_cast<double>(nCols);
            if (evenWidth < COL_DEFAULT_WIDTH) evenWidth = COL_DEFAULT_WIDTH;
            if (evenWidth > COL_MAX_WIDTH) evenWidth = COL_MAX_WIDTH;
            for (size_t i = 0; i < nCols; i++)
                columnWidths_[i] = evenWidth;
        }
    }

    // ── Header rendering with sort + resize handle ─────
    void DataGridView::BuildHeaders()
    {
        HeaderRow().Children().Clear();

        // Row-number column
        muxc::Border numHeader;
        numHeader.Width(ROW_NUM_WIDTH);
        numHeader.MinHeight(ROW_HEIGHT);
        numHeader.Padding(mux::Thickness{ 4.0, 0.0, 4.0, 0.0 });
        numHeader.BorderBrush(muxm::SolidColorBrush(
            winrt::Windows::UI::ColorHelper::FromArgb(40, 128, 128, 128)));
        numHeader.BorderThickness(mux::Thickness{ 0, 0, 1, 0 });
        {
            muxc::TextBlock lbl;
            lbl.Text(L"#");
            lbl.FontSize(11.0);
            lbl.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            lbl.VerticalAlignment(mux::VerticalAlignment::Center);
            lbl.HorizontalAlignment(mux::HorizontalAlignment::Center);
            lbl.Opacity(0.5);
            numHeader.Child(lbl);
        }
        HeaderRow().Children().Append(numHeader);

        for (size_t ci = 0; ci < data_.columnNames.size(); ci++)
        {
            auto& colName = data_.columnNames[ci];
            double colWidth = (ci < columnWidths_.size()) ? columnWidths_[ci] : COL_DEFAULT_WIDTH;

            // Container: header cell + resize grip
            muxc::Grid cellGrid;
            cellGrid.Width(colWidth);
            cellGrid.MinHeight(ROW_HEIGHT);

            // Two columns: content (star) + resize grip (4px)
            auto contentCol = mux::GridLength{ 1.0, mux::GridUnitType::Star };
            auto gripCol    = mux::GridLength{ 8.0, mux::GridUnitType::Pixel };
            muxc::ColumnDefinition cd1; cd1.Width(contentCol);
            muxc::ColumnDefinition cd2; cd2.Width(gripCol);
            cellGrid.ColumnDefinitions().Append(cd1);
            cellGrid.ColumnDefinitions().Append(cd2);

            // Header button (sort on click)
            muxc::Button headerBtn;
            headerBtn.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
            headerBtn.VerticalAlignment(mux::VerticalAlignment::Stretch);
            headerBtn.Padding(mux::Thickness{ 8.0, 0.0, 4.0, 0.0 });
            headerBtn.Background(muxm::SolidColorBrush(winrt::Windows::UI::Colors::Transparent()));
            headerBtn.BorderThickness(mux::Thickness{ 0, 0, 0, 0 });
            muxc::Grid::SetColumn(headerBtn, 0);

            // Look up optional metadata (PK / FK / nullable / full type
            // string) by column name. Header falls back gracefully if
            // SetColumnMetadata was never called (e.g., ad-hoc query view).
            const DBModels::ColumnInfo* meta = nullptr;
            for (const auto& m : columnMetadata_)
            {
                if (m.name == colName) { meta = &m; break; }
            }

            // Resolve type string: prefer metadata (has length info like
            // varchar(255)), else QueryResult.columnTypes.
            std::wstring colType;
            if (meta && !meta->dataType.empty()) colType = meta->dataType;
            else if (ci < data_.columnTypes.size()) colType = data_.columnTypes[ci];

            // Header content layout: Grid with [auto PK] [auto FK] [* label]
            // [auto sort]. Star column on the label lets it auto-stretch to
            // whatever space is left after the icons — when the user resizes
            // the outer cellGrid, the label's available width tracks live
            // via layout pass, no manual MaxWidth bookkeeping needed.
            muxc::Grid headerContent;
            {
                muxc::ColumnDefinition pkCol, fkCol, labelCol, sortCol;
                pkCol.Width(mux::GridLength{ 0.0, mux::GridUnitType::Auto });
                fkCol.Width(mux::GridLength{ 0.0, mux::GridUnitType::Auto });
                labelCol.Width(mux::GridLength{ 1.0, mux::GridUnitType::Star });
                sortCol.Width(mux::GridLength{ 0.0, mux::GridUnitType::Auto });
                headerContent.ColumnDefinitions().Append(pkCol);
                headerContent.ColumnDefinitions().Append(fkCol);
                headerContent.ColumnDefinitions().Append(labelCol);
                headerContent.ColumnDefinitions().Append(sortCol);
            }

            // PK icon (key glyph) — gold-ish tint so it reads as a badge.
            if (meta && meta->isPrimaryKey)
            {
                muxc::FontIcon pkIcon;
                pkIcon.Glyph(L"\xE192");
                pkIcon.FontSize(10.0);
                pkIcon.Margin(mux::Thickness{ 0.0, 0.0, 4.0, 0.0 });
                pkIcon.Foreground(muxm::SolidColorBrush(
                    winrt::Windows::UI::ColorHelper::FromArgb(255, 220, 170, 40)));
                muxc::ToolTip pkTip;
                pkTip.Content(winrt::box_value(winrt::hstring(L"Primary Key")));
                muxc::ToolTipService::SetToolTip(pkIcon, pkTip);
                muxc::Grid::SetColumn(pkIcon, 0);
                headerContent.Children().Append(pkIcon);
            }

            // FK icon (link glyph). Wrap in a button so the click jumps
            // to the referenced table — separate from header sort click.
            if (meta && meta->isForeignKey)
            {
                muxc::Button fkBtn;
                fkBtn.Padding(mux::Thickness{ 2.0, 0.0, 2.0, 0.0 });
                fkBtn.Margin(mux::Thickness{ 0.0, 0.0, 4.0, 0.0 });
                fkBtn.Background(muxm::SolidColorBrush(winrt::Windows::UI::Colors::Transparent()));
                fkBtn.BorderThickness(mux::Thickness{ 0, 0, 0, 0 });
                fkBtn.MinWidth(0.0);
                fkBtn.MinHeight(0.0);
                muxc::FontIcon fkIcon;
                fkIcon.Glyph(L"\xE71B");
                fkIcon.FontSize(10.0);
                fkIcon.Foreground(muxm::SolidColorBrush(
                    winrt::Windows::UI::ColorHelper::FromArgb(255, 88, 166, 255)));
                fkBtn.Content(fkIcon);

                std::wstring refTable  = meta->fkReferencedTable;
                std::wstring refColumn = meta->fkReferencedColumn;
                std::wstring fkTipText = L"Foreign Key → " + refTable;
                if (!refColumn.empty()) fkTipText += L"." + refColumn;
                fkTipText += L"\n(click to open referenced table)";
                muxc::ToolTip fkTip;
                fkTip.Content(winrt::box_value(winrt::hstring(fkTipText)));
                muxc::ToolTipService::SetToolTip(fkBtn, fkTip);

                fkBtn.Click([this, refTable](auto&&, auto&&)
                {
                    if (OnForeignKeyClicked && !refTable.empty())
                        OnForeignKeyClicked(refTable, L"");
                });
                muxc::Grid::SetColumn(fkBtn, 1);
                headerContent.Children().Append(fkBtn);
            }

            // Label at column 2 — fills the star column. NO MaxWidth — the
            // grid layout constrains it; trimming kicks in only when the
            // column is actually narrower than the text. Nullable columns
            // render in italics so the distinction is obvious at a glance
            // without stealing extra header space for a separate indicator.
            muxc::TextBlock lbl;
            lbl.Text(winrt::hstring(colName));
            lbl.FontSize(11.0);
            lbl.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            lbl.VerticalAlignment(mux::VerticalAlignment::Center);
            lbl.TextTrimming(mux::TextTrimming::CharacterEllipsis);
            lbl.TextWrapping(mux::TextWrapping::NoWrap);
            if (meta && meta->nullable)
                lbl.FontStyle(winrt::Windows::UI::Text::FontStyle::Italic);
            muxc::Grid::SetColumn(lbl, 2);
            headerContent.Children().Append(lbl);

            // Tooltip on the whole header button summarising every piece
            // of metadata we have — the spot users reach for when the
            // column's declared type or constraints are not obvious.
            {
                std::wstring tipText;
                if (!colType.empty()) tipText = colType;
                if (meta)
                {
                    tipText += tipText.empty() ? L"" : L"\n";
                    tipText += meta->nullable ? L"NULL" : L"NOT NULL";
                    if (meta->isPrimaryKey) tipText += L"\nPRIMARY KEY";
                    if (meta->isForeignKey)
                    {
                        tipText += L"\nFK → " + meta->fkReferencedTable;
                        if (!meta->fkReferencedColumn.empty())
                            tipText += L"." + meta->fkReferencedColumn;
                    }
                    if (!meta->defaultValue.empty())
                        tipText += L"\nDEFAULT " + meta->defaultValue;
                }
                if (!tipText.empty())
                {
                    muxc::ToolTip headerTip;
                    headerTip.Content(winrt::box_value(winrt::hstring(tipText)));
                    muxc::ToolTipService::SetToolTip(headerBtn, headerTip);
                }
            }

            // Sort indicator at column 3 (only if this column is the sort key)
            if (sortColumn_ == colName)
            {
                muxc::FontIcon sortIcon;
                sortIcon.FontSize(9.0);
                sortIcon.Glyph(sortAscending_ ? L"\xE70E" : L"\xE70D");
                sortIcon.Opacity(0.7);
                sortIcon.Margin(mux::Thickness{ 4.0, 0.0, 0.0, 0.0 });
                muxc::Grid::SetColumn(sortIcon, 3);
                headerContent.Children().Append(sortIcon);
            }

            headerBtn.Content(headerContent);

            std::wstring col = colName;
            headerBtn.Click([this, col](auto&&, auto&&)
            {
                if (sortColumn_ == col)
                    sortAscending_ = !sortAscending_;
                else
                {
                    sortColumn_ = col;
                    sortAscending_ = true;
                }
                BuildHeaders();
                if (OnSortRequested)
                    OnSortRequested(sortColumn_, sortAscending_);
            });

            cellGrid.Children().Append(headerBtn);

            // Resize grip — wider hit area (8px) with visible line (1px) inside
            muxc::Grid resizeGrip;
            resizeGrip.Width(8.0);
            resizeGrip.Background(muxm::SolidColorBrush(winrt::Windows::UI::Colors::Transparent()));
            muxc::Grid::SetColumn(resizeGrip, 1);

            // Visible 1px line centered in the grip
            muxc::Border gripLine;
            gripLine.Width(1.0);
            gripLine.HorizontalAlignment(mux::HorizontalAlignment::Center);
            gripLine.Background(muxm::SolidColorBrush(
                winrt::Windows::UI::ColorHelper::FromArgb(50, 128, 128, 128)));
            gripLine.IsHitTestVisible(false);
            resizeGrip.Children().Append(gripLine);

            // Cursor: use IUIElementProtected.ProtectedCursor — the only
            // reliable WinUI 3 cursor API. cppwinrt strips ProtectedCursor
            // from the public projection of UIElement, but the ABI interface
            // winrt::Microsoft::UI::Xaml::IUIElementProtected is publicly
            // declared and any UIElement implements it. Set it inside the
            // grip's Loaded handler so it runs after the element is fully
            // attached to the visual tree (ProtectedCursor throws otherwise).
            //
            // Win32 SetCursor() in PointerMoved was tried first but races
            // with the WinUI input pipeline's WM_SETCURSOR handling and
            // never sticks visually.
            resizeGrip.Loaded(
                [](winrt::Windows::Foundation::IInspectable const& sender,
                   mux::RoutedEventArgs const&)
                {
                    static auto sizeWECursor =
                        winrt::Microsoft::UI::Input::InputSystemCursor::Create(
                            winrt::Microsoft::UI::Input::InputSystemCursorShape::SizeWestEast);
                    if (auto el = sender.try_as<mux::UIElement>())
                        el.as<winrt::Microsoft::UI::Xaml::IUIElementProtected>()
                          .ProtectedCursor(sizeWECursor);
                });

            // Resize drag via PointerPressed/Moved/Released
            int colIndex = static_cast<int>(ci);
            resizeGrip.PointerPressed([this, colIndex](winrt::Windows::Foundation::IInspectable const& sender,
                                                        mux::Input::PointerRoutedEventArgs const& e)
            {
                resizingCol_ = colIndex;
                auto point = e.GetCurrentPoint(this->try_as<mux::UIElement>());
                resizeStartX_ = point.Position().X;
                resizeStartWidth_ = (colIndex < static_cast<int>(columnWidths_.size()))
                    ? columnWidths_[colIndex] : COL_DEFAULT_WIDTH;
                auto el = sender.try_as<mux::UIElement>();
                if (el) el.CapturePointer(e.Pointer());
                e.Handled(true);
            });

            resizeGrip.PointerMoved([this](winrt::Windows::Foundation::IInspectable const&,
                                           mux::Input::PointerRoutedEventArgs const& e)
            {
                if (resizingCol_ < 0) { return; }
                auto point = e.GetCurrentPoint(this->try_as<mux::UIElement>());
                double delta = point.Position().X - resizeStartX_;
                double newWidth = resizeStartWidth_ + delta;
                if (newWidth < COL_MIN_WIDTH) newWidth = COL_MIN_WIDTH;
                if (newWidth > COL_MAX_WIDTH) newWidth = COL_MAX_WIDTH;
                if (resizingCol_ < static_cast<int>(columnWidths_.size()))
                {
                    columnWidths_[resizingCol_] = newWidth;
                    // Update header cell width live
                    // Header: index 0 = row num, then columns start at 1
                    uint32_t headerIdx = static_cast<uint32_t>(resizingCol_) + 1;
                    if (headerIdx < HeaderRow().Children().Size())
                    {
                        auto headerEl = HeaderRow().Children().GetAt(headerIdx);
                        auto grid = headerEl.try_as<muxc::Grid>();
                        if (grid) grid.Width(newWidth);
                    }
                }
                e.Handled(true);
            });

            resizeGrip.PointerReleased([this](winrt::Windows::Foundation::IInspectable const& sender,
                                              mux::Input::PointerRoutedEventArgs const& e)
            {
                if (resizingCol_ >= 0)
                {
                    auto el = sender.try_as<mux::UIElement>();
                    if (el) el.ReleasePointerCapture(e.Pointer());
                    resizingCol_ = -1;
                    // Rebuild rows with new column widths
                    BuildRows();
                }
                e.Handled(true);
            });

            cellGrid.Children().Append(resizeGrip);

            // Right border for the whole cell
            muxc::Border rightBorder;
            rightBorder.HorizontalAlignment(mux::HorizontalAlignment::Right);
            rightBorder.Width(1.0);
            rightBorder.Background(muxm::SolidColorBrush(
                winrt::Windows::UI::ColorHelper::FromArgb(40, 128, 128, 128)));
            rightBorder.IsHitTestVisible(false);
            muxc::Grid::SetColumnSpan(rightBorder, 2);
            cellGrid.Children().Append(rightBorder);

            HeaderRow().Children().Append(cellGrid);
        }
    }

    // ── Row rendering (full sync) ───────────────────────
    //
    // Render every row of the current page in one pass. The user wants the
    // whole page on first paint (database tool feel). Per-cell cost is the
    // bottleneck on wide tables, so BuildRowElement is hand-tuned to issue
    // the minimum number of WinRT property crossings per TextBlock.
    //
    // After this returns, every row is realized in the visual tree and the
    // ScrollViewer handles scrolling natively — no per-frame rebuild work,
    // no scroll lag.
    void DataGridView::BuildRows()
    {
        EnsureCellStyles();

        auto rows = DataRows();
        rows.Visibility(mux::Visibility::Collapsed);  // coalesce layout passes
        rows.Children().Clear();

        const int totalRows = static_cast<int>(data_.rows.size());

        // Chunked render to keep the UI thread responsive on large
        // result sets. Building 3k+ row visuals in one synchronous
        // pass froze the app for 5+ seconds. The async tail (BuildRowsTail)
        // co_awaits resume_after between chunks so layout + paint actually
        // pump between chunks — TryEnqueue chains and DispatcherQueueTimer
        // both proved insufficient.
        constexpr int kFirstChunk = 100;

        int firstEnd = (std::min)(totalRows, kFirstChunk);
        for (int i = 0; i < firstEnd; ++i)
            rows.Children().Append(BuildRowElement(i));

        rows.Visibility(mux::Visibility::Visible);

        // Re-apply selection highlight (selectedRow_ may carry over from
        // a previous load if the host hasn't reset it).
        if (selectedRow_ >= 0 && selectedRow_ < firstEnd)
            HighlightRow(selectedRow_);

        if (firstEnd >= totalRows) return;

        // Bump generation token so any in-flight tail from a previous
        // SetData call notices it has been superseded and bails out.
        const uint64_t gen = ++buildRowsGeneration_;
        BuildRowsTail(gen, firstEnd, totalRows);
    }

    namespace {
        // Resume the awaiting coroutine on the UI dispatcher's thread.
        // WinAppSDK doesn't ship a resume_foreground overload for
        // Microsoft.UI.Dispatching.DispatcherQueue, so this is the
        // equivalent custom awaiter — TryEnqueue parks h.resume() on
        // the UI thread and the coroutine continues there.
        struct ResumeOnDispatcher
        {
            winrt::Microsoft::UI::Dispatching::DispatcherQueue dq;
            bool await_ready() const noexcept { return false; }
            void await_suspend(std::coroutine_handle<> h) const
            {
                dq.TryEnqueue([h] { h.resume(); });
            }
            void await_resume() const noexcept {}
        };
    }

    winrt::fire_and_forget DataGridView::BuildRowsTail(
        uint64_t gen, int startIdx, int totalRows)
    {
        constexpr int kChunk = 100;

        // Strong ref keeps the page alive for the duration of the
        // streaming render even if the user navigates away.
        auto self = get_strong();
        // Capture the UI dispatcher so each iteration can hop back
        // after the delay (resume_after lands on a thread-pool thread).
        auto dq = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();

        try
        {
            for (int i = startIdx; i < totalRows; i += kChunk)
            {
                if (self->buildRowsGeneration_ != gen) co_return; // superseded
                int end = (std::min)(totalRows, i + kChunk);
                auto rows = self->DataRows();
                if (!rows) co_return;
                for (int j = i; j < end; ++j)
                    rows.Children().Append(self->BuildRowElement(j));
                if (self->selectedRow_ >= i && self->selectedRow_ < end)
                    self->HighlightRow(self->selectedRow_);
                if (end >= totalRows) break;
                // Yield ~1 frame so layout + paint + input get to run.
                // resume_after lands on the thread pool — hop back to
                // the UI thread via the custom awaiter before touching
                // XAML in the next pass.
                co_await winrt::resume_after(std::chrono::milliseconds(16));
                if (!dq) co_return;
                co_await ResumeOnDispatcher{ dq };
            }
        }
        catch (...) { /* page closed mid-stream — drop quietly */ }
    }

    // Build a single row's UI element. Layout: a horizontal StackPanel that
    // IS the row container, the click target, the bg fill, and the cell
    // host. No Button (drops the Button template + visual states), no Grid
    // (drops N+1 ColumnDefinitions per row), no Grid::SetColumn calls.
    //
    // Children layout: index 0 = row number, index k+1 = data cell for col k.
    // The row's data index is stored in Tag for selection / edit lookup.
    muxc::StackPanel DataGridView::BuildRowElement(int rowIdx)
    {
        const auto& row = data_.rows[rowIdx];
        const bool alternate = (rowIdx % 2 == 1);
        const size_t nCols = data_.columnNames.size();

        auto altRowBg = muxm::SolidColorBrush(
            winrt::Windows::UI::ColorHelper::FromArgb(8, 128, 128, 128));
        auto normalRowBg = muxm::SolidColorBrush(
            winrt::Windows::UI::Colors::Transparent());

        muxc::StackPanel rowPanel;
        rowPanel.Orientation(muxc::Orientation::Horizontal);
        rowPanel.Height(ROW_HEIGHT);
        rowPanel.Background(alternate ? altRowBg : normalRowBg);
        // Tag = data row index. Used by findRowPanelByDataIndex /
        // GetRowPanel / HighlightRow to locate the row visual.
        rowPanel.Tag(winrt::box_value(static_cast<int32_t>(rowIdx)));

        // Row number cell — uses the cached row-num style (4 properties
        // baked in). Only Text + Width set per call.
        muxc::TextBlock numLbl;
        if (rowNumStyle_) numLbl.Style(rowNumStyle_);
        numLbl.Text(winrt::hstring(std::to_wstring(rowIdx + 1)));
        numLbl.Width(ROW_NUM_WIDTH);
        rowPanel.Children().Append(numLbl);

        // Statics shared across all rows / cells. Constructed exactly once
        // per process — keeps the inner loop free of std::wstring allocations
        // for the common short-cell path.
        static const std::wstring kEmptyStr;
        static const std::wstring kNullText{ L"NULL" };

        // Reusable scratch buffer for cells that overflow MAX_CELL_DISPLAY_CHARS.
        // Hoisted out of the column loop and reserved upfront so a long cell
        // costs an in-place `assign` (no heap alloc), not a fresh wstring.
        std::wstring truncBuf;
        truncBuf.reserve(MAX_CELL_DISPLAY_CHARS + 1);

        // Data cells. With the cached cellStyle_, per-cell crossings are
        // just: Style + Text + Width (+ Opacity only on null/empty). The
        // 4 static properties (VerticalAlignment, TextTrimming, TextWrapping,
        // Padding, FontSize) come from the Style in a single property set.
        for (size_t ci = 0; ci < nCols; ci++)
        {
            const auto& colName = data_.columnNames[ci];
            double colWidth = (ci < columnWidths_.size()) ? columnWidths_[ci] : COL_DEFAULT_WIDTH;

            // Bind val by const-ref to the map's storage — zero alloc, zero
            // copy in the inner loop's hottest path.
            auto it = row.find(colName);
            const std::wstring& val = (it != row.end()) ? it->second : kEmptyStr;

            const bool isNull = DBModels::isNullCell(val);
            // Pick the display source by pointer so the short-cell path
            // does no string work at all. EXPLAIN output bypasses the
            // truncation entirely — its lines are always short enough
            // and must render verbatim.
            const std::wstring* displayPtr;
            if (isNull)
            {
                displayPtr = &kNullText;
            }
            else if (!skipCellTruncation_ && val.size() > MAX_CELL_DISPLAY_CHARS)
            {
                truncBuf.assign(val, 0, MAX_CELL_DISPLAY_CHARS);
                truncBuf.push_back(L'\x2026'); // single-char ellipsis
                displayPtr = &truncBuf;
            }
            else
            {
                displayPtr = &val;
            }

            muxc::TextBlock cellLbl;
            if (cellStyle_) cellLbl.Style(cellStyle_);
            cellLbl.Text(winrt::hstring(*displayPtr));
            cellLbl.Width(colWidth);
            if (isNull || val.empty())
                cellLbl.Opacity(0.4);

            // Stash the column index on the cell so the context menu's
            // "Copy cell" handler can recover the column name and pull
            // the full untruncated value out of data_.rows.
            cellLbl.Tag(winrt::box_value(static_cast<int32_t>(ci)));

            rowPanel.Children().Append(cellLbl);
        }

        // Selection on Tap. Tap fires on any FrameworkElement.
        const int ri = rowIdx;
        rowPanel.Tapped(
            [this, ri](winrt::Windows::Foundation::IInspectable const&,
                       mux::Input::TappedRoutedEventArgs const&)
        {
            SelectRow(ri);
        });

        // Right-click → also select the row + remember which cell was
        // hit so the context menu's "Copy cell" item knows what to copy.
        rowPanel.RightTapped(
            [this, ri](winrt::Windows::Foundation::IInspectable const&,
                       mux::Input::RightTappedRoutedEventArgs const& e)
        {
            SelectRow(ri);
            contextCol_ = -1;
            // Walk up the OriginalSource chain looking for the cell
            // TextBlock — its Tag carries the column index.
            auto src = e.OriginalSource().try_as<mux::DependencyObject>();
            while (src)
            {
                if (auto tb = src.try_as<muxc::TextBlock>())
                {
                    auto tag = tb.Tag();
                    if (tag)
                    {
                        try { contextCol_ = winrt::unbox_value<int32_t>(tag); }
                        catch (...) {}
                    }
                    break;
                }
                src = mux::Media::VisualTreeHelper::GetParent(src);
            }
        });

        // DoubleTap → inline edit. CRITICAL: do NOT capture the row panel
        // by value here — that would store a strong COM ref to the panel
        // inside a delegate the panel itself owns, forming a reference
        // cycle that survives Children().Clear() and leaks every old row
        // forever (C++/WinRT has no GC). Look the panel up by data index
        // at click time instead; the O(N) walk is irrelevant on a click.
        rowPanel.DoubleTapped(
            [this, ri](winrt::Windows::Foundation::IInspectable const&,
                       mux::Input::DoubleTappedRoutedEventArgs const& e)
        {
            auto src = e.OriginalSource().try_as<mux::FrameworkElement>();
            if (!src) return;
            auto sp = findRowPanelByDataIndex(DataRows(), ri);
            if (!sp) return;
            auto children = sp.Children();
            // Skip index 0 (row number); data cells start at index 1.
            for (uint32_t k = 1; k < children.Size(); k++)
            {
                if (children.GetAt(k) == src)
                {
                    BeginCellEdit(ri, static_cast<size_t>(k - 1));
                    e.Handled(true);
                    return;
                }
            }
        });

        return rowPanel;
    }

    void DataGridView::SelectRow(int index)
    {
        int prevRow = selectedRow_;
        selectedRow_ = index;
        if (prevRow >= 0) HighlightRow(prevRow);
        HighlightRow(index);
        if (OnRowSelected) OnRowSelected(index);

        // Park focus on the UserControl so subsequent KeyDown events
        // (Delete, Ctrl+C, etc.) are routed to Grid_KeyDown. Row panels
        // themselves are not focusable.
        this->Focus(mux::FocusState::Programmatic);
    }

    // Find the realized row StackPanel for a given DATA index by walking
    // DataRows().Children() and matching the Tag we set in BuildRowElement.
    // Returns nullptr if the row hasn't been realized.
    static muxc::StackPanel findRowPanelByDataIndex(
        muxc::Panel const& container, int dataIdx)
    {
        auto children = container.Children();
        for (uint32_t k = 0; k < children.Size(); k++)
        {
            auto sp = children.GetAt(k).try_as<muxc::StackPanel>();
            if (!sp) continue;
            auto tag = sp.Tag();
            if (!tag) continue;
            int idx = winrt::unbox_value<int32_t>(tag);
            if (idx == dataIdx) return sp;
        }
        return nullptr;
    }

    void DataGridView::HighlightRow(int index)
    {
        if (index < 0 || index >= static_cast<int>(data_.rows.size()))
            return;
        auto sp = findRowPanelByDataIndex(DataRows(), index);
        if (!sp) return;

        // Pending-delete visual wins over alternate-row shading: if the
        // row is in deletedRows_, paint it red + dim regardless of
        // whether it's currently selected. This prevents an earlier
        // delete's red mark from disappearing when the user clicks a
        // different row (which triggered HighlightRow on the previous
        // row and unconditionally reset its background).
        const bool isDeleted = (deletedRows_.count(index) > 0);
        if (isDeleted)
        {
            sp.Background(muxm::SolidColorBrush(
                winrt::Windows::UI::ColorHelper::FromArgb(40, 196, 43, 28)));
            sp.Opacity(0.5);
            return;
        }

        if (index == selectedRow_)
        {
            sp.Background(muxm::SolidColorBrush(
                winrt::Windows::UI::ColorHelper::FromArgb(30, 0, 120, 212)));
        }
        else
        {
            bool alternate = (index % 2 == 1);
            auto bg = alternate
                ? winrt::Windows::UI::ColorHelper::FromArgb(8, 128, 128, 128)
                : winrt::Windows::UI::Colors::Transparent();
            sp.Background(muxm::SolidColorBrush(bg));
        }
        sp.Opacity(1.0);
    }

    // ── Mark/clear row visual states ─────────────────
    void DataGridView::MarkRowDeleted(int index)
    {
        if (index < 0 || index >= static_cast<int>(data_.rows.size()))
            return;
        // Track the delete so HighlightRow / row rebuild can re-paint
        // it when selection changes or the grid re-renders.
        deletedRows_.insert(index);
        auto sp = findRowPanelByDataIndex(DataRows(), index);
        if (!sp) return;
        sp.Background(muxm::SolidColorBrush(
            winrt::Windows::UI::ColorHelper::FromArgb(40, 196, 43, 28)));
        sp.Opacity(0.5);
    }

    void DataGridView::ClearRowMarks()
    {
        deletedRows_.clear();
        for (uint32_t i = 0; i < DataRows().Children().Size(); i++)
        {
            auto sp = DataRows().Children().GetAt(i).try_as<muxc::StackPanel>();
            if (!sp) continue;
            int dataIdx = static_cast<int>(i);
            if (auto tag = sp.Tag()) dataIdx = winrt::unbox_value<int32_t>(tag);
            bool alternate = (dataIdx % 2 == 1);
            auto bg = alternate
                ? winrt::Windows::UI::ColorHelper::FromArgb(8, 128, 128, 128)
                : winrt::Windows::UI::Colors::Transparent();
            sp.Background(muxm::SolidColorBrush(bg));
            sp.Opacity(1.0);
        }
    }

    // ── Inline cell editing ──────────────────────────
    // Resolve the row's StackPanel for a given DATA index. Returns nullptr
    // if the row hasn't been realized.
    muxc::StackPanel DataGridView::GetRowPanel(int rowIndex)
    {
        if (rowIndex < 0 || rowIndex >= static_cast<int>(data_.rows.size()))
            return nullptr;
        return findRowPanelByDataIndex(DataRows(), rowIndex);
    }

    // Inline edit: row StackPanel children index 0 = row number, index k+1 =
    // data cell for column k. We swap children[colIndex+1] with a TextBox.
    void DataGridView::BeginCellEdit(int rowIndex, size_t colIndex)
    {
        // Read-only mode: bail out before we touch the visual tree. Used
        // for Redis adapters where inline SQL-style edits cannot map to
        // Redis commands safely.
        if (readOnly_) return;

        if (rowIndex < 0 || rowIndex >= static_cast<int>(data_.rows.size()))
            return;
        if (colIndex >= data_.columnNames.size()) return;

        auto rowPanel = GetRowPanel(rowIndex);
        if (!rowPanel) return;

        const uint32_t targetIdx = static_cast<uint32_t>(colIndex) + 1u;
        if (targetIdx >= rowPanel.Children().Size()) return;
        auto existingLbl = rowPanel.Children().GetAt(targetIdx).try_as<muxc::TextBlock>();
        if (!existingLbl) return; // already editing (TextBox at this slot)

        auto& colName = data_.columnNames[colIndex];
        std::wstring oldValue;
        auto it = data_.rows[rowIndex].find(colName);
        if (it != data_.rows[rowIndex].end()) oldValue = it->second;

        double colWidth = (colIndex < columnWidths_.size())
            ? columnWidths_[colIndex] : COL_DEFAULT_WIDTH;

        // Build the editor — match cell sizing. SQL NULL shows as empty
        // TextBox; user can type a value to set, or leave empty to keep NULL.
        muxc::TextBox editBox;
        editBox.Text(winrt::hstring(DBModels::isNullCell(oldValue) ? L"" : oldValue));
        editBox.FontSize(12.0);
        editBox.Width(colWidth);
        editBox.Padding(mux::Thickness{ 4.0, 3.0, 4.0, 3.0 });
        editBox.MinHeight(0.0);
        editBox.Height(ROW_HEIGHT);
        editBox.MinWidth(0.0);
        editBox.BorderThickness(mux::Thickness{ 0, 0, 0, 0 });
        editBox.VerticalAlignment(mux::VerticalAlignment::Center);
        editBox.Background(muxm::SolidColorBrush(
            winrt::Windows::UI::ColorHelper::FromArgb(30, 0, 120, 212)));
        editBox.Resources().Insert(
            winrt::box_value(L"TextControlThemePadding"),
            winrt::box_value(mux::Thickness{ 4.0, 3.0, 4.0, 3.0 }));
        editBox.Resources().Insert(
            winrt::box_value(L"TextControlThemeMinHeight"),
            winrt::box_value(0.0));
        editBox.SelectionStart(static_cast<int32_t>(editBox.Text().size()));

        // Replace TextBlock with TextBox at the same Children index
        rowPanel.Children().RemoveAt(targetIdx);
        rowPanel.Children().InsertAt(targetIdx, editBox);
        editBox.Focus(mux::FocusState::Programmatic);

        // Track which cell is editing so FlushEdit() can locate the
        // TextBox and commit its text on Ctrl+S without waiting for
        // LostFocus (which doesn't fire when a keyboard accelerator
        // handles the key at the page level).
        editingRowIndex_ = rowIndex;
        editingColIndex_ = static_cast<int>(colIndex);

        // Save on Enter key. Empty input -> SQL NULL sentinel.
        editBox.KeyDown([this, rowIndex, colIndex, oldValue]
            (winrt::Windows::Foundation::IInspectable const& sender,
             mux::Input::KeyRoutedEventArgs const& e)
        {
            if (e.Key() == winrt::Windows::System::VirtualKey::Enter)
            {
                auto tb = sender.try_as<muxc::TextBox>();
                if (!tb) return;
                std::wstring newValue(tb.Text());
                if (newValue.empty()) newValue = DBModels::nullValue();
                CommitCellEdit(rowIndex, colIndex, oldValue, newValue);
                e.Handled(true);
            }
            else if (e.Key() == winrt::Windows::System::VirtualKey::Escape)
            {
                CommitCellEdit(rowIndex, colIndex, oldValue, oldValue);
                e.Handled(true);
            }
        });

        // Save on focus lost
        editBox.LostFocus([this, rowIndex, colIndex, oldValue]
            (winrt::Windows::Foundation::IInspectable const& sender, mux::RoutedEventArgs const&)
        {
            auto tb = sender.try_as<muxc::TextBox>();
            if (!tb) return;
            std::wstring newValue(tb.Text());
            if (newValue.empty()) newValue = DBModels::nullValue();
            CommitCellEdit(rowIndex, colIndex, oldValue, newValue);
        });
    }

    void DataGridView::CommitCellEdit(int rowIndex, size_t colIndex,
        const std::wstring& oldValue, const std::wstring& newValue)
    {
        auto rowPanel = GetRowPanel(rowIndex);
        if (!rowPanel) return;

        const uint32_t targetIdx = static_cast<uint32_t>(colIndex) + 1u;
        if (targetIdx >= rowPanel.Children().Size()) return;
        // Double-fire guard: if the slot is already a TextBlock the prior
        // call (Enter / LostFocus) already committed — bail out.
        if (!rowPanel.Children().GetAt(targetIdx).try_as<muxc::TextBox>()) return;

        // Clear in-flight edit tracking — FlushEdit() relies on this to
        // know when nothing is being edited.
        editingRowIndex_ = -1;
        editingColIndex_ = -1;

        auto& colName = data_.columnNames[colIndex];
        data_.rows[rowIndex][colName] = newValue;

        double colWidth = (colIndex < columnWidths_.size())
            ? columnWidths_[colIndex] : COL_DEFAULT_WIDTH;
        // Render NULL sentinel as visible text "NULL" with dim opacity.
        // Truncate to keep the post-edit cell consistent with BuildRowElement.
        const bool isNull = DBModels::isNullCell(newValue);
        std::wstring display;
        if (isNull)
        {
            display = L"NULL";
        }
        else if (newValue.size() > MAX_CELL_DISPLAY_CHARS)
        {
            display.reserve(MAX_CELL_DISPLAY_CHARS + 1);
            display.assign(newValue, 0, MAX_CELL_DISPLAY_CHARS);
            display.push_back(L'\x2026');
        }
        else
        {
            display = newValue;
        }

        muxc::TextBlock lbl;
        if (cellStyle_) lbl.Style(cellStyle_);
        lbl.Text(winrt::hstring(display));
        lbl.Width(colWidth);
        if (isNull || newValue.empty())
            lbl.Opacity(0.4);

        rowPanel.Children().RemoveAt(targetIdx);
        rowPanel.Children().InsertAt(targetIdx, lbl);

        if (newValue != oldValue && OnCellEdited)
            OnCellEdited(rowIndex, colName, oldValue, newValue);
    }

    // Locate the TextBox for the currently-edited cell and commit its
    // text synchronously. No-op if nothing is being edited. Used on
    // Ctrl+S and on "click outside the TextBox" so the user's in-flight
    // text always lands in ChangeTracker before SQL generation — Enter
    // is still the dedicated accept key but no longer the only one.
    void DataGridView::FlushEdit()
    {
        if (editingRowIndex_ < 0 || editingColIndex_ < 0) return;
        const int rowIdx = editingRowIndex_;
        const int colIdx = editingColIndex_;

        auto rowPanel = GetRowPanel(rowIdx);
        if (!rowPanel) { editingRowIndex_ = -1; editingColIndex_ = -1; return; }
        const uint32_t targetIdx = static_cast<uint32_t>(colIdx) + 1u;
        if (targetIdx >= rowPanel.Children().Size()) { editingRowIndex_ = -1; editingColIndex_ = -1; return; }
        auto tb = rowPanel.Children().GetAt(targetIdx).try_as<muxc::TextBox>();
        if (!tb) { editingRowIndex_ = -1; editingColIndex_ = -1; return; }

        std::wstring newValue(tb.Text());
        // Match existing Enter / LostFocus semantics: an empty TextBox
        // means "SQL NULL". User who wanted literal empty string still
        // has to set NULL explicitly elsewhere (same as before).
        if (newValue.empty()) newValue = DBModels::nullValue();

        if (colIdx >= static_cast<int>(data_.columnNames.size())) return;
        const auto& colName = data_.columnNames[colIdx];
        std::wstring oldValue;
        auto it = data_.rows[rowIdx].find(colName);
        if (it != data_.rows[rowIdx].end()) oldValue = it->second;

        CommitCellEdit(rowIdx, colIdx, oldValue, newValue);
    }

    // ── Key shortcuts: Delete mark-deleted ───
    //
    // Ctrl+C used to copy the whole row TSV, but it fought the natural
    // cell-level copy users expect — selecting text inside a cell and
    // hitting Ctrl+C was dumping every column instead of the selection.
    // Row-copy stays available via CopySelectedRowToClipboard() for a
    // future context-menu wiring; the hotkey itself is gone so the
    // built-in TextBox Ctrl+C behaviour during inline edit wins.
    void DataGridView::Grid_KeyDown(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& e)
    {
        auto key = e.Key();

        // Delete key → mark selected row as deleted (pending commit).
        // No-op when read-only (e.g. Redis) or when nothing is selected.
        // Focused TextBox cells eat the key first during inline edit, so
        // this only fires when focus is on the row itself.
        if (key == winrt::Windows::System::VirtualKey::Delete)
        {
            if (readOnly_) return;
            if (selectedRow_ < 0) return;
            if (OnDeleteRequested)
            {
                OnDeleteRequested();
                e.Handled(true);
            }
        }
    }

    void DataGridView::CopySelectedRowToClipboard()
    {
        auto* row = GetSelectedRow();
        if (!row) return;
        std::wstring tsv;
        bool first = true;
        for (auto& col : data_.columnNames)
        {
            if (!first) tsv += L'\t';
            tsv += col;
            first = false;
        }
        tsv += L'\n';
        first = true;
        for (auto& col : data_.columnNames)
        {
            if (!first) tsv += L'\t';
            auto it = row->find(col);
            if (it != row->end())
            {
                // Render NULL sentinel as visible "NULL" in clipboard text
                tsv += DBModels::isNullCell(it->second) ? std::wstring(L"NULL") : it->second;
            }
            first = false;
        }
        winrt::Windows::ApplicationModel::DataTransfer::DataPackage dataPackage;
        dataPackage.SetText(winrt::hstring(tsv));
        winrt::Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(dataPackage);
    }

    void DataGridView::CopyCellToClipboard()
    {
        if (selectedRow_ < 0 || contextCol_ < 0) return;
        if (selectedRow_ >= static_cast<int>(data_.rows.size())) return;
        if (contextCol_ >= static_cast<int>(data_.columnNames.size())) return;

        const auto& colName = data_.columnNames[contextCol_];
        auto& row = data_.rows[selectedRow_];
        auto it = row.find(colName);
        std::wstring text;
        if (it != row.end())
        {
            // Render NULL sentinel as visible "NULL" so paste targets see
            // a real string and not an empty clipboard payload.
            text = DBModels::isNullCell(it->second) ? std::wstring(L"NULL") : it->second;
        }
        winrt::Windows::ApplicationModel::DataTransfer::DataPackage pkg;
        pkg.SetText(winrt::hstring(text));
        winrt::Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(pkg);
    }

    // ── Scroll sync ────────────────────────────────────
    // Header H-scroll follows data H-scroll. No row-rebuild work happens
    // here — all rows are realized after BuildRows + chunked completion,
    // so scrolling is handled natively by ScrollViewer.
    void DataGridView::DataScroller_ViewChanged(
        winrt::Windows::Foundation::IInspectable const&,
        muxc::ScrollViewerViewChangedEventArgs const&)
    {
        SyncHeaderScroll();
    }

    void DataGridView::SyncHeaderScroll()
    {
        double hOffset = DataScroller().HorizontalOffset();
        HeaderScroller().ChangeView(hOffset, nullptr, nullptr);
    }
}
