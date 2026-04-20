#pragma once
//
// TrayIconService.h
// Gridex
//
// Lightweight Shell_NotifyIconW wrapper. Uses MainHwnd as the
// message target (subclassed) so we don't need a second message
// pump. Exposes the right-click menu (Open Gridex / Start-Stop
// MCP / Quit) and tooltips reflecting current MCP state.
//
// Lifetime: TrayIconService::Initialize once after MainWindow is
// activated, TrayIconService::Shutdown on process exit. Safe to
// call repeatedly.

#include <windows.h>
#include <string>

namespace DBModels
{
    namespace TrayIconService
    {
        // Install the icon; hooks MainHwnd's WndProc for tray
        // callback + right-click menu. No-op if already installed.
        void Initialize(HWND mainHwnd);

        // Remove the icon + restore the original WndProc.
        void Shutdown();

        // Refresh tooltip + icon after server state changes.
        // Called from MCPPage::StartStop_Click.
        void Refresh();
    }
}
