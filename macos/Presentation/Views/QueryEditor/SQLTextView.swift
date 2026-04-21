// SQLTextView.swift
// Gridex
//
// Custom NSTextView subclass for SQL editing with autocomplete support.

import AppKit

final class SQLTextView: NSTextView {

    /// Bridge to the SwiftUI coordinator that manages autocomplete.
    weak var completionCoordinator: SQLEditorView.Coordinator?

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        commonInit()
    }

    override init(frame frameRect: NSRect, textContainer container: NSTextContainer?) {
        super.init(frame: frameRect, textContainer: container)
        commonInit()
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        commonInit()
    }

    private func commonInit() {
        font = NSFont.monospacedSystemFont(ofSize: 14, weight: .regular)
        textContainerInset = NSSize(width: 12, height: 8)
        disableAutoSubstitutions()
    }

    override func keyDown(with event: NSEvent) {
        let flags = event.modifierFlags.intersection(.deviceIndependentFlagsMask)

        // Cmd+Shift+R: Execute query
        if flags == [.command, .shift] && event.charactersIgnoringModifiers?.lowercased() == "r" {
            NotificationCenter.default.post(name: .executeQuery, object: self)
            return
        }

        // Let completion coordinator handle arrow keys, enter, tab, escape
        if let coordinator = completionCoordinator, coordinator.handleKeyForCompletion(event) {
            return
        }

        // Ctrl+Space or Cmd+.: Manual trigger autocomplete
        if (flags == .control && event.charactersIgnoringModifiers == " ") ||
           (flags == .command && event.charactersIgnoringModifiers == ".") {
            super.keyDown(with: event)
            completionCoordinator?.triggerCompletionNow()
            return
        }

        super.keyDown(with: event)
    }
}
