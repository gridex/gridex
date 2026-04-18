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
#include "Models/ShortcutsCatalog.h"
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

                // Ctrl+/ → Keyboard Shortcuts dialog. Uses VK_OEM_2 (0xBF)
                // for the main-keyboard "/" key; the named VirtualKey enum
                // only covers Divide (numpad) which would miss the usual
                // convention. F1 is also registered below as a fallback
                // because some keyboard layouts remap Oem2 (e.g. German
                // uses "-" on that key).
                auto shortcutsAccel = mux::Input::KeyboardAccelerator();
                shortcutsAccel.Key(static_cast<winrt::Windows::System::VirtualKey>(0xBF));
                shortcutsAccel.Modifiers(winrt::Windows::System::VirtualKeyModifiers::Control);
                shortcutsAccel.Invoked([this](auto&&, mux::Input::KeyboardAcceleratorInvokedEventArgs const& args)
                {
                    ShowShortcutsDialog();
                    args.Handled(true);
                });
                content.KeyboardAccelerators().Append(shortcutsAccel);

                // F1 → same dialog. Universal Help key, layout-safe fallback.
                auto helpAccel = mux::Input::KeyboardAccelerator();
                helpAccel.Key(winrt::Windows::System::VirtualKey::F1);
                helpAccel.Invoked([this](auto&&, mux::Input::KeyboardAcceleratorInvokedEventArgs const& args)
                {
                    ShowShortcutsDialog();
                    args.Handled(true);
                });
                content.KeyboardAccelerators().Append(helpAccel);
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

    // Triggered by Ctrl+/ or F1 — shows a ContentDialog with the full
    // list of hotkeys grouped by category. Markup is built entirely in
    // code via DBModels::BuildShortcutsView() so the Settings page and
    // this dialog render the same view from one source of truth.
    void MainWindow::ShowShortcutsDialog()
    {
        auto content = this->Content().try_as<mux::FrameworkElement>();
        if (!content) return;
        auto xamlRoot = content.XamlRoot();
        if (!xamlRoot) return;

        mux::Controls::ContentDialog dlg;
        dlg.Title(winrt::box_value(winrt::hstring(L"Keyboard Shortcuts")));
        dlg.CloseButtonText(L"Close");
        dlg.DefaultButton(mux::Controls::ContentDialogButton::Close);
        dlg.XamlRoot(xamlRoot);

        // Scroll wrapper — list may grow beyond the default dialog height
        // as more shortcuts get registered. MaxHeight keeps the dialog
        // itself constrained so it never spans the full window height.
        mux::Controls::ScrollViewer scroller;
        scroller.MaxHeight(520.0);
        scroller.HorizontalScrollBarVisibility(mux::Controls::ScrollBarVisibility::Disabled);
        scroller.VerticalScrollBarVisibility(mux::Controls::ScrollBarVisibility::Auto);
        scroller.Content(DBModels::BuildShortcutsView());
        dlg.Content(scroller);

        // Widen the default 456px ContentDialog so the action label and
        // key cap fit on one line without aggressive truncation.
        dlg.Resources().Insert(
            winrt::box_value(L"ContentDialogMaxWidth"),
            winrt::box_value(640.0));
        dlg.Resources().Insert(
            winrt::box_value(L"ContentDialogMinWidth"),
            winrt::box_value(520.0));

        try { dlg.ShowAsync(); }
        catch (...) { /* best effort; never block app on dialog failures */ }
    }
}
