// MCPAdvancedView.swift
// Gridex
//
// Advanced MCP server settings.

import SwiftUI

struct MCPAdvancedView: View {
    // Rate limiting
    @AppStorage("mcp.rateLimit.queriesPerMinute") private var queriesPerMinute = 60
    @AppStorage("mcp.rateLimit.queriesPerHour") private var queriesPerHour = 1000
    @AppStorage("mcp.rateLimit.writesPerMinute") private var writesPerMinute = 10
    @AppStorage("mcp.rateLimit.ddlPerMinute") private var ddlPerMinute = 1

    // Timeouts
    @AppStorage("mcp.timeout.query") private var queryTimeout = 30
    @AppStorage("mcp.timeout.approval") private var approvalTimeout = 60
    @AppStorage("mcp.timeout.connection") private var connectionTimeout = 10

    // Audit log
    @AppStorage("mcp.audit.retentionDays") private var retentionDays = 90
    @AppStorage("mcp.audit.maxSizeMB") private var maxSizeMB = 100

    // Security
    @AppStorage("mcp.security.requireApprovalForWrites") private var requireApprovalForWrites = true
    @AppStorage("mcp.security.allowRemoteHTTP") private var allowRemoteHTTP = false

    @State private var showResetConfirmation = false

    var body: some View {
        Form {
            Section("Rate Limits") {
                stepper("Queries per minute", value: $queriesPerMinute, in: 10...200, step: 10)
                stepper("Queries per hour", value: $queriesPerHour, in: 100...5000, step: 100)
                stepper("Writes per minute", value: $writesPerMinute, in: 1...50, step: 1)
                stepper("DDL per minute", value: $ddlPerMinute, in: 1...10, step: 1)
            }

            Section("Timeouts") {
                stepper("Query timeout", value: $queryTimeout, in: 5...300, step: 5, suffix: "s")
                stepper("Approval timeout", value: $approvalTimeout, in: 10...300, step: 10, suffix: "s")
                stepper("Connection timeout", value: $connectionTimeout, in: 5...60, step: 5, suffix: "s")
            }

            Section("Audit Log") {
                LabeledContent("Retention") {
                    Picker("", selection: $retentionDays) {
                        Text("7 days").tag(7)
                        Text("30 days").tag(30)
                        Text("90 days").tag(90)
                        Text("1 year").tag(365)
                        Text("Forever").tag(0)
                    }
                    .labelsHidden()
                    .frame(width: 130)
                }

                stepper("Max log size", value: $maxSizeMB, in: 10...500, step: 10, suffix: "MB")

                LabeledContent("Location") {
                    HStack(spacing: 6) {
                        Text("~/Library/Application Support/Gridex/")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            .textSelection(.enabled)
                            .lineLimit(1)
                            .truncationMode(.middle)

                        Button {
                            openLogFolder()
                        } label: {
                            Image(systemName: "folder")
                        }
                        .buttonStyle(.borderless)
                        .help("Open in Finder")
                    }
                }
            }

            Section("Security") {
                Toggle("Require approval for write operations", isOn: $requireApprovalForWrites)

                Toggle("Allow remote HTTP connections", isOn: $allowRemoteHTTP)

                if allowRemoteHTTP {
                    HStack(alignment: .top, spacing: 8) {
                        Image(systemName: "exclamationmark.triangle.fill")
                            .foregroundStyle(.orange)
                            .font(.caption)
                        Text("Remote HTTP allows connections from other machines. Use only on trusted networks.")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }
            }

            Section {
                HStack {
                    Spacer()
                    Button("Reset to Defaults…") {
                        showResetConfirmation = true
                    }
                }
            }
        }
        .formStyle(.grouped)
        .alert("Reset All Settings?", isPresented: $showResetConfirmation) {
            Button("Cancel", role: .cancel) {}
            Button("Reset", role: .destructive) { resetToDefaults() }
        } message: {
            Text("This will restore all MCP settings to their default values.")
        }
    }

    private func stepper(_ title: String, value: Binding<Int>, in range: ClosedRange<Int>, step: Int, suffix: String = "") -> some View {
        LabeledContent(title) {
            HStack(spacing: 4) {
                Text("\(value.wrappedValue)\(suffix)")
                    .font(.system(.body, design: .monospaced))
                    .foregroundStyle(.secondary)
                    .monospacedDigit()
                    .frame(minWidth: 60, alignment: .trailing)

                Stepper("", value: value, in: range, step: step)
                    .labelsHidden()
            }
        }
    }

    private func openLogFolder() {
        let appSupport = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first!
        let gridexDir = appSupport.appendingPathComponent("Gridex")
        NSWorkspace.shared.open(gridexDir)
    }

    private func resetToDefaults() {
        queriesPerMinute = 60
        queriesPerHour = 1000
        writesPerMinute = 10
        ddlPerMinute = 1
        queryTimeout = 30
        approvalTimeout = 60
        connectionTimeout = 10
        retentionDays = 90
        maxSizeMB = 100
        requireApprovalForWrites = true
        allowRemoteHTTP = false
    }
}
