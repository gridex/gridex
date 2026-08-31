import XCTest
import AppKit
@testable import Gridex

@MainActor
final class TabNavigationTests: XCTestCase {
    func test_clickSelection_activatesChosenTabWithoutRemovingTabs() {
        let (state, first, second) = makeTwoTabState()
        state.selectTab(id: first)
        state.selectTab(id: second)
        XCTAssertEqual(state.activeTabId, second)
        XCTAssertEqual(state.tabs.map(\.id), [first, second])
    }

    func test_xClose_onInactiveTabKeepsActiveTab() {
        let (state, first, second) = makeTwoTabState()
        state.selectTab(id: first)
        state.closeTab(id: second)
        XCTAssertEqual(state.activeTabId, first)
        XCTAssertEqual(state.tabs.map(\.id), [first])
    }

    func test_middleClickInsideInvokesCloseOnce() {
        let view = MiddleClickNSView(frame: NSRect(x: 0, y: 0, width: 120, height: 38))
        var calls = 0
        view.action = { calls += 1 }
        XCTAssertTrue(view.handleMiddleClick(at: NSPoint(x: 60, y: 19)))
        XCTAssertEqual(calls, 1)
    }

    func test_middleClickOutsideDoesNotInvokeClose() {
        let view = MiddleClickNSView(frame: NSRect(x: 0, y: 0, width: 120, height: 38))
        var calls = 0
        view.action = { calls += 1 }
        XCTAssertFalse(view.handleMiddleClick(at: NSPoint(x: 121, y: 19)))
        XCTAssertEqual(calls, 0)
    }

    func test_controlTab_cyclesForwardAndWraps() {
        let (state, first, second) = makeTwoTabState()
        state.selectTab(id: first)
        state.selectNextTab()
        XCTAssertEqual(state.activeTabId, second)
        state.selectNextTab()
        XCTAssertEqual(state.activeTabId, first)
    }

    func test_controlShiftTab_cyclesBackwardAndWraps() {
        let (state, first, second) = makeTwoTabState()
        state.selectTab(id: first)
        state.selectPreviousTab()
        XCTAssertEqual(state.activeTabId, second)
        state.selectPreviousTab()
        XCTAssertEqual(state.activeTabId, first)
    }

    func test_navigationWithOneTabIsNoOp() {
        let state = AppState()
        state.openTable(name: "only", schema: nil)
        guard let only = state.activeTabId else {
            return XCTFail("openTable must activate the new tab")
        }
        state.selectNextTab()
        state.selectPreviousTab()
        XCTAssertEqual(state.activeTabId, only)
    }

    private func makeTwoTabState() -> (AppState, UUID, UUID) {
        let state = AppState()
        state.openTable(name: "first", schema: nil)
        let first = state.activeTabId!
        state.openTable(name: "second", schema: nil)
        let second = state.activeTabId!
        return (state, first, second)
    }
}
