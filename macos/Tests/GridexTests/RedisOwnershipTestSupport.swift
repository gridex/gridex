// RedisOwnershipTestSupport.swift
// Gridex

import Foundation

actor RedisOwnershipTestGate {
    private var hasEntered = false
    private var hasBeenReleased = false
    private var entryWaiters: [CheckedContinuation<Void, Never>] = []
    private var releaseWaiter: CheckedContinuation<Void, Never>?

    func waitUntilEntered() async {
        guard !hasEntered else { return }
        await withCheckedContinuation { continuation in
            entryWaiters.append(continuation)
        }
    }

    func enterAndWait() async {
        hasEntered = true
        let waiters = entryWaiters
        entryWaiters.removeAll()
        waiters.forEach { $0.resume() }

        guard !hasBeenReleased else { return }
        await withCheckedContinuation { continuation in
            releaseWaiter = continuation
        }
    }

    func release() {
        hasBeenReleased = true
        releaseWaiter?.resume()
        releaseWaiter = nil
    }
}
