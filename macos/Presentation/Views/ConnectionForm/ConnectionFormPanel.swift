// ConnectionFormPanel.swift
// Gridex
//
// Opens ConnectionFormView as a native NSPanel with traffic-light buttons.

import SwiftUI
import AppKit

@MainActor
final class ConnectionFormPanel {

    private static var current: NSPanel?

    // MARK: - Open

    static func open(
        databaseType: DatabaseType,
        existingConfig: ConnectionConfig? = nil,
        existingPassword: String = "",
        existingSSHPassword: String = "",
        appState: AppState
    ) {
        current?.close()
        current = nil

        let isEditing = existingConfig != nil

        let formView = ConnectionFormView(
            databaseType: databaseType,
            existingConfig: existingConfig,
            existingPassword: existingPassword,
            existingSSHPassword: existingSSHPassword,
            onConnect: { config, pw, sshPw in
                Task { @MainActor in
                    do {
                        if isEditing {
                            try await appState.container.connectionRepository.update(config)
                        } else {
                            try await appState.container.connectionRepository.save(config)
                        }
                    } catch { appState.connectionError = "Save failed: \(error.localizedDescription)" }
                    persistCredentials(config: config, dbPassword: pw, sshPassword: sshPw, appState: appState)
                    ConnectionFormPanel.close()
                    await appState.connect(config: config, password: pw, sshPassword: sshPw)
                }
            },
            onTest: { config, pw, sshPw in
                await runTest(config: config, dbPassword: pw, sshPassword: sshPw, appState: appState)
            },
            onSave: { config, pw, sshPw in
                Task { @MainActor in
                    do {
                        if isEditing {
                            try await appState.container.connectionRepository.update(config)
                        } else {
                            try await appState.container.connectionRepository.save(config)
                        }
                    } catch { appState.connectionError = "Save failed: \(error.localizedDescription)" }
                    persistCredentials(config: config, dbPassword: pw, sshPassword: sshPw, appState: appState)
                    ConnectionFormPanel.close()
                    await appState.loadSavedConnections()
                }
            },
            onCancel: {
                ConnectionFormPanel.close()
            }
        )
        .environmentObject(appState)

        let controller = NSHostingController(rootView: formView)

        let panel = NSPanel(
            contentRect: NSRect(x: 0, y: 0, width: 580, height: 520),
            styleMask: [.titled, .closable, .miniaturizable, .resizable],
            backing: .buffered,
            defer: false
        )
        panel.title = "\(databaseType.displayName) Connection"
        panel.contentViewController = controller
        panel.layoutIfNeeded()

        // Center the panel over the main app window (not the screen).
        // Done after layout so panel.frame.size reflects the actual size.
        let parentWindow = NSApp.mainWindow
            ?? NSApp.windows.first(where: { $0.canBecomeMain && !($0 is NSPanel) && $0.isVisible })
        if let parent = parentWindow {
            let parentFrame = parent.frame
            let panelSize = panel.frame.size
            let origin = NSPoint(
                x: parentFrame.midX - panelSize.width / 2,
                y: parentFrame.midY - panelSize.height / 2
            )
            panel.setFrameOrigin(origin)
        } else {
            panel.center()
        }

        panel.makeKeyAndOrderFront(nil)

        current = panel
    }

    // MARK: - Close

    static func close() {
        current?.close()
        current = nil
    }

    // MARK: - Helpers

    @MainActor
    private static func persistCredentials(
        config: ConnectionConfig,
        dbPassword: String,
        sshPassword: String,
        appState: AppState
    ) {
        let keychain = appState.container.keychainService
        let dbKey = "db.password.\(config.id.uuidString)"
        let sshKey = "ssh.password.\(config.id.uuidString)"

        do {
            if dbPassword.isEmpty {
                try? keychain.delete(key: dbKey)
            } else {
                try keychain.save(key: dbKey, value: dbPassword)
            }
            if config.sshConfig != nil, !sshPassword.isEmpty {
                try keychain.save(key: sshKey, value: sshPassword)
            } else {
                try? keychain.delete(key: sshKey)
            }
        } catch {
            appState.connectionError = "Keychain error: \(error.localizedDescription)"
        }
    }

    private static func runTest(
        config: ConnectionConfig,
        dbPassword: String,
        sshPassword: String,
        appState: AppState
    ) async -> ConnectionTestResult {
        let adapter: any DatabaseAdapter = switch config.databaseType {
        case .sqlite: SQLiteAdapter()
        case .postgresql: PostgreSQLAdapter()
        case .mysql: MySQLAdapter()
        case .redis: RedisAdapter()
        case .mongodb: MongoDBAdapter()
        case .mssql: MSSQLAdapter()
        case .clickhouse: ClickHouseAdapter()
        }

        let sshService = appState.container.sshTunnelService
        let testConnectionId = UUID()

        let start = CFAbsoluteTimeGetCurrent()
        var effectiveConfig = config

        // Establish SSH tunnel for the duration of the test so the adapter connects
        // to 127.0.0.1:<localPort> instead of the remote host.
        if let ssh = config.sshConfig {
            do {
                let remoteHost = config.host ?? "127.0.0.1"
                let remotePort = config.port ?? config.databaseType.defaultPort
                let localPort = try await sshService.establish(
                    connectionId: testConnectionId,
                    config: ssh,
                    remoteHost: remoteHost,
                    remotePort: remotePort,
                    password: sshPassword
                )
                effectiveConfig.host = "127.0.0.1"
                effectiveConfig.port = Int(localPort)
            } catch {
                return ConnectionTestResult(
                    success: false, serverVersion: nil,
                    latency: CFAbsoluteTimeGetCurrent() - start,
                    errorMessage: "SSH tunnel failed: \(friendlyError(error))"
                )
            }
        }

        do {
            try await adapter.connect(config: effectiveConfig, password: dbPassword)
            let ver = try? await adapter.serverVersion()
            let latency = CFAbsoluteTimeGetCurrent() - start
            try? await adapter.disconnect()
            await sshService.disconnect(connectionId: testConnectionId)
            return ConnectionTestResult(success: true, serverVersion: ver,
                                        latency: latency, errorMessage: nil)
        } catch {
            try? await adapter.disconnect()
            await sshService.disconnect(connectionId: testConnectionId)
            return ConnectionTestResult(
                success: false, serverVersion: nil,
                latency: CFAbsoluteTimeGetCurrent() - start,
                errorMessage: friendlyError(error)
            )
        }
    }

    /// Maps low-level NIO/network errors to readable messages.
    private static func friendlyError(_ error: Error) -> String {
        let ns = error as NSError
        let desc = error.localizedDescription
        if desc.contains("NIOCore.IOError") || ns.domain.contains("NIO") {
            return "Cannot reach host — check hostname, port, and firewall."
        }
        return desc
    }
}
