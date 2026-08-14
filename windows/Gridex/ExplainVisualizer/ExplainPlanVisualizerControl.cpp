#include "pch.h"
// Undo windows.h max/min macros — they collide with std::max/min/clamp
// used heavily in the rendering math.
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif
#include "xaml-includes.h"
#include "ExplainPlanVisualizerControl.h"
#if __has_include("ExplainPlanVisualizerControl.g.cpp")
#include "ExplainPlanVisualizerControl.g.cpp"
#endif

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Text.h>

#include <algorithm>
#include <functional>
#include <iomanip>
#include <sstream>

namespace winrt::Gridex::implementation
{
    namespace EX   = ::Gridex::ExplainViz;
    namespace mux  = winrt::Microsoft::UI::Xaml;
    namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
    namespace muxm = winrt::Microsoft::UI::Xaml::Media;
    namespace muxs = winrt::Microsoft::UI::Xaml::Shapes;
    using winrt::Windows::UI::Color;
    using winrt::Windows::Foundation::IInspectable;
    // Thickness lives in Microsoft.UI.Xaml — not in Windows.Foundation
    // (despite Padding/Margin signatures using the type by struct name).

    // ── Small XAML helpers (mirror cell-builders elsewhere in the codebase) ──
    static muxc::TextBlock TB(const std::wstring& text, double fontSize, double opacity = 1.0,
                              bool semibold = false)
    {
        muxc::TextBlock t;
        t.Text(winrt::hstring(text));
        t.FontSize(fontSize);
        t.Opacity(opacity);
        if (semibold) t.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        t.IsTextSelectionEnabled(false);
        return t;
    }

    static muxc::FontIcon FI(const std::wstring& glyph, double size)
    {
        muxc::FontIcon f;
        f.Glyph(winrt::hstring(glyph));
        f.FontSize(size);
        return f;
    }

    static muxm::SolidColorBrush Brush(Color c)
    {
        return muxm::SolidColorBrush(c);
    }

    // Renamed from MakeColor() — wingdi.h #defines RGB as a macro that would
    // mangle the function signature.
    static Color MakeColor(uint8_t r, uint8_t g, uint8_t b)
    {
        return Color{ 255, r, g, b };
    }

    static muxc::Border CardBorder()
    {
        muxc::Border b;
        b.CornerRadius(mux::CornerRadiusHelper::FromRadii(10, 10, 10, 10));
        b.BorderThickness(mux::Thickness{ 1, 1, 1, 1 });
        b.Padding(mux::Thickness{ 16, 14, 16, 14 });
        try {
            b.Background(unbox_value<muxm::Brush>(
                mux::Application::Current().Resources().Lookup(
                    box_value(L"LayerFillColorDefaultBrush"))));
            b.BorderBrush(unbox_value<muxm::Brush>(
                mux::Application::Current().Resources().Lookup(
                    box_value(L"CardStrokeColorDefaultBrush"))));
        } catch (...) {}
        return b;
    }

    // ── Formatting helpers ──────────────────────────────
    static std::wstring fmtMs(double ms)
    {
        std::wstringstream ss;
        if (ms >= 10) ss << std::fixed << std::setprecision(0) << ms;
        else ss << std::fixed << std::setprecision(2) << ms;
        return ss.str();
    }

    static std::wstring fmtNum(double n)
    {
        // Locale-friendly thousand separator (commas).
        std::wstringstream ss;
        ss << static_cast<long long>(n);
        std::wstring s = ss.str();
        std::wstring out;
        int c = 0;
        for (auto it = s.rbegin(); it != s.rend(); ++it) {
            if (c && c % 3 == 0 && *it != L'-') out += L',';
            out += *it;
            ++c;
        }
        std::reverse(out.begin(), out.end());
        return out;
    }

    static std::wstring fmtBytes(uint64_t n)
    {
        std::wstringstream ss;
        if (n < 1024)            { ss << n << L" B"; }
        else if (n < 1024*1024)  { ss << std::fixed << std::setprecision(1) << (n/1024.0) << L" KB"; }
        else if (n < 1024ULL*1024*1024) { ss << std::fixed << std::setprecision(2) << (n/(1024.0*1024)) << L" MB"; }
        else                     { ss << std::fixed << std::setprecision(2) << (n/(1024.0*1024*1024)) << L" GB"; }
        return ss.str();
    }

    ExplainPlanVisualizerControl::ExplainPlanVisualizerControl()
    {
        InitializeComponent();
    }

    // ── Public API ────────────────────────────────────
    void ExplainPlanVisualizerControl::SetPlan(std::shared_ptr<EX::PlanDocument> doc)
    {
        doc_ = doc;
        if (!doc_ || !doc_->root) {
            ShowError(L"Plan was empty.");
            return;
        }
        // Run heuristics — may upgrade NodeLevel on the tree.
        issues_ = EX::ExplainPlanHeuristics::analyze(*doc_);
        recs_ = EX::ExplainPlanHeuristics::recommend(*doc_, issues_);
        // Default selection: hottest node (Bad with highest pct), else root.
        selectedNodeId_.clear();
        EX::PlanNode* hot = nullptr;
        std::function<void(EX::PlanNode&)> walk = [&](EX::PlanNode& n) {
            if (n.level == EX::NodeLevel::Bad &&
                (!hot || n.pctOfTotal > hot->pctOfTotal)) hot = &n;
            for (auto& c : n.children) walk(*c);
        };
        walk(*doc_->root);
        selectedNodeId_ = hot ? hot->id : doc_->root->id;

        SwitchState(State::Loaded);
        RenderInfoBar();
        RenderMetrics();
        RenderPlanTree();
        RenderFlyout();
        RenderWaterfall();
        RenderRecommendations();
    }

    void ExplainPlanVisualizerControl::ShowEmpty() { SwitchState(State::Empty); }
    void ExplainPlanVisualizerControl::ShowLoading() { SwitchState(State::Loading); }
    void ExplainPlanVisualizerControl::ShowError(const std::wstring& m)
    {
        ErrorText().Text(winrt::hstring(m));
        SwitchState(State::Error);
    }
    void ExplainPlanVisualizerControl::Reset() { doc_.reset(); ShowEmpty(); }

    void ExplainPlanVisualizerControl::SwitchState(State s)
    {
        EmptyState().Visibility(s == State::Empty ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        LoadingState().Visibility(s == State::Loading ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        ErrorState().Visibility(s == State::Error ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        LoadedState().Visibility(s == State::Loaded ? mux::Visibility::Visible : mux::Visibility::Collapsed);
    }

    // ── Click handlers ──────────────────────────────
    void ExplainPlanVisualizerControl::OnRunClicked(IInspectable const&, mux::RoutedEventArgs const&)
    {
        if (OnRunRequested) OnRunRequested();
    }
    void ExplainPlanVisualizerControl::OnCopyPlanClicked(IInspectable const&, mux::RoutedEventArgs const&)
    {
        if (OnCopyPlanRequested && doc_) OnCopyPlanRequested(doc_->rawJson);
    }
    void ExplainPlanVisualizerControl::OnHotOnlyToggled(IInspectable const&, mux::RoutedEventArgs const&)
    {
        // IsChecked() can be null on indeterminate state; default false.
        auto v = HotOnlyToggle().IsChecked();
        hotOnly_ = v ? v.Value() : false;
        RenderPlanTree();
    }

    // ── InfoBar ──────────────────────────────
    void ExplainPlanVisualizerControl::RenderInfoBar()
    {
        if (issues_.empty()) {
            WarnInfoBar().IsOpen(false);
            return;
        }
        const auto& top = issues_.front();
        WarnInfoBar().Severity(
            top.severity == EX::IssueSeverity::Bad ? muxc::InfoBarSeverity::Error :
            top.severity == EX::IssueSeverity::Warn ? muxc::InfoBarSeverity::Warning :
                                                       muxc::InfoBarSeverity::Informational);
        WarnInfoBar().Title(winrt::hstring(top.title));
        WarnInfoBar().Message(winrt::hstring(top.detail));
        WarnInfoBar().IsOpen(true);
    }

    // ── Metrics ──────────────────────────────
    void ExplainPlanVisualizerControl::RenderMetrics()
    {
        if (!doc_ || !doc_->root) return;
        // Total time
        double total = doc_->executionMs > 0 ? doc_->executionMs : 0.0;
        double planning = doc_->planningMs;
        std::wstring delta = planning > 0
            ? L"planning " + fmtMs(planning) + L"ms · execution " + fmtMs(total) + L"ms"
            : L"execution " + fmtMs(total) + L"ms";
        CardTotalTime().Content(BuildMetricCard(
            L"" /*clock*/, L"Total time", fmtMs(total), L"ms", delta,
            std::min(100.0, total / 1000.0 * 50.0),  // arbitrary bar 0..100%
            total > 1000 ? L"bad" : total > 250 ? L"warn" : L"good"));

        // Rows
        double rowsActual = doc_->root->actualRows;
        double rowsScanned = 0.0;
        std::function<void(const EX::PlanNode&)> sumScan = [&](const EX::PlanNode& n) {
            if (n.kind == EX::NodeKind::SeqScan ||
                n.kind == EX::NodeKind::IndexScan ||
                n.kind == EX::NodeKind::IndexOnlyScan ||
                n.kind == EX::NodeKind::BitmapHeapScan)
                rowsScanned += n.actualRows + n.rowsRemovedByFilter;
            for (auto& c : n.children) sumScan(*c);
        };
        sumScan(*doc_->root);
        std::wstring rowsDelta = rowsScanned > 0
            ? L"selectivity " + fmtMs(rowsActual / rowsScanned * 100) + L"%"
            : L"";
        CardRows().Content(BuildMetricCard(
            L"" /*list*/, L"Rows returned", fmtNum(rowsActual),
            rowsScanned > 0 ? (L" / " + fmtNum(rowsScanned) + L" scanned") : L"",
            rowsDelta,
            rowsScanned > 0 ? std::min(100.0, rowsActual / rowsScanned * 100) : 0.0,
            L""));

        // Buffers
        uint64_t totalHit = 0, totalRead = 0;
        std::function<void(const EX::PlanNode&)> sumBuf = [&](const EX::PlanNode& n) {
            totalHit += n.sharedHit;
            totalRead += n.sharedRead;
            for (auto& c : n.children) sumBuf(*c);
        };
        sumBuf(*doc_->root);
        uint64_t bufBytes = (totalHit + totalRead) * 8ULL * 1024; // 8 KB pages
        std::wstring bufDelta = std::to_wstring(totalHit) + L" hit · "
                               + std::to_wstring(totalRead) + L" read";
        CardBuffers().Content(BuildMetricCard(
            L"" /*memory*/, L"Buffers", fmtBytes(bufBytes), L"", bufDelta,
            totalRead > 1000 ? 70.0 : 30.0,
            totalRead > 1000 ? L"warn" : L""));

        // Planner accuracy — biggest est/actual mismatch ratio in tree.
        double worstFactor = 1.0;
        std::function<void(const EX::PlanNode&)> walk = [&](const EX::PlanNode& n) {
            if (n.planRows >= 100 && n.actualRows >= 100) {
                double f = std::max(n.planRows, n.actualRows) /
                           std::max(1.0, std::min(n.planRows, n.actualRows));
                if (f > worstFactor) worstFactor = f;
            }
            for (auto& c : n.children) walk(*c);
        };
        walk(*doc_->root);
        std::wstringstream accDelta;
        accDelta << std::fixed << std::setprecision(1) << worstFactor << L"× worst-case error";
        CardAccuracy().Content(BuildMetricCard(
            L"" /*target*/, L"Planner accuracy", fmtMs(worstFactor), L"×",
            accDelta.str(),
            std::min(100.0, (worstFactor - 1) * 20.0),
            worstFactor >= 5 ? L"bad" : worstFactor >= 2 ? L"warn" : L"good"));

        // Plan summary header text.
        int nodeCount = 0;
        std::function<void(const EX::PlanNode&)> countNodes = [&](const EX::PlanNode& n) {
            ++nodeCount;
            for (auto& c : n.children) countNodes(*c);
        };
        countNodes(*doc_->root);
        PlanCountText().Text(winrt::hstring(
            std::to_wstring(nodeCount) + L" nodes · planner cost "
            + fmtMs(doc_->root->totalCost)));

        MetaText().Text(winrt::hstring(
            L"Postgres · execution " + fmtMs(total) + L"ms · planning " + fmtMs(planning) + L"ms"));
    }

    mux::UIElement ExplainPlanVisualizerControl::BuildMetricCard(
        const std::wstring& glyph, const std::wstring& label,
        const std::wstring& value, const std::wstring& unit,
        const std::wstring& delta, double barPct, const std::wstring& tone)
    {
        auto outer = CardBorder();
        muxc::StackPanel sp;
        sp.Spacing(8);

        // Header row: glyph + label
        muxc::StackPanel head;
        head.Orientation(muxc::Orientation::Horizontal);
        head.Spacing(6);
        head.Children().Append(FI(glyph, 14));
        head.Children().Append(TB(label, 12, 0.7));
        sp.Children().Append(head);

        // Value + unit
        muxc::StackPanel vrow;
        vrow.Orientation(muxc::Orientation::Horizontal);
        vrow.Spacing(4);
        vrow.VerticalAlignment(mux::VerticalAlignment::Bottom);
        auto val = TB(value, 28, 1.0, true);
        vrow.Children().Append(val);
        if (!unit.empty()) vrow.Children().Append(TB(unit, 12, 0.6));
        sp.Children().Append(vrow);

        // Delta line
        if (!delta.empty()) {
            auto d = TB(delta, 12, 0.7);
            if (tone == L"bad") d.Foreground(Brush(MakeColor(196, 43, 28)));
            else if (tone == L"warn") d.Foreground(Brush(MakeColor(224, 168, 0)));
            else if (tone == L"good") d.Foreground(Brush(MakeColor(16, 124, 16)));
            sp.Children().Append(d);
        }

        // Mini bar
        muxc::Grid bar;
        bar.Height(6);
        bar.CornerRadius(mux::CornerRadiusHelper::FromUniformRadius(3));
        try {
            bar.Background(unbox_value<muxm::Brush>(
                mux::Application::Current().Resources().Lookup(
                    box_value(L"ControlAltFillColorTertiaryBrush"))));
        } catch (...) {}
        muxc::Grid fill;
        fill.HorizontalAlignment(mux::HorizontalAlignment::Left);
        fill.CornerRadius(mux::CornerRadiusHelper::FromUniformRadius(2));
        Color c = tone == L"bad" ? MakeColor(196, 43, 28)
                : tone == L"warn" ? MakeColor(224, 168, 0)
                : MakeColor(120, 96, 217); // accent
        fill.Background(Brush(c));
        // Setting width via percent on a Grid child is tricky — we wrap in
        // a Border with explicit Width using the parent's actual size on
        // first SizeChanged. For static design we approximate using a
        // fixed-width container scaled by barPct% of card content width.
        // Position the fill on initial layout AND on every resize so the
        // bar is non-empty before the parent grid first measures it.
        auto sizeFill = [fill, barPct](double w) {
            fill.Width(std::max(2.0, w * std::clamp(barPct, 0.0, 100.0) / 100.0));
        };
        bar.SizeChanged([sizeFill](IInspectable const&, mux::SizeChangedEventArgs const& e) {
            sizeFill(e.NewSize().Width);
        });
        bar.Loaded([bar, sizeFill](IInspectable const&, mux::RoutedEventArgs const&) {
            sizeFill(bar.ActualWidth());
        });
        bar.Children().Append(fill);
        sp.Children().Append(bar);

        outer.Child(sp);
        return outer;
    }

    // ── Plan tree ──────────────────────────────
    std::shared_ptr<EX::PlanNode> ExplainPlanVisualizerControl::FindNode(
        const std::shared_ptr<EX::PlanNode>& n, const std::wstring& id)
    {
        if (!n) return nullptr;
        if (n->id == id) return n;
        for (auto& c : n->children) {
            if (auto r = FindNode(c, id)) return r;
        }
        return nullptr;
    }

    void ExplainPlanVisualizerControl::RenderPlanTree()
    {
        PlanTreeContainer().Children().Clear();
        if (!doc_ || !doc_->root) return;
        BuildNodeRow(doc_->root, 0);
    }

    void ExplainPlanVisualizerControl::BuildNodeRow(
        const std::shared_ptr<EX::PlanNode>& node, int depth)
    {
        if (!node) return;
        // Hot-only filter: skip nodes that aren't Bad/Warn unless they have
        // a hot descendant (we still need to descend).
        bool isHot = node->level != EX::NodeLevel::Good;
        bool hasHotChild = false;
        std::function<void(const EX::PlanNode&)> findHot = [&](const EX::PlanNode& n) {
            if (n.level != EX::NodeLevel::Good) hasHotChild = true;
            for (auto& c : n.children) findHot(*c);
        };
        findHot(*node);

        bool include = (!hotOnly_) || isHot || hasHotChild;
        if (!include) return;

        // Row layout: indent | glyph | body | heat
        muxc::Grid row;
        row.Padding(mux::Thickness{ 8.0 + depth * 18.0, 6, 8, 6 });
        row.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        muxc::ColumnDefinition c0; c0.Width(mux::GridLengthHelper::Auto());
        muxc::ColumnDefinition c1; c1.Width(mux::GridLengthHelper::FromValueAndType(1, mux::GridUnitType::Star));
        muxc::ColumnDefinition c2; c2.Width(mux::GridLengthHelper::Auto());
        row.ColumnDefinitions().Append(c0);
        row.ColumnDefinitions().Append(c1);
        row.ColumnDefinitions().Append(c2);

        // Selection highlight
        if (node->id == selectedNodeId_) {
            try {
                row.Background(unbox_value<muxm::Brush>(
                    mux::Application::Current().Resources().Lookup(
                        box_value(L"AccentFillColorTertiaryBrush"))));
            } catch (...) {}
        }
        row.CornerRadius(mux::CornerRadiusHelper::FromUniformRadius(6));

        // Glyph badge
        muxc::Border g;
        g.Width(34); g.Height(28);
        g.CornerRadius(mux::CornerRadiusHelper::FromUniformRadius(5));
        g.Padding(mux::Thickness{ 4, 2, 4, 2 });
        g.Margin(mux::Thickness{ 0, 0, 10, 0 });
        g.VerticalAlignment(mux::VerticalAlignment::Center);
        g.Background(Brush(HeatBgColor(node->level)));
        auto gtxt = TB(KindGlyph(node->kind), 13, 1.0, true);
        gtxt.HorizontalAlignment(mux::HorizontalAlignment::Center);
        gtxt.VerticalAlignment(mux::VerticalAlignment::Center);
        gtxt.Foreground(Brush(HeatColor(node->level)));
        g.Child(gtxt);
        muxc::Grid::SetColumn(g, 0);
        row.Children().Append(g);

        // Body — op + relation + detail + cost line
        muxc::StackPanel body;
        body.Spacing(2);
        muxc::StackPanel headRow;
        headRow.Orientation(muxc::Orientation::Horizontal);
        headRow.Spacing(8);
        auto opTxt = TB(node->op, 14, 1.0, true);
        headRow.Children().Append(opTxt);
        if (!node->relation.empty()) {
            std::wstring relText = node->relation;
            if (!node->alias.empty()) relText += L" " + node->alias;
            auto rel = TB(relText, 13, 0.85);
            try {
                rel.FontFamily(muxm::FontFamily(L"Cascadia Code, Consolas, monospace"));
            } catch (...) {}
            headRow.Children().Append(rel);
        }
        if (node->level == EX::NodeLevel::Bad) {
            muxc::Border pill;
            pill.CornerRadius(mux::CornerRadiusHelper::FromUniformRadius(8));
            pill.Padding(mux::Thickness{ 6, 1, 6, 1 });
            pill.Background(Brush(MakeColor(253, 231, 233)));
            auto pt = TB(L"hot path", 10, 1.0);
            pt.Foreground(Brush(MakeColor(196, 43, 28)));
            pill.Child(pt);
            headRow.Children().Append(pill);
        } else if (node->level == EX::NodeLevel::Warn) {
            muxc::Border pill;
            pill.CornerRadius(mux::CornerRadiusHelper::FromUniformRadius(8));
            pill.Padding(mux::Thickness{ 6, 1, 6, 1 });
            pill.Background(Brush(MakeColor(255, 244, 206)));
            auto pt = TB(L"watch", 10, 1.0);
            pt.Foreground(Brush(MakeColor(138, 109, 0)));
            pill.Child(pt);
            headRow.Children().Append(pill);
        }
        body.Children().Append(headRow);

        if (!node->detail.empty()) {
            auto d = TB(node->detail, 12, 0.7);
            d.TextWrapping(mux::TextWrapping::NoWrap);
            d.TextTrimming(mux::TextTrimming::CharacterEllipsis);
            body.Children().Append(d);
        }

        // Cost / rows / actual line
        std::wstringstream cost;
        cost << L"cost " << std::fixed << std::setprecision(2) << node->startupCost
             << L"…" << std::fixed << std::setprecision(2) << node->totalCost
             << L"  ·  rows " << fmtNum(node->actualRows)
             << L"  ·  width " << node->planWidth;
        if (node->analyzed) {
            cost << L"  ·  actual " << fmtMs(node->actualTotal) << L" ms";
            if (node->actualLoops > 1) cost << L" × " << node->actualLoops << L" loops";
        }
        auto costTxt = TB(cost.str(), 12, 0.6);
        try {
            costTxt.FontFamily(muxm::FontFamily(L"Cascadia Code, Consolas, monospace"));
        } catch (...) {}
        body.Children().Append(costTxt);

        muxc::Grid::SetColumn(body, 1);
        row.Children().Append(body);

        // Heat bar
        muxc::StackPanel heat;
        heat.Width(140);
        heat.Spacing(4);
        heat.VerticalAlignment(mux::VerticalAlignment::Center);
        std::wstringstream pctTxt;
        pctTxt << std::fixed << std::setprecision(1) << node->pctOfTotal << L"%";
        auto pct = TB(pctTxt.str(), 13, 1.0, true);
        pct.HorizontalAlignment(mux::HorizontalAlignment::Right);
        pct.Foreground(Brush(HeatColor(node->level)));
        heat.Children().Append(pct);
        muxc::Grid hbar;
        hbar.Height(7);
        hbar.CornerRadius(mux::CornerRadiusHelper::FromUniformRadius(2));
        try {
            hbar.Background(unbox_value<muxm::Brush>(
                mux::Application::Current().Resources().Lookup(
                    box_value(L"ControlAltFillColorTertiaryBrush"))));
        } catch (...) {}
        muxc::Grid hfill;
        hfill.HorizontalAlignment(mux::HorizontalAlignment::Left);
        hfill.CornerRadius(mux::CornerRadiusHelper::FromUniformRadius(2));
        hfill.Background(Brush(HeatColor(node->level)));
        double pctFill = std::max(2.0, std::min(100.0, node->pctOfTotal));
        auto sizeHFill = [hfill, pctFill](double w) {
            hfill.Width(std::max(2.0, w * pctFill / 100.0));
        };
        hbar.SizeChanged([sizeHFill](IInspectable const&, mux::SizeChangedEventArgs const& e) {
            sizeHFill(e.NewSize().Width);
        });
        hbar.Loaded([hbar, sizeHFill](IInspectable const&, mux::RoutedEventArgs const&) {
            sizeHFill(hbar.ActualWidth());
        });
        hbar.Children().Append(hfill);
        heat.Children().Append(hbar);
        muxc::Grid::SetColumn(heat, 2);
        row.Children().Append(heat);

        // Click → select
        std::wstring nid = node->id;
        row.PointerReleased([this, nid](IInspectable const&, auto const&) {
            SelectNode(nid);
        });
        row.PointerEntered([row](IInspectable const&, auto const&) {
            try {
                row.Background(unbox_value<muxm::Brush>(
                    mux::Application::Current().Resources().Lookup(
                        box_value(L"ControlFillColorSecondaryBrush"))));
            } catch (...) {}
        });
        PlanTreeContainer().Children().Append(row);

        // Children — recurse.
        for (auto& c : node->children) BuildNodeRow(c, depth + 1);
    }

    void ExplainPlanVisualizerControl::SelectNode(const std::wstring& id)
    {
        if (id == selectedNodeId_) return;
        selectedNodeId_ = id;
        RenderPlanTree();
        RenderFlyout();
    }

    // ── Flyout ──────────────────────────────
    void ExplainPlanVisualizerControl::RenderFlyout()
    {
        FlyoutContainer().Children().Clear();
        if (!doc_) return;
        auto node = FindNode(doc_->root, selectedNodeId_);
        if (!node) node = doc_->root;

        // Header
        muxc::StackPanel head;
        head.Orientation(muxc::Orientation::Horizontal);
        head.Spacing(10);
        muxc::Border g;
        g.Width(42); g.Height(36);
        g.CornerRadius(mux::CornerRadiusHelper::FromUniformRadius(7));
        g.Background(Brush(HeatBgColor(node->level)));
        auto gtxt = TB(KindGlyph(node->kind), 16, 1.0, true);
        gtxt.HorizontalAlignment(mux::HorizontalAlignment::Center);
        gtxt.VerticalAlignment(mux::VerticalAlignment::Center);
        gtxt.Foreground(Brush(HeatColor(node->level)));
        g.Child(gtxt);
        head.Children().Append(g);

        muxc::StackPanel ht;
        std::wstring title = node->op;
        if (!node->relation.empty()) title += L" · " + node->relation;
        ht.Children().Append(TB(title, 16, 1.0, true));
        if (!node->detail.empty()) {
            auto d = TB(node->detail, 12, 0.7);
            d.TextWrapping(mux::TextWrapping::Wrap);
            ht.Children().Append(d);
        }
        head.Children().Append(ht);
        FlyoutContainer().Children().Append(head);

        // Section helper — title + key/value grid
        auto section = [&](const std::wstring& title, std::vector<std::pair<std::wstring, std::wstring>> kv) {
            muxc::StackPanel s; s.Spacing(4); s.Margin(mux::Thickness{ 0, 8, 0, 0 });
            auto t = TB(title, 12, 0.55, true);
            s.Children().Append(t);
            for (auto& [k, v] : kv) {
                muxc::Grid row; row.Padding(mux::Thickness{ 0, 3, 0, 3 });
                muxc::ColumnDefinition c1; c1.Width(mux::GridLengthHelper::FromValueAndType(120, mux::GridUnitType::Pixel));
                muxc::ColumnDefinition c2; c2.Width(mux::GridLengthHelper::FromValueAndType(1, mux::GridUnitType::Star));
                row.ColumnDefinitions().Append(c1);
                row.ColumnDefinitions().Append(c2);
                auto kt = TB(k, 12, 0.55);
                muxc::Grid::SetColumn(kt, 0);
                row.Children().Append(kt);
                auto vt = TB(v, 13, 0.95);
                vt.TextWrapping(mux::TextWrapping::Wrap);
                muxc::Grid::SetColumn(vt, 1);
                row.Children().Append(vt);
                s.Children().Append(row);
            }
            FlyoutContainer().Children().Append(s);
        };

        std::wstringstream pct; pct << std::fixed << std::setprecision(1) << node->pctOfTotal << L"%";
        section(L"TIMING", {
            { L"Startup cost", fmtMs(node->startupCost) },
            { L"Total cost",   fmtMs(node->totalCost) },
            { L"Actual time",  fmtMs(node->actualTotal) + L" ms" },
            { L"Loops",        std::to_wstring(node->actualLoops) },
            { L"% of total",   pct.str() },
        });

        std::wstring estErr = L"—";
        if (node->planRows >= 1 && node->actualRows >= 1) {
            double f = std::max(node->planRows, node->actualRows) /
                       std::max(1.0, std::min(node->planRows, node->actualRows));
            std::wstringstream s;
            s << std::fixed << std::setprecision(1) << f << L"× "
              << (node->actualRows > node->planRows ? L"under" : L"over");
            estErr = s.str();
        }
        section(L"ROWS", {
            { L"Estimated", fmtNum(node->planRows) },
            { L"Actual",    fmtNum(node->actualRows) },
            { L"Estimation error", estErr },
            { L"Rows removed by filter", fmtNum(node->rowsRemovedByFilter) },
            { L"Width", std::to_wstring(node->planWidth) + L" bytes" },
        });

        if (node->sharedHit > 0 || node->sharedRead > 0 || node->tempRead > 0 || node->tempWritten > 0) {
            section(L"BUFFERS", {
                { L"Shared hit",  fmtNum(node->sharedHit) + L" blocks" },
                { L"Shared read", fmtNum(node->sharedRead) + L" blocks" },
                { L"Shared dirtied", fmtNum(node->sharedDirtied) },
                { L"Temp read",  fmtNum(node->tempRead) },
                { L"Temp written", fmtNum(node->tempWritten) },
            });
        }

        if (!node->outputCols.empty()) {
            muxc::StackPanel s; s.Spacing(4); s.Margin(mux::Thickness{ 0, 8, 0, 0 });
            s.Children().Append(TB(L"OUTPUT", 11, 0.55, true));
            muxc::StackPanel chips;
            chips.Orientation(muxc::Orientation::Horizontal);
            chips.Spacing(4);
            for (size_t i = 0; i < node->outputCols.size() && i < 12; ++i) {
                muxc::Border chip;
                chip.CornerRadius(mux::CornerRadiusHelper::FromUniformRadius(8));
                chip.Padding(mux::Thickness{ 6, 2, 6, 2 });
                try {
                    chip.Background(unbox_value<muxm::Brush>(
                        mux::Application::Current().Resources().Lookup(
                            box_value(L"ControlFillColorSecondaryBrush"))));
                } catch (...) {}
                chip.Child(TB(node->outputCols[i], 10, 0.85));
                chips.Children().Append(chip);
            }
            s.Children().Append(chips);
            FlyoutContainer().Children().Append(s);
        }
    }

    // ── Waterfall ──────────────────────────────
    void ExplainPlanVisualizerControl::RenderWaterfall()
    {
        WaterfallContainer().Children().Clear();
        if (!doc_) return;

        // Build a flat sequence of leaves + their parents, in execution
        // order (post-order DFS = children before parents). Track cumulative
        // start time per node by summing previous siblings' durations.
        struct Step { std::wstring label, glyph; std::wstring cls;
                       double startMs, durMs; EX::NodeLevel level; };
        std::vector<Step> seq;
        double cursor = 0.0;

        std::function<void(const EX::PlanNode&)> walk = [&](const EX::PlanNode& n) {
            // Children execute first in PG plan model.
            double childrenSum = 0.0;
            for (auto& c : n.children) {
                walk(*c);
                childrenSum += c->actualTotal * c->actualLoops;
            }
            double dur = std::max(0.0, n.actualTotal * n.actualLoops - childrenSum);
            std::wstring label = n.op + (n.relation.empty() ? L"" : L" · " + n.relation);
            seq.push_back({ label, KindGlyph(n.kind), KindClass(n.kind),
                             cursor, dur, n.level });
            cursor += dur;
        };
        walk(*doc_->root);

        double maxMs = std::max(1.0, doc_->executionMs);

        // Axis row
        muxc::Grid axis;
        axis.Padding(mux::Thickness{ 0, 0, 0, 4 });
        muxc::ColumnDefinition a0; a0.Width(mux::GridLengthHelper::FromValueAndType(220, mux::GridUnitType::Pixel));
        muxc::ColumnDefinition a1; a1.Width(mux::GridLengthHelper::FromValueAndType(1, mux::GridUnitType::Star));
        muxc::ColumnDefinition a2; a2.Width(mux::GridLengthHelper::FromValueAndType(70, mux::GridUnitType::Pixel));
        axis.ColumnDefinitions().Append(a0);
        axis.ColumnDefinitions().Append(a1);
        axis.ColumnDefinitions().Append(a2);
        auto t1 = TB(L"Node", 10, 0.5);
        muxc::Grid::SetColumn(t1, 0);
        axis.Children().Append(t1);
        // Tick row
        muxc::Grid ticks;
        for (int i = 0; i <= 4; ++i) {
            auto tick = TB(fmtMs(maxMs * i / 4) + L" ms", 10, 0.5);
            tick.HorizontalAlignment(i == 0 ? mux::HorizontalAlignment::Left
                                              : i == 4 ? mux::HorizontalAlignment::Right
                                                        : mux::HorizontalAlignment::Center);
            muxc::ColumnDefinition cd;
            cd.Width(mux::GridLengthHelper::FromValueAndType(1, mux::GridUnitType::Star));
            ticks.ColumnDefinitions().Append(cd);
            muxc::Grid::SetColumn(tick, i);
            ticks.Children().Append(tick);
        }
        muxc::Grid::SetColumn(ticks, 1);
        axis.Children().Append(ticks);
        auto t3 = TB(L"Duration", 10, 0.5);
        t3.HorizontalAlignment(mux::HorizontalAlignment::Right);
        muxc::Grid::SetColumn(t3, 2);
        axis.Children().Append(t3);
        WaterfallContainer().Children().Append(axis);

        for (auto& s : seq) {
            WaterfallContainer().Children().Append(BuildWaterfallRow(
                s.label, s.glyph, s.cls, s.startMs, s.durMs, maxMs, s.level));
        }
    }

    mux::UIElement ExplainPlanVisualizerControl::BuildWaterfallRow(
        const std::wstring& label, const std::wstring& glyph, const std::wstring& glyphCls,
        double startMs, double durMs, double maxMs, EX::NodeLevel level)
    {
        muxc::Grid row;
        muxc::ColumnDefinition c0; c0.Width(mux::GridLengthHelper::FromValueAndType(220, mux::GridUnitType::Pixel));
        muxc::ColumnDefinition c1; c1.Width(mux::GridLengthHelper::FromValueAndType(1, mux::GridUnitType::Star));
        muxc::ColumnDefinition c2; c2.Width(mux::GridLengthHelper::FromValueAndType(70, mux::GridUnitType::Pixel));
        row.ColumnDefinitions().Append(c0);
        row.ColumnDefinitions().Append(c1);
        row.ColumnDefinitions().Append(c2);
        row.Padding(mux::Thickness{ 0, 3, 0, 3 });

        // Name
        muxc::StackPanel name;
        name.Orientation(muxc::Orientation::Horizontal);
        name.Spacing(6);
        name.VerticalAlignment(mux::VerticalAlignment::Center);
        muxc::Border g;
        g.Width(24); g.Height(20);
        g.CornerRadius(mux::CornerRadiusHelper::FromUniformRadius(4));
        g.Background(Brush(HeatBgColor(level)));
        auto gt = TB(glyph, 10, 1.0, true);
        gt.HorizontalAlignment(mux::HorizontalAlignment::Center);
        gt.VerticalAlignment(mux::VerticalAlignment::Center);
        gt.Foreground(Brush(HeatColor(level)));
        g.Child(gt);
        name.Children().Append(g);
        auto lt = TB(label, 11, 0.85);
        lt.TextTrimming(mux::TextTrimming::CharacterEllipsis);
        name.Children().Append(lt);
        muxc::Grid::SetColumn(name, 0);
        row.Children().Append(name);

        // Track
        muxc::Grid track;
        track.Height(16);
        track.CornerRadius(mux::CornerRadiusHelper::FromUniformRadius(3));
        try {
            track.Background(unbox_value<muxm::Brush>(
                mux::Application::Current().Resources().Lookup(
                    box_value(L"ControlAltFillColorTertiaryBrush"))));
        } catch (...) {}
        track.VerticalAlignment(mux::VerticalAlignment::Center);
        muxc::Grid::SetColumn(track, 1);

        muxc::Grid bar;
        bar.Height(10);
        bar.HorizontalAlignment(mux::HorizontalAlignment::Left);
        bar.VerticalAlignment(mux::VerticalAlignment::Center);
        bar.CornerRadius(mux::CornerRadiusHelper::FromUniformRadius(2));
        bar.Background(Brush(HeatColor(level)));
        double startFrac = std::clamp(startMs / maxMs, 0.0, 1.0);
        double durFrac   = std::clamp(durMs / maxMs, 0.001, 1.0);
        auto sizeBar = [bar, startFrac, durFrac](double w) {
            bar.Margin(mux::Thickness{ w * startFrac, 0, 0, 0 });
            bar.Width(std::max(2.0, w * durFrac));
        };
        track.SizeChanged([sizeBar](IInspectable const&, mux::SizeChangedEventArgs const& e) {
            sizeBar(e.NewSize().Width);
        });
        track.Loaded([track, sizeBar](IInspectable const&, mux::RoutedEventArgs const&) {
            sizeBar(track.ActualWidth());
        });
        track.Children().Append(bar);
        row.Children().Append(track);

        // Duration text
        auto d = TB(fmtMs(durMs) + L" ms", 11, 0.85);
        d.HorizontalAlignment(mux::HorizontalAlignment::Right);
        d.VerticalAlignment(mux::VerticalAlignment::Center);
        muxc::Grid::SetColumn(d, 2);
        row.Children().Append(d);
        return row;
    }

    // ── Recommendations ──────────────────────────────
    void ExplainPlanVisualizerControl::RenderRecommendations()
    {
        RecsContainer().Children().Clear();
        if (recs_.empty()) {
            auto empty = TB(L"No tuning suggestions — the planner looks healthy.", 12, 0.55);
            empty.Margin(mux::Thickness{ 4, 6, 0, 0 });
            RecsContainer().Children().Append(empty);
            return;
        }
        for (auto& r : recs_) {
            RecsContainer().Children().Append(BuildRecCard(r));
        }
    }

    mux::UIElement ExplainPlanVisualizerControl::BuildRecCard(const EX::Recommendation& r)
    {
        auto card = CardBorder();
        // Severity tone — left-edge accent.
        Color tone =
            r.severity == EX::IssueSeverity::Bad  ? MakeColor(196, 43, 28) :
            r.severity == EX::IssueSeverity::Warn ? MakeColor(224, 168, 0) :
                                                     MakeColor(120, 96, 217);
        card.BorderBrush(Brush(tone));
        card.BorderThickness(mux::Thickness{ 4, 1, 1, 1 });

        muxc::Grid grid;
        muxc::ColumnDefinition c1; c1.Width(mux::GridLengthHelper::FromValueAndType(1, mux::GridUnitType::Star));
        muxc::ColumnDefinition c2; c2.Width(mux::GridLengthHelper::Auto());
        grid.ColumnDefinitions().Append(c1);
        grid.ColumnDefinitions().Append(c2);

        muxc::StackPanel left;
        left.Spacing(3);
        left.Children().Append(TB(r.title, 13, 1.0, true));
        left.Children().Append(TB(r.rationale, 11, 0.65));
        if (!r.previewSql.empty()) {
            auto sql = TB(r.previewSql, 11, 0.85);
            try {
                sql.FontFamily(muxm::FontFamily(L"Cascadia Code, Consolas, monospace"));
            } catch (...) {}
            sql.Margin(mux::Thickness{ 0, 4, 0, 0 });
            left.Children().Append(sql);
        }
        muxc::Grid::SetColumn(left, 0);
        grid.Children().Append(left);

        muxc::StackPanel actions;
        actions.Orientation(muxc::Orientation::Horizontal);
        actions.Spacing(6);
        actions.VerticalAlignment(mux::VerticalAlignment::Center);
        muxc::Button preview;
        preview.Content(box_value(L"Preview SQL"));
        std::wstring sqlCopy = r.previewSql;
        preview.Click([this, sqlCopy](IInspectable const&, mux::RoutedEventArgs const&) {
            if (OnPreviewSqlRequested) OnPreviewSqlRequested(sqlCopy);
        });
        actions.Children().Append(preview);
        muxc::Grid::SetColumn(actions, 1);
        grid.Children().Append(actions);

        card.Child(grid);
        return card;
    }

    // ── Glyph + color helpers ──────────────────────────────
    Color ExplainPlanVisualizerControl::HeatColor(EX::NodeLevel l)
    {
        switch (l) {
        case EX::NodeLevel::Bad:  return MakeColor(196, 43, 28);
        case EX::NodeLevel::Warn: return MakeColor(138, 109, 0);
        default:                  return MakeColor(16, 124, 16);
        }
    }
    Color ExplainPlanVisualizerControl::HeatBgColor(EX::NodeLevel l)
    {
        switch (l) {
        case EX::NodeLevel::Bad:  return MakeColor(253, 231, 233);
        case EX::NodeLevel::Warn: return MakeColor(255, 244, 206);
        default:                  return MakeColor(230, 244, 230);
        }
    }
    std::wstring ExplainPlanVisualizerControl::KindGlyph(EX::NodeKind k)
    {
        switch (k) {
        case EX::NodeKind::Limit:        return L"L";
        case EX::NodeKind::Sort:         return L"↧"; // ↧
        case EX::NodeKind::HashJoin:     return L"⋈"; // ⋈
        case EX::NodeKind::MergeJoin:    return L"⋈";
        case EX::NodeKind::NestedLoop:   return L"NL";
        case EX::NodeKind::SeqScan:      return L"SS";
        case EX::NodeKind::IndexScan:    return L"IX";
        case EX::NodeKind::IndexOnlyScan:return L"IO";
        case EX::NodeKind::BitmapHeapScan:return L"BH";
        case EX::NodeKind::BitmapIndexScan:return L"BI";
        case EX::NodeKind::Hash:         return L"#";
        case EX::NodeKind::Aggregate:    return L"Σ"; // Σ
        case EX::NodeKind::GroupAggregate:return L"Σ";
        case EX::NodeKind::HashAggregate:return L"Σ";
        case EX::NodeKind::WindowAgg:    return L"W";
        case EX::NodeKind::Materialize:  return L"M";
        case EX::NodeKind::Gather:       return L"G";
        case EX::NodeKind::GatherMerge:  return L"GM";
        case EX::NodeKind::Append:       return L"+";
        case EX::NodeKind::Result:       return L"R";
        case EX::NodeKind::SubqueryScan: return L"SQ";
        case EX::NodeKind::CteScan:      return L"CT";
        case EX::NodeKind::Unique:       return L"U";
        case EX::NodeKind::Memoize:      return L"≡"; // ≡
        default:                          return L"·";
        }
    }
    std::wstring ExplainPlanVisualizerControl::KindClass(EX::NodeKind k)
    {
        switch (k) {
        case EX::NodeKind::SeqScan:      return L"seq";
        case EX::NodeKind::IndexScan:
        case EX::NodeKind::IndexOnlyScan:
        case EX::NodeKind::BitmapHeapScan:
        case EX::NodeKind::BitmapIndexScan: return L"idx";
        case EX::NodeKind::HashJoin:
        case EX::NodeKind::MergeJoin:
        case EX::NodeKind::NestedLoop:   return L"join";
        case EX::NodeKind::Sort:         return L"sort";
        case EX::NodeKind::Hash:
        case EX::NodeKind::Aggregate:
        case EX::NodeKind::GroupAggregate:
        case EX::NodeKind::HashAggregate:return L"agg";
        default:                          return L"";
        }
    }
}
