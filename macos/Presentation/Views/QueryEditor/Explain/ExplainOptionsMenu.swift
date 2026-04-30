// ExplainOptionsMenu.swift
// Gridex
//
// pgAdmin4-style dropdown attached to the Explain button. Surfaces every PG
// EXPLAIN option as a toggle, with cross-disable + version gating already
// enforced in the model (`ExplainOptions.canEnable` / `isAvailable`).
//
// Visible only for PostgreSQL connections — the other engines have no
// option list. The dropdown vanishes for them; the plain Explain button
// remains.

import SwiftUI

struct ExplainOptionsMenu: View {
    @Binding var options: ExplainOptions

    /// PG server major version, when known (`SHOW server_version_num` / 10000).
    /// Pass nil to permit every option (server will reject if it can't handle).
    let serverMajorVersion: Int?

    var body: some View {
        Menu {
            // Quick preset — sets the four options a senior dev would tick
            // before debugging a slow SELECT (Analyze, Buffers, Verbose,
            // Summary). Keeps current Format so users on Tree view don't get
            // bumped back to Text. The "(SELECT)" suffix is a load-bearing
            // hint that this turns on ANALYZE → query is actually executed.
            Button("Apply ‘Profile’ preset (SELECT)") {
                options = ExplainOptions.profilePreset(currentFormat: options.format)
            }

            Divider()

            // ANALYZE — toggle prominently at the top. It's the load-bearing
            // setting (off = plan only / safe; on = execute query / measure).
            Toggle("Analyze (execute query)", isOn: $options.analyze)

            Divider()

            // Plain bool options, alphabetised after Analyze for predictability.
            Toggle("Buffers", isOn: $options.buffers)
            Toggle("Costs",   isOn: $options.costs)
            versionGatedToggle("Generic Plan", $options.genericPlan, gate: .genericPlan)
            versionGatedToggle("Memory",       $options.memory,      gate: .memory)
            Toggle("Settings", isOn: $options.settings)
                .disabled(!ExplainOptions.isAvailable(.settings, on: serverMajorVersion))
            Toggle("Summary", isOn: $options.summary)
            Toggle("Timing",  isOn: $options.timing)
                .disabled(!options.canEnable(.timing))
            Toggle("Verbose", isOn: $options.verbose)
            Toggle("WAL", isOn: $options.wal)
                .disabled(!options.canEnable(.wal) || !ExplainOptions.isAvailable(.wal, on: serverMajorVersion))

            Divider()

            // Serialize — submenu like pgAdmin4. Off-state renders as a plain
            // disabled item; the values appear when ANALYZE is on.
            Menu("Serialize") {
                ForEach(ExplainOptions.Serialize.allCases) { value in
                    Button {
                        options.serialize = value
                    } label: {
                        if options.serialize == value {
                            Label(value.displayName, systemImage: "checkmark")
                        } else {
                            Text(value.displayName)
                        }
                    }
                }
            }
            .disabled(!options.canEnable(.serialize) || !ExplainOptions.isAvailable(.serialize, on: serverMajorVersion))

            // Format — submenu. JSON unlocks visual-tree rendering downstream.
            Menu("Format") {
                ForEach(ExplainOptions.Format.allCases) { value in
                    Button {
                        options.format = value
                    } label: {
                        if options.format == value {
                            Label(value.displayName, systemImage: "checkmark")
                        } else {
                            Text(value.displayName)
                        }
                    }
                }
            }
        } label: {
            // Just a chevron next to the Explain button — caller draws the
            // primary "Explain" button itself, this is the disclosure.
            Image(systemName: "chevron.down")
                .font(.system(size: 9, weight: .semibold))
                .foregroundStyle(.secondary)
        }
        .menuStyle(.borderlessButton)
        .menuIndicator(.hidden)
        .controlSize(.small)
        .fixedSize()
        .help("EXPLAIN options")
    }

    @ViewBuilder
    private func versionGatedToggle(
        _ title: String,
        _ binding: Binding<Bool>,
        gate: ExplainOptions.VersionGated
    ) -> some View {
        let available = ExplainOptions.isAvailable(gate, on: serverMajorVersion)
        Toggle(available ? title : "\(title) (PG \(gate.minPostgresMajor)+)", isOn: binding)
            .disabled(!available)
    }
}
