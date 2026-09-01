// RedisAppStateTests.swift
// Gridex

import XCTest
@testable import Gridex

@MainActor
final class RedisAppStateTests: XCTestCase {

    func test_requestRedisKeyBrowserRefresh_incrementsNonce() {
        let state = AppState()
        let originalNonce = state.redisKeyBrowserRefreshNonce

        state.requestRedisKeyBrowserRefresh()

        XCTAssertEqual(state.redisKeyBrowserRefreshNonce, originalNonce + 1)
    }

    func test_loadSidebar_forRedisClearsGenericItemsAndRequestsBrowserRefresh() async {
        let state = AppState()
        state.sidebarItems = [
            SidebarItem(title: "Legacy", type: .group("legacy"))
        ]
        let originalNonce = state.redisKeyBrowserRefreshNonce

        await state.loadSidebar(config: redisConfig(), adapter: RedisAdapter())

        XCTAssertTrue(state.sidebarItems.isEmpty)
        XCTAssertEqual(state.redisKeyBrowserRefreshNonce, originalNonce + 1)
    }

    func test_refreshSidebar_forRedisRequestsBrowserRefreshImmediately() {
        let state = AppState()
        state.activeAdapter = RedisAdapter()
        state.activeConfig = redisConfig()
        let originalNonce = state.redisKeyBrowserRefreshNonce

        state.refreshSidebar()

        XCTAssertEqual(state.redisKeyBrowserRefreshNonce, originalNonce + 1)
    }

    func test_openRedisFlatKeyList_reusesKeysTabAndShowsItsFilterBar() throws {
        let state = AppState()

        state.openRedisFlatKeyList()

        let firstTabID = try XCTUnwrap(state.activeTabId)
        XCTAssertEqual(state.tabs.count, 1)
        XCTAssertEqual(state.tabs.first?.type, .dataGrid)
        XCTAssertEqual(state.tabs.first?.tableName, "Keys")
        XCTAssertTrue(state.cachedDataGridState(for: firstTabID).showFilterBar)

        state.cachedDataGridState(for: firstTabID).showFilterBar = false
        state.openRedisFlatKeyList()

        XCTAssertEqual(state.tabs.count, 1)
        XCTAssertEqual(state.activeTabId, firstTabID)
        XCTAssertTrue(state.cachedDataGridState(for: firstTabID).showFilterBar)
    }

    func test_openRedisFlatKeyList_reusesOnlyWithinTheCurrentDatabase() throws {
        let state = AppState()
        state.currentDatabaseName = "db0"

        state.openRedisFlatKeyList()
        let db0TabID = try XCTUnwrap(state.activeTabId)

        state.currentDatabaseName = "db1"
        state.openRedisFlatKeyList()
        let db1TabID = try XCTUnwrap(state.activeTabId)

        XCTAssertEqual(state.tabs.count, 2)
        XCTAssertNotEqual(db1TabID, db0TabID)
        XCTAssertEqual(state.tabs.first(where: { $0.id == db0TabID })?.databaseName, "db0")
        XCTAssertEqual(state.tabs.first(where: { $0.id == db1TabID })?.databaseName, "db1")

        state.currentDatabaseName = "db0"
        state.openRedisFlatKeyList()

        XCTAssertEqual(state.tabs.count, 2)
        XCTAssertEqual(state.activeTabId, db0TabID)
    }

    func test_openRedisKeyDetail_reusesSameNamedKeyOnlyWithinCurrentDatabase() throws {
        let state = AppState()
        state.currentDatabaseName = "db0"

        state.openRedisKeyDetail(key: "session:42")
        let db0TabID = try XCTUnwrap(state.activeTabId)

        state.currentDatabaseName = "db1"
        state.openRedisKeyDetail(key: "session:42")
        let db1TabID = try XCTUnwrap(state.activeTabId)

        XCTAssertEqual(state.tabs.count, 2)
        XCTAssertNotEqual(db1TabID, db0TabID)
        XCTAssertEqual(state.tabs.first(where: { $0.id == db0TabID })?.databaseName, "db0")
        XCTAssertEqual(state.tabs.first(where: { $0.id == db1TabID })?.databaseName, "db1")

        state.currentDatabaseName = "db0"
        state.openRedisKeyDetail(key: "session:42")

        XCTAssertEqual(state.tabs.count, 2)
        XCTAssertEqual(state.activeTabId, db0TabID)
    }

    func test_flatKeyListOpenStateBecomesFalseAfterCloseAndCanReopen() throws {
        let state = AppState()
        state.currentDatabaseName = "db3"

        XCTAssertFalse(state.isRedisFlatKeyListOpen)
        state.openRedisFlatKeyList()
        let closedTabID = try XCTUnwrap(state.activeTabId)
        XCTAssertTrue(state.isRedisFlatKeyListOpen)

        state.closeTab(id: closedTabID)

        XCTAssertFalse(state.isRedisFlatKeyListOpen)

        state.openRedisFlatKeyList()
        let reopenedTabID = try XCTUnwrap(state.activeTabId)

        XCTAssertTrue(state.isRedisFlatKeyListOpen)
        XCTAssertNotEqual(reopenedTabID, closedTabID)
        XCTAssertEqual(state.tabs.first(where: { $0.id == reopenedTabID })?.databaseName, "db3")
    }

    private func redisConfig() -> ConnectionConfig {
        ConnectionConfig(name: "Redis", databaseType: .redis)
    }
}
