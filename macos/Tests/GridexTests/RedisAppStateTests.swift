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

    private func redisConfig() -> ConnectionConfig {
        ConnectionConfig(name: "Redis", databaseType: .redis)
    }
}
