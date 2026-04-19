// ConnectionFormView.swift
// Gridex
//
// SwiftUI form for creating/editing database connections (Gridex-style layout).

import SwiftUI
import AppKit

struct ConnectionFormView: View {
    let databaseType: DatabaseType
    var existingConfig: ConnectionConfig?
    var existingPassword: String = ""
    var existingSSHPassword: String = ""

    @State private var name: String = ""
    @State private var host: String = "localhost"
    @State private var port: String = ""
    @State private var database: String = ""
    @State private var username: String = ""
    @State private var password: String = ""
    @State private var sqliteFilePath: String = ""
    @State private var connectionString: String = ""
    @State private var connStringError: String?

    // Tag
    @State private var colorTag: ColorTag = .blue

    // SSL
    @State private var sslMode: SSLMode = .preferred
    @State private var sslKeyPath: String = ""
    @State private var sslCertPath: String = ""
    @State private var sslCACertPath: String = ""

    // SSH
    @State private var sshEnabled: Bool = false
    @State private var sshHost: String = ""
    @State private var sshPort: String = "22"
    @State private var sshUsername: String = ""
    @State private var sshPassword: String = ""
    @State private var sshAuthMethod: SSHAuthMethod = .password
    @State private var sshKeyPath: String = ""

    // Password storage
    @State private var storeInKeychain: Bool = true

    // Test
    @State private var isTesting: Bool = false
    @State private var testResult: ConnectionTestResult?

    var onConnect: ((ConnectionConfig, String, String) -> Void)?
    var onTest: ((ConnectionConfig, String, String) async -> ConnectionTestResult)?
    var onSave: ((ConnectionConfig, String, String) -> Void)?
    var onCancel: (() -> Void)?

    private var isEditing: Bool { existingConfig != nil }
    private let labelWidth: CGFloat = 110

    var body: some View {
        VStack(spacing: 0) {
            // Form fields
            VStack(spacing: 16) {
                // Name
                TPRow(label: "Name", labelWidth: labelWidth) {
                    TPTextField(placeholder: "Connection name", text: $name)
                }

                // Status color + Tag
                TPRow(label: "Status color", labelWidth: labelWidth) {
                    HStack(spacing: 0) {
                        HStack(spacing: 6) {
                            ForEach(ColorTag.allCases, id: \.self) { tag in
                                RoundedRectangle(cornerRadius: 3)
                                    .fill(tag.swiftUIColor)
                                    .frame(width: 28, height: 20)
                                    .overlay(
                                        RoundedRectangle(cornerRadius: 3)
                                            .stroke(Color.white, lineWidth: colorTag == tag ? 2 : 0)
                                    )
                                    .onTapGesture { colorTag = tag }
                            }
                        }

                        Spacer()

                        // Tag dropdown
                        HStack(spacing: 4) {
                            Text("Tag")
                                .font(.system(size: 13))
                                .foregroundStyle(.secondary)
                                .frame(width: 30, alignment: .trailing)

                            Picker("", selection: $colorTag) {
                                ForEach(ColorTag.allCases, id: \.self) { tag in
                                    Text(tag.environmentHint).tag(tag)
                                }
                            }
                            .labelsHidden()
                            .pickerStyle(.menu)
                            .frame(width: 120)
                        }
                    }
                }

                // MongoDB: connection string field (paste full URI to auto-fill)
                if databaseType == .mongodb {
                    TPRow(label: "URI", labelWidth: labelWidth) {
                        HStack(spacing: 8) {
                            TPTextField(placeholder: "mongodb://user:pass@host:27017/db or mongodb+srv://...", text: $connectionString)
                            Button("Parse") { parseMongoConnectionString() }
                                .disabled(connectionString.isEmpty)
                        }
                    }
                    if let err = connStringError {
                        TPRow(label: "", labelWidth: labelWidth) {
                            Text(err)
                                .font(.system(size: 11))
                                .foregroundStyle(.red)
                        }
                    }
                }

                if databaseType == .sqlite {
                    // SQLite file
                    TPRow(label: "File", labelWidth: labelWidth) {
                        HStack(spacing: 8) {
                            TPTextField(placeholder: "path/to/database.db", text: $sqliteFilePath)
                            Button("Browse...") { pickSQLiteFile() }
                        }
                    }
                } else {
                    // Host/Socket + Port
                    HStack(spacing: 0) {
                        TPRow(label: "Host/Socket", labelWidth: labelWidth) {
                            TPTextField(placeholder: "127.0.0.1", text: $host)
                        }

                        HStack(spacing: 8) {
                            Text("Port")
                                .font(.system(size: 13))
                                .foregroundStyle(.secondary)
                            TPTextField(placeholder: String(databaseType.defaultPort), text: $port)
                                .frame(width: 72)
                        }
                    }

                    if databaseType != .redis {
                        // User + Other options (not needed for Redis)
                        TPRow(label: "User", labelWidth: labelWidth) {
                            HStack(spacing: 0) {
                                TPTextField(placeholder: "user name", text: $username)
                                Spacer().frame(width: 12)
                                TPButton(title: "Other options") {}
                                    .frame(width: 130)
                            }
                        }
                    }

                    // Password + Store in keychain
                    TPRow(label: "Password", labelWidth: labelWidth) {
                        HStack(spacing: 0) {
                            ZStack(alignment: .trailing) {
                                TPTextField(placeholder: databaseType == .redis ? "optional" : "password", text: $password, isSecure: true)
                                if !password.isEmpty {
                                    Text("\(password.count)")
                                        .font(.system(size: 10, weight: .medium, design: .monospaced))
                                        .foregroundStyle(.tertiary)
                                        .padding(.trailing, 6)
                                        .allowsHitTesting(false)
                                }
                            }
                            Spacer().frame(width: 12)
                            Picker("", selection: $storeInKeychain) {
                                Text("Store in keychain").tag(true)
                                Text("Don't store").tag(false)
                            }
                            .labelsHidden()
                            .pickerStyle(.menu)
                            .frame(width: 170)
                        }
                    }

                    if databaseType == .redis {
                        // Redis: database number + TLS toggle
                        TPRow(label: "Database", labelWidth: labelWidth) {
                            HStack(spacing: 0) {
                                TPTextField(placeholder: "0 (default)", text: $database)
                                Spacer().frame(width: 12)
                                Toggle("TLS (rediss://)", isOn: Binding(
                                    get: { sslMode != .disabled },
                                    set: { sslMode = $0 ? .required : .disabled }
                                ))
                                .toggleStyle(.checkbox)
                                .font(.system(size: 12))
                            }
                        }
                    } else {
                        // Database + SSL mode
                        TPRow(label: "Database", labelWidth: labelWidth) {
                            HStack(spacing: 0) {
                                TPTextField(placeholder: "database name", text: $database)
                                Spacer().frame(width: 12)
                                HStack(spacing: 4) {
                                    Text("SSL mode")
                                        .font(.system(size: 13))
                                        .foregroundStyle(.secondary)
                                    Picker("", selection: $sslMode) {
                                        ForEach(SSLMode.allCases, id: \.self) { mode in
                                            Text(mode.displayName).tag(mode)
                                        }
                                    }
                                    .labelsHidden()
                                    .pickerStyle(.menu)
                                    .frame(width: 120)
                                }
                            }
                        }
                    }

                    // SSL keys (not applicable to non-SQL databases)
                    if databaseType != .redis && databaseType != .mongodb {
                    TPRow(label: "SSL keys", labelWidth: labelWidth) {
                        HStack(spacing: 6) {
                            TPFileButton(title: "Key...", path: $sslKeyPath)
                            TPFileButton(title: "Cert...", path: $sslCertPath)
                            TPFileButton(title: "CA Cert...", path: $sslCACertPath)

                            Button {
                                sslKeyPath = ""
                                sslCertPath = ""
                                sslCACertPath = ""
                            } label: {
                                Image(systemName: "minus")
                                    .font(.system(size: 11, weight: .medium))
                                    .frame(width: 24, height: 24)
                            }
                        }
                    }
                    } // end if !redis

                    // SSH section (expanded)
                    if sshEnabled {
                        Divider()
                            .padding(.vertical, 2)

                        // SSH Host + Port
                        HStack(spacing: 0) {
                            TPRow(label: "SSH Host", labelWidth: labelWidth) {
                                TPTextField(placeholder: "ssh.example.com", text: $sshHost)
                            }

                            HStack(spacing: 8) {
                                Text("Port")
                                    .font(.system(size: 13))
                                    .foregroundStyle(.secondary)
                                TPTextField(placeholder: "22", text: $sshPort)
                                    .frame(width: 72)
                            }
                        }

                        // SSH User
                        TPRow(label: "SSH User", labelWidth: labelWidth) {
                            HStack(spacing: 0) {
                                TPTextField(placeholder: "username", text: $sshUsername)
                                Spacer().frame(width: 12)
                                Picker("", selection: $sshAuthMethod) {
                                    Text("Password").tag(SSHAuthMethod.password)
                                    Text("Private Key").tag(SSHAuthMethod.privateKey)
                                    Text("Key + Passphrase").tag(SSHAuthMethod.keyWithPassphrase)
                                }
                                .labelsHidden()
                                .pickerStyle(.menu)
                                .frame(width: 150)
                            }
                        }

                        // SSH Password
                        if sshAuthMethod == .password || sshAuthMethod == .keyWithPassphrase {
                            TPRow(label: "SSH Password", labelWidth: labelWidth) {
                                TPTextField(placeholder: "password", text: $sshPassword, isSecure: true)
                            }
                        }

                        // SSH Key
                        if sshAuthMethod == .privateKey || sshAuthMethod == .keyWithPassphrase {
                            TPRow(label: "SSH Key", labelWidth: labelWidth) {
                                HStack(spacing: 8) {
                                    TPTextField(placeholder: "~/.ssh/id_rsa", text: $sshKeyPath)
                                    Button("Browse...") { pickSSHKey() }
                                }
                            }
                        }
                    }
                }
            }
            .padding(.horizontal, 24)
            .padding(.vertical, 20)

            // Test result
            if let result = testResult {
                HStack(spacing: 6) {
                    Image(systemName: result.success ? "checkmark.circle.fill" : "xmark.circle.fill")
                    Text(result.success
                         ? "Connected! \(result.serverVersion ?? "") (\(Int(result.latency * 1000))ms)"
                         : result.errorMessage ?? "Connection failed")
                        .font(.system(size: 12))
                    Spacer()
                }
                .foregroundColor(result.success ? .green : .red)
                .padding(.horizontal, 24)
                .padding(.bottom, 10)
            }

            Divider()

            // Bottom bar — Gridex style
            HStack(spacing: 8) {
                if databaseType != .sqlite {
                    Button {
                        withAnimation(.spring(response: 0.25, dampingFraction: 0.9)) {
                            sshEnabled.toggle()
                        }
                    } label: {
                        Text(sshEnabled ? "Close SSH" : "Over SSH")
                            .font(.system(size: 13))
                    }
                }

                Spacer()

                Button("Save") { save() }
                    .disabled(name.isEmpty)

                Button {
                    Task { await testConnection() }
                } label: {
                    if isTesting {
                        ProgressView()
                            .controlSize(.small)
                            .scaleEffect(0.7)
                            .frame(width: 40)
                    } else {
                        Text("Test")
                    }
                }
                .disabled(isTesting || !isFormValid)

                Button("Connect") { connectAction() }
                    .buttonStyle(.borderedProminent)
                    .disabled(!isFormValid)
                    .keyboardShortcut(.defaultAction)
            }
            .padding(.horizontal, 24)
            .padding(.vertical, 12)
        }
        .frame(width: 580)
        .onAppear {
            if let config = existingConfig {
                populateFromConfig(config)
                password = existingPassword
                sshPassword = existingSSHPassword
            } else {
                port = String(databaseType.defaultPort)
            }
        }
    }

    // MARK: - Populate from existing config (edit mode)

    private func populateFromConfig(_ config: ConnectionConfig) {
        name = config.name
        host = config.host ?? "localhost"
        port = String(config.port ?? databaseType.defaultPort)
        database = config.database ?? ""
        username = config.username ?? ""
        sslMode = config.sslEnabled ? .required : .preferred
        colorTag = config.colorTag ?? .blue
        sqliteFilePath = config.filePath ?? ""

        // SSL certificates (for mTLS)
        sslKeyPath = config.sslKeyPath ?? ""
        sslCertPath = config.sslCertPath ?? ""
        sslCACertPath = config.sslCACertPath ?? ""

        if let ssh = config.sshConfig {
            sshEnabled = true
            sshHost = ssh.host
            sshPort = String(ssh.port)
            sshUsername = ssh.username
            sshAuthMethod = ssh.authMethod
            sshKeyPath = ssh.keyPath ?? ""
        }
    }

    // MARK: - Validation

    private var isFormValid: Bool {
        if databaseType == .sqlite {
            return !sqliteFilePath.isEmpty
        }
        return !host.isEmpty
    }

    // MARK: - Actions

    private func buildConfig() -> ConnectionConfig {
        var sshConfig: SSHTunnelConfig?
        if sshEnabled && databaseType != .sqlite {
            sshConfig = SSHTunnelConfig(
                host: sshHost,
                port: Int(sshPort) ?? 22,
                username: sshUsername,
                authMethod: sshAuthMethod,
                keyPath: sshAuthMethod != .password ? sshKeyPath : nil
            )
        }

        let sslEnabled = sslMode != .disabled

        let isFileDB = databaseType == .sqlite

        // For MongoDB: if a connection string was provided, store it in host so the
        // adapter can pass it through to MongoKitten unchanged (preserves SRV lookup,
        // replica set hosts, query options, etc.)
        let isMongoURI = databaseType == .mongodb
            && (connectionString.hasPrefix("mongodb://") || connectionString.hasPrefix("mongodb+srv://"))
        let effectiveHost: String? = {
            if isFileDB { return nil }
            if isMongoURI { return connectionString.trimmingCharacters(in: .whitespacesAndNewlines) }
            return host
        }()

        return ConnectionConfig(
            id: existingConfig?.id ?? UUID(),
            name: name.isEmpty ? (isFileDB ? "SQLite" : host) : name,
            databaseType: databaseType,
            host: effectiveHost,
            port: isFileDB ? nil : (Int(port) ?? databaseType.defaultPort),
            database: isFileDB ? nil : (database.isEmpty ? nil : database),
            username: (isFileDB || databaseType == .redis) ? nil : username,
            sslEnabled: isFileDB ? false : sslEnabled,
            colorTag: colorTag,
            group: existingConfig?.group,
            sslKeyPath: sslKeyPath.isEmpty ? nil : sslKeyPath,
            sslCertPath: sslCertPath.isEmpty ? nil : sslCertPath,
            sslCACertPath: sslCACertPath.isEmpty ? nil : sslCACertPath,
            filePath: databaseType == .sqlite ? sqliteFilePath : nil,
            sshConfig: sshConfig
        )
    }

    private func connectAction() {
        onConnect?(buildConfig(), password, sshPassword)
    }

    private func save() {
        onSave?(buildConfig(), password, sshPassword)
    }

    private func testConnection() async {
        isTesting = true
        testResult = nil
        defer { isTesting = false }
        testResult = await onTest?(buildConfig(), password, sshPassword) ?? ConnectionTestResult(
            success: false, serverVersion: nil, latency: 0, errorMessage: "No handler"
        )
    }

    /// Parse a MongoDB connection string and populate the form fields.
    /// Supports both `mongodb://` and `mongodb+srv://` formats.
    private func parseMongoConnectionString() {
        connStringError = nil
        let trimmed = connectionString.trimmingCharacters(in: .whitespacesAndNewlines)

        guard trimmed.hasPrefix("mongodb://") || trimmed.hasPrefix("mongodb+srv://") else {
            connStringError = "URI must start with mongodb:// or mongodb+srv://"
            return
        }

        let isSrv = trimmed.hasPrefix("mongodb+srv://")
        let prefixLength = isSrv ? "mongodb+srv://".count : "mongodb://".count
        var rest = String(trimmed.dropFirst(prefixLength))

        // Strip query string and parse for tls/ssl flag
        var hasTLS = isSrv  // SRV implies TLS by default
        if let qIdx = rest.firstIndex(of: "?") {
            let query = String(rest[rest.index(after: qIdx)...])
            rest = String(rest[..<qIdx])
            for param in query.split(separator: "&") {
                let parts = param.split(separator: "=", maxSplits: 1)
                if parts.count == 2 {
                    let key = String(parts[0]).lowercased()
                    let value = String(parts[1]).lowercased()
                    if (key == "tls" || key == "ssl") && (value == "true" || value == "1") {
                        hasTLS = true
                    }
                }
            }
        }

        // Extract database (path component after host)
        var dbName: String?
        if let slashIdx = rest.firstIndex(of: "/") {
            let dbPart = String(rest[rest.index(after: slashIdx)...])
            if !dbPart.isEmpty { dbName = dbPart }
            rest = String(rest[..<slashIdx])
        }

        // Extract user:password (before @)
        var user: String?
        var pass: String?
        if let atIdx = rest.lastIndex(of: "@") {
            let credPart = String(rest[..<atIdx])
            rest = String(rest[rest.index(after: atIdx)...])
            if let colonIdx = credPart.firstIndex(of: ":") {
                user = String(credPart[..<colonIdx]).removingPercentEncoding ?? String(credPart[..<colonIdx])
                pass = String(credPart[credPart.index(after: colonIdx)...]).removingPercentEncoding ?? String(credPart[credPart.index(after: colonIdx)...])
            } else {
                user = credPart.removingPercentEncoding ?? credPart
            }
        }

        // Extract host:port (rest may contain comma-separated hosts for replica sets — take first)
        let firstHost = rest.split(separator: ",").first.map(String.init) ?? rest
        var hostName = firstHost
        var portNum: String?
        if let colonIdx = firstHost.firstIndex(of: ":") {
            hostName = String(firstHost[..<colonIdx])
            portNum = String(firstHost[firstHost.index(after: colonIdx)...])
        }

        guard !hostName.isEmpty else {
            connStringError = "Could not parse host from URI"
            return
        }

        // Apply parsed values to form
        host = hostName
        port = portNum ?? (isSrv ? "" : "27017")
        if let dbName = dbName { database = dbName }
        if let user = user { username = user }
        if let pass = pass { password = pass }
        if hasTLS {
            sslMode = .required
        }
        if name.isEmpty {
            name = hostName
        }
    }

    private func pickSQLiteFile() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.database, .data]
        panel.allowsOtherFileTypes = true
        panel.canChooseFiles = true
        panel.canChooseDirectories = false
        panel.message = "Select a SQLite database file"
        panel.prompt = "Open"
        if panel.runModal() == .OK, let url = panel.url {
            sqliteFilePath = url.path
            if name.isEmpty {
                name = url.deletingPathExtension().lastPathComponent
            }
        }
    }

    private func pickSSHKey() {
        let panel = NSOpenPanel()
        panel.canChooseFiles = true
        panel.canChooseDirectories = false
        panel.allowsMultipleSelection = false
        panel.directoryURL = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent(".ssh")
        panel.message = "Select SSH private key"
        panel.prompt = "Select"
        if panel.runModal() == .OK, let url = panel.url {
            sshKeyPath = url.path
        }
    }
}

// MARK: - Gridex-style Row (label left, content right)

struct TPRow<Content: View>: View {
    let label: String
    var labelWidth: CGFloat = 110
    @ViewBuilder let content: Content

    var body: some View {
        HStack(spacing: 12) {
            Text(label)
                .font(.system(size: 13))
                .foregroundStyle(.secondary)
                .frame(width: labelWidth, alignment: .trailing)
            content
        }
    }
}

// MARK: - Gridex-style flat button

struct TPButton: View {
    let title: String
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Text(title)
                .font(.system(size: 12))
                .frame(maxWidth: .infinity)
        }
    }
}

// MARK: - No-flicker TextField (no focus ring animation)

struct TPTextField: NSViewRepresentable {
    var placeholder: String
    @Binding var text: String
    var isSecure: Bool = false

    func makeNSView(context: Context) -> NSTextField {
        let field = isSecure ? NSSecureTextField() : NSTextField()
        field.placeholderString = placeholder
        field.focusRingType = .none
        field.bezelStyle = .roundedBezel
        field.font = .systemFont(ofSize: 13)
        field.delegate = context.coordinator
        return field
    }

    func updateNSView(_ nsView: NSTextField, context: Context) {
        if nsView.stringValue != text {
            nsView.stringValue = text
        }
    }

    func makeCoordinator() -> Coordinator { Coordinator(text: $text) }

    class Coordinator: NSObject, NSTextFieldDelegate {
        @Binding var text: String
        init(text: Binding<String>) { _text = text }
        func controlTextDidChange(_ obj: Notification) {
            if let field = obj.object as? NSTextField {
                text = field.stringValue
            }
        }
    }
}

// MARK: - Gridex-style file picker button

struct TPFileButton: View {
    let title: String
    @Binding var path: String

    var body: some View {
        Button {
            let panel = NSOpenPanel()
            panel.canChooseFiles = true
            panel.canChooseDirectories = false
            panel.allowsMultipleSelection = false
            panel.message = "Select \(title.replacingOccurrences(of: "...", with: "")) file"
            panel.prompt = "Select"
            if panel.runModal() == .OK, let url = panel.url {
                path = url.path
            }
        } label: {
            Text(path.isEmpty ? title : URL(fileURLWithPath: path).lastPathComponent)
                .font(.system(size: 12))
                .lineLimit(1)
                .frame(maxWidth: .infinity)
        }
    }
}
