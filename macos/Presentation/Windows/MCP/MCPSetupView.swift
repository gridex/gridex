// MCPSetupView.swift
// Gridex
//
// Quick setup guide for configuring AI clients.

import SwiftUI

struct MCPSetupView: View {
    @AppStorage("mcp.httpEnabled") private var httpEnabled = false
    @AppStorage("mcp.httpPort") private var httpPort = 3333

    @State private var copiedClient: String?
    @State private var selectedClient: MCPClientType = .claudeDesktop
    @State private var installResult: InstallResult?
    @State private var showInstallAlert = false

    enum InstallResult {
        case success(String)
        case error(String)

        var title: String {
            switch self {
            case .success: return "Installed"
            case .error: return "Install Failed"
            }
        }

        var message: String {
            switch self {
            case .success(let msg), .error(let msg): return msg
            }
        }
    }

    var body: some View {
        Form {
            Section {
                HStack(spacing: 10) {
                    Image(systemName: "info.circle.fill")
                        .foregroundStyle(.blue)
                    VStack(alignment: .leading, spacing: 2) {
                        Text("This is a config file, not a terminal command")
                            .font(.callout.weight(.medium))
                        Text("Use \"Install\" below to add Gridex to your client automatically, or copy and paste into the config file manually.")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                    Spacer()
                }
                .padding(.vertical, 2)
            }

            Section("Client") {
                Picker("AI Client", selection: $selectedClient) {
                    ForEach(MCPClientType.allCases) { client in
                        Label(client.displayName, systemImage: client.iconName).tag(client)
                    }
                }
                .pickerStyle(.menu)

                LabeledContent("Config file") {
                    HStack(spacing: 6) {
                        Text(selectedClient.configPath)
                            .font(.system(.caption, design: .monospaced))
                            .textSelection(.enabled)
                            .foregroundStyle(.secondary)
                            .lineLimit(1)
                            .truncationMode(.middle)

                        Button {
                            openConfigPath()
                        } label: {
                            Image(systemName: "arrow.up.right.square")
                        }
                        .buttonStyle(.borderless)
                        .help("Show in Finder")
                    }
                }
            }

            Section {
                HStack {
                    Button {
                        installConfig()
                    } label: {
                        HStack(spacing: 6) {
                            Image(systemName: "wand.and.stars")
                            Text("Install for \(selectedClient.displayName)")
                        }
                        .frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.borderedProminent)
                    .controlSize(.large)

                    Button {
                        copyConfig()
                    } label: {
                        HStack(spacing: 6) {
                            Image(systemName: copiedClient == selectedClient.rawValue ? "checkmark" : "doc.on.clipboard")
                            Text(copiedClient == selectedClient.rawValue ? "Copied" : "Copy")
                        }
                    }
                    .buttonStyle(.bordered)
                    .controlSize(.large)
                }
            } header: {
                Text("Quick Install")
            } footer: {
                Text("Install merges Gridex into your existing config automatically. Backup of the original file is kept next to it.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section {
                ScrollView(.horizontal, showsIndicators: false) {
                    Text(selectedClient.configJSON(httpPort: httpPort, httpEnabled: httpEnabled))
                        .font(.system(.body, design: .monospaced))
                        .textSelection(.enabled)
                        .padding(12)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
                .background(Color(nsColor: .textBackgroundColor))
                .clipShape(RoundedRectangle(cornerRadius: 6))
            } header: {
                Text("Configuration Preview")
            } footer: {
                Text("This is the JSON that will be merged into the mcpServers section of your config file.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section("Manual Steps") {
                stepRow(1, "Click \"Install\" above, or copy the JSON manually")
                stepRow(2, "If manual: open the config file (Finder button)")
                stepRow(3, "Paste the JSON inside \"mcpServers\": { ... }")
                stepRow(4, "Save the file and restart \(selectedClient.displayName)")
                stepRow(5, "Ask: \"List my database connections\"")
            }
        }
        .formStyle(.grouped)
        .alert(installResult?.title ?? "", isPresented: $showInstallAlert, presenting: installResult) { _ in
            Button("OK") {}
        } message: { result in
            Text(result.message)
        }
    }

    private func stepRow(_ number: Int, _ text: String) -> some View {
        HStack(alignment: .top, spacing: 10) {
            Text("\(number)")
                .font(.caption.weight(.semibold))
                .foregroundStyle(.white)
                .frame(width: 18, height: 18)
                .background(Color.accentColor)
                .clipShape(Circle())

            Text(text)
                .font(.callout)

            Spacer()
        }
    }

    private func copyConfig() {
        let config = selectedClient.configJSON(httpPort: httpPort, httpEnabled: httpEnabled)
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(config, forType: .string)

        withAnimation { copiedClient = selectedClient.rawValue }

        DispatchQueue.main.asyncAfter(deadline: .now() + 2) {
            withAnimation {
                if copiedClient == selectedClient.rawValue {
                    copiedClient = nil
                }
            }
        }
    }

    private func openConfigPath() {
        let path = expandedConfigPath()
        let url = URL(fileURLWithPath: path)
        if FileManager.default.fileExists(atPath: path) {
            NSWorkspace.shared.activateFileViewerSelecting([url])
        } else {
            NSWorkspace.shared.open(url.deletingLastPathComponent())
        }
    }

    private func expandedConfigPath() -> String {
        selectedClient.configPath
            .replacingOccurrences(of: "~", with: FileManager.default.homeDirectoryForCurrentUser.path)
    }

    // MARK: - Install

    private func installConfig() {
        let fileManager = FileManager.default
        let path = expandedConfigPath()
        let url = URL(fileURLWithPath: path)

        // Ensure parent directory exists
        let dir = url.deletingLastPathComponent()
        if !fileManager.fileExists(atPath: dir.path) {
            do {
                try fileManager.createDirectory(at: dir, withIntermediateDirectories: true)
            } catch {
                installResult = .error("Could not create config directory: \(error.localizedDescription)")
                showInstallAlert = true
                return
            }
        }

        // Read existing JSON or start fresh
        var existing: [String: Any] = [:]
        if fileManager.fileExists(atPath: path) {
            if let data = try? Data(contentsOf: url),
               let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
                existing = json

                // Backup original
                let backupURL = url.appendingPathExtension("bak")
                try? data.write(to: backupURL)
            } else if let data = try? Data(contentsOf: url), !data.isEmpty {
                installResult = .error("Existing config file is not valid JSON. Please fix or delete it, then try again.")
                showInstallAlert = true
                return
            }
        }

        // Build gridex entry
        let gridexEntry: [String: Any]
        if selectedClient == .claudeCode && httpEnabled {
            gridexEntry = [
                "url": "http://127.0.0.1:\(httpPort)/mcp"
            ]
        } else {
            let gridexPath = Bundle.main.bundlePath.isEmpty
                ? "/Applications/Gridex.app/Contents/MacOS/Gridex"
                : "\(Bundle.main.bundlePath)/Contents/MacOS/Gridex"
            gridexEntry = [
                "command": gridexPath,
                "args": ["--mcp-stdio"]
            ]
        }

        // Merge into mcpServers
        var mcpServers = existing["mcpServers"] as? [String: Any] ?? [:]
        mcpServers["gridex"] = gridexEntry
        existing["mcpServers"] = mcpServers

        // Write back with pretty formatting
        do {
            let data = try JSONSerialization.data(
                withJSONObject: existing,
                options: [.prettyPrinted, .sortedKeys]
            )
            try data.write(to: url)
            installResult = .success("Gridex has been added to \(selectedClient.displayName)'s config. Restart \(selectedClient.displayName) to apply.")
            showInstallAlert = true
        } catch {
            installResult = .error("Could not write config file: \(error.localizedDescription)")
            showInstallAlert = true
        }
    }
}

enum MCPClientType: String, CaseIterable, Identifiable {
    case claudeDesktop = "claude_desktop"
    case cursor = "cursor"
    case windsurf = "windsurf"
    case claudeCode = "claude_code"

    var id: String { rawValue }

    var displayName: String {
        switch self {
        case .claudeDesktop: return "Claude Desktop"
        case .cursor: return "Cursor"
        case .windsurf: return "Windsurf"
        case .claudeCode: return "Claude Code"
        }
    }

    var iconName: String {
        switch self {
        case .claudeDesktop: return "message.fill"
        case .cursor: return "cursorarrow"
        case .windsurf: return "wind"
        case .claudeCode: return "terminal.fill"
        }
    }

    var configPath: String {
        switch self {
        case .claudeDesktop:
            return "~/Library/Application Support/Claude/claude_desktop_config.json"
        case .cursor:
            return "~/.cursor/mcp.json"
        case .windsurf:
            return "~/.windsurf/mcp.json"
        case .claudeCode:
            return "~/.claude/settings.json"
        }
    }

    func configJSON(httpPort: Int, httpEnabled: Bool) -> String {
        let gridexPath = Bundle.main.executablePath ?? "/Applications/Gridex.app/Contents/MacOS/Gridex"

        switch self {
        case .claudeDesktop, .cursor, .windsurf:
            return """
            {
              "mcpServers": {
                "gridex": {
                  "command": "\(gridexPath)",
                  "args": ["--mcp-stdio"]
                }
              }
            }
            """
        case .claudeCode:
            if httpEnabled {
                return """
                {
                  "mcpServers": {
                    "gridex": {
                      "url": "http://127.0.0.1:\(httpPort)/mcp"
                    }
                  }
                }
                """
            } else {
                return """
                {
                  "mcpServers": {
                    "gridex": {
                      "command": "\(gridexPath)",
                      "args": ["--mcp-stdio"]
                    }
                  }
                }
                """
            }
        }
    }
}
