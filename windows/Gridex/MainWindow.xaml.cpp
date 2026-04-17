#include "pch.h"
#include "xaml-includes.h"
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.Interop.h>
#include <winrt/Microsoft.UI.Interop.h>
#include <winrt/Windows.System.h>
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include "HomePage.h"
#include "WorkspacePage.h"
#include "Models/AppSettings.h"
#include "UpdateService.h"
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

namespace winrt::Gridex::implementation
{
    namespace mux = winrt::Microsoft::UI::Xaml;

    MainWindow::MainWindow()
    {
        this->Activated([this](auto const&, auto const&)
        {
            static bool initialized = false;
            if (initialized) return;
            initialized = true;

            auto appWindow = this->AppWindow();
            appWindow.Resize(winrt::Windows::Graphics::SizeInt32{ 1280, 800 });

            // Title-bar icon. Gridex.rc embeds the phoenix .ico as
            // resource ID 1 inside Gridex.exe; WinUI 3 does not pick
            // that up for the AppWindow title bar automatically, so we
            // load the HICON from our own module and hand it back to
            // WinUI via the Win32 interop IconId bridge.
            if (HICON hIcon = ::LoadIconW(::GetModuleHandleW(nullptr),
                                          MAKEINTRESOURCEW(1)))
            {
                try
                {
                    auto iconId = winrt::Microsoft::UI::GetIconIdFromIcon(hIcon);
                    appWindow.SetIcon(iconId);
                }
                catch (...) { /* best effort -- ignore failure */ }
            }

            if (auto content = this->Content().try_as<mux::FrameworkElement>())
            {
                content.RequestedTheme(mux::ElementTheme::Dark);

                // Wire keyboard shortcuts in code-behind (no XAML accelerators = no tooltip leak)
                auto settingsAccel = mux::Input::KeyboardAccelerator();
                settingsAccel.Key(winrt::Windows::System::VirtualKey::P);
                settingsAccel.Modifiers(static_cast<winrt::Windows::System::VirtualKeyModifiers>(
                    static_cast<uint32_t>(winrt::Windows::System::VirtualKeyModifiers::Control) |
                    static_cast<uint32_t>(winrt::Windows::System::VirtualKeyModifiers::Shift)));
                settingsAccel.Invoked([this](auto&&, mux::Input::KeyboardAcceleratorInvokedEventArgs const& args)
                {
                    // Remember current page for Back button
                    auto s = DBModels::AppSettings::Load();
                    auto currentContent = ContentFrame().Content();
                    if (currentContent.try_as<winrt::Gridex::WorkspacePage>())
                        s.lastPageBeforeSettings = L"Gridex.WorkspacePage";
                    else if (currentContent.try_as<winrt::Gridex::HomePage>())
                        s.lastPageBeforeSettings = L"Gridex.HomePage";
                    s.Save();

                    NavigateTo(L"Gridex.SettingsPage");
                    args.Handled(true);
                });
                content.KeyboardAccelerators().Append(settingsAccel);

                auto homeAccel = mux::Input::KeyboardAccelerator();
                homeAccel.Key(winrt::Windows::System::VirtualKey::H);
                homeAccel.Modifiers(winrt::Windows::System::VirtualKeyModifiers::Control);
                homeAccel.Invoked([this](auto&&, mux::Input::KeyboardAcceleratorInvokedEventArgs const& args)
                {
                    NavigateTo(L"Gridex.HomePage");
                    args.Handled(true);
                });
                content.KeyboardAccelerators().Append(homeAccel);
            }

            // Blocking update check BEFORE navigating to HomePage.
            //
            // The UpdateCheckOverlay from MainWindow.xaml is visible until
            // we call DismissUpdateOverlayAndEnterApp(). Flow:
            //   1. Run CheckForUpdateAsync on a background thread.
            //   2. On the UI thread, if an update is available, show a
            //      ContentDialog. Primary -> DownloadAndApplyAsync (process
            //      exits). Close / error / no-update -> proceed to HomePage.
            // Errors (no network, running uninstalled from dev, etc.) are
            // treated as "no update" so the user still enters the app.
            //
            // This replaces the previous 5-second silent-check timer --
            // no more background auto-check once the user is inside. Manual
            // check remains available via Settings > Check Now.
            auto dispatcher = this->DispatcherQueue();
            ::Gridex::CheckForUpdateAsync(
                [this, dispatcher](::Gridex::UpdateCheckResult r)
                {
                    dispatcher.TryEnqueue([this, r]()
                    {
                        if (!r.hasUpdate)
                        {
                            EnterAppAfterUpdateCheck();
                            return;
                        }

                        auto content = this->Content().try_as<mux::FrameworkElement>();
                        auto xamlRoot = content ? content.XamlRoot() : nullptr;
                        if (!xamlRoot)
                        {
                            EnterAppAfterUpdateCheck();
                            return;
                        }

                        std::wstring msg = L"Current: " + r.currentVersion +
                                           L"\nNew:     " + r.newVersion +
                                           L"\n\nDownload and install now? The app will restart.";
                        winrt::Microsoft::UI::Xaml::Controls::ContentDialog dlg;
                        dlg.Title(winrt::box_value(winrt::hstring(
                            L"Gridex " + r.newVersion + L" is available")));
                        dlg.Content(winrt::box_value(winrt::hstring(msg)));
                        dlg.PrimaryButtonText(L"Install");
                        dlg.CloseButtonText(L"Later");
                        dlg.DefaultButton(
                            winrt::Microsoft::UI::Xaml::Controls::ContentDialogButton::Primary);
                        dlg.XamlRoot(xamlRoot);

                        try
                        {
                            auto op = dlg.ShowAsync();
                            op.Completed([this](auto const& asyncOp,
                                                winrt::Windows::Foundation::AsyncStatus status)
                            {
                                bool install = false;
                                if (status == winrt::Windows::Foundation::AsyncStatus::Completed)
                                {
                                    try
                                    {
                                        install = asyncOp.GetResults() ==
                                            winrt::Microsoft::UI::Xaml::Controls::ContentDialogResult::Primary;
                                    }
                                    catch (...) {}
                                }
                                if (install)
                                {
                                    // Status text on overlay while the download
                                    // runs; process exits from inside the worker.
                                    UpdateCheckStatusText().Text(L"Downloading update...");
                                    ::Gridex::DownloadAndApplyAsync(
                                        [](std::wstring) {},
                                        [](std::wstring) {});
                                    // Do NOT call EnterAppAfterUpdateCheck --
                                    // the worker will ExitProcess and Velopack
                                    // restarts us on the new version.
                                    return;
                                }
                                EnterAppAfterUpdateCheck();
                            });
                        }
                        catch (...)
                        {
                            EnterAppAfterUpdateCheck();
                        }
                    });
                });
        });
    }

    // Hide the update-check overlay and navigate ContentFrame to HomePage.
    // Called exactly once, either after a completed check with no update,
    // or after the user dismisses the update dialog with "Later".
    void MainWindow::EnterAppAfterUpdateCheck()
    {
        UpdateCheckOverlay().Visibility(mux::Visibility::Collapsed);
        NavigateTo(L"Gridex.HomePage");
    }

    void MainWindow::NavigateTo(const wchar_t* pageTypeName)
    {
        winrt::Windows::UI::Xaml::Interop::TypeName pageType;
        pageType.Name = pageTypeName;
        pageType.Kind = winrt::Windows::UI::Xaml::Interop::TypeKind::Metadata;
        ContentFrame().Navigate(pageType);
    }

    void MainWindow::SettingsAccelerator_Invoked(
        mux::Input::KeyboardAccelerator const&,
        mux::Input::KeyboardAcceleratorInvokedEventArgs const& args)
    {
        NavigateTo(L"Gridex.SettingsPage");
        args.Handled(true);
    }

    void MainWindow::NewQueryAccelerator_Invoked(
        mux::Input::KeyboardAccelerator const&,
        mux::Input::KeyboardAcceleratorInvokedEventArgs const& args)
    {
        args.Handled(true);
    }

    void MainWindow::CloseTabAccelerator_Invoked(
        mux::Input::KeyboardAccelerator const&,
        mux::Input::KeyboardAcceleratorInvokedEventArgs const& args)
    {
        args.Handled(true);
    }

    void MainWindow::HomeAccelerator_Invoked(
        mux::Input::KeyboardAccelerator const&,
        mux::Input::KeyboardAcceleratorInvokedEventArgs const& args)
    {
        NavigateTo(L"Gridex.HomePage");
        args.Handled(true);
    }
}
