// RedisDataGridLifecycleTests.swift
// Gridex

import XCTest
@testable import Gridex

@MainActor
final class RedisDataGridLifecycleTests: XCTestCase {

    func test_openRedisFlatKeyListBindsExactTabContextToCachedGridBeforeDisplay() throws {
        let appState = AppState()
        establishRedisContext(on: appState, databaseName: "db0")
        let expectedContext = try XCTUnwrap(appState.currentRedisContext)

        appState.openRedisFlatKeyList()

        let tab = try XCTUnwrap(appState.tabs.first)
        XCTAssertEqual(tab.redisContext, expectedContext)
        XCTAssertEqual(
            appState.cachedDataGridState(for: tab.id).redisContext,
            expectedContext
        )
    }

    func test_redisContextMakesGridReadOnlyAndAllMutationEntryPointsNoOp() async {
        let state = DataGridViewState()
        state.bindRedisContext(redisContext(databaseName: "db0", revision: 1))
        state.columns = [ColumnHeader(name: "key", dataType: "string")]
        state.rows = [[.string("session:42")]]
        state.rebuildDisplayCache()

        XCTAssertFalse(state.allowsMutations)

        state.commitCellEdit(rowIndex: 0, colIdx: 0, newText: "changed")
        state.commitDateEdit(rowIndex: 0, colIdx: 0, newValue: .string("dated"))
        state.addNewRow()
        state.commitNewRowEdit(rowIndex: 0, colIdx: 0, newText: "inserted")
        state.selectedRows = [0]
        state.deleteSelectedRows()
        state.prepareCommit()
        await state.executeCommit()

        XCTAssertEqual(state.rows, [[.string("session:42")]])
        XCTAssertEqual(state.totalRows, 0)
        XCTAssertTrue(state.insertedRowIndices.isEmpty)
        XCTAssertFalse(state.hasPendingChanges)
        XCTAssertTrue(state.commitSQL.isEmpty)
        XCTAssertFalse(state.showCommitPreview)

        XCTAssertTrue(DataGridViewState().allowsMutations)
    }

    func test_liveRedisLoadUsesOneOperationOnlyForTheExactBoundContext() async throws {
        DataGridViewState.clearMetadataCache()
        let appState = AppState()
        let adapter = establishRedisContext(on: appState, databaseName: "db0")
        let staleContext = try XCTUnwrap(appState.currentRedisContext)
        var fetchCount = 0
        var fetchedWithActiveAdapter = false
        let state = DataGridViewState { receivedAdapter, _ in
            fetchCount += 1
            fetchedWithActiveAdapter = receivedAdapter === adapter
            return self.queryResult(key: "current:key")
        }
        state.appState = appState
        state.bindRedisContext(staleContext)

        appState.currentDatabaseName = "db1"
        await state.load(tableName: "Keys", schema: nil, adapter: adapter)

        XCTAssertEqual(fetchCount, 0)
        XCTAssertTrue(state.rows.isEmpty)
        XCTAssertFalse(state.isLoading)

        let currentContext = try XCTUnwrap(appState.currentRedisContext)
        state.bindRedisContext(currentContext)
        await state.load(tableName: "Keys", schema: nil, adapter: adapter)

        XCTAssertEqual(fetchCount, 1)
        XCTAssertTrue(fetchedWithActiveAdapter)
        XCTAssertEqual(state.redisContext, currentContext)
        XCTAssertEqual(state.rows, [[.string("current:key")]])
        XCTAssertFalse(state.isLoading)
    }

    func test_fullAndPageLoadsShareLatestOwnershipWhenOlderFailureCompletesFirst() async {
        let state = DataGridViewState()
        let context = redisContext(databaseName: "db0", revision: 1)
        let olderGate = RedisOwnershipTestGate()
        let newerGate = RedisOwnershipTestGate()
        state.bindRedisContext(context)

        await state.performRedisLoad(in: context) {
            Optional<Result<QueryResult, Error>>.some(
                .success(self.queryResult(key: "retained:key"))
            )
        }

        let fullLoad = Task { @MainActor in
            await state.performRedisLoad(in: context) {
                await olderGate.enterAndWait()
                return Optional<Result<QueryResult, Error>>.some(
                    .failure(ExpectedRedisDataGridError.obsolete)
                )
            }
        }
        await olderGate.waitUntilEntered()

        let pageLoad = Task { @MainActor in
            await state.performRedisLoad(in: context) {
                await newerGate.enterAndWait()
                return Optional<Result<QueryResult, Error>>.some(
                    .success(self.queryResult(key: "page:key"))
                )
            }
        }
        await newerGate.waitUntilEntered()

        await olderGate.release()
        await fullLoad.value

        XCTAssertTrue(state.isLoading)
        XCTAssertEqual(state.rows, [[.string("retained:key")]])
        XCTAssertNil(state.errorMessage)

        await newerGate.release()
        await pageLoad.value

        XCTAssertFalse(state.isLoading)
        XCTAssertEqual(state.rows, [[.string("page:key")]])
        XCTAssertNil(state.errorMessage)
    }

    func test_olderFullLoadCompletingLastCannotReplaceNewerPageFailure() async {
        let state = DataGridViewState()
        let context = redisContext(databaseName: "db0", revision: 1)
        let olderGate = RedisOwnershipTestGate()
        state.bindRedisContext(context)

        await state.performRedisLoad(in: context) {
            Optional<Result<QueryResult, Error>>.some(
                .success(self.queryResult(key: "retained:key"))
            )
        }

        let fullLoad = Task { @MainActor in
            await state.performRedisLoad(in: context) {
                await olderGate.enterAndWait()
                return Optional<Result<QueryResult, Error>>.some(
                    .success(self.queryResult(key: "obsolete:key"))
                )
            }
        }
        await olderGate.waitUntilEntered()

        await state.performRedisLoad(in: context) {
            Optional<Result<QueryResult, Error>>.some(
                .failure(ExpectedRedisDataGridError.current)
            )
        }

        XCTAssertFalse(state.isLoading)
        XCTAssertEqual(state.rows, [[.string("retained:key")]])
        XCTAssertEqual(state.errorMessage, "current page failed")

        await olderGate.release()
        await fullLoad.value

        XCTAssertFalse(state.isLoading)
        XCTAssertEqual(state.rows, [[.string("retained:key")]])
        XCTAssertEqual(state.errorMessage, "current page failed")
    }

    func test_oldDb0LoadCannotPublishAfterDb1ThenNewDb0Context() async {
        let state = DataGridViewState()
        let oldDB0 = redisContext(databaseName: "db0", revision: 1)
        let db1 = redisContext(databaseName: "db1", revision: 2)
        let newDB0 = redisContext(databaseName: "db0", revision: 3)
        let oldGate = RedisOwnershipTestGate()
        state.bindRedisContext(oldDB0)

        let oldLoad = Task { @MainActor in
            await state.performRedisLoad(in: oldDB0) {
                await oldGate.enterAndWait()
                return Optional<Result<QueryResult, Error>>.some(
                    .success(self.queryResult(key: "old-db0:key"))
                )
            }
        }
        await oldGate.waitUntilEntered()

        state.bindRedisContext(db1)
        XCTAssertTrue(state.rows.isEmpty)
        await state.performRedisLoad(in: db1) {
            Optional<Result<QueryResult, Error>>.some(
                .success(self.queryResult(key: "db1:key"))
            )
        }
        state.bindRedisContext(newDB0)
        XCTAssertTrue(state.rows.isEmpty)
        await state.performRedisLoad(in: newDB0) {
            Optional<Result<QueryResult, Error>>.some(
                .success(self.queryResult(key: "new-db0:key"))
            )
        }

        await oldGate.release()
        await oldLoad.value

        XCTAssertEqual(state.redisContext, newDB0)
        XCTAssertEqual(state.rows, [[.string("new-db0:key")]])
        XCTAssertNil(state.errorMessage)
        XCTAssertFalse(state.isLoading)
    }

    func test_filteredRedisLoadsKeepTotalUnknownAndNeverReusePageCount() async throws {
        DataGridViewState.clearMetadataCache()
        let appState = AppState()
        let adapter = establishRedisContext(on: appState, databaseName: "db0")
        let db0 = try XCTUnwrap(appState.currentRedisContext)
        var fetchCount = 0

        let makeState: () -> DataGridViewState = {
            let state = DataGridViewState { _, _ in
                fetchCount += 1
                return self.queryResult(key: "result-\(fetchCount)")
            }
            state.appState = appState
            state.activeFilter = FilterExpression(
                conditions: [
                    FilterCondition(
                        column: "key",
                        op: .like,
                        value: .string("session:*")
                    )
                ],
                combinator: .and
            )
            return state
        }

        let firstState = makeState()
        firstState.bindRedisContext(db0)
        await firstState.load(tableName: "Keys", schema: nil, adapter: adapter)

        XCTAssertEqual(fetchCount, 1)
        XCTAssertEqual(firstState.totalRows, 0)
        XCTAssertNil(firstState.statusRowCount)

        firstState.totalRows = 9_999
        await firstState.loadPage(1)

        XCTAssertEqual(fetchCount, 2)
        XCTAssertEqual(firstState.rows, [[.string("result-2")]])
        XCTAssertEqual(firstState.totalRows, 0)
        XCTAssertNil(firstState.statusRowCount)

        appState.currentDatabaseName = "db1"
        let db1 = try XCTUnwrap(appState.currentRedisContext)
        let secondState = makeState()
        secondState.bindRedisContext(db1)
        await secondState.load(tableName: "Keys", schema: nil, adapter: adapter)

        XCTAssertEqual(fetchCount, 3)
        XCTAssertEqual(secondState.rows, [[.string("result-3")]])
        XCTAssertEqual(secondState.totalRows, 0)
        XCTAssertNil(secondState.statusRowCount)
    }

    @discardableResult
    private func establishRedisContext(
        on state: AppState,
        databaseName: String
    ) -> RedisAdapter {
        let adapter = RedisAdapter()
        state.activeConnectionId =
            UUID(uuidString: "11111111-1111-1111-1111-111111111111")!
        state.activeAdapter = adapter
        state.currentDatabaseName = databaseName
        return adapter
    }

    private func redisContext(
        databaseName: String,
        revision: UInt64
    ) -> AppState.RedisTabContext {
        AppState.RedisTabContext(
            connectionID: UUID(uuidString: "11111111-1111-1111-1111-111111111111")!,
            databaseName: databaseName,
            sessionID: UUID(uuidString: "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA")!,
            databaseRevision: revision
        )
    }

    private func queryResult(key: String) -> QueryResult {
        QueryResult(
            columns: [ColumnHeader(name: "key", dataType: "string")],
            rows: [[.string(key)]],
            rowsAffected: 0,
            executionTime: 0.01,
            queryType: .select
        )
    }
}

private enum ExpectedRedisDataGridError: LocalizedError {
    case current
    case obsolete

    var errorDescription: String? {
        switch self {
        case .current:
            "current page failed"
        case .obsolete:
            "obsolete full load failed"
        }
    }
}
