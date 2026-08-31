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
}
