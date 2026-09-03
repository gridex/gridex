// RedisKeyDetailViewLifecycleTests.swift
// Gridex

import XCTest
@testable import Gridex

@MainActor
final class RedisKeyDetailViewLifecycleTests: XCTestCase {

    func test_taskQueuedBeforeDeactivationCannotEnterOperation() async throws {
        let lifecycle = RedisKeyDetailViewLifecycle()
        var didRun = false

        lifecycle.activate()
        let task = try XCTUnwrap(lifecycle.task {
            didRun = true
        })
        lifecycle.deactivate()

        await task.value

        XCTAssertFalse(didRun)
    }

    func test_taskFromPreviousLifetimeCannotEnterAfterReactivation() async throws {
        let lifecycle = RedisKeyDetailViewLifecycle()
        var didRun = false

        lifecycle.activate()
        let obsoleteTask = try XCTUnwrap(lifecycle.task {
            didRun = true
        })
        lifecycle.deactivate()
        lifecycle.activate()

        await obsoleteTask.value

        XCTAssertFalse(didRun)
    }

    func test_taskFromCurrentLifetimeEntersOperation() async throws {
        let lifecycle = RedisKeyDetailViewLifecycle()
        var didRun = false

        lifecycle.activate()
        let task = try XCTUnwrap(lifecycle.task {
            didRun = true
        })

        await task.value

        XCTAssertTrue(didRun)
    }
}
