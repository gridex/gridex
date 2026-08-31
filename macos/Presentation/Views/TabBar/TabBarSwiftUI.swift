// TabBarSwiftUI.swift
// Gridex
//
// SwiftUI tab bar for content area. Supports Chrome-style tab groups
// when multiple databases are open within a single connection.

import SwiftUI

struct TabBarSwiftUIView: View {
    @EnvironmentObject private var appState: AppState

    var body: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 0) {
                ForEach(Array(appState.groupedTabs.enumerated()), id: \.offset) { _, section in
                    // Group header (only in multi-database mode)
                    if let group = section.group {
                        TabGroupHeaderView(group: group)
                    }

                    // Tab items (hidden when group is collapsed)
                    if section.group?.isCollapsed != true {
                        ForEach(section.tabs) { tab in
                            TabItemView(tab: tab, isActive: tab.id == appState.activeTabId)
                        }
                    }
                }

                // [+] new tab button
                Button(action: { appState.openNewQueryTab() }) {
                    Image(systemName: "plus")
                        .font(.system(size: 12))
                        .foregroundStyle(.secondary)
                        .frame(width: 36, height: 38)
                }
                .buttonStyle(.plain)
                .pointerCursor()
                .help("New Query Tab")

                Spacer()
            }
        }
        .frame(height: 38)
        .background(Color(nsColor: .windowBackgroundColor))
        .overlay(alignment: .bottom) { Divider() }
    }
}

// MARK: - Tab Group Header

struct TabGroupHeaderView: View {
    let group: AppState.TabGroup
    @EnvironmentObject private var appState: AppState
    @State private var isHovering = false
    @State private var isRenaming = false
    @State private var renameText = ""

    private var groupColor: Color {
        Color(nsColor: group.color.nsColor)
    }

    private var tabCount: Int {
        appState.tabs.filter { $0.databaseName == group.id }.count
    }

    var body: some View {
        HStack(spacing: 4) {
            Image(systemName: group.isCollapsed ? "chevron.right" : "chevron.down")
                .font(.system(size: 8, weight: .semibold))

            if isRenaming {
                TextField("", text: $renameText, onCommit: {
                    let trimmed = renameText.trimmingCharacters(in: .whitespaces)
                    if !trimmed.isEmpty {
                        appState.renameGroup(group.id, newName: trimmed)
                    }
                    isRenaming = false
                })
                .textFieldStyle(.plain)
                .font(.system(size: 11, weight: .medium))
                .frame(width: 80)
                .onAppear { renameText = group.label }
                .onExitCommand { isRenaming = false }
            } else {
                Text(group.label)
                    .font(.system(size: 11, weight: .medium))
                    .lineLimit(1)
            }

            if group.isCollapsed {
                Text("\(tabCount)")
                    .font(.system(size: 9))
                    .padding(.horizontal, 4)
                    .padding(.vertical, 1)
                    .background(groupColor.opacity(0.3))
                    .clipShape(Capsule())
            }
        }
        .foregroundColor(groupColor)
        .padding(.horizontal, 10)
        .frame(height: 38)
        .background(groupColor.opacity(isHovering ? 0.12 : 0.07))
        .contentShape(Rectangle())
        .pointerCursor()
        .onTapGesture { appState.toggleGroupCollapsed(group.id) }
        .onHover { isHovering = $0 }
        .overlay(alignment: .bottom) {
            Rectangle()
                .fill(groupColor)
                .frame(height: 2)
        }
        .contextMenu {
            Button("Close Group") {
                appState.closeGroup(group.id)
            }

            Divider()

            Button("Rename Group") {
                isRenaming = true
            }

            Menu("Group Color") {
                ForEach(ColorTag.allCases, id: \.self) { color in
                    Button {
                        appState.changeGroupColor(group.id, color: color)
                    } label: {
                        Label(color.environmentHint, systemImage: "circle.fill")
                    }
                }
            }
        }
    }
}

// MARK: - Tab Item

struct TabItemView: View {
    let tab: AppState.ContentTab
    let isActive: Bool

    @EnvironmentObject private var appState: AppState
    @State private var isHovering = false
    @State private var isRenaming = false
    @State private var renameText = ""

    /// Use the group color for the active tab accent when in multi-database mode
    private var accentColor: Color {
        if let dbName = tab.databaseName,
           let group = appState.tabGroups[dbName] {
            return Color(nsColor: group.color.nsColor)
        }
        return Color.accentColor
    }

    var body: some View {
        HStack(spacing: 7) {
            if isRenaming {
                Image(systemName: tabIcon)
                    .font(.system(size: 11))

                TextField("", text: $renameText, onCommit: {
                    let trimmed = renameText.trimmingCharacters(in: .whitespaces)
                    if !trimmed.isEmpty {
                        appState.renameTab(id: tab.id, newTitle: trimmed)
                    }
                    isRenaming = false
                })
                .textFieldStyle(.plain)
                .font(.system(size: 12))
                .frame(width: 100)
                .onAppear { renameText = tab.title }
                .onExitCommand { isRenaming = false }
            } else {
                Button(action: { appState.selectTab(id: tab.id) }) {
                    HStack(spacing: 7) {
                        Image(systemName: tabIcon)
                            .font(.system(size: 11))

                        Text(tab.title)
                            .font(.system(size: 12))
                            .lineLimit(1)
                    }
                }
                .buttonStyle(.plain)
            }

            Button(action: { appState.closeTab(id: tab.id) }) {
                Image(systemName: "xmark")
                    .font(.system(size: 8, weight: .bold))
                    .foregroundStyle(.secondary)
            }
            .buttonStyle(.plain)
            .opacity(isHovering || isActive ? 1 : 0)
        }
        .padding(.horizontal, 14)
        .frame(height: 38)
        .contentShape(Rectangle())
        .background(isActive ? Color(nsColor: .controlBackgroundColor) : .clear)
        .overlay(alignment: .bottom) {
            if isActive {
                Rectangle()
                    .fill(accentColor)
                    .frame(height: 2)
            }
        }
        .overlay(alignment: .trailing) {
            Divider().frame(height: 18)
        }
        .pointerCursor()
        .overlay(MiddleClickOverlay { appState.closeTab(id: tab.id) })
        .onHover { isHovering = $0 }
        .contextMenu {
            Button("New Tab") {
                appState.openNewQueryTab()
            }

            Divider()

            Button("Close Tab") {
                appState.closeTab(id: tab.id)
            }

            Button("Close Other Tabs") {
                appState.closeOtherTabs(except: tab.id)
            }
            .disabled(appState.tabs.count <= 1)

            Button("Close Tabs to the Right") {
                appState.closeTabsToTheRight(of: tab.id)
            }
            .disabled(appState.tabs.last?.id == tab.id)

            Divider()

            Button("Close All Tabs") {
                appState.closeAllTabs()
            }

            Divider()

            Button("Rename Tab") {
                isRenaming = true
            }
        }
    }

    private var tabIcon: String {
        switch tab.type {
        case .dataGrid: return "tablecells"
        case .queryEditor: return "doc.text"
        case .tableStructure: return "list.bullet.rectangle"
        case .tableList: return "tablecells.badge.ellipsis"
        case .functionDetail: return "function"
        case .createTable: return "plus.rectangle"
        case .erDiagram: return "point.3.connected.trianglepath.dotted"
        case .redisKeyDetail: return "key.fill"
        case .redisServerInfo: return "chart.bar"
        case .redisSlowLog: return "tortoise"
        }
    }
}

// MARK: - Middle Click (scroll wheel button) to close tab

struct MiddleClickOverlay: NSViewRepresentable {
    let action: () -> Void

    func makeNSView(context: Context) -> MiddleClickNSView {
        let view = MiddleClickNSView()
        view.action = action
        return view
    }

    func updateNSView(_ nsView: MiddleClickNSView, context: Context) {
        nsView.action = action
    }
}

class MiddleClickNSView: NSView {
    var action: (() -> Void)?
    private var monitor: Any?

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        if window != nil, monitor == nil {
            monitor = NSEvent.addLocalMonitorForEvents(matching: .otherMouseDown) { [weak self] event in
                guard let self, event.buttonNumber == 2 else { return event }
                let locationInView = self.convert(event.locationInWindow, from: nil)
                return self.handleMiddleClick(at: locationInView) ? nil : event
            }
        }
    }

    @discardableResult
    func handleMiddleClick(at point: NSPoint) -> Bool {
        guard bounds.contains(point), let action else { return false }
        action()
        return true
    }

    override func removeFromSuperview() {
        if let monitor { NSEvent.removeMonitor(monitor) }
        monitor = nil
        super.removeFromSuperview()
    }

    override func viewDidMoveToSuperview() {
        super.viewDidMoveToSuperview()
        if superview == nil, let monitor {
            NSEvent.removeMonitor(monitor)
            self.monitor = nil
        }
    }

    // Completely transparent to all direct mouse events
    override func hitTest(_ point: NSPoint) -> NSView? {
        nil
    }
}
