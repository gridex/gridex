#pragma once
// Native WinUI 3 visualizer for Postgres EXPLAIN ANALYZE plans. Lives
// inside the QueryEditorView's results-panel "Visualize" tab. The
// control owns its own state (empty / loading / loaded / error) and
// expects the host to drive the EXPLAIN execution via the OnRunRequested
// callback then push the resulting JSON via SetPlan / ShowError.

#include "ExplainPlanVisualizerControl.g.h"
#include "ExplainPlanTypes.h"
#include "ExplainPlanHeuristics.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace winrt::Gridex::implementation
{
    struct ExplainPlanVisualizerControl : ExplainPlanVisualizerControlT<ExplainPlanVisualizerControl>
    {
        ExplainPlanVisualizerControl();

        // Push a parsed plan and render. Switches to LoadedState.
        void SetPlan(std::shared_ptr<::Gridex::ExplainViz::PlanDocument> doc);

        // Show empty state (no plan run yet).
        void ShowEmpty();

        // Show progress while EXPLAIN runs.
        void ShowLoading();

        // Show error message.
        void ShowError(const std::wstring& message);

        // Reset to empty (used when query tab switches — last plan is stale).
        // Cheap to call repeatedly; only re-renders when transitioning state.
        void Reset();

        // ── Host callbacks ──────────────────────────────
        // Fired when the user clicks "Run with Explain Analyze" or "Re-run".
        std::function<void()> OnRunRequested;
        // User clicked "Preview SQL" on a recommendation — host opens it
        // in a new query tab.
        std::function<void(std::wstring sql)> OnPreviewSqlRequested;
        // User clicked Copy plan — host writes raw JSON to clipboard
        // (we send the cached doc.rawJson).
        std::function<void(std::wstring rawJson)> OnCopyPlanRequested;

        // ── XAML event handlers (bound from .xaml) ──────
        void OnRunClicked(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnCopyPlanClicked(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnHotOnlyToggled(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);

    private:
        std::shared_ptr<::Gridex::ExplainViz::PlanDocument> doc_;
        std::vector<::Gridex::ExplainViz::Issue> issues_;
        std::vector<::Gridex::ExplainViz::Recommendation> recs_;
        std::wstring selectedNodeId_;
        bool hotOnly_ = false;

        enum class State { Empty, Loading, Loaded, Error };
        void SwitchState(State s);

        // Render passes — invoked from SetPlan in order.
        void RenderInfoBar();
        void RenderMetrics();
        void RenderPlanTree();
        void RenderFlyout();
        void RenderWaterfall();
        void RenderRecommendations();

        // Recursively build a node row for the plan tree. `depth` controls
        // indentation; `parentExpanded` lets us pre-collapse via the toggle.
        void BuildNodeRow(
            const std::shared_ptr<::Gridex::ExplainViz::PlanNode>& node,
            int depth);

        // Helpers
        winrt::Microsoft::UI::Xaml::UIElement BuildMetricCard(
            const std::wstring& glyph,
            const std::wstring& label,
            const std::wstring& value,
            const std::wstring& unit,
            const std::wstring& delta,
            double barPct,
            const std::wstring& tone /* "good"|"warn"|"bad"|"" */);
        winrt::Microsoft::UI::Xaml::UIElement BuildRecCard(
            const ::Gridex::ExplainViz::Recommendation& r);
        winrt::Microsoft::UI::Xaml::UIElement BuildWaterfallRow(
            const std::wstring& label,
            const std::wstring& glyph,
            const std::wstring& glyphCls,
            double startMs, double durMs, double maxMs,
            ::Gridex::ExplainViz::NodeLevel level);

        // Theme-aware accent colors for heat/glyph badges.
        winrt::Windows::UI::Color HeatColor(::Gridex::ExplainViz::NodeLevel l);
        winrt::Windows::UI::Color HeatBgColor(::Gridex::ExplainViz::NodeLevel l);
        std::wstring KindGlyph(::Gridex::ExplainViz::NodeKind k);
        std::wstring KindClass(::Gridex::ExplainViz::NodeKind k);

        // Rebuilds + re-selects when user clicks a node.
        void SelectNode(const std::wstring& id);

        // Walk plan to find node by id.
        std::shared_ptr<::Gridex::ExplainViz::PlanNode> FindNode(
            const std::shared_ptr<::Gridex::ExplainViz::PlanNode>& n,
            const std::wstring& id);
    };
}
namespace winrt::Gridex::factory_implementation
{
    struct ExplainPlanVisualizerControl
        : ExplainPlanVisualizerControlT<ExplainPlanVisualizerControl, implementation::ExplainPlanVisualizerControl> {};
}
