#include "pch.h"
#include "xaml-includes.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"
#include "UpdateService.h"
#include <shellapi.h>
#include <string>
#include <cwchar>

using namespace winrt;
using namespace Microsoft::UI::Xaml;

#include <microsoft.ui.xaml.window.h>

namespace winrt::Gridex::implementation
{
    // Detect whether the Microsoft Edge WebView2 Runtime is installed.
    // Gridex bundles Microsoft.Web.WebView2.Core.dll (the projection) but
    // the actual WebView2 runtime is a separate system component. On
    // Windows 11 and recently-updated Windows 10 (20H2+) it ships with
    // Edge automatically; fresh/stripped installs may be missing it and
    // any Gridex feature that uses a WebView2 element (ER Diagram) will
    // crash. Probe the registry key documented by Microsoft for WebView2
    // Runtime detection.
    static bool WebView2RuntimeIsInstalled()
    {
        static const wchar_t* const kClientsSubKey =
            L"SOFTWARE\\WOW6432Node\\Microsoft\\EdgeUpdate\\Clients\\"
            L"{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}";
        static const wchar_t* const kClientsSubKey32 =
            L"SOFTWARE\\Microsoft\\EdgeUpdate\\Clients\\"
            L"{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}";

        HKEY hKey = nullptr;
        for (HKEY root : { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER })
        {
            if (RegOpenKeyExW(root, kClientsSubKey, 0, KEY_READ, &hKey) == ERROR_SUCCESS)
            {
                RegCloseKey(hKey);
                return true;
            }
            if (RegOpenKeyExW(root, kClientsSubKey32, 0, KEY_READ, &hKey) == ERROR_SUCCESS)
            {
                RegCloseKey(hKey);
                return true;
            }
        }
        return false;
    }

    // If WebView2 Runtime is missing, run the bundled
    // MicrosoftEdgeWebView2Setup.exe bootstrap installer silently and wait
    // up to 2 minutes for it to finish. Best-effort: any failure is
    // swallowed because Gridex's startup path doesn't require WebView2 --
    // only the ER Diagram feature does. Worst case the user sees a
    // one-time startup delay on first run of a fresh install.
    static void EnsureWebView2Runtime()
    {
        if (WebView2RuntimeIsInstalled()) return;

        // Resolve path to the bundled bootstrap next to Gridex.exe.
        wchar_t modulePath[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH) == 0) return;

        std::wstring path(modulePath);
        auto slash = path.find_last_of(L'\\');
        if (slash == std::wstring::npos) return;
        std::wstring installer = path.substr(0, slash) + L"\\MicrosoftEdgeWebView2Setup.exe";

        if (GetFileAttributesW(installer.c_str()) == INVALID_FILE_ATTRIBUTES) return;

        SHELLEXECUTEINFOW sei{};
        sei.cbSize       = sizeof(sei);
        sei.fMask        = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb       = L"open";
        sei.lpFile       = installer.c_str();
        sei.lpParameters = L"/silent /install";
        sei.nShow        = SW_HIDE;
        if (ShellExecuteExW(&sei) && sei.hProcess)
        {
            WaitForSingleObject(sei.hProcess, 120'000); // 2 min cap
            CloseHandle(sei.hProcess);
        }
    }

    HWND App::MainHwnd = nullptr;
    /// <summary>
    /// Initializes the singleton application object.  This is the first line of authored code
    /// executed, and as such is the logical equivalent of main() or WinMain().
    /// </summary>
    App::App()
    {
        // CRITICAL: run BEFORE anything else. VelopackApp::Build().Run()
        // handles --veloapp-install / --veloapp-updated / --veloapp-obsolete
        // / --veloapp-uninstall lifecycle hooks and may call ExitProcess
        // itself so the installer's hook wait completes successfully and
        // the full WinUI main window never opens during an install.
        ::Gridex::InitVelopackApp();

        // Bootstrap the WebView2 Runtime before any XAML / WinAppSDK code
        // touches WebView2. Fast no-op (single registry probe) once the
        // runtime is installed.
        EnsureWebView2Runtime();

        // Xaml objects should not call InitializeComponent during construction.
        // See https://github.com/microsoft/cppwinrt/tree/master/nuget#initializecomponent

#if defined _DEBUG && !defined DISABLE_XAML_GENERATED_BREAK_ON_UNHANDLED_EXCEPTION
        UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& e)
        {
            if (IsDebuggerPresent())
            {
                auto errorMessage = e.Message();
                __debugbreak();
            }
        });
#endif
    }

    /// <summary>
    /// Invoked when the application is launched.
    /// </summary>
    /// <param name="e">Details about the launch request and process.</param>
    void App::OnLaunched([[maybe_unused]] LaunchActivatedEventArgs const& e)
    {
        window = make<MainWindow>();
        window.Activate();

        auto windowNative = window.as<IWindowNative>();
        windowNative->get_WindowHandle(&MainHwnd);
    }
}
