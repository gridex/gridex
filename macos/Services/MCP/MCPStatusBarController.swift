// MCPStatusBarController.swift
// Gridex
//
// Menu bar status indicator for MCP server.

import AppKit
import SwiftUI

@MainActor
class MCPStatusBarController: NSObject, NSMenuDelegate {
    static let shared = MCPStatusBarController()

    private var statusItem: NSStatusItem?
    private var connectedClients = 0
    private var pendingApproval = false

    private var isEnabled: Bool {
        UserDefaults.standard.bool(forKey: "mcp.enabled")
    }

    private override init() {
        super.init()
        setupStatusItem()
    }

    private func setupStatusItem() {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        statusItem?.isVisible = true
        updateIcon()
        setupMenu()
    }

    func refresh() {
        updateIcon()
    }

    private func updateIcon() {
        guard let button = statusItem?.button else { return }

        let iconName: String
        let color: NSColor

        if !isEnabled {
            iconName = "server.rack"
            color = .systemGray
        } else if pendingApproval {
            iconName = "server.rack"
            color = .systemOrange
        } else if connectedClients > 0 {
            iconName = "server.rack"
            color = .systemBlue
        } else {
            iconName = "server.rack"
            color = .systemGreen
        }

        let config = NSImage.SymbolConfiguration(pointSize: 14, weight: .medium)
        if let image = NSImage(systemSymbolName: iconName, accessibilityDescription: "MCP Server") {
            let coloredImage = image.withSymbolConfiguration(config)
            button.image = coloredImage
            button.contentTintColor = color
        }

        button.toolTip = statusTooltip()
    }

    private func statusTooltip() -> String {
        if !isEnabled {
            return "MCP Server: Disabled"
        }
        if pendingApproval {
            return "MCP Server: Approval Required"
        }
        if connectedClients > 0 {
            return "MCP Server: \(connectedClients) client(s) connected"
        }
        return "MCP Server: Running"
    }

    private func setupMenu() {
        let menu = NSMenu()
        menu.delegate = self

        // Status header
        let headerItem = NSMenuItem(title: "MCP Server", action: nil, keyEquivalent: "")
        headerItem.isEnabled = false
        headerItem.tag = 0
        menu.addItem(headerItem)

        menu.addItem(NSMenuItem.separator())

        // Toggle
        let toggleItem = NSMenuItem(
            title: "Enable MCP Server",
            action: #selector(toggleServer),
            keyEquivalent: ""
        )
        toggleItem.target = self
        toggleItem.tag = 1
        menu.addItem(toggleItem)

        menu.addItem(NSMenuItem.separator())

        // Open MCP Window
        let openItem = NSMenuItem(
            title: "Open MCP Server...",
            action: #selector(openMCPWindow),
            keyEquivalent: ""
        )
        openItem.target = self
        menu.addItem(openItem)

        // Activity Log
        let activityItem = NSMenuItem(
            title: "View Activity Log",
            action: #selector(openActivityLog),
            keyEquivalent: ""
        )
        activityItem.target = self
        menu.addItem(activityItem)

        self.statusItem?.menu = menu
    }

    // NSMenuDelegate - update toggle state when menu opens
    nonisolated func menuWillOpen(_ menu: NSMenu) {
        Task { @MainActor in
            if let toggleItem = menu.item(withTag: 1) {
                toggleItem.state = self.isEnabled ? .on : .off
            }
        }
    }

    @objc private func toggleServer() {
        let newValue = !isEnabled
        UserDefaults.standard.set(newValue, forKey: "mcp.enabled")
        updateIcon()

        // Start or stop MCP server
        Task {
            if newValue {
                await DependencyContainer.shared.bootstrapMCPServer()
            } else {
                await DependencyContainer.shared.mcpServer.stop()
            }
        }
    }

    @objc private func openMCPWindow() {
        MCPWindowController.show()
    }

    @objc private func openActivityLog() {
        MCPWindowController.show()
        // TODO: Navigate to activity section
    }

    // MARK: - Public API

    func updateClientCount(_ count: Int) {
        connectedClients = count
        updateIcon()
    }

    func showPendingApproval(_ pending: Bool) {
        pendingApproval = pending
        updateIcon()

        if pending {
            // Pulse the icon
            NSApp.requestUserAttention(.criticalRequest)
        }
    }

    func show() {
        statusItem?.isVisible = true
    }

    func hide() {
        statusItem?.isVisible = false
    }
}
