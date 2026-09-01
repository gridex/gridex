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
        let adapter = RedisAdapter()
        let sessionID = ObjectIdentifier(adapter)
        let db0 = RedisKeyBrowserContext(
            connectionID: connectionID,
            databaseName: "db0",
            sessionID: sessionID
        )
        let db1 = RedisKeyBrowserContext(
            connectionID: connectionID,
            databaseName: "db1",
            sessionID: sessionID
        )
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
        let firstAdapter = RedisAdapter()
        let secondAdapter = RedisAdapter()
        let firstScope = RedisKeyBrowserContext(
            connectionID: firstConnection,
            databaseName: "db4",
            sessionID: ObjectIdentifier(firstAdapter)
        )
        let otherConnectionSameDatabase = RedisKeyBrowserContext(
            connectionID: secondConnection,
            databaseName: "db4",
            sessionID: ObjectIdentifier(secondAdapter)
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

    func test_contentState_doesNotExposePriorAdapterSessionForSameConnectionAndDatabase() {
        let connectionID = UUID(uuidString: "11111111-1111-1111-1111-111111111111")!
        let firstAdapter = RedisAdapter()
        let replacementAdapter = RedisAdapter()
        let firstSession = RedisKeyBrowserContext(
            connectionID: connectionID,
            databaseName: "db0",
            sessionID: ObjectIdentifier(firstAdapter)
        )
        let replacementSession = RedisKeyBrowserContext(
            connectionID: connectionID,
            databaseName: "db0",
            sessionID: ObjectIdentifier(replacementAdapter)
        )
        var state = RedisKeyBrowserContentState()

        state.beginLoading(in: firstSession)
        state.publishSuccess(
            RedisKeyScanResult(keys: ["old:session"], isTruncated: false),
            in: firstSession
        )

        XCTAssertNil(state.visibleResult(in: replacementSession))

        state.beginLoading(in: replacementSession)
        state.publishFailure("replacement scan failed", in: replacementSession)

        XCTAssertNil(state.visibleResult(in: replacementSession))
        XCTAssertEqual(
            state.visibleErrorMessage(in: replacementSession),
            "replacement scan failed"
        )
        XCTAssertNil(state.visibleResult(in: firstSession))
    }

    func test_openKeyAccessibilityLabel_usesTheFullConcreteKey() {
        XCTAssertEqual(
            RedisKeyBrowserBehavior.openKeyAccessibilityLabel(for: "user:1001"),
            "Open key user:1001"
        )
    }
}
