// RedisAdapterDatabaseSelectionTests.swift
// Gridex

import XCTest
@testable import Gridex

final class RedisAdapterDatabaseSelectionTests: XCTestCase {

    func test_successfulSelectAdvancesTrackedCurrentDatabase() {
        var selection = RedisDatabaseSelectionState(initialDatabase: 0)

        selection.recordSuccessfulSelect(arguments: ["12"])

        XCTAssertEqual(selection.currentDatabase, 12)
    }
}
