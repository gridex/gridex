// RedisAddKeySheet.swift
// Gridex
//
// Sheet for creating a new Redis key with type selection.

import SwiftUI

struct RedisAddKeySheet: View {
    @EnvironmentObject private var appState: AppState
    @Environment(\.dismiss) private var dismiss

    let redisContext: AppState.RedisTabContext

    @State private var keyName = ""
    @State private var keyType: RedisKeyType = .string
    @State private var ttlString = ""
    @State private var errorMessage: String?
    @State private var isCreating = false

    // String
    @State private var stringValue = ""
    // Hash
    @State private var hashFields: [(field: String, value: String)] = [("", "")]
    // List
    @State private var listItems: [String] = [""]
    // Set
    @State private var setMembers: [String] = [""]
    // ZSet
    @State private var zsetMembers: [(member: String, score: String)] = [("", "0")]

    var body: some View {
        VStack(spacing: 0) {
            Text("Add Key")
                .font(.system(size: 14, weight: .semibold))
                .padding(.top, 16)

            Form {
                TextField("Key", text: $keyName)

                Picker("Type", selection: $keyType) {
                    ForEach(RedisKeyType.allCases, id: \.self) { t in
                        Text(t.rawValue.uppercased()).tag(t)
                    }
                }

                switch keyType {
                case .string:
                    TextField("Value", text: $stringValue)
                case .hash:
                    hashFieldsEditor
                case .list:
                    listItemsEditor
                case .set:
                    setMembersEditor
                case .zset:
                    zsetMembersEditor
                }

                TextField("TTL (seconds, optional)", text: $ttlString)
            }
            .formStyle(.grouped)
            .scrollContentBackground(.hidden)

            if let err = errorMessage {
                Text(err).foregroundStyle(.red).font(.system(size: 11)).padding(.horizontal)
            }

            HStack {
                Button("Cancel") { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Spacer()
                Button("Create") { createKey() }
                    .keyboardShortcut(.defaultAction)
                    .disabled(keyName.isEmpty || isCreating)
            }
            .padding()
        }
        .frame(width: 420)
    }

    // MARK: - Editors

    private var hashFieldsEditor: some View {
        Section("Fields") {
            ForEach(hashFields.indices, id: \.self) { i in
                HStack {
                    TextField("field", text: Binding(get: { hashFields[i].field }, set: { hashFields[i].field = $0 }))
                    TextField("value", text: Binding(get: { hashFields[i].value }, set: { hashFields[i].value = $0 }))
                    removeButton { hashFields.remove(at: i) }
                }
            }
            addButton { hashFields.append(("", "")) }
        }
    }

    private var listItemsEditor: some View {
        Section("Items") {
            ForEach(listItems.indices, id: \.self) { i in
                HStack {
                    TextField("item \(i)", text: Binding(get: { listItems[i] }, set: { listItems[i] = $0 }))
                    removeButton { listItems.remove(at: i) }
                }
            }
            addButton { listItems.append("") }
        }
    }

    private var setMembersEditor: some View {
        Section("Members") {
            ForEach(setMembers.indices, id: \.self) { i in
                HStack {
                    TextField("member", text: Binding(get: { setMembers[i] }, set: { setMembers[i] = $0 }))
                    removeButton { setMembers.remove(at: i) }
                }
            }
            addButton { setMembers.append("") }
        }
    }

    private var zsetMembersEditor: some View {
        Section("Members") {
            ForEach(zsetMembers.indices, id: \.self) { i in
                HStack {
                    TextField("member", text: Binding(get: { zsetMembers[i].member }, set: { zsetMembers[i].member = $0 }))
                    TextField("score", text: Binding(get: { zsetMembers[i].score }, set: { zsetMembers[i].score = $0 }))
                        .frame(width: 80)
                    removeButton { zsetMembers.remove(at: i) }
                }
            }
            addButton { zsetMembers.append(("", "0")) }
        }
    }

    private func addButton(action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Label("Add", systemImage: "plus.circle")
                .font(.system(size: 12))
        }.buttonStyle(.plain)
    }

    private func removeButton(action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: "minus.circle").foregroundStyle(.red)
        }.buttonStyle(.plain)
    }

    // MARK: - Create

    private func createKey() {
        isCreating = true
        errorMessage = nil

        let data: RedisKeyData
        switch keyType {
        case .string: data = .string(value: stringValue)
        case .hash: data = .hash(fields: hashFields.filter { !$0.field.isEmpty })
        case .list: data = .list(items: listItems.filter { !$0.isEmpty })
        case .set: data = .set(members: setMembers.filter { !$0.isEmpty })
        case .zset: data = .zset(members: zsetMembers.compactMap {
            guard !$0.member.isEmpty else { return nil }
            return (member: $0.member, score: Double($0.score) ?? 0)
        })
        }

        let ttl = Int(ttlString)
        Task {
            let result = await appState.performRedisOperation(for: redisContext) { redis in
                try await redis.redisInsertTyped(
                    key: keyName,
                    type: keyType,
                    data: data,
                    ttl: ttl
                )
            }
            switch result {
            case .success?:
                appState.requestRedisKeyBrowserRefresh()
                dismiss()
                NotificationCenter.default.post(name: .reloadData, object: nil)
            case .failure(let error)?:
                errorMessage = error.localizedDescription
                isCreating = false
            case nil:
                errorMessage = "The Redis connection or database changed. Reopen Add Key and try again."
                isCreating = false
            }
        }
    }
}
