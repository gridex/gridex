// RedisKeyBrowserBehaviorTests.swift
// Gridex

import XCTest
@testable import Gridex

final class RedisKeyBrowserBehaviorTests: XCTestCase {

    func test_effectiveDelimiter_defaultsWhitespaceOnlyInputToColon() {
        XCTAssertEqual(RedisKeyBrowserBehavior.effectiveDelimiter(for: ""), ":")
        XCTAssertEqual(RedisKeyBrowserBehavior.effectiveDelimiter(for: "  \n\t  "), ":")
    }

    func test_effectiveDelimiter_preservesNonblankInputExactly() {
        XCTAssertEqual(RedisKeyBrowserBehavior.effectiveDelimiter(for: "::"), "::")
        XCTAssertEqual(RedisKeyBrowserBehavior.effectiveDelimiter(for: " :: "), " :: ")
    }

    func test_shouldPublish_acceptsOnlyCurrentNoncancelledLoad() {
        XCTAssertTrue(RedisKeyBrowserBehavior.shouldPublish(
            capturedNonce: 7,
            currentNonce: 7,
            isCancelled: false
        ))
        XCTAssertFalse(RedisKeyBrowserBehavior.shouldPublish(
            capturedNonce: 6,
            currentNonce: 7,
            isCancelled: false
        ))
        XCTAssertFalse(RedisKeyBrowserBehavior.shouldPublish(
            capturedNonce: 7,
            currentNonce: 7,
            isCancelled: true
        ))
    }

    func test_contentState_doesNotExposeDb0ResultWhenDb1LoadFails() {
        let connectionID = UUID(uuidString: "11111111-1111-1111-1111-111111111111")!
        let db0 = RedisKeyBrowserContext(connectionID: connectionID, databaseName: "db0")
        let db1 = RedisKeyBrowserContext(connectionID: connectionID, databaseName: "db1")
        var state = RedisKeyBrowserContentState()

        state.beginLoading(in: db0)
        state.publishSuccess(
            RedisKeyScanResult(keys: ["db0:only"], isTruncated: false),
            in: db0
        )
        XCTAssertEqual(state.visibleResult(in: db0)?.keys, ["db0:only"])

        state.beginLoading(in: db1)
        state.publishFailure("db1 scan failed", in: db1)

        XCTAssertNil(state.visibleResult(in: db1))
        XCTAssertEqual(state.visibleErrorMessage(in: db1), "db1 scan failed")
        XCTAssertNil(state.visibleResult(in: db0))
    }

    func test_contentState_scopesRetainedResultAndErrorToConnectionAndDatabase() {
        let firstConnection = UUID(uuidString: "11111111-1111-1111-1111-111111111111")!
        let secondConnection = UUID(uuidString: "22222222-2222-2222-2222-222222222222")!
        let firstScope = RedisKeyBrowserContext(connectionID: firstConnection, databaseName: "db4")
        let otherConnectionSameDatabase = RedisKeyBrowserContext(
            connectionID: secondConnection,
            databaseName: "db4"
        )
        var state = RedisKeyBrowserContentState()

        state.beginLoading(in: firstScope)
        state.publishSuccess(
            RedisKeyScanResult(keys: ["session:42"], isTruncated: false),
            in: firstScope
        )
        state.beginLoading(in: firstScope)
        state.publishFailure("refresh failed", in: firstScope)

        XCTAssertEqual(state.visibleResult(in: firstScope)?.keys, ["session:42"])
        XCTAssertEqual(state.visibleErrorMessage(in: firstScope), "refresh failed")
        XCTAssertNil(state.visibleResult(in: otherConnectionSameDatabase))
        XCTAssertNil(state.visibleErrorMessage(in: otherConnectionSameDatabase))
    }

    func test_openKeyAccessibilityLabel_usesTheFullConcreteKey() {
        XCTAssertEqual(
            RedisKeyBrowserBehavior.openKeyAccessibilityLabel(for: "user:1001"),
            "Open key user:1001"
        )
    }
}
