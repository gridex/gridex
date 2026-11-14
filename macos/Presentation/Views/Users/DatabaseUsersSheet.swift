// DatabaseUsersSheet.swift
// Gridex
//
// Database → Manage Users…: list, create, re-password, drop users and apply access
// presets on the current database. Works with any adapter conforming to UserManageable.

import SwiftUI

struct DatabaseUsersSheet: View {
    @EnvironmentObject private var appState: AppState
    @Environment(\.dismiss) private var dismiss

    @State private var users: [DatabaseUser] = []
    @State private var selection: DatabaseUser.ID?
    @State private var errorMessage: String?
    @State private var showAddUser = false
    @State private var passwordTarget: DatabaseUser?
    @State private var accessTarget: DatabaseUser?
    @State private var dropTarget: DatabaseUser?

    private var manager: UserManageable? { appState.activeAdapter as? UserManageable }
    private var database: String { appState.currentDatabaseName ?? "" }
    private var usesHosts: Bool { appState.activeAdapter?.databaseType == .mysql }
    private var selectedUser: DatabaseUser? { users.first { $0.id == selection } }

    var body: some View {
        VStack(spacing: 0) {
            Text("Users")
                .font(.system(size: 14, weight: .semibold))
                .padding(.top, 16)
                .padding(.bottom, 12)

            List(users, selection: $selection) { user in
                HStack(spacing: 6) {
                    Image(systemName: user.canLogin ? "person" : "person.2")
                        .foregroundStyle(.secondary)
                        .frame(width: 16)
                    Text(user.name).font(.system(size: 12))
                    if let host = user.host {
                        Text("@\(host)").font(.system(size: 12)).foregroundStyle(.secondary)
                    }
                    Spacer()
                    if user.isSuperuser { badge("superuser") }
                    if !user.canLogin { badge("no login") }
                }
            }
            .frame(height: 280)

            if let errorMessage {
                Text(errorMessage)
                    .font(.system(size: 11))
                    .foregroundStyle(.red)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.horizontal, 16)
                    .padding(.top, 8)
            }

            Divider().padding(.top, 8)

            HStack(spacing: 8) {
                Button { showAddUser = true } label: { Image(systemName: "plus") }
                    .help("New user")
                Button { dropTarget = selectedUser } label: { Image(systemName: "minus") }
                    .help("Delete user")
                    .disabled(selectedUser == nil)
                Button("Change Password...") { passwordTarget = selectedUser }
                    .disabled(selectedUser == nil)
                Button("Set Access...") { accessTarget = selectedUser }
                    .disabled(selectedUser == nil || database.isEmpty)
                Spacer()
                Button("Done") { dismiss() }
                    .keyboardShortcut(.defaultAction)
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 12)
        }
        .frame(width: 500)
        .task { await reload() }
        .sheet(isPresented: $showAddUser) {
            AddDatabaseUserSheet(usesHosts: usesHosts, database: database) { name, host, password, access in
                guard let manager else { return }
                try await manager.createUser(name: name, host: host, password: password)
                if access != .none {
                    let user = DatabaseUser(name: name, host: host, canLogin: true, isSuperuser: false)
                    try await manager.setAccess(access, for: user, database: database)
                }
                await reload()
                selection = users.first { $0.name == name && $0.host == host }?.id
            }
        }
        .sheet(item: $passwordTarget) { user in
            ChangeUserPasswordSheet(user: user) { password in
                try await manager?.changePassword(for: user, to: password)
            }
        }
        .sheet(item: $accessTarget) { user in
            SetUserAccessSheet(user: user, database: database, isPostgres: !usesHosts) { level in
                try await manager?.setAccess(level, for: user, database: database)
            }
        }
        .confirmationDialog(
            "Delete user \"\(dropTarget?.id ?? "")\"?",
            isPresented: Binding(get: { dropTarget != nil }, set: { if !$0 { dropTarget = nil } }),
            presenting: dropTarget
        ) { user in
            Button("Delete", role: .destructive) { Task { await drop(user) } }
        } message: { _ in
            Text(usesHosts
                 ? "This permanently deletes the account and all its privileges."
                 : "This permanently deletes the role. Its privileges in \"\(database)\" are revoked first; objects it owns or privileges in other databases must be removed before it can be deleted.")
        }
    }

    private func badge(_ text: String) -> some View {
        Text(text)
            .font(.system(size: 10))
            .foregroundStyle(.secondary)
            .padding(.horizontal, 5)
            .padding(.vertical, 1)
            .background(Color.secondary.opacity(0.15), in: Capsule())
    }

    private func reload() async {
        guard let manager else { return }
        do {
            users = try await manager.listUsers()
            errorMessage = nil
        } catch {
            errorMessage = DataGridViewState.detailedErrorMessage(error)
        }
    }

    private func drop(_ user: DatabaseUser) async {
        do {
            try await manager?.dropUser(user)
            selection = nil
            await reload()
        } catch {
            errorMessage = DataGridViewState.detailedErrorMessage(error)
        }
    }
}

// MARK: - Sub-sheets

/// Title, form fields, inline error and Cancel/OK; runs `action` and dismisses on success.
private struct UserFormSheet<Fields: View>: View {
    let title: String
    let okTitle: String
    let canSubmit: Bool
    let action: () async throws -> Void
    @ViewBuilder let fields: Fields

    @Environment(\.dismiss) private var dismiss
    @State private var isWorking = false
    @State private var errorMessage: String?

    var body: some View {
        VStack(spacing: 0) {
            Text(title)
                .font(.system(size: 14, weight: .semibold))
                .padding(.top, 16)
                .padding(.bottom, 12)

            Grid(alignment: .trailing, horizontalSpacing: 10, verticalSpacing: 10) {
                fields
            }
            .padding(.horizontal, 20)

            if let errorMessage {
                Text(errorMessage)
                    .font(.system(size: 11))
                    .foregroundStyle(.red)
                    .fixedSize(horizontal: false, vertical: true)
                    .padding(.horizontal, 20)
                    .padding(.top, 8)
            }

            Divider().padding(.top, 16)

            HStack {
                Button("Cancel") { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Spacer()
                Button {
                    Task { await submit() }
                } label: {
                    if isWorking { ProgressView().controlSize(.small) }
                    Text(okTitle)
                }
                .keyboardShortcut(.defaultAction)
                .disabled(!canSubmit || isWorking)
            }
            .padding(.horizontal, 20)
            .padding(.vertical, 12)
        }
        .frame(width: 380)
    }

    private func submit() async {
        isWorking = true
        errorMessage = nil
        defer { isWorking = false }
        do {
            try await action()
            dismiss()
        } catch {
            errorMessage = DataGridViewState.detailedErrorMessage(error)
        }
    }
}

/// Password + confirmation rows shared by the create and change-password sheets.
private struct PasswordRows: View {
    @Binding var password: String
    @Binding var confirm: String

    var body: some View {
        GridRow {
            Text("Password:").font(.system(size: 12))
            SecureField("", text: $password).textFieldStyle(.roundedBorder).frame(width: 220)
        }
        GridRow {
            Text("Confirm:").font(.system(size: 12))
            SecureField("", text: $confirm).textFieldStyle(.roundedBorder).frame(width: 220)
        }
        if !confirm.isEmpty && confirm != password {
            GridRow {
                Color.clear.gridCellUnsizedAxes([.horizontal, .vertical])
                Text("Passwords don't match")
                    .font(.system(size: 11))
                    .foregroundStyle(.red)
                    .frame(width: 220, alignment: .leading)
            }
        }
    }
}

private struct AddDatabaseUserSheet: View {
    let usesHosts: Bool
    let database: String
    let onCreate: (String, String?, String, DatabaseAccessLevel) async throws -> Void

    @State private var name = ""
    @State private var host = "%"
    @State private var password = ""
    @State private var confirm = ""
    @State private var access: DatabaseAccessLevel = .none

    var body: some View {
        UserFormSheet(
            title: "New User",
            okTitle: "Create",
            canSubmit: !name.isEmpty && !password.isEmpty && password == confirm && (!usesHosts || !host.isEmpty),
            action: { try await onCreate(name, usesHosts ? host : nil, password, access) }
        ) {
            GridRow {
                Text("Name:").font(.system(size: 12))
                TextField("", text: $name).textFieldStyle(.roundedBorder).frame(width: 220)
            }
            if usesHosts {
                GridRow {
                    Text("Host:").font(.system(size: 12))
                    TextField("% = any host", text: $host).textFieldStyle(.roundedBorder).frame(width: 220)
                }
            }
            PasswordRows(password: $password, confirm: $confirm)
            if !database.isEmpty {
                GridRow {
                    Text("Access to \(database):").font(.system(size: 12))
                    Picker("", selection: $access) {
                        ForEach(DatabaseAccessLevel.allCases, id: \.self) { Text($0.displayName).tag($0) }
                    }
                    .labelsHidden()
                    .frame(width: 220)
                }
            }
        }
    }
}

private struct ChangeUserPasswordSheet: View {
    let user: DatabaseUser
    let onChange: (String) async throws -> Void

    @State private var password = ""
    @State private var confirm = ""

    var body: some View {
        UserFormSheet(
            title: "Change Password for \(user.id)",
            okTitle: "Change",
            canSubmit: !password.isEmpty && password == confirm,
            action: { try await onChange(password) }
        ) {
            PasswordRows(password: $password, confirm: $confirm)
        }
    }
}

private struct SetUserAccessSheet: View {
    let user: DatabaseUser
    let database: String
    let isPostgres: Bool
    let onApply: (DatabaseAccessLevel) async throws -> Void

    @State private var access: DatabaseAccessLevel = .readOnly

    var body: some View {
        UserFormSheet(
            title: "Access for \(user.id)",
            okTitle: "Apply",
            canSubmit: true,
            action: { try await onApply(access) }
        ) {
            GridRow {
                Text("Access to \(database):").font(.system(size: 12))
                Picker("", selection: $access) {
                    ForEach(DatabaseAccessLevel.allCases, id: \.self) { Text($0.displayName).tag($0) }
                }
                .labelsHidden()
                .frame(width: 220)
            }
            GridRow {
                Color.clear.gridCellUnsizedAxes([.horizontal, .vertical])
                Text(isPostgres
                     ? "Replaces the user's privileges on every schema you own in this database, including tables you create later."
                     : "Replaces the user's privileges on all tables in this database.")
                    .font(.system(size: 11))
                    .foregroundStyle(.secondary)
                    .frame(width: 220, alignment: .leading)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
    }
}
