// AppDelegate.swift
// Gridex
//
// Legacy AppDelegate — kept for applicationWillTerminate cleanup.
// The @main entry point is now GridexApp.swift.

import AppKit

final class AppDelegate: NSObject, NSApplicationDelegate {

    func applicationWillFinishLaunching(_ notification: Notification) {
        // Register bundle identifier for SPM builds (fixes "missing main bundle identifier" warning)
        if Bundle.main.bundleIdentifier == nil {
            UserDefaults.standard.set("com.gridex.app", forKey: "CFBundleIdentifier")
        }
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        // Ensure app activates and shows window when launched from terminal/SPM
        NSApplication.shared.setActivationPolicy(.regular)
        NSApplication.shared.activate(ignoringOtherApps: true)

        // Load persisted LLM provider configs into the in-memory registry so
        // AIChatView / other callers can resolve them by name without waiting.
        Task { await DependencyContainer.shared.bootstrapProviderRegistry() }

        // Thin scrollbars app-wide; horizontal always visible
        UserDefaults.standard.set("Always", forKey: "AppleShowScrollBars")

        NotificationCenter.default.addObserver(
            forName: NSView.frameDidChangeNotification,
            object: nil,
            queue: .main
        ) { notification in
            guard let scrollView = notification.object as? NSScrollView else { return }
            // Mini control size for thin scrollbars
            if let vScroller = scrollView.verticalScroller, vScroller.controlSize != .mini {
                vScroller.controlSize = .mini
            }
            scrollView.hasHorizontalScroller = true
            if let hScroller = scrollView.horizontalScroller, hScroller.controlSize != .mini {
                hScroller.controlSize = .mini
            }
        }

        // Make sure the main window is visible and set up close interception
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
            self.installCloseInterceptor()
        }

        // Also observe new windows in case SwiftUI recreates the window.
        // Only install on the primary main window — skip NSPanel (e.g. ConnectionFormPanel)
        // and any auxiliary windows, otherwise closing them would route through
        // WindowCloseInterceptor and terminate the app.
        NotificationCenter.default.addObserver(
            forName: NSWindow.didBecomeKeyNotification,
            object: nil,
            queue: .main
        ) { [weak self] notification in
            guard let window = notification.object as? NSWindow,
                  window.delegate !== WindowCloseInterceptor.shared,
                  !(window is NSPanel),
                  window.canBecomeMain else { return }
            self?.installCloseInterceptor(for: window)
        }
    }

    private func installCloseInterceptor(for window: NSWindow? = nil) {
        let win = window ?? NSApplication.shared.windows.first
        guard let win else { return }
        win.makeKeyAndOrderFront(nil)
        win.delegate = WindowCloseInterceptor.shared
    }

    func applicationWillTerminate(_ notification: Notification) {
        // Cleanup: disconnect all active database connections
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        true
    }

    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        if !flag {
            for window in sender.windows {
                window.makeKeyAndOrderFront(nil)
            }
        }
        return true
    }

    func applicationSupportsSecureRestorableState(_ app: NSApplication) -> Bool {
        true
    }
}

// Intercepts the window close button for multi-window support.
// - Multiple windows open: close the window normally (MainView.onDisappear cleans up).
// - Last window: disconnect and show Home instead of closing (preserves single-window UX).
final class WindowCloseInterceptor: NSObject, NSWindowDelegate {
    static let shared = WindowCloseInterceptor()

    func windowShouldClose(_ sender: NSWindow) -> Bool {
        let otherMainWindows = NSApp.windows.filter {
            $0 !== sender && $0.canBecomeMain && !($0 is NSPanel) && $0.isVisible
        }

        if otherMainWindows.isEmpty {
            // Last window — keep it open, just disconnect
            NotificationCenter.default.post(name: .windowCloseRequested, object: sender)
            return false
        } else {
            // Other windows exist — close this one
            return true
        }
    }
}

extension Notification.Name {
    static let windowCloseRequested = Notification.Name("Gridex.windowCloseRequested")
}
