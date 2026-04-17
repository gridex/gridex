#pragma once

#include "QueryEditorView.g.h"
#include "Models/QueryResult.h"
#include <functional>

namespace winrt::Gridex::implementation
{
    struct QueryEditorView : QueryEditorViewT<QueryEditorView>
    {
        QueryEditorView();

        void RunQuery_Click(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void FormatQuery_Click(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void ClearEditor_Click(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);

        std::function<DBModels::QueryResult(const std::wstring& sql)> OnExecuteQuery;

        void SetSql(const std::wstring& sql);
        std::wstring GetSql() const;

        // Set table/column/function names for autocomplete suggestions
        void SetSchemaCompletions(
            const std::vector<std::wstring>& tables,
            const std::vector<std::wstring>& columns,
            const std::vector<std::wstring>& functions);

    private:
        DBModels::QueryResult lastResult_;
        winrt::Microsoft::UI::Xaml::Controls::TextBox sqlEditor_{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::ListView suggestList_{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Border suggestPopup_{ nullptr };
        bool editorReady_ = false;
        bool editorCreated_ = false;
        bool buttonsWired_ = false;
        bool suppressTextChange_ = false;
        std::wstring pendingSql_;

        // Autocomplete data
        std::vector<std::wstring> schemaTableNames_;
        std::vector<std::wstring> schemaColumnNames_;
        std::vector<std::wstring> schemaFunctionNames_;

        // SQL keywords for suggestion
        static const std::vector<std::wstring>& SqlKeywords();

        // Result grid column-width state (drag-to-resize, same pattern as
        // DataGridView). `resultColumnWidths_` is the source of truth; header
        // cells are updated live on PointerMoved and rows are rebuilt on
        // PointerReleased so we only pay the per-row re-render cost once per
        // drag gesture.
        std::vector<double> resultColumnWidths_;
        int resizingResultCol_ = -1;
        double resizeResultStartX_ = 0.0;
        double resizeResultStartWidth_ = 0.0;

        static constexpr double RESULT_COL_MIN_WIDTH     = 60.0;
        static constexpr double RESULT_COL_MAX_WIDTH     = 600.0;
        static constexpr double RESULT_COL_DEFAULT_WIDTH = 150.0;

        void EnsureEditorCreated();
        void ExecuteCurrentQuery();
        void ShowResult(const DBModels::QueryResult& result);
        void ShowError(const std::wstring& message);
        void BuildResultHeaders(const DBModels::QueryResult& result);
        void BuildResultRows(const DBModels::QueryResult& result);
        void OnEditorTextChanged();
        void ForceShowSuggestions();
        void ShowSuggestions(const std::vector<std::wstring>& items);
        void HideSuggestions();
        void ApplySuggestion(const std::wstring& text);
        std::wstring GetCurrentWord() const;
        std::wstring GetPreviousKeyword() const;

        winrt::Windows::Foundation::IAsyncAction RunScript(winrt::hstring script);
    };
}

namespace winrt::Gridex::factory_implementation
{
    struct QueryEditorView : QueryEditorViewT<QueryEditorView, implementation::QueryEditorView>
    {
    };
}
