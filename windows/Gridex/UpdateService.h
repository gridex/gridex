#pragma once
// UpdateService -- thin wrapper around Velopack C++ SDK.
//
// Responsibilities:
//   1. Early-startup hook via VelopackApp::Build().Run() (called from App::App()).
//   2. Async "check for updates" that hits the R2 feed without blocking UI.
//   3. "Download + apply + restart" flow triggered from user consent dialog.
//
// Feed URL (kUpdateFeedUrl) must point at the directory containing
// releases.stable.json / RELEASES-stable / *.nupkg on R2. The final '/'
// is required.
//
// All async entry points run the Velopack calls on a std::thread so the
// WinUI dispatcher stays responsive. Callbacks fire on that background
// thread -- the caller is responsible for marshalling back to the UI
// via DispatcherQueue().TryEnqueue() when touching XAML.

#include <string>
#include <functional>

namespace Gridex
{
    // R2 feed URL (hardcoded -- single source of truth).
    extern const wchar_t* const kUpdateFeedUrl;

    struct UpdateCheckResult
    {
        bool hasUpdate = false;
        std::wstring currentVersion;  // always filled
        std::wstring newVersion;      // filled when hasUpdate == true
        std::wstring errorMessage;    // filled when check failed
    };

    // Run VelopackApp::Build().Run() -- handles --veloapp-install /
    // --veloapp-updated / --veloapp-obsolete / --veloapp-uninstall hooks
    // and may terminate the process. MUST be called as the very first
    // line of App::App() before any WinUI initialization.
    void InitVelopackApp();

    // Fire-and-forget check. `onComplete` runs on a background thread.
    void CheckForUpdateAsync(std::function<void(UpdateCheckResult)> onComplete);

    // Download + apply + restart. Caller should confirm with the user
    // first. `onStatus` receives progress messages ("Downloading...",
    // "Installing..."), `onError` receives failure reason. Both run on
    // a background thread. On success the process exits inside this
    // call and the Velopack updater takes over.
    void DownloadAndApplyAsync(
        std::function<void(std::wstring)> onStatus,
        std::function<void(std::wstring)> onError);
}
