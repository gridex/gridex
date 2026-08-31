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
}
