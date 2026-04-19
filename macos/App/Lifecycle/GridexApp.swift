// GridexApp.swift
// Gridex
//
// SwiftUI application entry point.

import SwiftUI
import SwiftData

struct GridexApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var appDelegate
    @FocusedObject private var focusedAppState: AppState?
    @ObservedObject private var updater = UpdaterService.shared

    private var currentAppState: AppState? { focusedAppState ?? AppState.active }

    var body: some Scene {
        WindowGroup {
            WindowRoot()
                .frame(minWidth: 900, minHeight: 500)
        }
        .defaultSize(width: 900, height: 500)
        .windowToolbarStyle(.unified(showsTitle: false))
        .commands {
            CommandGroup(replacing: .appSettings) {
                Button("Settings...") {
                    currentAppState?.showSettings = true
                }
                .keyboardShortcut(",", modifiers: .command)
            }

            CommandGroup(after: .appInfo) {
                Button("Check for Updates…") {
                    updater.checkForUpdates()
                }
                .disabled(!updater.canCheckForUpdates)
            }

            CommandGroup(after: .newItem) {
                // Cmd+N → default WindowGroup "New Window" (opens a fresh connection picker)

                Button("New Query") {
                    currentAppState?.openNewQueryTab()
                }
                .keyboardShortcut("n", modifiers: [.command, .shift])
                .disabled(currentAppState?.activeAdapter == nil)

                Button("New Connection") {
                    currentAppState?.showConnectionForm = true
                }
                .keyboardShortcut("t", modifiers: .command)

                Divider()

                Button("Close Tab") {
                    currentAppState?.closeActiveTab()
                }
                .keyboardShortcut("w", modifiers: .command)

                Button("Disconnect") {
                    currentAppState?.disconnect()
                }
                .keyboardShortcut("d", modifiers: [.command, .shift])
                .disabled(currentAppState?.activeAdapter == nil)
            }

            CommandGroup(after: .sidebar) {
                Button("Toggle Sidebar") {
                    currentAppState?.sidebarVisible.toggle()
                }
                .keyboardShortcut("\\", modifiers: .command)

                Button("Toggle AI Panel") {
                    currentAppState?.aiPanelVisible.toggle()
                }
                .keyboardShortcut("a", modifiers: [.command, .shift])

                Divider()

                Button("MCP Server...") {
                    MCPWindowController.show()
                }
                .keyboardShortcut("m", modifiers: [.command, .shift])
            }

            CommandMenu("Database") {
                Button("Open Database...") {
                    currentAppState?.showDatabaseSwitcher = true
                }
                .keyboardShortcut("k", modifiers: .command)
                .disabled(currentAppState?.activeAdapter == nil)

                Button("New Table...") {
                    currentAppState?.showNewTableSheet = true
                }
                .disabled(currentAppState?.activeAdapter == nil)
            }

            CommandMenu("Data") {
                Button("Commit Changes") {
                    NotificationCenter.default.post(name: .commitChanges, object: nil)
                }
                .keyboardShortcut("s", modifiers: .command)

                Button("Reload Data") {
                    NotificationCenter.default.post(name: .reloadData, object: nil)
                }
                .keyboardShortcut("r", modifiers: .command)

                Divider()

                Button("Toggle Filters") {
                    NotificationCenter.default.post(name: .toggleFilterBar, object: nil)
                }
                .keyboardShortcut("f", modifiers: .command)
            }

            CommandMenu("Query") {
                Button("Execute") {
                    NotificationCenter.default.post(name: .executeQuery, object: nil)
                }
                .keyboardShortcut("r", modifiers: [.command, .shift])

                Button("Explain") {
                    NotificationCenter.default.post(name: .explainQuery, object: nil)
                }
                .keyboardShortcut("e", modifiers: .command)

                Divider()

                Button("Format SQL") {
                    NotificationCenter.default.post(name: .formatSQL, object: nil)
                }
                .keyboardShortcut("b", modifiers: [.command, .shift])
            }
        }

        Settings {
            SettingsView()
        }
    }
}

extension Notification.Name {
    static let executeQuery = Notification.Name("Gridex.executeQuery")
    static let executeSelection = Notification.Name("Gridex.executeSelection")
    static let explainQuery = Notification.Name("Gridex.explainQuery")
    static let formatSQL = Notification.Name("Gridex.formatSQL")
    static let deleteSelectedRows = Notification.Name("Gridex.deleteSelectedRows")
    static let commitChanges = Notification.Name("Gridex.commitChanges")
    static let reloadData = Notification.Name("Gridex.reloadData")
    static let toggleFilterBar = Notification.Name("Gridex.toggleFilterBar")
}
