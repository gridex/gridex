// RedisAdapterKeyNameScanTests.swift
// Gridex

import XCTest
@testable import Gridex

final class RedisAdapterKeyNameScanTests: XCTestCase {

    func test_scanKeyNames_withoutConnectionReportsConnectionLostDuringScan() async {
        let adapter = RedisAdapter()

        do {
            _ = try await adapter.scanKeyNames(matching: "session:*", maximumCount: 25)
            XCTFail("Expected a disconnected names-only scan to fail")
        } catch let error as GridexError {
            XCTAssertEqual(
                error,
                .queryExecutionFailed("Connection lost during SCAN")
            )
        } catch {
            XCTFail("Expected GridexError, got \(error)")
        }
    }

    func test_scanLoop_cancellationBeforeSecondPageStopsPagination() async {
        let calls = RedisScanCallRecorder()

        let task = Task {
            try await RedisKeyScanLoop.run(
                maximumCount: 10,
                pageBudget: 10
            ) { cursor in
                await calls.record(cursor)
                if cursor == "0" {
                    withUnsafeCurrentTask { $0?.cancel() }
                    return RedisKeyScanPage(cursor: "1", keys: ["first"])
                }
                return RedisKeyScanPage(cursor: "0", keys: ["unexpected"])
            }
        }

        do {
            _ = try await task.value
            XCTFail("Expected cancellation before requesting page two")
        } catch is CancellationError {
            // Expected: the loop observes cancellation before the next request.
        } catch {
            XCTFail("Expected CancellationError, got \(error)")
        }

        let recordedCalls = await calls.snapshot()
        XCTAssertEqual(recordedCalls, ["0"])
    }

    func test_scanLoop_roundTripsOpaqueUInt64SizedCursor() async throws {
        let opaqueCursor = "18446744073709551615"
        let calls = RedisScanCallRecorder()

        let result = try await RedisKeyScanLoop.run(
            maximumCount: 10,
            pageBudget: 10
        ) { cursor in
            await calls.record(cursor)
            switch cursor {
            case "0":
                return RedisKeyScanPage(cursor: opaqueCursor, keys: ["first"])
            case opaqueCursor:
                return RedisKeyScanPage(cursor: "0", keys: ["second"])
            default:
                throw UnexpectedRedisScanCursor(cursor: cursor)
            }
        }

        let recordedCalls = await calls.snapshot()
        XCTAssertEqual(recordedCalls, ["0", opaqueCursor])
        XCTAssertEqual(result.keys, ["first", "second"])
        XCTAssertFalse(result.isTruncated)
    }

    func test_scanLoop_rejectsMalformedTopLevelResponse() async {
        let malformedResponse: RedisKeyScanPage? = nil

        await assertMalformedScanResponse {
            malformedResponse
        }
    }

    func test_scanLoop_rejectsMalformedCursor() async {
        let malformedResponse = RedisKeyScanPage(
            cursor: "not-a-cursor",
            keys: ["key"]
        )

        await assertMalformedScanResponse {
            malformedResponse
        }
    }

    func test_scanLoop_rejectsMalformedKeysPayload() async {
        let malformedResponse = RedisKeyScanPage(
            cursor: "0",
            keys: nil
        )

        await assertMalformedScanResponse {
            malformedResponse
        }
    }

    func test_scanLoop_deduplicatesKeysAcrossPages() async throws {
        let result = try await RedisKeyScanLoop.run(
            maximumCount: 10,
            pageBudget: 10
        ) { cursor in
            switch cursor {
            case "0":
                return RedisKeyScanPage(cursor: "1", keys: ["b", "a", "b"])
            case "1":
                return RedisKeyScanPage(cursor: "0", keys: ["b", "c"])
            default:
                throw UnexpectedRedisScanCursor(cursor: cursor)
            }
        }

        XCTAssertEqual(result.keys, ["a", "b", "c"])
        XCTAssertFalse(result.isTruncated)
    }

    func test_scanLoop_pageBudgetReturnsTruncatedResult() async throws {
        let calls = RedisScanCallRecorder()

        let result = try await RedisKeyScanLoop.run(
            maximumCount: 10,
            pageBudget: 2
        ) { cursor in
            await calls.record(cursor)
            switch cursor {
            case "0":
                return RedisKeyScanPage(cursor: "1", keys: ["a"])
            case "1":
                return RedisKeyScanPage(cursor: "2", keys: ["b"])
            case "2":
                return RedisKeyScanPage(cursor: "0", keys: ["unexpected"])
            default:
                throw UnexpectedRedisScanCursor(cursor: cursor)
            }
        }

        let recordedCalls = await calls.snapshot()
        XCTAssertEqual(recordedCalls, ["0", "1"])
        XCTAssertEqual(result.keys, ["a", "b"])
        XCTAssertTrue(result.isTruncated)
    }

    private func assertMalformedScanResponse(
        fetchPage: @escaping @Sendable () async throws -> RedisKeyScanPage?
    ) async {
        do {
            _ = try await RedisKeyScanLoop.run(
                maximumCount: 10,
                pageBudget: 10
            ) { _ in
                try await fetchPage()
            }
            XCTFail("Expected malformed SCAN response to fail")
        } catch let error as GridexError {
            XCTAssertEqual(
                error,
                .queryExecutionFailed("Malformed SCAN response")
            )
        } catch {
            XCTFail("Expected GridexError, got \(error)")
        }
    }
}

private actor RedisScanCallRecorder {
    private var cursors: [String] = []

    func record(_ cursor: String) {
        cursors.append(cursor)
    }

    func snapshot() -> [String] {
        cursors
    }
}

private struct UnexpectedRedisScanCursor: Error {
    let cursor: String
}
