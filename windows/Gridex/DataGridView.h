#pragma once

#include "DataGridView.g.h"
#include "Models/QueryResult.h"
#include "Models/ColumnInfo.h"
#include <functional>

namespace winrt::Gridex::implementation
{
    struct DataGridView : DataGridViewT<DataGridView>
    {
        DataGridView();

        void SetData(const DBModels::QueryResult& result);

        // Optional: feed full column metadata (PK / FK / nullable / full
        // dataType string) so the header can render PK / FK icons, a
        // tooltip with the declared type, and italicize nullable columns.
        // Safe to omit — SetData alone still works with types from
        // QueryResult.columnTypes but without PK/FK/nullable decoration.
        void SetColumnMetadata(const std::vector<DBModels::ColumnInfo>& meta);

        // Put the grid in read-only mode (disables inline cell editing).
        // Used for adapters like Redis where the "table" is a key/value
        // projection and inline SQL-style edit does not map to a single
        // Redis command (hash / list / set would be silently overwritten
        // as a plain string via SET).
        void SetReadOnly(bool ro) { readOnly_ = ro; }
        bool IsReadOnly() const { return readOnly_; }

        // Callback fired when user selects a row (zero-based index)
        std::function<void(int rowIndex)> OnRowSelected;

        // Callback for column sort — host re-queries with ORDER BY
        std::function<void(const std::wstring& column, bool ascending)> OnSortRequested;

        // Callback when cell is edited (rowIndex, columnName, oldValue, newValue)
        std::function<void(int, const std::wstring&, const std::wstring&, const std::wstring&)> OnCellEdited;

        // Callback when user presses Delete to mark the selected row as
        // deleted (pending commit). Host routes this to DeleteSelectedRow.
        std::function<void()> OnDeleteRequested;

        // Callback when user picks "Refresh" from the grid context menu —
        // host re-runs the current tab's query to pull fresh rows.
        std::function<void()> OnRefreshRequested;

        // Callback when user clicks an FK icon in a column header — host
        // navigates to the referenced table/schema. Arg schema may be
        // empty; host falls back to the current connection's default.
        std::function<void(const std::wstring& refTable,
                           const std::wstring& refSchema)> OnForeignKeyClicked;

        // Get selected row data for copy
        const DBModels::TableRow* GetSelectedRow() const;

        // Expose selected row index for CRUD operations
        int GetSelectedRowIndex() const { return selectedRow_; }

        // Flush any in-flight inline cell edit synchronously. Call this
        // right before committing pending changes (Ctrl+S / Commit button)
        // so the value still sitting in an unfocused TextBox lands in the
        // ChangeTracker before SQL is generated. No-op when nothing is
        // being edited.
        void FlushEdit();

        // Mark a row as deleted (red bg, dimmed text)
        void MarkRowDeleted(int index);

        // Clear all row visual marks (after discard/commit)
        void ClearRowMarks();

        void Grid_KeyDown(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& e);

        void DataScroller_ViewChanged(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Controls::ScrollViewerViewChangedEventArgs const& e);

    private:
        DBModels::QueryResult data_;
        std::vector<DBModels::ColumnInfo> columnMetadata_;
        int selectedRow_ = -1;

        // Tracks the (row, col) that is currently in inline-edit mode
        // (TextBox swapped in place of TextBlock). -1/-1 when nothing is
        // being edited. Set by BeginCellEdit, cleared by CommitCellEdit;
        // read by FlushEdit to locate the TextBox and commit its text
        // without relying on LostFocus timing.
        int editingRowIndex_ = -1;
        int editingColIndex_ = -1;
        std::wstring sortColumn_;
        bool sortAscending_ = true;
        bool readOnly_ = false;

        // Disable the 80-char cell truncation for single-column results
        // like PostgreSQL EXPLAIN/EXPLAIN ANALYZE (one "QUERY PLAN"
        // column whose values are full plan lines that need to be read
        // verbatim, not cut off with an ellipsis). Set in SetData based
        // on the incoming schema.
        bool skipCellTruncation_ = false;

        // Column widths for uniform sizing + resize
        std::vector<double> columnWidths_;
        int resizingCol_ = -1;
        double resizeStartX_ = 0.0;
        double resizeStartWidth_ = 0.0;

        // Cached XAML cell styles. Looked up once on first BuildRows; reused
        // across every cell so per-cell crossings drop to ~3 (Style + Text +
        // Width) instead of 5-6 individual property sets.
        winrt::Microsoft::UI::Xaml::Style cellStyle_{ nullptr };
        winrt::Microsoft::UI::Xaml::Style rowNumStyle_{ nullptr };

        void EnsureCellStyles();

        void ComputeColumnWidths();
        void BuildHeaders();
        // Render every row of the current page synchronously. Per-cell cost
        // is the bottleneck on wide tables. The current refactor drops the
        // per-row Button + Grid + N+1 ColumnDefinitions in favor of a single
        // horizontal StackPanel that doubles as the click target, the bg
        // fill, and the cell container — eliminating ~3100 ColumnDefinition
        // objects (for a 100x30 page) and the Button visual-state machinery.
        void BuildRows();
        winrt::Microsoft::UI::Xaml::Controls::StackPanel BuildRowElement(int rowIdx);
        void SelectRow(int index);
        void HighlightRow(int index);
        void CopySelectedRowToClipboard();
        void SyncHeaderScroll();
        // Inline edit: row is a horizontal StackPanel; cell at column k is
        // child at index k+1 (index 0 = row number). We swap that child
        // with a TextBox in BeginCellEdit and back in CommitCellEdit.
        // (Non-const because DataRows() accessor is non-const in WinRT projection.)
        winrt::Microsoft::UI::Xaml::Controls::StackPanel GetRowPanel(int rowIndex);
        void BeginCellEdit(int rowIndex, size_t colIndex);
        void CommitCellEdit(int rowIndex, size_t colIndex,
            const std::wstring& oldValue, const std::wstring& newValue);

        static constexpr double COL_DEFAULT_WIDTH = 150.0;
        static constexpr double COL_MIN_WIDTH     = 60.0;
        static constexpr double COL_MAX_WIDTH     = 500.0;
        static constexpr double ROW_HEIGHT        = 28.0;
        static constexpr double ROW_NUM_WIDTH     = 44.0;
    };
}

namespace winrt::Gridex::factory_implementation
{
    struct DataGridView : DataGridViewT<DataGridView, implementation::DataGridView>
    {
    };
}
