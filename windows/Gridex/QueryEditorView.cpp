#include "pch.h"
#include "xaml-includes.h"
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.System.h>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include "QueryEditorView.h"
#if __has_include("QueryEditorView.g.cpp")
#include "QueryEditorView.g.cpp"
#endif

namespace winrt::Gridex::implementation
{
    namespace mux  = winrt::Microsoft::UI::Xaml;
    namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
    namespace muxm = winrt::Microsoft::UI::Xaml::Media;

    // SQL keywords for autocomplete
    const std::vector<std::wstring>& QueryEditorView::SqlKeywords()
    {
        static std::vector<std::wstring> kw = {
            L"SELECT", L"FROM", L"WHERE", L"AND", L"OR", L"NOT", L"IN",
            L"JOIN", L"LEFT JOIN", L"RIGHT JOIN", L"INNER JOIN", L"OUTER JOIN",
            L"ON", L"AS", L"GROUP BY", L"ORDER BY", L"HAVING", L"LIMIT", L"OFFSET",
            L"INSERT INTO", L"VALUES", L"UPDATE", L"SET", L"DELETE FROM",
            L"CREATE TABLE", L"ALTER TABLE", L"DROP TABLE",
            L"DISTINCT", L"COUNT", L"SUM", L"AVG", L"MIN", L"MAX",
            L"IS NULL", L"IS NOT NULL", L"LIKE", L"BETWEEN", L"EXISTS",
            L"CASE", L"WHEN", L"THEN", L"ELSE", L"END",
            L"ASC", L"DESC", L"UNION", L"UNION ALL",
            L"PRIMARY KEY", L"FOREIGN KEY", L"REFERENCES", L"DEFAULT",
            L"NOT NULL", L"UNIQUE", L"INDEX", L"CASCADE",
            L"BEGIN", L"COMMIT", L"ROLLBACK",
            L"TRUE", L"FALSE", L"NULL",
            L"COALESCE", L"NULLIF", L"CAST", L"EXTRACT",
            L"NOW()", L"CURRENT_TIMESTAMP", L"CURRENT_DATE",
        };
        return kw;
    }

    QueryEditorView::QueryEditorView()
    {
        InitializeComponent();

        this->Loaded([this](winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
        {
            EnsureEditorCreated();
        });

        this->RegisterPropertyChangedCallback(
            mux::UIElement::VisibilityProperty(),
            [this](mux::DependencyObject const&, mux::DependencyProperty const&)
            {
                if (this->Visibility() == mux::Visibility::Visible)
                    EnsureEditorCreated();
            });
    }

    void QueryEditorView::EnsureEditorCreated()
    {
        if (editorCreated_) return;
        editorCreated_ = true;

        // Wire buttons
        RunQueryButton().Click(
            [this](winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
            { RunQuery_Click({}, {}); });
        FormatQueryButton().Click(
            [this](winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
            { FormatQuery_Click({}, {}); });
        ClearEditorButton().Click(
            [this](winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
            { ClearEditor_Click({}, {}); });

        // Create SQL editor TextBox
        sqlEditor_ = muxc::TextBox();
        sqlEditor_.AcceptsReturn(true);
        sqlEditor_.TextWrapping(mux::TextWrapping::NoWrap);
        sqlEditor_.FontFamily(mux::Media::FontFamily(L"Cascadia Code,Consolas,monospace"));
        sqlEditor_.FontSize(13.0);
        sqlEditor_.PlaceholderText(L"-- Write SQL here... (Ctrl+Enter to run, Ctrl+Space for suggestions)");
        sqlEditor_.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        sqlEditor_.VerticalAlignment(mux::VerticalAlignment::Stretch);
        sqlEditor_.Padding(mux::Thickness{ 12, 8, 12, 8 });
        sqlEditor_.BorderThickness(mux::Thickness{ 0,0,0,0 });
        sqlEditor_.Background(muxm::SolidColorBrush(winrt::Windows::UI::Colors::Transparent()));

        // Text changed → trigger autocomplete
        sqlEditor_.TextChanged(
            [this](winrt::Windows::Foundation::IInspectable const&, muxc::TextChangedEventArgs const&)
            {
                if (!suppressTextChange_)
                    OnEditorTextChanged();
            });

        // Key handling: use PreviewKeyDown to intercept BEFORE TextBox processes Enter
        sqlEditor_.PreviewKeyDown(
            [this](winrt::Windows::Foundation::IInspectable const&, mux::Input::KeyRoutedEventArgs const& e)
            {
                auto ctrlState = winrt::Microsoft::UI::Input::InputKeyboardSource::GetKeyStateForCurrentThread(
                    winrt::Windows::System::VirtualKey::Control);
                bool ctrlDown = (static_cast<int>(ctrlState) &
                    static_cast<int>(winrt::Windows::UI::Core::CoreVirtualKeyStates::Down)) != 0;

                if (e.Key() == winrt::Windows::System::VirtualKey::Enter && ctrlDown)
                {
                    ExecuteCurrentQuery();
                    e.Handled(true);
                    return;
                }

                if (e.Key() == winrt::Windows::System::VirtualKey::Space && ctrlDown)
                {
                    // Force show ALL suggestions based on context
                    ForceShowSuggestions();
                    e.Handled(true);
                    return;
                }

                // If suggest popup visible, handle navigation
                if (SuggestPopup().Visibility() == mux::Visibility::Visible)
                {
                    if (e.Key() == winrt::Windows::System::VirtualKey::Escape)
                    {
                        HideSuggestions();
                        e.Handled(true);
                        return;
                    }
                    if (e.Key() == winrt::Windows::System::VirtualKey::Tab ||
                        (e.Key() == winrt::Windows::System::VirtualKey::Enter && !ctrlDown))
                    {
                        // Apply selected suggestion
                        auto selected = SuggestList().SelectedItem();
                        if (selected)
                        {
                            auto item = selected.try_as<muxc::TextBlock>();
                            if (item)
                                ApplySuggestion(std::wstring(item.Text()));
                        }
                        else if (SuggestList().Items().Size() > 0)
                        {
                            // Apply first item
                            auto first = SuggestList().Items().GetAt(0).try_as<muxc::TextBlock>();
                            if (first)
                                ApplySuggestion(std::wstring(first.Text()));
                        }
                        HideSuggestions();
                        e.Handled(true);
                        return;
                    }
                    if (e.Key() == winrt::Windows::System::VirtualKey::Down)
                    {
                        int idx = SuggestList().SelectedIndex();
                        if (idx < static_cast<int>(SuggestList().Items().Size()) - 1)
                            SuggestList().SelectedIndex(idx + 1);
                        e.Handled(true);
                        return;
                    }
                    if (e.Key() == winrt::Windows::System::VirtualKey::Up)
                    {
                        int idx = SuggestList().SelectedIndex();
                        if (idx > 0) SuggestList().SelectedIndex(idx - 1);
                        e.Handled(true);
                        return;
                    }
                }
            });

        EditorContainer().Children().InsertAt(0, sqlEditor_); // Insert before popup
        editorReady_ = true;

        // Wire suggest list item click
        SuggestList().ItemClick(
            [this](winrt::Windows::Foundation::IInspectable const&, muxc::ItemClickEventArgs const& e)
            {
                auto item = e.ClickedItem().try_as<muxc::TextBlock>();
                if (item)
                    ApplySuggestion(std::wstring(item.Text()));
                HideSuggestions();
            });

        if (!pendingSql_.empty())
        {
            sqlEditor_.Text(winrt::hstring(pendingSql_));
            pendingSql_.clear();
        }
    }

    // ── Autocomplete logic ─────────────────────────────

    std::wstring QueryEditorView::GetCurrentWord() const
    {
        if (!sqlEditor_) return L"";
        std::wstring text(sqlEditor_.Text());
        int pos = sqlEditor_.SelectionStart();
        if (pos <= 0 || pos > static_cast<int>(text.size())) return L"";

        // Walk backwards from cursor to find word start
        int start = pos - 1;
        while (start >= 0 && text[start] != L' ' && text[start] != L'\n' &&
               text[start] != L'\r' && text[start] != L'\t' &&
               text[start] != L'(' && text[start] != L')' &&
               text[start] != L',' && text[start] != L';')
        {
            start--;
        }
        start++;

        if (start >= pos) return L"";
        return text.substr(start, pos - start);
    }

    void QueryEditorView::ForceShowSuggestions()
    {
        std::wstring currentWord = GetCurrentWord();
        std::wstring prefix = currentWord;
        std::transform(prefix.begin(), prefix.end(), prefix.begin(), ::towlower);

        std::wstring prevKw = GetPreviousKeyword();
        // If current word is empty, use previous keyword as context directly
        if (currentWord.empty() && !prevKw.empty())
        {
            // After space — context determines what to show
        }

        bool wantTables = (prevKw == L"FROM" || prevKw == L"JOIN" || prevKw == L"INTO" ||
                           prevKw == L"UPDATE" || prevKw == L"TABLE");
        bool wantColumns = (prevKw == L"SELECT" || prevKw == L"WHERE" || prevKw == L"AND" ||
                            prevKw == L"OR" || prevKw == L"ON" || prevKw == L"SET" ||
                            prevKw == L"BY" || prevKw == L"HAVING" || prevKw == L"DISTINCT");

        std::vector<std::wstring> matches;

        if (wantTables || (!wantColumns && !wantTables))
        {
            for (auto& t : schemaTableNames_)
            {
                if (prefix.empty() || [&]() {
                    std::wstring lower = t;
                    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
                    return lower.find(prefix) == 0;
                }())
                    matches.push_back(t);
            }
        }

        if (wantColumns || (!wantColumns && !wantTables))
        {
            for (auto& c : schemaColumnNames_)
            {
                if (prefix.empty() || [&]() {
                    std::wstring lower = c;
                    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
                    return lower.find(prefix) == 0;
                }())
                    matches.push_back(c);
            }
        }

        // Functions
        for (auto& f : schemaFunctionNames_)
        {
            if (prefix.empty() || [&]() {
                std::wstring lower = f;
                std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
                return lower.find(prefix) == 0;
            }())
                matches.push_back(f);
        }

        // Keywords (limit to 10 when showing all)
        int kwCount = 0;
        for (auto& kw : SqlKeywords())
        {
            if (kwCount >= 10) break;
            if (prefix.empty() || [&]() {
                std::wstring lower = kw;
                std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
                return lower.find(prefix) == 0;
            }())
            {
                matches.push_back(kw);
                kwCount++;
            }
        }

        if (!matches.empty())
            ShowSuggestions(matches);
    }

    // Determine SQL context keyword before current word
    std::wstring QueryEditorView::GetPreviousKeyword() const
    {
        if (!sqlEditor_) return L"";
        std::wstring text(sqlEditor_.Text());
        int pos = sqlEditor_.SelectionStart();

        // Skip current word backwards
        int i = pos - 1;
        while (i >= 0 && text[i] != L' ' && text[i] != L'\n' && text[i] != L'\t') i--;
        // Skip whitespace
        while (i >= 0 && (text[i] == L' ' || text[i] == L'\n' || text[i] == L'\t' || text[i] == L'\r')) i--;
        // Read previous word
        int end = i + 1;
        while (i >= 0 && text[i] != L' ' && text[i] != L'\n' && text[i] != L'\t' &&
               text[i] != L'(' && text[i] != L')' && text[i] != L',' && text[i] != L';') i--;
        i++;

        if (i >= end) return L"";
        std::wstring kw = text.substr(i, end - i);
        std::transform(kw.begin(), kw.end(), kw.begin(), ::towupper);
        return kw;
    }

    void QueryEditorView::OnEditorTextChanged()
    {
        std::wstring currentWord = GetCurrentWord();
        if (currentWord.empty())
        {
            HideSuggestions();
            return;
        }

        std::wstring prefix = currentWord;
        std::transform(prefix.begin(), prefix.end(), prefix.begin(), ::towlower);

        // Determine context from previous keyword
        std::wstring prevKw = GetPreviousKeyword();
        bool wantTables = (prevKw == L"FROM" || prevKw == L"JOIN" || prevKw == L"INTO" ||
                           prevKw == L"UPDATE" || prevKw == L"TABLE");
        bool wantColumns = (prevKw == L"SELECT" || prevKw == L"WHERE" || prevKw == L"AND" ||
                            prevKw == L"OR" || prevKw == L"ON" || prevKw == L"SET" ||
                            prevKw == L"BY" || prevKw == L"HAVING" || prevKw == L"DISTINCT");

        std::vector<std::wstring> matches;

        // Context-aware: prioritize relevant items
        if (wantTables)
        {
            for (auto& t : schemaTableNames_)
            {
                std::wstring lower = t;
                std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
                if (lower.find(prefix) == 0) matches.push_back(t);
            }
        }

        if (wantColumns)
        {
            for (auto& c : schemaColumnNames_)
            {
                std::wstring lower = c;
                std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
                if (lower.find(prefix) == 0) matches.push_back(c);
            }
        }

        // Always suggest matching tables/columns/functions if no context match yet
        if (!wantTables && !wantColumns)
        {
            for (auto& t : schemaTableNames_)
            {
                std::wstring lower = t;
                std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
                if (lower.find(prefix) == 0) matches.push_back(t);
            }
            for (auto& c : schemaColumnNames_)
            {
                std::wstring lower = c;
                std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
                if (lower.find(prefix) == 0) matches.push_back(c);
            }
        }

        // Functions
        for (auto& f : schemaFunctionNames_)
        {
            std::wstring lower = f;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
            if (lower.find(prefix) == 0) matches.push_back(f);
        }

        // SQL keywords (always)
        for (auto& kw : SqlKeywords())
        {
            std::wstring lower = kw;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
            if (lower.find(prefix) == 0 && matches.size() < 10)
                matches.push_back(kw);
        }

        if (matches.empty() || matches.size() > 15)
        {
            HideSuggestions();
            return;
        }

        ShowSuggestions(matches);
    }

    void QueryEditorView::ShowSuggestions(const std::vector<std::wstring>& items)
    {
        SuggestList().Items().Clear();
        for (auto& item : items)
        {
            muxc::TextBlock tb;
            // Check if has icon prefix (unicode char + space)
            if (item.size() > 2 && item[1] == L' ' && item[0] > 0xE000)
            {
                // Strip icon for display, just show text
                tb.Text(winrt::hstring(item.substr(2)));
            }
            else
            {
                tb.Text(winrt::hstring(item));
            }
            tb.FontSize(12.0);
            tb.FontFamily(mux::Media::FontFamily(L"Cascadia Code,Consolas,monospace"));
            tb.Padding(mux::Thickness{ 8, 3, 8, 3 });
            SuggestList().Items().Append(tb);
        }
        if (SuggestList().Items().Size() > 0)
            SuggestList().SelectedIndex(0);
        SuggestPopup().Visibility(mux::Visibility::Visible);
    }

    void QueryEditorView::HideSuggestions()
    {
        SuggestPopup().Visibility(mux::Visibility::Collapsed);
    }

    void QueryEditorView::ApplySuggestion(const std::wstring& text)
    {
        if (!sqlEditor_) return;

        std::wstring fullText(sqlEditor_.Text());
        int cursorPos = sqlEditor_.SelectionStart();

        // Find current word boundaries
        int start = cursorPos - 1;
        while (start >= 0 && fullText[start] != L' ' && fullText[start] != L'\n' &&
               fullText[start] != L'\r' && fullText[start] != L'\t' &&
               fullText[start] != L'(' && fullText[start] != L')' &&
               fullText[start] != L',' && fullText[start] != L';')
        {
            start--;
        }
        start++;

        // Replace current word with suggestion
        suppressTextChange_ = true;
        std::wstring newText = fullText.substr(0, start) + text + fullText.substr(cursorPos);
        sqlEditor_.Text(winrt::hstring(newText));
        sqlEditor_.SelectionStart(static_cast<int32_t>(start + text.size()));
        sqlEditor_.SelectionLength(0);
        suppressTextChange_ = false;

        sqlEditor_.Focus(mux::FocusState::Programmatic);
    }

    void QueryEditorView::SetSchemaCompletions(
        const std::vector<std::wstring>& tables,
        const std::vector<std::wstring>& columns,
        const std::vector<std::wstring>& functions)
    {
        schemaTableNames_ = tables;
        schemaColumnNames_ = columns;
        schemaFunctionNames_ = functions;
    }

    // ── Core methods ───────────────────────────────────

    namespace {
        // QueryPerf: temporary instrumentation to pin down the freeze on
        // big result sets. Writes phase markers to %TEMP%\gridex-perf.log
        // so they survive across runs and are easy to paste back. Strip
        // once we have the answer.
        std::wstring perfLogPath()
        {
            wchar_t tmp[MAX_PATH];
            DWORD n = GetTempPathW(MAX_PATH, tmp);
            if (!n) return L"C:\\gridex-perf.log";
            return std::wstring(tmp, n) + L"gridex-perf.log";
        }
        void perfLog(const wchar_t* line)
        {
            FILE* f = nullptr;
            _wfopen_s(&f, perfLogPath().c_str(), L"a, ccs=UTF-8");
            if (!f) return;
            SYSTEMTIME st; GetLocalTime(&st);
            fwprintf(f, L"%02d:%02d:%02d.%03d %ls\n",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, line);
            fclose(f);
            OutputDebugStringW(line);
            OutputDebugStringW(L"\n");
        }
        struct QueryPerfStage {
            std::wstring name;
            std::chrono::steady_clock::time_point t0;
            QueryPerfStage(const wchar_t* n)
                : name(n), t0(std::chrono::steady_clock::now())
            {
                perfLog((L"[QueryPerf] " + name + L" START").c_str());
            }
            ~QueryPerfStage()
            {
                auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0).count();
                wchar_t buf[256];
                swprintf_s(buf, L"[QueryPerf] %ls END  %lldms",
                    name.c_str(), (long long)dur);
                perfLog(buf);
            }
        };

        // UI-thread resumer (WinAppSDK ships no resume_foreground for
        // Microsoft.UI.Dispatching.DispatcherQueue, so this is the
        // equivalent custom awaiter). Used by ExecuteCurrentQuery to
        // hop back to the UI thread after the bg query finishes.
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

    winrt::fire_and_forget QueryEditorView::ExecuteCurrentQuery()
    {
        if (!sqlEditor_) co_return;
        std::wstring sql(sqlEditor_.Text());
        if (sql.empty()) co_return;
        HideSuggestions();

        QueryStatusText().Text(L"Running...");
        QueryLoadingText().Text(L"Running query...");
        QueryLoadingOverlay().Visibility(mux::Visibility::Visible);
        perfLog(L"[QueryPerf] ====== ExecuteCurrentQuery ======");

        if (!OnExecuteQuery)
        {
            QueryLoadingOverlay().Visibility(mux::Visibility::Collapsed);
            co_return;
        }

        // Run the query on a background thread so the UI thread can
        // paint the loading overlay + handle input. Without the hop,
        // adapter->execute blocks the UI for the entire query duration
        // and the overlay never appears.
        auto self = get_strong();
        auto dq = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
        auto cb = OnExecuteQuery;
        DBModels::QueryResult result;

        co_await winrt::resume_background();
        try
        {
            QueryPerfStage s(L"OnExecuteQuery (adapter->execute)");
            result = cb(sql);
        }
        catch (...) { result.success = false; result.error = L"Query execution failed."; }

        // Hop back to the UI thread to render.
        if (dq) co_await ResumeOnDispatcher{ dq };

        self->lastResult_ = std::move(result);
        wchar_t buf[256];
        swprintf_s(buf, L"[QueryPerf] result.rows=%zu cols=%zu success=%d",
            self->lastResult_.rows.size(), self->lastResult_.columnNames.size(),
            (int)self->lastResult_.success);
        perfLog(buf);

        if (self->lastResult_.success)
        {
            QueryPerfStage s(L"ShowResult (sync portion)");
            self->ShowResult(self->lastResult_);
        }
        else
        {
            self->ShowError(self->lastResult_.error);
        }

        // Hide overlay after the sync portion of ShowResult has rendered
        // first chunk (the async tail keeps streaming behind the scenes).
        self->QueryLoadingOverlay().Visibility(mux::Visibility::Collapsed);
    }

    void QueryEditorView::SetSql(const std::wstring& sql)
    {
        if (sqlEditor_)
        {
            suppressTextChange_ = true;
            sqlEditor_.Text(winrt::hstring(sql));
            suppressTextChange_ = false;
        }
        else
            pendingSql_ = sql;
    }

    std::wstring QueryEditorView::GetSql() const
    {
        if (sqlEditor_)
            return std::wstring(sqlEditor_.Text());
        return pendingSql_;
    }

    winrt::Windows::Foundation::IAsyncAction QueryEditorView::RunScript(winrt::hstring)
    {
        co_return;
    }

    void QueryEditorView::RunQuery_Click(
        winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
    {
        ExecuteCurrentQuery();
    }

    void QueryEditorView::FormatQuery_Click(
        winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
    {
        if (!sqlEditor_) return;
        std::wstring sql(sqlEditor_.Text());
        std::vector<std::pair<std::wstring, std::wstring>> keywords = {
            {L"select ", L"SELECT "}, {L"from ", L"FROM "}, {L"where ", L"WHERE "},
            {L"and ", L"AND "}, {L"or ", L"OR "}, {L"join ", L"JOIN "},
            {L"left ", L"LEFT "}, {L"right ", L"RIGHT "}, {L"inner ", L"INNER "},
            {L"outer ", L"OUTER "}, {L"on ", L"ON "}, {L"group by ", L"GROUP BY "},
            {L"order by ", L"ORDER BY "}, {L"having ", L"HAVING "},
            {L"insert into ", L"INSERT INTO "}, {L"values ", L"VALUES "},
            {L"update ", L"UPDATE "}, {L"set ", L"SET "},
            {L"delete from ", L"DELETE FROM "}, {L"create table ", L"CREATE TABLE "},
            {L"alter table ", L"ALTER TABLE "}, {L"drop table ", L"DROP TABLE "},
            {L"limit ", L"LIMIT "}, {L"offset ", L"OFFSET "},
            {L"as ", L"AS "}, {L"distinct ", L"DISTINCT "},
            {L"not ", L"NOT "}, {L"in ", L"IN "}, {L"is ", L"IS "},
            {L"null", L"NULL"}, {L"like ", L"LIKE "},
        };
        for (auto& [lower, upper] : keywords)
        {
            size_t pos = 0;
            std::wstring sqlLower = sql;
            std::transform(sqlLower.begin(), sqlLower.end(), sqlLower.begin(), ::towlower);
            while ((pos = sqlLower.find(lower, pos)) != std::wstring::npos)
            {
                sql.replace(pos, lower.size(), upper);
                sqlLower.replace(pos, lower.size(), upper);
                pos += upper.size();
            }
        }
        suppressTextChange_ = true;
        sqlEditor_.Text(winrt::hstring(sql));
        suppressTextChange_ = false;
    }

    void QueryEditorView::ClearEditor_Click(
        winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
    {
        if (sqlEditor_)
        {
            suppressTextChange_ = true;
            sqlEditor_.Text(L"");
            suppressTextChange_ = false;
        }
        HideSuggestions();
    }

    // ── Results display ────────────────────────────────

    void QueryEditorView::ShowResult(const DBModels::QueryResult& result)
    {
        {
            QueryPerfStage s(L"ResultsContainer.Clear");
            ResultsContainer().Children().Clear();
        }

        std::wstring status = std::to_wstring(result.totalRows) + L" rows  \u00B7  " +
            std::to_wstring(static_cast<int>(result.executionTimeMs)) + L" ms";
        // Cap how many rows we realize in the visual tree. Building +
        // clearing 47k XAML elements (1817 rows x 25 cols x 1.x children
        // each) freezes the UI for seconds even with chunked render.
        // Until we wire ItemsRepeater virtualization, truncate to a
        // sane upper bound and surface the truncation in the status.
        constexpr int kMaxRenderedRows = 500;
        const int totalRowsAll = static_cast<int>(result.rows.size());
        const int truncatedTo  = (totalRowsAll > kMaxRenderedRows)
                                     ? kMaxRenderedRows : totalRowsAll;
        if (totalRowsAll > truncatedTo)
            status += L"  ·  showing first " + std::to_wstring(truncatedTo);
        QueryStatusText().Text(winrt::hstring(status));
        ResultsHeader().Visibility(mux::Visibility::Visible);
        ResultsSummaryText().Text(winrt::hstring(status));

        if (result.columnNames.empty()) return;

        // Initialize per-column widths: distribute the container width evenly,
        // clamped to [RESULT_COL_MIN_WIDTH, RESULT_COL_MAX_WIDTH]. Stored in
        // resultColumnWidths_ so resize drags can mutate them without
        // recomputing the initial layout.
        const size_t nCols = result.columnNames.size();
        double colWidth = RESULT_COL_DEFAULT_WIDTH;
        double availWidth = this->ActualWidth() - 20.0;
        if (availWidth > 0 && nCols > 0)
        {
            colWidth = availWidth / static_cast<double>(nCols);
            if (colWidth < 80.0) colWidth = 80.0;
            if (colWidth > 400.0) colWidth = 400.0;
        }
        resultColumnWidths_.assign(nCols, colWidth);

        {
            QueryPerfStage s(L"BuildResultHeaders");
            BuildResultHeaders(result);
        }

        // Render only the truncated slice. Copy is cheap relative to the
        // XAML element creation cost we just avoided.
        if (totalRowsAll > truncatedTo)
        {
            DBModels::QueryResult sliced = result;
            sliced.rows.resize(truncatedTo);
            BuildResultRows(sliced);
        }
        else
        {
            BuildResultRows(result);
        }
    }

    // ── Result-grid headers with drag-to-resize grip ──────────────
    //
    // Mirrors DataGridView::BuildHeaders but without sort / row-number
    // column. Each header cell is a 2-column Grid: [label *] [grip 8px].
    // The grip captures pointer events, updates its own header Grid.Width
    // on PointerMoved for immediate visual feedback, and rebuilds the row
    // StackPanels once on PointerReleased.
    void QueryEditorView::BuildResultHeaders(const DBModels::QueryResult& result)
    {
        muxc::StackPanel headerRow;
        headerRow.Orientation(muxc::Orientation::Horizontal);
        headerRow.Background(muxm::SolidColorBrush(
            winrt::Windows::UI::ColorHelper::FromArgb(15, 128, 128, 128)));

        // Share ONE QueryResult instance across all column lambdas.
        // Previously each column copied the entire result by value into
        // its own PointerReleased handler — for a 1817×25 result that's
        // 25 deep copies of a million-wstring table (4+ seconds on
        // mid-size queries; user hit a hard freeze). shared_ptr keeps
        // the rebuild path working (resize-end rebuilds rows from the
        // same data) without the per-column duplication.
        auto resultShared = std::make_shared<DBModels::QueryResult>(result);

        for (size_t ci = 0; ci < result.columnNames.size(); ++ci)
        {
            const double w = (ci < resultColumnWidths_.size())
                ? resultColumnWidths_[ci] : RESULT_COL_DEFAULT_WIDTH;

            muxc::Grid cellGrid;
            cellGrid.Width(w);

            muxc::ColumnDefinition contentCd, gripCd;
            contentCd.Width(mux::GridLength{ 1.0, mux::GridUnitType::Star });
            gripCd.Width(mux::GridLength{ 8.0, mux::GridUnitType::Pixel });
            cellGrid.ColumnDefinitions().Append(contentCd);
            cellGrid.ColumnDefinitions().Append(gripCd);

            // Header label
            muxc::Border labelWrap;
            labelWrap.Padding(mux::Thickness{ 8, 4, 4, 4 });
            labelWrap.BorderBrush(muxm::SolidColorBrush(
                winrt::Windows::UI::ColorHelper::FromArgb(40, 128, 128, 128)));
            labelWrap.BorderThickness(mux::Thickness{ 0, 0, 0, 1 });
            muxc::Grid::SetColumn(labelWrap, 0);

            muxc::TextBlock lbl;
            lbl.Text(winrt::hstring(result.columnNames[ci]));
            lbl.FontSize(11.0);
            lbl.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            lbl.TextTrimming(mux::TextTrimming::CharacterEllipsis);
            lbl.TextWrapping(mux::TextWrapping::NoWrap);
            labelWrap.Child(lbl);
            cellGrid.Children().Append(labelWrap);

            // Resize grip (transparent 8px hit area with 1px centered line).
            muxc::Grid resizeGrip;
            resizeGrip.Background(muxm::SolidColorBrush(winrt::Windows::UI::Colors::Transparent()));
            muxc::Grid::SetColumn(resizeGrip, 1);

            muxc::Border gripLine;
            gripLine.Width(1.0);
            gripLine.HorizontalAlignment(mux::HorizontalAlignment::Center);
            gripLine.Background(muxm::SolidColorBrush(
                winrt::Windows::UI::ColorHelper::FromArgb(50, 128, 128, 128)));
            gripLine.IsHitTestVisible(false);
            resizeGrip.Children().Append(gripLine);

            // Cursor via ProtectedCursor (same rationale as DataGridView).
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

            const int colIndex = static_cast<int>(ci);

            resizeGrip.PointerPressed(
                [this, colIndex](winrt::Windows::Foundation::IInspectable const& sender,
                                 mux::Input::PointerRoutedEventArgs const& e)
                {
                    resizingResultCol_ = colIndex;
                    auto point = e.GetCurrentPoint(this->try_as<mux::UIElement>());
                    resizeResultStartX_ = point.Position().X;
                    resizeResultStartWidth_ = (colIndex < static_cast<int>(resultColumnWidths_.size()))
                        ? resultColumnWidths_[colIndex] : RESULT_COL_DEFAULT_WIDTH;
                    if (auto el = sender.try_as<mux::UIElement>()) el.CapturePointer(e.Pointer());
                    e.Handled(true);
                });

            resizeGrip.PointerMoved(
                [this](winrt::Windows::Foundation::IInspectable const&,
                       mux::Input::PointerRoutedEventArgs const& e)
                {
                    if (resizingResultCol_ < 0) return;
                    auto point = e.GetCurrentPoint(this->try_as<mux::UIElement>());
                    double delta = point.Position().X - resizeResultStartX_;
                    double newWidth = resizeResultStartWidth_ + delta;
                    if (newWidth < RESULT_COL_MIN_WIDTH) newWidth = RESULT_COL_MIN_WIDTH;
                    if (newWidth > RESULT_COL_MAX_WIDTH) newWidth = RESULT_COL_MAX_WIDTH;
                    if (resizingResultCol_ < static_cast<int>(resultColumnWidths_.size()))
                    {
                        resultColumnWidths_[resizingResultCol_] = newWidth;
                        // Live-update the header cell width. Row #0 inside
                        // ResultsContainer is the header StackPanel; its
                        // child ci is this column's cell Grid.
                        auto container = ResultsContainer();
                        if (container.Children().Size() > 0)
                        {
                            auto hdr = container.Children().GetAt(0)
                                .try_as<muxc::StackPanel>();
                            if (hdr && static_cast<uint32_t>(resizingResultCol_) < hdr.Children().Size())
                            {
                                auto cell = hdr.Children().GetAt(resizingResultCol_)
                                    .try_as<muxc::Grid>();
                                if (cell) cell.Width(newWidth);
                            }
                        }
                    }
                    e.Handled(true);
                });

            resizeGrip.PointerReleased(
                [this, resultShared](winrt::Windows::Foundation::IInspectable const& sender,
                                     mux::Input::PointerRoutedEventArgs const& e)
                {
                    if (resizingResultCol_ >= 0)
                    {
                        if (auto el = sender.try_as<mux::UIElement>())
                            el.ReleasePointerCapture(e.Pointer());
                        resizingResultCol_ = -1;
                        // Rebuild rows only -- keeping the header avoids
                        // rewiring every grip's pointer handlers.
                        BuildResultRows(*resultShared);
                    }
                    e.Handled(true);
                });

            cellGrid.Children().Append(resizeGrip);
            headerRow.Children().Append(cellGrid);
        }

        ResultsContainer().Children().Append(headerRow);
    }

    // ── Result-grid rows (header-independent, rebuilt on resize end) ──
    namespace {
        // Build a single result-row StackPanel. Pulled out so the
        // chunked tail can call it without duplicating the cell layout.
        muxc::StackPanel BuildResultRow(
            const DBModels::QueryResult& result, int i,
            const std::vector<double>& colWidths,
            double defaultWidth)
        {
            auto& row = result.rows[i];
            muxc::StackPanel rowPanel;
            rowPanel.Orientation(muxc::Orientation::Horizontal);
            if (i % 2 == 1)
                rowPanel.Background(muxm::SolidColorBrush(
                    winrt::Windows::UI::ColorHelper::FromArgb(8, 128, 128, 128)));

            for (size_t ci = 0; ci < result.columnNames.size(); ++ci)
            {
                const auto& col = result.columnNames[ci];
                const double w = (ci < colWidths.size())
                    ? colWidths[ci] : defaultWidth;

                std::wstring val;
                auto it = row.find(col);
                if (it != row.end()) val = it->second;
                const bool isNull = DBModels::isNullCell(val);
                const std::wstring display = isNull ? std::wstring(L"NULL") : val;

                muxc::Border cell;
                cell.Width(w);
                cell.Padding(mux::Thickness{ 8, 4, 8, 4 });
                cell.BorderBrush(muxm::SolidColorBrush(
                    winrt::Windows::UI::ColorHelper::FromArgb(20, 128, 128, 128)));
                cell.BorderThickness(mux::Thickness{ 0, 0, 1, 0 });
                muxc::TextBlock lbl;
                lbl.Text(winrt::hstring(display));
                lbl.FontSize(12.0);
                lbl.IsTextSelectionEnabled(true);
                lbl.TextTrimming(mux::TextTrimming::CharacterEllipsis);
                if (isNull || val.empty()) lbl.Opacity(0.4);
                cell.Child(lbl);
                rowPanel.Children().Append(cell);
            }
            return rowPanel;
        }
    }

    // Chunked render: synchronous first chunk for instant first paint,
    // then a coroutine streams the rest in 100-row batches yielding
    // ~1 frame between chunks. Mirrors DataGridView's tail pattern.
    void QueryEditorView::BuildResultRows(const DBModels::QueryResult& result)
    {
        QueryPerfStage stage(L"BuildResultRows (sync first chunk)");
        // Remove any existing row StackPanels but keep the header (index 0).
        auto children = ResultsContainer().Children();
        while (children.Size() > 1) children.RemoveAt(1);

        const int totalRows = static_cast<int>(result.rows.size());
        constexpr int kFirstChunk = 100;
        int firstEnd = (totalRows < kFirstChunk) ? totalRows : kFirstChunk;

        for (int i = 0; i < firstEnd; ++i)
            ResultsContainer().Children().Append(
                BuildResultRow(result, i, resultColumnWidths_, RESULT_COL_DEFAULT_WIDTH));

        wchar_t buf[256];
        swprintf_s(buf, L"[QueryPerf]   first chunk %d rows appended", firstEnd);
        perfLog(buf);

        if (firstEnd >= totalRows) return;

        // New generation cancels any in-flight tail from a previous run.
        const uint64_t gen = ++buildResultGeneration_;
        BuildResultRowsTail(result, gen, firstEnd);
    }

    winrt::fire_and_forget QueryEditorView::BuildResultRowsTail(
        DBModels::QueryResult result, uint64_t gen, int startIdx)
    {
        constexpr int kChunk = 100;
        const int totalRows = static_cast<int>(result.rows.size());
        auto self = get_strong();
        auto dq = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();

        try
        {
            for (int i = startIdx; i < totalRows; i += kChunk)
            {
                if (self->buildResultGeneration_ != gen) co_return; // superseded
                int end = (totalRows < i + kChunk) ? totalRows : i + kChunk;
                auto container = self->ResultsContainer();
                if (!container) co_return;
                for (int j = i; j < end; ++j)
                    container.Children().Append(
                        BuildResultRow(result, j,
                                       self->resultColumnWidths_,
                                       RESULT_COL_DEFAULT_WIDTH));
                if (end >= totalRows) break;
                co_await winrt::resume_after(std::chrono::milliseconds(16));
                if (!dq) co_return;
                co_await ResumeOnDispatcher{ dq };
            }
        }
        catch (...) { /* page closed mid-stream — drop quietly */ }
    }

    void QueryEditorView::ShowError(const std::wstring& message)
    {
        ResultsContainer().Children().Clear();
        QueryStatusText().Text(L"Error");
        ResultsHeader().Visibility(mux::Visibility::Collapsed);

        muxc::TextBlock errText;
        errText.Text(winrt::hstring(message));
        errText.FontSize(12.0);
        errText.Padding(mux::Thickness{ 12, 8, 12, 8 });
        errText.Foreground(muxm::SolidColorBrush(
            winrt::Windows::UI::ColorHelper::FromArgb(255, 196, 43, 28)));
        errText.TextWrapping(mux::TextWrapping::Wrap);
        errText.IsTextSelectionEnabled(true);
        ResultsContainer().Children().Append(errText);
    }
}
