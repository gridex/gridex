#pragma once

#include "MCPPage.g.h"
#include "Models/MCP/MCPAuditEntry.h"
#include <vector>

namespace winrt::Gridex::implementation
{
    // MCPPage — Gridex MCP Server dashboard (5 tabs, mirrors macOS).
    // Overview tab is fully wired; Connections / Activity / Setup / Config
    // have minimum viable bodies and will grow in follow-up sprints.
    struct MCPPage : MCPPageT<MCPPage>
    {
        MCPPage();

        void Back_Click(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);

        void StartStop_Click(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);

        void HttpToggle_Toggled(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);

        // Port persists on edit (auto-save) so the Overview tab
        // doesn't need a dedicated Save button for this field.
        void HttpPortBox_ValueChanged(
            winrt::Microsoft::UI::Xaml::Controls::NumberBox const& sender,
            winrt::Microsoft::UI::Xaml::Controls::NumberBoxValueChangedEventArgs const& args);

        void CopyConfig_Click(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);

        void SaveConfig_Click(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);

        void InstallClaude_Click(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);

        // Re-render Setup tab (preview JSON + config path + button
        // label) whenever the AI-client dropdown changes.
        void ClientPicker_SelectionChanged(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& e);

        // Connections tab handlers
        void ConnFilter_Changed(
            winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBox const& sender,
            winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBoxTextChangedEventArgs const& e);
        void ConnFilterCombo_SelectionChanged(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& e);

        // Activity tab handlers
        void ActFilter_Changed(
            winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBox const& sender,
            winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBoxTextChangedEventArgs const& e);
        void ActStatusCombo_SelectionChanged(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& e);
        void ActRefresh_Click(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);

        // Pivot lazy-loads PivotItems; refresh the tab's body when
        // it first becomes selected so named controls are live.
        void Tabs_SelectionChanged(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& e);

        void ManageConnections_Click(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);

    private:
        void RefreshUI();
        void RefreshOverviewUptime();
        void RenderSetupForSelectedClient();
        winrt::Microsoft::UI::Xaml::DispatcherTimer uptimeTimer_{ nullptr };
        void ApplyStartStopButton(bool running);
        void RefreshConnectionsTab();
        void RefreshActivityTab();
        void SelectActivityEntry(int index);

        // Cache tailed audit entries so a click on a row can look
        // up the full record by index. Populated by RefreshActivityTab.
        std::vector<DBModels::MCPAuditEntry> cachedActivity_;
        int selectedActivityIndex_ = -1;
    };
}

namespace winrt::Gridex::factory_implementation
{
    struct MCPPage : MCPPageT<MCPPage, implementation::MCPPage> {};
}
