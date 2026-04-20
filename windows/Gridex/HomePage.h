#pragma once

#include "Models/ConnectionConfig.h"
#include <vector>
#include "HomePage.g.h"

namespace winrt::Gridex::implementation
{
    struct HomePage : HomePageT<HomePage>
    {
        HomePage();

        void NewConnection_Click(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void NewGroup_Click(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        winrt::fire_and_forget ShowNewGroupDialogAsync();
        void Settings_Click(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OpenMcp_Click(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);

        // Opens the import wizard for TablePlus / DBeaver /
        // DataGrip / Navicat connections. See ImportConnections.cpp
        // for the dialog build-up; too much UI code to keep inline.
        winrt::fire_and_forget ImportConnections_Click(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void ConnectionItem_Click(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Controls::ItemClickEventArgs const& e);
        void SearchBox_TextChanged(
            winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBox const& sender,
            winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBoxTextChangedEventArgs const& e);

    private:
        std::vector<DBModels::ConnectionConfig> allConnections_;
        void RefreshList(const std::wstring& searchQuery = L"");
        void UpdateEmptyState();
        void ShowNewConnectionFlow();
        void AddConnectionCard(const DBModels::ConnectionConfig& config);
        void AddGroupHeader(const std::wstring& name, int count);
        void DeleteConnection(const std::wstring& id);
        void EditConnection(const std::wstring& id);
        void TestConnectionAsync(
            const DBModels::ConnectionConfig& config, const std::wstring& password);
    };
}

namespace winrt::Gridex::factory_implementation
{
    struct HomePage : HomePageT<HomePage, implementation::HomePage>
    {
    };
}
