// RedisKeyBrowserViewStateTests.swift
// Gridex

import XCTest
@testable import Gridex

@MainActor
final class RedisKeyBrowserViewStateTests: XCTestCase {

    func test_newerScanKeepsOwnershipWhenOlderSuccessCompletesFirst() async {
        let state = RedisKeyBrowserViewState()
        let context = redisContext()
        let olderGate = RedisOwnershipTestGate()
        let newerGate = RedisOwnershipTestGate()

        let olderScan = Task { @MainActor in
            await state.scan(in: context) {
                await olderGate.enterAndWait()
                return Optional<Result<RedisKeyScanResult, Error>>.some(
                    .success(RedisKeyScanResult(
                        keys: ["obsolete:key"],
                        isTruncated: false
                    ))
                )
            }
        }
        await olderGate.waitUntilEntered()

        let newerScan = Task { @MainActor in
            await state.scan(in: context) {
                await newerGate.enterAndWait()
                return Optional<Result<RedisKeyScanResult, Error>>.some(
                    .success(RedisKeyScanResult(
                        keys: ["current:key"],
                        isTruncated: false
                    ))
                )
            }
        }
        await newerGate.waitUntilEntered()

        await olderGate.release()
        await olderScan.value

        XCTAssertTrue(state.isLoading)
        XCTAssertNil(state.visibleResult(in: context))
        XCTAssertNil(state.visibleErrorMessage(in: context))

        await newerGate.release()
        await newerScan.value

        XCTAssertFalse(state.isLoading)
        XCTAssertEqual(state.visibleResult(in: context)?.keys, ["current:key"])
        XCTAssertNil(state.visibleErrorMessage(in: context))
    }

    func test_newerScanKeepsOwnershipWhenOlderFailureCompletesFirst() async {
        let state = RedisKeyBrowserViewState()
        let context = redisContext()
        let olderGate = RedisOwnershipTestGate()
        let newerGate = RedisOwnershipTestGate()

        await state.scan(in: context) {
            Optional<Result<RedisKeyScanResult, Error>>.some(
                .success(RedisKeyScanResult(
                    keys: ["retained:key"],
                    isTruncated: false
                ))
            )
        }

        let olderScan = Task { @MainActor in
            await state.scan(in: context) {
                await olderGate.enterAndWait()
                return Optional<Result<RedisKeyScanResult, Error>>.some(
                    .failure(ExpectedBrowserScanError.failed)
                )
            }
        }
        await olderGate.waitUntilEntered()

        let newerScan = Task { @MainActor in
            await state.scan(in: context) {
                await newerGate.enterAndWait()
                return Optional<Result<RedisKeyScanResult, Error>>.some(
                    .success(RedisKeyScanResult(
                        keys: ["fresh:key"],
                        isTruncated: false
                    ))
                )
            }
        }
        await newerGate.waitUntilEntered()

        await olderGate.release()
        await olderScan.value

        XCTAssertTrue(state.isLoading)
        XCTAssertEqual(state.visibleResult(in: context)?.keys, ["retained:key"])
        XCTAssertNil(state.visibleErrorMessage(in: context))

        await newerGate.release()
        await newerScan.value

        XCTAssertFalse(state.isLoading)
        XCTAssertEqual(state.visibleResult(in: context)?.keys, ["fresh:key"])
        XCTAssertNil(state.visibleErrorMessage(in: context))
    }

    func test_currentStaleScanStopsLoadingWithoutPublishingContent() async {
        let state = RedisKeyBrowserViewState()
        let context = redisContext()

        await state.scan(in: context) {
            Optional<Result<RedisKeyScanResult, Error>>.none
        }

        XCTAssertFalse(state.isLoading)
        XCTAssertNil(state.visibleResult(in: context))
        XCTAssertNil(state.visibleErrorMessage(in: context))
    }

    func test_currentFailurePublishesErrorAndStopsLoading() async {
        let state = RedisKeyBrowserViewState()
        let context = redisContext()

        await state.scan(in: context) {
            Optional<Result<RedisKeyScanResult, Error>>.some(
                .failure(ExpectedBrowserScanError.failed)
            )
        }

        XCTAssertFalse(state.isLoading)
        XCTAssertNil(state.visibleResult(in: context))
        XCTAssertEqual(
            state.visibleErrorMessage(in: context),
            "scan failed"
        )
    }

    func test_deactivateInvalidatesRunningScanAndStopsItsLoadingState() async {
        let state = RedisKeyBrowserViewState()
        let context = redisContext()
        let gate = RedisOwnershipTestGate()

        let scan = Task { @MainActor in
            await state.scan(in: context) {
                await gate.enterAndWait()
                return Optional<Result<RedisKeyScanResult, Error>>.some(
                    .success(RedisKeyScanResult(
                        keys: ["stale:key"],
                        isTruncated: false
                    ))
                )
            }
        }
        await gate.waitUntilEntered()

        state.deactivate()
        XCTAssertFalse(state.isLoading)

        await gate.release()
        await scan.value

        XCTAssertFalse(state.isLoading)
        XCTAssertNil(state.visibleResult(in: context))
        XCTAssertNil(state.visibleErrorMessage(in: context))
    }

    private func redisContext() -> AppState.RedisTabContext {
        AppState.RedisTabContext(
            connectionID: UUID(uuidString: "11111111-1111-1111-1111-111111111111")!,
            databaseName: "db0",
            sessionID: UUID(uuidString: "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA")!,
            databaseRevision: 3
        )
    }
}

private enum ExpectedBrowserScanError: LocalizedError {
    case failed

    var errorDescription: String? {
        "scan failed"
    }
}
