//
// TrayIconService.cpp
//
// Subclasses MainHwnd so Shell_NotifyIcon callbacks land in the
// existing message loop. Hooks WM_APP+1 (tray notify),
// WM_COMMAND (menu), WM_CLOSE (divert to hide instead of quit).

#include "TrayIconService.h"
#include "MCP/MCPServerHost.h"
#include "../Models/AppSettings.h"
#include <commctrl.h>
#include <shellapi.h>
#include <string>
#include <mutex>
#include <chrono>
#pragma comment(lib, "Comctl32.lib")

namespace DBModels { namespace TrayIconService {

namespace {
    constexpr UINT kTrayMessage = WM_APP + 1;
    constexpr UINT_PTR kSubclassId = 1;

    constexpr UINT kMenuOpen  = 40001;
    constexpr UINT kMenuToggle= 40002;
    constexpr UINT kMenuQuit  = 40003;

    std::mutex g_mtx;
    bool g_installed = false;
    HWND g_hwnd = nullptr;
    NOTIFYICONDATAW g_nid{};

    HICON loadAppIcon()
    {
        // Same icon as the taskbar — the EXE's first resource.
        HICON h = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
        if (!h) h = LoadIconW(nullptr, IDI_APPLICATION);
        return h;
    }

    std::wstring tooltip()
    {
        const bool running = []{
            auto srv = MCPServerHost::instance();
            return srv && srv->isRunning();
        }();
        return running
            ? L"Gridex — MCP Server running"
            : L"Gridex — MCP Server stopped";
    }

    void showMenu(HWND hwnd)
    {
        HMENU m = CreatePopupMenu();
        if (!m) return;
        AppendMenuW(m, MF_STRING, kMenuOpen, L"Open Gridex");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

        const bool running = []{
            auto srv = MCPServerHost::instance();
            return srv && srv->isRunning();
        }();
        AppendMenuW(m, MF_STRING, kMenuToggle,
            running ? L"Stop MCP Server" : L"Start MCP Server");

        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, kMenuQuit, L"Quit Gridex");

        POINT pt; GetCursorPos(&pt);
        // Required so the menu dismisses when clicking elsewhere.
        SetForegroundWindow(hwnd);
        TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
        DestroyMenu(m);
    }

    LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                          UINT_PTR /*id*/, DWORD_PTR /*ref*/)
    {
        if (msg == kTrayMessage)
        {
            const UINT ev = LOWORD(lp);
            if (ev == WM_LBUTTONDBLCLK || ev == WM_LBUTTONUP)
            {
                ShowWindow(hwnd, SW_SHOW);
                SetForegroundWindow(hwnd);
                return 0;
            }
            if (ev == WM_RBUTTONUP || ev == WM_CONTEXTMENU)
            {
                showMenu(hwnd);
                return 0;
            }
        }
        else if (msg == WM_COMMAND && HIWORD(wp) == 0)
        {
            const UINT id = LOWORD(wp);
            if (id == kMenuOpen)
            {
                ShowWindow(hwnd, SW_SHOW);
                SetForegroundWindow(hwnd);
                return 0;
            }
            if (id == kMenuToggle)
            {
                auto srv = MCPServerHost::instance();
                auto s = AppSettings::Load();
                if (srv && srv->isRunning())
                {
                    MCPServerHost::stop();
                    s.mcpEnabled = false;
                    s.mcpStartTime = 0;
                }
                else
                {
                    // GUI mode — HttpOnly; mirrors MCPPage::StartStop.
                    std::wstring w(L"dev");
                    std::string ver;
                    for (wchar_t c : w) ver.push_back(static_cast<char>(c & 0x7F));
                    MCPServerHost::ensureCreated(s, ver, MCPTransportMode::HttpOnly);
                    MCPServerHost::start();
                    s.mcpEnabled = true;
                    s.mcpStartTime = static_cast<int64_t>(
                        std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count());
                }
                s.Save();
                Refresh();
                return 0;
            }
            if (id == kMenuQuit)
            {
                MCPServerHost::stop();
                PostQuitMessage(0);
                ExitProcess(0);
                return 0;
            }
        }
        return DefSubclassProc(hwnd, msg, wp, lp);
    }
}

void Initialize(HWND mainHwnd)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_installed || !mainHwnd) return;
    g_hwnd = mainHwnd;

    SetWindowSubclass(mainHwnd, Proc, 1, 0);

    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = mainHwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = kTrayMessage;
    g_nid.hIcon = loadAppIcon();
    const auto tip = tooltip();
    wcsncpy_s(g_nid.szTip, tip.c_str(), _TRUNCATE);

    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_installed = true;
}

void Shutdown()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_installed) return;
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (g_hwnd) RemoveWindowSubclass(g_hwnd, Proc, 1);
    g_installed = false;
    g_hwnd = nullptr;
}

void Refresh()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_installed) return;
    const auto tip = tooltip();
    wcsncpy_s(g_nid.szTip, tip.c_str(), _TRUNCATE);
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

}} // namespace
