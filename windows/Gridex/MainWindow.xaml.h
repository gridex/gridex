#pragma once

#include "MainWindow.g.h"

namespace winrt::Gridex::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();

        void SettingsAccelerator_Invoked(
            winrt::Microsoft::UI::Xaml::Input::KeyboardAccelerator const& sender,
            winrt::Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const& args);
        void NewQueryAccelerator_Invoked(
            winrt::Microsoft::UI::Xaml::Input::KeyboardAccelerator const& sender,
            winrt::Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const& args);
        void CloseTabAccelerator_Invoked(
            winrt::Microsoft::UI::Xaml::Input::KeyboardAccelerator const& sender,
            winrt::Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const& args);
        void HomeAccelerator_Invoked(
            winrt::Microsoft::UI::Xaml::Input::KeyboardAccelerator const& sender,
            winrt::Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const& args);

    private:
        void NavigateTo(const wchar_t* pageTypeName);
        void EnterAppAfterUpdateCheck();
        void ShowShortcutsDialog();
    };
}

namespace winrt::Gridex::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
