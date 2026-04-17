#include "pch.h"
#include "UpdateService.h"
#include "GridexVersion.h"

#include <thread>
#include <stdexcept>

// Velopack C++ SDK -- vendored under windows/vendor/velopack/include/.
// Velopack.hpp is the thin C++ wrapper over the C API (Velopack.h).
// Static lib velopack_libc_win_x64_msvc.lib is linked via vcxproj.
#include "Velopack.hpp"

namespace Gridex
{
    // R2 public feed -- trailing slash required so Velopack can append
    // releases.stable.json / RELEASES-stable / *.nupkg cleanly.
    const wchar_t* const kUpdateFeedUrl = L"https://cdn.gridex.app/windows/";

    static std::string WideToUtf8(const std::wstring& w)
    {
        if (w.empty()) return {};
        int need = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                         nullptr, 0, nullptr, nullptr);
        std::string out(need, '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                              out.data(), need, nullptr, nullptr);
        return out;
    }

    static std::wstring Utf8ToWide(const std::string& s)
    {
        if (s.empty()) return {};
        int need = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(),
                                         nullptr, 0);
        std::wstring out(need, L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(),
                              out.data(), need);
        return out;
    }

    void InitVelopackApp()
    {
        // Handles --veloapp-install / --veloapp-updated / --veloapp-obsolete
        // / --veloapp-uninstall lifecycle flags. May call ExitProcess itself.
        // Safe to call even when app isn't installed via Velopack (no-op).
        try
        {
            Velopack::VelopackApp::Build().Run();
        }
        catch (...)
        {
            // Swallow -- startup must never be blocked by updater init.
        }
    }

    void CheckForUpdateAsync(std::function<void(UpdateCheckResult)> onComplete)
    {
        std::thread([onComplete = std::move(onComplete)]()
        {
            UpdateCheckResult r;
            const std::string feedUtf8 = WideToUtf8(kUpdateFeedUrl);
            try
            {
                Velopack::UpdateManager mgr(feedUtf8);
                r.currentVersion = Utf8ToWide(mgr.GetCurrentVersion());

                auto updInfo = mgr.CheckForUpdates();
                if (updInfo.has_value())
                {
                    r.hasUpdate = true;
                    r.newVersion = Utf8ToWide(updInfo->TargetFullRelease.Version);
                }
            }
            catch (const std::exception& e)
            {
                // UpdateManager ctor throws when the app is NOT installed
                // via Velopack (dev/debug runs); CheckForUpdates throws on
                // network / parse errors. Both land here.
                r.errorMessage = Utf8ToWide(e.what());
            }
            catch (...)
            {
                r.errorMessage = L"Unknown error during update check";
            }
            onComplete(r);
        }).detach();
    }

    void DownloadAndApplyAsync(
        std::function<void(std::wstring)> onStatus,
        std::function<void(std::wstring)> onError)
    {
        std::thread([onStatus = std::move(onStatus),
                     onError  = std::move(onError)]()
        {
            const std::string feedUtf8 = WideToUtf8(kUpdateFeedUrl);
            try
            {
                Velopack::UpdateManager mgr(feedUtf8);

                onStatus(L"Checking for updates...");
                auto updInfo = mgr.CheckForUpdates();
                if (!updInfo.has_value())
                {
                    onError(L"No update available");
                    return;
                }

                onStatus(L"Downloading update...");
                mgr.DownloadUpdates(*updInfo);

                onStatus(L"Installing update...");
                // Spawns the updater and tells it to wait for this process
                // to exit. We must ExitProcess shortly after.
                mgr.WaitExitThenApplyUpdates(*updInfo);

                // Give the updater a brief moment to start watching our
                // PID, then quit so it can swap files and relaunch us.
                ::Sleep(500);
                ::ExitProcess(0);
            }
            catch (const std::exception& e)
            {
                onError(Utf8ToWide(e.what()));
            }
            catch (...)
            {
                onError(L"Unknown error during update");
            }
        }).detach();
    }
}
