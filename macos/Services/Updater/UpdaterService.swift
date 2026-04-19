// UpdaterService.swift
// Gridex
//
// Thin wrapper around Sparkle's SPUStandardUpdaterController.
// Reads SUFeedURL and SUPublicEDKey from Info.plist.
// Exposes @Published canCheckForUpdates so menu items can disable themselves
// while Sparkle is busy.

import Foundation
import SwiftUI
import Sparkle

@MainActor
final class UpdaterService: ObservableObject {
    static let shared = UpdaterService()

    private let controller: SPUStandardUpdaterController?

    @Published var canCheckForUpdates: Bool = false

    private init() {
        let c = SPUStandardUpdaterController(
            startingUpdater: true,
            updaterDelegate: nil,
            userDriverDelegate: nil
        )
        controller = c
        canCheckForUpdates = c.updater.canCheckForUpdates

        c.updater.publisher(for: \.canCheckForUpdates)
            .receive(on: RunLoop.main)
            .assign(to: &$canCheckForUpdates)
    }

    func checkForUpdates() {
        controller?.checkForUpdates(nil)
    }
}
