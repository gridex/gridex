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
        establishRedisContext(
            on: state,
            adapter: RedisAdapter(),
            connectionID: UUID(uuidString: "11111111-1111-1111-1111-111111111111")!,
            databaseName: "db0"
        )

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
        establishRedisContext(
            on: state,
            adapter: RedisAdapter(),
            connectionID: UUID(uuidString: "11111111-1111-1111-1111-111111111111")!,
            databaseName: "db0"
        )

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

    func test_openRedisFlatKeyList_reusesOnlyWithinTheActiveConnection() throws {
        let firstConnection = UUID(uuidString: "11111111-1111-1111-1111-111111111111")!
        let secondConnection = UUID(uuidString: "22222222-2222-2222-2222-222222222222")!
        let firstAdapter = RedisAdapter()
        let secondAdapter = RedisAdapter()
        let state = AppState()
        establishRedisContext(
            on: state,
            adapter: firstAdapter,
            connectionID: firstConnection,
            databaseName: "db0"
        )

        state.openRedisFlatKeyList()
        let firstConnectionTabID = try XCTUnwrap(state.activeTabId)
        let firstConnectionContext = try XCTUnwrap(
            state.tabs.first(where: { $0.id == firstConnectionTabID })?.redisContext
        )
        XCTAssertEqual(firstConnectionContext.connectionID, firstConnection)
        XCTAssertEqual(firstConnectionContext.databaseName, "db0")
        XCTAssertTrue(state.activeRedisAdapter(for: firstConnectionContext) === firstAdapter)

        state.activeConnectionId = secondConnection
        state.activeAdapter = secondAdapter
        XCTAssertFalse(state.isRedisFlatKeyListOpen)
        state.openRedisFlatKeyList()
        let secondConnectionTabID = try XCTUnwrap(state.activeTabId)

        XCTAssertEqual(state.tabs.count, 2)
        XCTAssertNotEqual(secondConnectionTabID, firstConnectionTabID)
        let secondConnectionContext = try XCTUnwrap(
            state.tabs.first(where: { $0.id == secondConnectionTabID })?.redisContext
        )
        XCTAssertEqual(secondConnectionContext.connectionID, secondConnection)
        XCTAssertEqual(secondConnectionContext.databaseName, "db0")
        XCTAssertTrue(state.activeRedisAdapter(for: secondConnectionContext) === secondAdapter)
        XCTAssertTrue(state.isRedisFlatKeyListOpen)

        state.activeConnectionId = firstConnection
        state.activeAdapter = firstAdapter
        XCTAssertNil(state.activeRedisAdapter(for: firstConnectionContext))
        state.openRedisFlatKeyList()
        let reacceptedFirstConnectionTabID = try XCTUnwrap(state.activeTabId)

        XCTAssertEqual(state.tabs.count, 3)
        XCTAssertNotEqual(reacceptedFirstConnectionTabID, firstConnectionTabID)
        XCTAssertNotEqual(reacceptedFirstConnectionTabID, secondConnectionTabID)
    }

    func test_openRedisKeyDetail_reusesSameNamedKeyOnlyWithinCurrentDatabase() throws {
        let state = AppState()
        establishRedisContext(
            on: state,
            adapter: RedisAdapter(),
            connectionID: UUID(uuidString: "11111111-1111-1111-1111-111111111111")!,
            databaseName: "db0"
        )

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

    func test_openRedisKeyDetail_reusesSameNamedKeyOnlyWithinActiveConnection() throws {
        let firstConnection = UUID(uuidString: "11111111-1111-1111-1111-111111111111")!
        let secondConnection = UUID(uuidString: "22222222-2222-2222-2222-222222222222")!
        let firstAdapter = RedisAdapter()
        let secondAdapter = RedisAdapter()
        let state = AppState()
        establishRedisContext(
            on: state,
            adapter: firstAdapter,
            connectionID: firstConnection,
            databaseName: "db0"
        )

        state.openRedisKeyDetail(key: "session:42")
        let firstConnectionTabID = try XCTUnwrap(state.activeTabId)
        let firstConnectionContext = try XCTUnwrap(
            state.tabs.first(where: { $0.id == firstConnectionTabID })?.redisContext
        )
        XCTAssertEqual(firstConnectionContext.connectionID, firstConnection)
        XCTAssertEqual(firstConnectionContext.databaseName, "db0")
        XCTAssertTrue(state.activeRedisAdapter(for: firstConnectionContext) === firstAdapter)

        state.activeConnectionId = secondConnection
        state.activeAdapter = secondAdapter
        state.openRedisKeyDetail(key: "session:42")
        let secondConnectionTabID = try XCTUnwrap(state.activeTabId)

        XCTAssertEqual(state.tabs.count, 2)
        XCTAssertNotEqual(secondConnectionTabID, firstConnectionTabID)
        let secondConnectionContext = try XCTUnwrap(
            state.tabs.first(where: { $0.id == secondConnectionTabID })?.redisContext
        )
        XCTAssertEqual(secondConnectionContext.connectionID, secondConnection)
        XCTAssertEqual(secondConnectionContext.databaseName, "db0")
        XCTAssertTrue(state.activeRedisAdapter(for: secondConnectionContext) === secondAdapter)

        state.activeConnectionId = firstConnection
        state.activeAdapter = firstAdapter
        XCTAssertNil(state.activeRedisAdapter(for: firstConnectionContext))
        state.openRedisKeyDetail(key: "session:42")
        let reacceptedFirstConnectionTabID = try XCTUnwrap(state.activeTabId)

        XCTAssertEqual(state.tabs.count, 3)
        XCTAssertNotEqual(reacceptedFirstConnectionTabID, firstConnectionTabID)
        XCTAssertNotEqual(reacceptedFirstConnectionTabID, secondConnectionTabID)
    }

    func test_activeRedisAdapterForTabContext_rejectsConnectionAndDatabaseChanges() throws {
        let firstConnection = UUID(uuidString: "11111111-1111-1111-1111-111111111111")!
        let secondConnection = UUID(uuidString: "22222222-2222-2222-2222-222222222222")!
        let adapter = RedisAdapter()
        let state = AppState()
        establishRedisContext(
            on: state,
            adapter: adapter,
            connectionID: firstConnection,
            databaseName: "db0"
        )
        state.openRedisKeyDetail(key: "session:42")
        let tabID = try XCTUnwrap(state.activeTabId)
        let capturedContext = try XCTUnwrap(
            state.tabs.first(where: { $0.id == tabID })?.redisContext
        )

        XCTAssertTrue(state.activeRedisAdapter(for: capturedContext) === adapter)

        state.activeConnectionId = secondConnection
        XCTAssertNil(state.activeRedisAdapter(for: capturedContext))

        state.activeConnectionId = firstConnection
        state.currentDatabaseName = "db1"
        XCTAssertNil(state.activeRedisAdapter(for: capturedContext))
    }

    func test_replacingRedisAdapterInvalidatesSameConnectionAndDatabaseTabs() throws {
        let connectionID = UUID(uuidString: "11111111-1111-1111-1111-111111111111")!
        let firstAdapter = RedisAdapter()
        let secondAdapter = RedisAdapter()
        let state = AppState()
        establishRedisContext(
            on: state,
            adapter: firstAdapter,
            connectionID: connectionID,
            databaseName: "db0"
        )

        state.openRedisFlatKeyList()
        let firstFlatTabID = try XCTUnwrap(state.activeTabId)
        state.openRedisKeyDetail(key: "session:42")
        let firstDetailTabID = try XCTUnwrap(state.activeTabId)
        let firstFlatContext = try XCTUnwrap(
            state.tabs.first(where: { $0.id == firstFlatTabID })?.redisContext
        )
        let firstDetailContext = try XCTUnwrap(
            state.tabs.first(where: { $0.id == firstDetailTabID })?.redisContext
        )

        state.activeAdapter = secondAdapter

        XCTAssertFalse(state.isRedisFlatKeyListOpen)
        XCTAssertNil(state.activeRedisAdapter(for: firstFlatContext))
        XCTAssertNil(state.activeRedisAdapter(for: firstDetailContext))

        state.openRedisFlatKeyList()
        let secondFlatTabID = try XCTUnwrap(state.activeTabId)
        state.openRedisKeyDetail(key: "session:42")
        let secondDetailTabID = try XCTUnwrap(state.activeTabId)

        XCTAssertEqual(state.tabs.count, 4)
        XCTAssertNotEqual(secondFlatTabID, firstFlatTabID)
        XCTAssertNotEqual(secondDetailTabID, firstDetailTabID)
        let secondFlatContext = try XCTUnwrap(
            state.tabs.first(where: { $0.id == secondFlatTabID })?.redisContext
        )
        let secondDetailContext = try XCTUnwrap(
            state.tabs.first(where: { $0.id == secondDetailTabID })?.redisContext
        )
        XCTAssertEqual(secondFlatContext.connectionID, connectionID)
        XCTAssertEqual(secondFlatContext.databaseName, "db0")
        XCTAssertEqual(secondDetailContext.connectionID, connectionID)
        XCTAssertEqual(secondDetailContext.databaseName, "db0")
        XCTAssertTrue(state.activeRedisAdapter(for: secondFlatContext) === secondAdapter)
        XCTAssertTrue(state.activeRedisAdapter(for: secondDetailContext) === secondAdapter)
    }

    func test_redisDatabaseTransitionInvalidatesOldContextBeforeAwaitingSelect() async throws {
        let connectionID = UUID(uuidString: "11111111-1111-1111-1111-111111111111")!
        let adapter = RedisAdapter()
        let state = AppState()
        establishRedisContext(
            on: state,
            adapter: adapter,
            connectionID: connectionID,
            databaseName: "db0"
        )
        state.openRedisFlatKeyList()
        let flatTabID = try XCTUnwrap(state.activeTabId)
        state.openRedisKeyDetail(key: "session:42")
        let detailTabID = try XCTUnwrap(state.activeTabId)
        let flatContext = try XCTUnwrap(
            state.tabs.first(where: { $0.id == flatTabID })?.redisContext
        )
        let detailContext = try XCTUnwrap(
            state.tabs.first(where: { $0.id == detailTabID })?.redisContext
        )
        var selectWasInvoked = false

        try await state.performRedisDatabaseTransition(to: "db1") {
            selectWasInvoked = true
            XCTAssertFalse(state.isRedisFlatKeyListOpen)
            XCTAssertNil(state.activeRedisAdapter(for: flatContext))
            XCTAssertNil(state.activeRedisAdapter(for: detailContext))

            await Task.yield()

            XCTAssertNil(state.activeRedisAdapter(for: flatContext))
            XCTAssertNil(state.activeRedisAdapter(for: detailContext))
        }

        XCTAssertTrue(selectWasInvoked)
        XCTAssertEqual(state.currentDatabaseName, "db1")
        XCTAssertNil(state.activeRedisAdapter(for: flatContext))
        XCTAssertNil(state.activeRedisAdapter(for: detailContext))
    }

    func test_overlappingRedisTransitions_newerSuccessOutranksOlderSuccess() async {
        let state = AppState()
        establishRedisContext(
            on: state,
            adapter: RedisAdapter(),
            connectionID: UUID(uuidString: "11111111-1111-1111-1111-111111111111")!,
            databaseName: "db0"
        )
        let olderGate = AsyncTestGate()
        let newerGate = AsyncTestGate()

        let olderTransition = Task { @MainActor in
            await state.performRedisDatabaseTransition(to: "db1") {
                await olderGate.enterAndWait()
            }
        }
        await olderGate.waitUntilEntered()

        let newerTransition = Task { @MainActor in
            await state.performRedisDatabaseTransition(to: "db2") {
                await newerGate.enterAndWait()
            }
        }

        await olderGate.release()
        await olderTransition.value

        await newerGate.waitUntilEntered()
        await newerGate.release()
        await newerTransition.value

        XCTAssertEqual(state.currentDatabaseName, "db2")
    }

    func test_overlappingRedisTransitions_olderFailureCannotRestoreOverNewerSuccess() async {
        let state = AppState()
        establishRedisContext(
            on: state,
            adapter: RedisAdapter(),
            connectionID: UUID(uuidString: "11111111-1111-1111-1111-111111111111")!,
            databaseName: "db0"
        )
        let olderGate = AsyncTestGate()
        let newerGate = AsyncTestGate()

        let olderTransition = Task { @MainActor in
            do {
                try await state.performRedisDatabaseTransition(to: "db1") {
                    await olderGate.enterAndWait()
                    throw ExpectedTransitionError.selectFailed
                }
                return false
            } catch ExpectedTransitionError.selectFailed {
                return true
            } catch {
                return false
            }
        }
        await olderGate.waitUntilEntered()

        let newerTransition = Task { @MainActor in
            await state.performRedisDatabaseTransition(to: "db2") {
                await newerGate.enterAndWait()
            }
        }

        await olderGate.release()
        let olderFailedAsExpected = await olderTransition.value

        await newerGate.waitUntilEntered()
        await newerGate.release()
        await newerTransition.value

        XCTAssertEqual(state.currentDatabaseName, "db2")
        XCTAssertTrue(olderFailedAsExpected)
        XCTAssertEqual(state.currentDatabaseName, "db2")
    }

    func test_overlappingRedisTransitions_newerFailureKeepsOlderSuccessfulDatabase() async {
        let state = AppState()
        establishRedisContext(
            on: state,
            adapter: RedisAdapter(),
            connectionID: UUID(uuidString: "11111111-1111-1111-1111-111111111111")!,
            databaseName: "db0"
        )
        let olderGate = AsyncTestGate()
        let newerGate = AsyncTestGate()

        let olderTransition = Task { @MainActor in
            await state.performRedisDatabaseTransition(to: "db1") {
                await olderGate.enterAndWait()
            }
        }
        await olderGate.waitUntilEntered()

        let newerTransition = Task { @MainActor in
            do {
                try await state.performRedisDatabaseTransition(to: "db2") {
                    await newerGate.enterAndWait()
                    throw ExpectedTransitionError.selectFailed
                }
                return false
            } catch ExpectedTransitionError.selectFailed {
                return true
            } catch {
                return false
            }
        }

        await olderGate.release()
        await olderTransition.value
        XCTAssertEqual(state.currentDatabaseName, "db1")

        await newerGate.waitUntilEntered()
        await newerGate.release()
        let newerFailedAsExpected = await newerTransition.value

        XCTAssertTrue(newerFailedAsExpected)
        XCTAssertEqual(state.currentDatabaseName, "db1")
    }

    func test_overlappingRedisTransitions_twoFailuresKeepOriginalDatabase() async {
        let state = AppState()
        establishRedisContext(
            on: state,
            adapter: RedisAdapter(),
            connectionID: UUID(uuidString: "11111111-1111-1111-1111-111111111111")!,
            databaseName: "db0"
        )
        let olderGate = AsyncTestGate()
        let newerGate = AsyncTestGate()

        let olderTransition = Task { @MainActor in
            do {
                try await state.performRedisDatabaseTransition(to: "db1") {
                    await olderGate.enterAndWait()
                    throw ExpectedTransitionError.selectFailed
                }
                return false
            } catch ExpectedTransitionError.selectFailed {
                return true
            } catch {
                return false
            }
        }
        await olderGate.waitUntilEntered()

        let newerTransition = Task { @MainActor in
            do {
                try await state.performRedisDatabaseTransition(to: "db2") {
                    await newerGate.enterAndWait()
                    throw ExpectedTransitionError.selectFailed
                }
                return false
            } catch ExpectedTransitionError.selectFailed {
                return true
            } catch {
                return false
            }
        }

        await olderGate.release()
        let olderFailedAsExpected = await olderTransition.value
        XCTAssertTrue(olderFailedAsExpected)
        XCTAssertEqual(state.currentDatabaseName, "db0")

        await newerGate.waitUntilEntered()
        await newerGate.release()
        let newerFailedAsExpected = await newerTransition.value

        XCTAssertTrue(newerFailedAsExpected)
        XCTAssertEqual(state.currentDatabaseName, "db0")
    }

    func test_overlappingRedisTransitions_serializeActualSelectClosuresInRevisionOrder() async {
        let state = AppState()
        establishRedisContext(
            on: state,
            adapter: RedisAdapter(),
            connectionID: UUID(uuidString: "11111111-1111-1111-1111-111111111111")!,
            databaseName: "db0"
        )
        let olderGate = AsyncTestGate()
        let newerGate = AsyncTestGate()
        let events = AsyncTestEventRecorder()

        let olderTransition = Task { @MainActor in
            await state.performRedisDatabaseTransition(to: "db1") {
                await events.record("db1:start")
                await olderGate.enterAndWait()
                await events.record("db1:end")
            }
        }
        await olderGate.waitUntilEntered()

        let newerTransition = Task { @MainActor in
            await state.performRedisDatabaseTransition(to: "db2") {
                await events.record("db2:start")
                await newerGate.enterAndWait()
                await events.record("db2:end")
            }
        }
        while state.redisDatabaseRevision < 2 {
            await Task.yield()
        }

        let newerEnteredBeforeOlderFinished = await newerGate.enteredSnapshot()
        XCTAssertFalse(newerEnteredBeforeOlderFinished)

        await olderGate.release()
        await olderTransition.value
        await newerGate.waitUntilEntered()
        await newerGate.release()
        await newerTransition.value

        let recordedEvents = await events.snapshot()
        XCTAssertEqual(
            recordedEvents,
            ["db1:start", "db1:end", "db2:start", "db2:end"]
        )
        XCTAssertEqual(state.currentDatabaseName, "db2")
    }

    func test_reacceptingSameRedisAdapterInvalidatesCapturedTabContexts() throws {
        let connectionID = UUID(uuidString: "11111111-1111-1111-1111-111111111111")!
        let adapter = RedisAdapter()
        let state = AppState()
        establishRedisContext(
            on: state,
            adapter: adapter,
            connectionID: connectionID,
            databaseName: "db0"
        )
        state.openRedisFlatKeyList()
        let flatTabID = try XCTUnwrap(state.activeTabId)
        state.openRedisKeyDetail(key: "session:42")
        let detailTabID = try XCTUnwrap(state.activeTabId)
        let flatContext = try XCTUnwrap(
            state.tabs.first(where: { $0.id == flatTabID })?.redisContext
        )
        let detailContext = try XCTUnwrap(
            state.tabs.first(where: { $0.id == detailTabID })?.redisContext
        )

        state.activeAdapter = nil
        state.activeConnectionId = nil
        state.currentDatabaseName = nil
        establishRedisContext(
            on: state,
            adapter: adapter,
            connectionID: connectionID,
            databaseName: "db0"
        )

        XCTAssertNil(state.activeRedisAdapter(for: flatContext))
        XCTAssertNil(state.activeRedisAdapter(for: detailContext))
        XCTAssertFalse(state.isRedisFlatKeyListOpen)
    }

    func test_redisSessionABA_doesNotReauthorizeFirstAdapterContexts() throws {
        let connectionID = UUID(uuidString: "11111111-1111-1111-1111-111111111111")!
        let firstAdapter = RedisAdapter()
        let interveningAdapter = RedisAdapter()
        let state = AppState()
        establishRedisContext(
            on: state,
            adapter: firstAdapter,
            connectionID: connectionID,
            databaseName: "db0"
        )
        state.openRedisKeyDetail(key: "session:42")
        let firstTabID = try XCTUnwrap(state.activeTabId)
        let firstContext = try XCTUnwrap(
            state.tabs.first(where: { $0.id == firstTabID })?.redisContext
        )

        establishRedisContext(
            on: state,
            adapter: interveningAdapter,
            connectionID: connectionID,
            databaseName: "db0"
        )
        XCTAssertNil(state.activeRedisAdapter(for: firstContext))

        establishRedisContext(
            on: state,
            adapter: firstAdapter,
            connectionID: connectionID,
            databaseName: "db0"
        )

        XCTAssertNil(state.activeRedisAdapter(for: firstContext))
    }

    func test_openRedisTabsRefuseIncompleteContext() {
        let connectionID = UUID(uuidString: "11111111-1111-1111-1111-111111111111")!

        let missingEverything = AppState()
        missingEverything.openRedisFlatKeyList()
        missingEverything.openRedisKeyDetail(key: "session:42")
        XCTAssertTrue(missingEverything.tabs.isEmpty)
        XCTAssertNil(missingEverything.activeTabId)
        XCTAssertFalse(missingEverything.isRedisFlatKeyListOpen)

        let missingAdapter = AppState()
        missingAdapter.activeConnectionId = connectionID
        missingAdapter.currentDatabaseName = "db0"
        missingAdapter.openRedisFlatKeyList()
        missingAdapter.openRedisKeyDetail(key: "session:42")
        XCTAssertTrue(missingAdapter.tabs.isEmpty)

        let missingConnection = AppState()
        missingConnection.activeAdapter = RedisAdapter()
        missingConnection.currentDatabaseName = "db0"
        missingConnection.openRedisFlatKeyList()
        missingConnection.openRedisKeyDetail(key: "session:42")
        XCTAssertTrue(missingConnection.tabs.isEmpty)

        let missingDatabase = AppState()
        missingDatabase.activeAdapter = RedisAdapter()
        missingDatabase.activeConnectionId = connectionID
        missingDatabase.openRedisFlatKeyList()
        missingDatabase.openRedisKeyDetail(key: "session:42")
        XCTAssertTrue(missingDatabase.tabs.isEmpty)
    }

    func test_flatKeyListOpenStateBecomesFalseAfterCloseAndCanReopen() throws {
        let state = AppState()
        establishRedisContext(
            on: state,
            adapter: RedisAdapter(),
            connectionID: UUID(uuidString: "11111111-1111-1111-1111-111111111111")!,
            databaseName: "db3"
        )

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

    private func establishRedisContext(
        on state: AppState,
        adapter: RedisAdapter,
        connectionID: UUID,
        databaseName: String
    ) {
        state.activeAdapter = adapter
        state.activeConnectionId = connectionID
        state.currentDatabaseName = databaseName
    }

    private func redisConfig() -> ConnectionConfig {
        ConnectionConfig(name: "Redis", databaseType: .redis)
    }
}

private enum ExpectedTransitionError: Error {
    case selectFailed
}

private actor AsyncTestGate {
    private var hasEntered = false
    private var hasBeenReleased = false
    private var entryWaiters: [CheckedContinuation<Void, Never>] = []
    private var releaseWaiter: CheckedContinuation<Void, Never>?

    func waitUntilEntered() async {
        guard !hasEntered else { return }
        await withCheckedContinuation { continuation in
            entryWaiters.append(continuation)
        }
    }

    func enteredSnapshot() -> Bool {
        hasEntered
    }

    func enterAndWait() async {
        hasEntered = true
        let waiters = entryWaiters
        entryWaiters.removeAll()
        waiters.forEach { $0.resume() }

        guard !hasBeenReleased else { return }
        await withCheckedContinuation { continuation in
            releaseWaiter = continuation
        }
    }

    func release() {
        hasBeenReleased = true
        releaseWaiter?.resume()
        releaseWaiter = nil
    }
}

private actor AsyncTestEventRecorder {
    private var events: [String] = []

    func record(_ event: String) {
        events.append(event)
    }

    func snapshot() -> [String] {
        events
    }
}
