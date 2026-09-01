import Combine
import Foundation
import XCTest
@testable import Gridex

@MainActor
final class DataGridRefreshTests: XCTestCase {
    func test_reloadAfterStructureChange_preservesAscendingHeaderSort() async throws {
        let (grid, adapter) = try await makeGrid()
        grid.sortColumn = "rank"
        grid.sortAscending = true
        await grid.loadPage(0)
        XCTAssertEqual(grid.rows.compactMap { $0[1].intValue }, [10, 20, 30])

        await grid.reloadAfterStructureChange()

        XCTAssertEqual(grid.sortColumn, "rank")
        XCTAssertTrue(grid.sortAscending)
        XCTAssertEqual(grid.rows.compactMap { $0[1].intValue }, [10, 20, 30])
        try await adapter.disconnect()
    }

    func test_reloadAfterStructureChange_preservesDescendingHeaderSort() async throws {
        let (grid, adapter) = try await makeGrid()
        grid.sortColumn = "rank"
        grid.sortAscending = false
        await grid.loadPage(0)
        XCTAssertEqual(grid.rows.compactMap { $0[1].intValue }, [30, 20, 10])

        await grid.reloadAfterStructureChange()

        XCTAssertEqual(grid.sortColumn, "rank")
        XCTAssertFalse(grid.sortAscending)
        XCTAssertEqual(grid.rows.compactMap { $0[1].intValue }, [30, 20, 10])
        try await adapter.disconnect()
    }

    func test_reloadAfterStructureChange_clearsRenamedHeaderSortAndLoadsFreshRows() async throws {
        let (grid, adapter) = try await makeGrid()
        grid.sortColumn = "rank"
        await grid.loadPage(0)
        XCTAssertEqual(grid.rows.compactMap { $0[1].intValue }, [10, 20, 30])

        _ = try await adapter.executeRaw(sql: "ALTER TABLE scores RENAME COLUMN rank TO score")
        await grid.reloadAfterStructureChange()

        XCTAssertNil(grid.sortColumn)
        XCTAssertEqual(grid.columns.map(\.name), ["id", "score"])
        XCTAssertEqual(grid.rows.compactMap { $0[1].intValue }, [30, 10, 20])
        try await adapter.disconnect()
    }

    func test_reloadAfterStructureChange_usesRenamedPrimaryKeyWhenNoHeaderSortIsSelected() async throws {
        let (grid, adapter) = try await makeTextPrimaryKeyGrid()
        XCTAssertNil(grid.sortColumn)
        XCTAssertEqual(grid.primaryKeyColumns, ["id"])
        await grid.loadPage(0)
        XCTAssertEqual(grid.rows.compactMap { $0[0].stringValue }, ["a", "b", "c"])

        _ = try await adapter.executeRaw(sql: "ALTER TABLE text_scores RENAME COLUMN id TO record_id")
        await grid.reloadAfterStructureChange()

        XCTAssertNil(grid.sortColumn)
        XCTAssertEqual(grid.primaryKeyColumns, ["record_id"])
        XCTAssertEqual(grid.columns.map(\.name), ["record_id", "score"])
        XCTAssertEqual(grid.rows.compactMap { $0[0].stringValue }, ["a", "b", "c"])
        try await adapter.disconnect()
    }

    func test_reloadAfterStructureChange_coalescesInvalidSortObserverIntoOneRowFetch() async throws {
        let (grid, adapter) = try await makeCountingGrid()
        grid.sortColumn = "rank"
        await grid.loadPage(0)
        adapter.resetCounts()
        adapter.fetchDelayNanoseconds = 50_000_000

        let observerLoad = expectation(description: "mounted sort observer reloads page")
        let sortObserver = grid.$sortColumn
            .dropFirst()
            .sink { sortColumn in
                Task { @MainActor in
                    await grid.handleSortColumnChange(sortColumn)
                    observerLoad.fulfill()
                }
            }

        _ = try await adapter.executeRaw(sql: "ALTER TABLE scores RENAME COLUMN rank TO score")
        await grid.reloadAfterStructureChange()
        await fulfillment(of: [observerLoad], timeout: 1)

        XCTAssertEqual(adapter.fetchRowsCount, 1)
        withExtendedLifetime(sortObserver) {}
        try await adapter.disconnect()
    }

    func test_invalidSortObserverDoesNotOverrideRefreshReplayPage() async throws {
        let (grid, adapter) = try await makeCountingGrid()
        let appState = AppState()
        grid.appState = appState
        grid.sortColumn = "rank"
        await grid.loadPage(0)
        adapter.resetCounts()
        appState.clearQueryLog()

        let observerGate = AsyncGate()
        let observerStarted = AsyncSignal()
        let observerFinished = expectation(description: "delayed sort observer finishes")
        let sortObserver = grid.$sortColumn
            .dropFirst()
            .sink { sortColumn in
                Task { @MainActor in
                    observerStarted.signal()
                    await observerGate.wait()
                    await grid.handleSortColumnChange(from: "rank", to: sortColumn)
                    observerFinished.fulfill()
                }
            }

        _ = try await adapter.executeRaw(sql: "ALTER TABLE scores RENAME COLUMN rank TO score")
        let descriptionGate = adapter.holdNextDescribeTable()
        let structureReload = Task { @MainActor in
            await grid.reloadAfterStructureChange()
        }
        await adapter.waitForHeldDescribeTable()

        await grid.loadPage(1)
        XCTAssertEqual(grid.currentPage, 1)

        descriptionGate.open()
        await observerStarted.wait()
        await structureReload.value

        let replayLogs = appState.queryLog.filter { $0.sql.hasPrefix("SELECT * FROM") }
        XCTAssertEqual(adapter.fetchRowsCount, 2)
        XCTAssertEqual(grid.currentPage, 1)
        XCTAssertEqual(replayLogs.count, 2)
        XCTAssertTrue(replayLogs.allSatisfy { $0.sql.contains("OFFSET 300") })

        observerGate.open()
        await fulfillment(of: [observerFinished], timeout: 1)

        let finalRowLogs = appState.queryLog.filter { $0.sql.hasPrefix("SELECT * FROM") }
        XCTAssertEqual(adapter.fetchRowsCount, 2)
        XCTAssertEqual(grid.currentPage, 1)
        XCTAssertEqual(finalRowLogs.count, 2)
        XCTAssertTrue(finalRowLogs.allSatisfy { $0.sql.contains("OFFSET 300") })
        withExtendedLifetime(sortObserver) {}
        try await adapter.disconnect()
    }

    func test_reloadAfterStructureChange_describesTableOnce() async throws {
        let (grid, adapter) = try await makeCountingGrid()
        adapter.resetCounts()

        await grid.reloadAfterStructureChange()

        XCTAssertEqual(adapter.describeTableCount, 1)
        try await adapter.disconnect()
    }

    func test_reloadAfterStructureChange_replaysLateUserSortAndFilterRequest() async throws {
        let (grid, adapter) = try await makeCountingGrid()
        grid.sortColumn = "rank"
        grid.sortAscending = true
        await grid.loadPage(0)

        let fetchGate = adapter.holdNextFetchAndCaptureTableDescription()
        let structureReload = Task { @MainActor in
            await grid.reloadAfterStructureChange()
        }
        await adapter.waitForHeldFetch()

        grid.sortAscending = false
        grid.activeFilter = FilterExpression(
            conditions: [FilterCondition(column: "rank", op: .greaterOrEqual, value: .integer(20))],
            combinator: .and
        )
        let sortRequestStarted = AsyncSignal()
        let filterRequestStarted = AsyncSignal()
        let lateSortRequest = Task { @MainActor in
            await Task.yield()
            sortRequestStarted.signal()
            await grid.loadPage(0)
        }
        let lateFilterRequest = Task { @MainActor in
            await Task.yield()
            filterRequestStarted.signal()
            await grid.applyFilter()
        }
        await sortRequestStarted.wait()
        await filterRequestStarted.wait()

        fetchGate.open()
        await structureReload.value
        await lateSortRequest.value
        await lateFilterRequest.value

        XCTAssertFalse(grid.sortAscending)
        XCTAssertEqual(grid.rows.compactMap { $0[1].intValue }, [30, 20])
        try await adapter.disconnect()
    }

    func test_initialLoadPublishesMetadataAndCacheAfterLaterFilteredSortRequest() async throws {
        DataGridViewState.clearMetadataCache()
        let adapter = CountingSQLiteAdapter()
        let config = ConnectionConfig(
            name: "initial-load-metadata-race",
            databaseType: .sqlite,
            filePath: ":memory:"
        )
        try await adapter.connect(config: config, password: nil)
        _ = try await adapter.executeRaw(sql: "CREATE TABLE scores (id INTEGER PRIMARY KEY, rank INTEGER NOT NULL)")
        _ = try await adapter.executeRaw(sql: "INSERT INTO scores (id, rank) VALUES (1, 30), (2, 10), (3, 20)")

        let appState = AppState()
        appState.activeAdapter = adapter
        appState.activeConnectionId = config.id
        appState.activeConfig = config
        appState.currentDatabaseName = "main"

        let grid = DataGridViewState()
        grid.appState = appState
        let descriptionGate = adapter.holdNextDescribeTable()
        let initialLoad = Task { @MainActor in
            await grid.load(tableName: "scores", schema: nil, adapter: adapter)
        }
        await adapter.waitForHeldDescribeTable()

        XCTAssertFalse(grid.isLoading)
        XCTAssertEqual(grid.rows.compactMap { $0[1].intValue }, [30, 10, 20])

        grid.sortColumn = "rank"
        grid.sortAscending = false
        grid.activeFilter = FilterExpression(
            conditions: [FilterCondition(column: "rank", op: .greaterOrEqual, value: .integer(20))],
            combinator: .and
        )
        await grid.applyFilter()
        XCTAssertEqual(grid.rows.compactMap { $0[1].intValue }, [30, 20])

        descriptionGate.open()
        await initialLoad.value

        XCTAssertEqual(grid.primaryKeyColumns, ["id"])
        XCTAssertEqual(grid.tableDescription?.columns.map(\.name), ["id", "rank"])

        adapter.failNextDescribeTable()
        let cacheProbe = DataGridViewState()
        cacheProbe.appState = appState
        await cacheProbe.load(tableName: "scores", schema: nil, adapter: adapter)
        XCTAssertEqual(cacheProbe.primaryKeyColumns, ["id"])
        XCTAssertEqual(cacheProbe.tableDescription?.columns.map(\.name), ["id", "rank"])
        await Task.yield()
        try await adapter.disconnect()
    }

    func test_newerFullLoadMetadataRemainsAuthoritativeWhenOlderLoadCompletesLast() async throws {
        DataGridViewState.clearMetadataCache()
        let adapter = CountingSQLiteAdapter(databaseType: .postgresql)
        let config = ConnectionConfig(
            name: "same-scope-full-load-metadata-race",
            databaseType: .sqlite,
            filePath: ":memory:"
        )
        try await adapter.connect(config: config, password: nil)
        _ = try await adapter.executeRaw(sql: "CREATE TABLE metadata_race (id TEXT PRIMARY KEY, status TEXT, parent_id TEXT)")
        _ = try await adapter.executeRaw(sql: "INSERT INTO metadata_race (id, status, parent_id) VALUES ('old-row', 'old', 'old-parent')")

        let appState = AppState()
        appState.activeAdapter = adapter
        appState.activeConnectionId = config.id
        appState.activeConfig = config
        appState.currentDatabaseName = "main"

        let olderDescription = makeMetadataRaceDescription(
            primaryKey: "id",
            defaultValue: "'old'",
            referencedTable: "old_parent",
            referencedColumn: "old_id"
        )
        let newerDescription = makeMetadataRaceDescription(
            primaryKey: "parent_id",
            defaultValue: "'new'",
            referencedTable: "new_parent",
            referencedColumn: "new_id"
        )
        let olderDescriptionRequest = adapter.enqueueHeldDescription(olderDescription)
        let newerDescriptionRequest = adapter.enqueueHeldDescription(newerDescription)
        let olderEnumRequest = adapter.enqueueHeldEnumValues(["status": ["old_a", "old_b"]])
        let newerEnumRequest = adapter.enqueueHeldEnumValues(["status": ["new_a", "new_b"]])

        let grid = DataGridViewState()
        grid.appState = appState
        let olderLoad = Task { @MainActor in
            await grid.load(tableName: "metadata_race", schema: nil, adapter: adapter)
        }
        await olderDescriptionRequest.started.wait()
        await olderEnumRequest.started.wait()

        _ = try await adapter.executeRaw(sql: "DELETE FROM metadata_race")
        _ = try await adapter.executeRaw(sql: "INSERT INTO metadata_race (id, status, parent_id) VALUES ('new-row', 'new', 'new-parent')")
        let newerLoad = Task { @MainActor in
            await grid.load(tableName: "metadata_race", schema: nil, adapter: adapter)
        }
        await newerDescriptionRequest.started.wait()
        await newerEnumRequest.started.wait()

        newerDescriptionRequest.gate.open()
        newerEnumRequest.gate.open()
        await newerLoad.value

        XCTAssertEqual(grid.rows.first?.first?.stringValue, "new-row")
        XCTAssertEqual(grid.primaryKeyColumns, ["parent_id"])
        XCTAssertEqual(grid.tableDescription, newerDescription)
        XCTAssertEqual(grid.columnDefaults["status"], "'new'")
        XCTAssertEqual(grid.foreignKeyColumns["parent_id"], "new_parent")
        XCTAssertEqual(grid.foreignKeyRefColumns["parent_id"], "new_id")
        XCTAssertEqual(grid.columnEnumValues["status"], ["new_a", "new_b"])

        olderDescriptionRequest.gate.open()
        olderEnumRequest.gate.open()
        await olderLoad.value

        XCTAssertEqual(grid.rows.first?.first?.stringValue, "new-row")
        XCTAssertEqual(grid.primaryKeyColumns, ["parent_id"])
        XCTAssertEqual(grid.tableDescription, newerDescription)
        XCTAssertEqual(grid.columnDefaults["status"], "'new'")
        XCTAssertEqual(grid.foreignKeyColumns["parent_id"], "new_parent")
        XCTAssertEqual(grid.foreignKeyRefColumns["parent_id"], "new_id")
        XCTAssertEqual(grid.columnEnumValues["status"], ["new_a", "new_b"])

        adapter.failNextDescribeTable()
        let cacheProbe = DataGridViewState()
        cacheProbe.appState = appState
        await cacheProbe.load(tableName: "metadata_race", schema: nil, adapter: adapter)
        XCTAssertEqual(cacheProbe.primaryKeyColumns, ["parent_id"])
        XCTAssertEqual(cacheProbe.tableDescription, newerDescription)
        XCTAssertEqual(cacheProbe.columnDefaults["status"], "'new'")
        XCTAssertEqual(cacheProbe.foreignKeyColumns["parent_id"], "new_parent")
        XCTAssertEqual(cacheProbe.foreignKeyRefColumns["parent_id"], "new_id")
        XCTAssertEqual(cacheProbe.columnEnumValues["status"], ["new_a", "new_b"])
        await Task.yield()
        try await adapter.disconnect()
    }

    func test_structureRefreshPublishesMetadataAndCacheWhileLaterFilteredSortWins() async throws {
        let (grid, adapter) = try await makeCountingGrid()
        let config = ConnectionConfig(
            name: "structure-refresh-metadata-race",
            databaseType: .sqlite,
            filePath: ":memory:"
        )
        let appState = AppState()
        appState.activeAdapter = adapter
        appState.activeConnectionId = config.id
        appState.activeConfig = config
        appState.currentDatabaseName = "main"
        grid.appState = appState
        await grid.load(tableName: "scores", schema: nil, adapter: adapter)

        grid.sortColumn = "rank"
        grid.sortAscending = true
        await grid.loadPage(0)
        _ = try await adapter.executeRaw(sql: "ALTER TABLE scores RENAME COLUMN id TO record_id")

        let descriptionGate = adapter.holdNextDescribeTable()
        let structureReload = Task { @MainActor in
            await grid.reloadAfterStructureChange()
        }
        await adapter.waitForHeldDescribeTable()

        grid.sortAscending = false
        grid.activeFilter = FilterExpression(
            conditions: [FilterCondition(column: "rank", op: .greaterOrEqual, value: .integer(20))],
            combinator: .and
        )
        await grid.applyFilter()
        XCTAssertEqual(grid.rows.compactMap { $0[1].intValue }, [30, 20])

        descriptionGate.open()
        await structureReload.value

        XCTAssertEqual(grid.sortColumn, "rank")
        XCTAssertFalse(grid.sortAscending)
        XCTAssertEqual(grid.rows.compactMap { $0[1].intValue }, [30, 20])
        XCTAssertEqual(grid.primaryKeyColumns, ["record_id"])
        XCTAssertEqual(grid.tableDescription?.columns.map(\.name), ["record_id", "rank"])

        adapter.failNextDescribeTable()
        let cacheProbe = DataGridViewState()
        cacheProbe.appState = appState
        await cacheProbe.load(tableName: "scores", schema: nil, adapter: adapter)
        XCTAssertEqual(cacheProbe.primaryKeyColumns, ["record_id"])
        XCTAssertEqual(cacheProbe.tableDescription?.columns.map(\.name), ["record_id", "rank"])
        await Task.yield()
        try await adapter.disconnect()
    }

    func test_structureRefreshReplaysLateFilterAfterImplicitPrimaryKeyRename() async throws {
        DataGridViewState.clearMetadataCache()
        let adapter = CountingSQLiteAdapter()
        let config = ConnectionConfig(
            name: "structure-refresh-implicit-primary-key-race",
            databaseType: .sqlite,
            filePath: ":memory:"
        )
        try await adapter.connect(config: config, password: nil)
        _ = try await adapter.executeRaw(sql: "CREATE TABLE text_scores (id TEXT PRIMARY KEY, score INTEGER NOT NULL)")
        _ = try await adapter.executeRaw(sql: "INSERT INTO text_scores (id, score) VALUES ('c', 30), ('a', 10), ('b', 20)")

        let grid = DataGridViewState()
        await grid.load(tableName: "text_scores", schema: nil, adapter: adapter)
        XCTAssertNil(grid.sortColumn)
        XCTAssertEqual(grid.primaryKeyColumns, ["id"])
        await grid.loadPage(0)
        XCTAssertEqual(grid.rows.compactMap { $0[0].stringValue }, ["a", "b", "c"])

        _ = try await adapter.executeRaw(sql: "ALTER TABLE text_scores RENAME COLUMN id TO record_id")
        let descriptionGate = adapter.holdNextDescribeTable()
        let structureReload = Task { @MainActor in
            await grid.reloadAfterStructureChange()
        }
        await adapter.waitForHeldDescribeTable()

        grid.activeFilter = FilterExpression(
            conditions: [FilterCondition(column: "score", op: .greaterOrEqual, value: .integer(20))],
            combinator: .and
        )
        await grid.applyFilter()

        descriptionGate.open()
        await structureReload.value

        XCTAssertNil(grid.sortColumn)
        XCTAssertEqual(grid.primaryKeyColumns, ["record_id"])
        XCTAssertEqual(grid.columns.map(\.name), ["record_id", "score"])
        XCTAssertEqual(grid.rows.compactMap { $0[0].stringValue }, ["b", "c"])
        XCTAssertEqual(grid.rows.compactMap { $0[1].intValue }, [20, 30])
        try await adapter.disconnect()
    }

    func test_reloadAfterStructureChange_discardsOlderLoadResultsAndMetadataCache() async throws {
        let (grid, adapter) = try await makeCountingGrid()
        DataGridViewState.clearMetadataCache()
        let appState = AppState()
        let config = ConnectionConfig(
            name: "shared-refresh-cache-scope",
            databaseType: .sqlite,
            filePath: ":memory:"
        )
        appState.activeAdapter = adapter
        appState.activeConnectionId = config.id
        appState.activeConfig = config
        appState.currentDatabaseName = "main"
        grid.appState = appState
        await grid.load(tableName: "scores", schema: nil, adapter: adapter)

        let fetchGate = adapter.holdNextFetchAndCaptureTableDescription()
        let olderLoad = Task { @MainActor in
            await grid.load(tableName: "scores", schema: nil, adapter: adapter)
        }
        await adapter.waitForHeldFetch()

        _ = try await adapter.executeRaw(sql: "ALTER TABLE scores RENAME COLUMN id TO record_id")
        await grid.reloadAfterStructureChange()
        XCTAssertEqual(grid.columns.map(\.name), ["record_id", "rank"])
        XCTAssertEqual(grid.primaryKeyColumns, ["record_id"])
        XCTAssertEqual(grid.tableDescription?.columns.map(\.name), ["record_id", "rank"])

        fetchGate.open()
        await olderLoad.value

        XCTAssertEqual(grid.columns.map(\.name), ["record_id", "rank"])
        XCTAssertEqual(grid.primaryKeyColumns, ["record_id"])
        XCTAssertEqual(grid.tableDescription?.columns.map(\.name), ["record_id", "rank"])

        let cacheProbe = DataGridViewState()
        cacheProbe.appState = appState
        await cacheProbe.load(tableName: "scores", schema: nil, adapter: adapter)
        XCTAssertEqual(cacheProbe.primaryKeyColumns, ["record_id"])
        XCTAssertEqual(cacheProbe.tableDescription?.columns.map(\.name), ["record_id", "rank"])
        try await adapter.disconnect()
    }

    func test_metadataCache_isolatesSameNamedTablesOnDifferentConnections() async throws {
        DataGridViewState.clearMetadataCache()
        let (firstGrid, firstAdapter) = try await makeConnectionGrid(
            name: "first-connection",
            createTableSQL: "CREATE TABLE scores (id INTEGER PRIMARY KEY, rank INTEGER NOT NULL)",
            insertSQL: "INSERT INTO scores (id, rank) VALUES (1, 30), (2, 10), (3, 20)"
        )
        let (secondGrid, secondAdapter) = try await makeConnectionGrid(
            name: "second-connection",
            createTableSQL: "CREATE TABLE scores (record_id TEXT PRIMARY KEY, rank INTEGER NOT NULL)",
            insertSQL: "INSERT INTO scores (record_id, rank) VALUES ('c', 30), ('a', 10), ('b', 20)"
        )

        await firstGrid.load(tableName: "scores", schema: nil, adapter: firstAdapter)
        await secondGrid.load(tableName: "scores", schema: nil, adapter: secondAdapter)

        XCTAssertEqual(firstGrid.primaryKeyColumns, ["id"])
        XCTAssertEqual(secondGrid.primaryKeyColumns, ["record_id"])
        XCTAssertEqual(secondGrid.tableDescription?.columns.map(\.name), ["record_id", "rank"])
        try await firstAdapter.disconnect()
        try await secondAdapter.disconnect()
    }

    func test_structureRefreshOnOneConnection_doesNotCancelSameNamedTablePageLoadOnAnother() async throws {
        let (firstGrid, firstAdapter) = try await makeCountingGrid()
        let (secondGrid, secondAdapter) = try await makeCountingGrid(clearingMetadataCache: false)

        let fetchGate = secondAdapter.holdNextFetchAndCaptureTableDescription()
        let secondPageLoad = Task { @MainActor in
            await secondGrid.loadPage(0)
        }
        await secondAdapter.waitForHeldFetch()

        await firstGrid.reloadAfterStructureChange()
        fetchGate.open()
        await secondPageLoad.value

        XCTAssertFalse(secondGrid.isLoading)
        XCTAssertEqual(secondGrid.rows.compactMap { $0[1].intValue }, [30, 10, 20])
        try await firstAdapter.disconnect()
        try await secondAdapter.disconnect()
    }

    func test_structureRefreshOnSameLogicalTable_settlesOtherGridHeldPageLoad() async throws {
        let (firstGrid, adapter) = try await makeCountingGrid()
        let config = ConnectionConfig(
            name: "shared-loading-owner-scope",
            databaseType: .sqlite,
            filePath: ":memory:"
        )
        let appState = AppState()
        appState.activeAdapter = adapter
        appState.activeConnectionId = config.id
        appState.activeConfig = config
        appState.currentDatabaseName = "main"
        firstGrid.appState = appState
        await firstGrid.load(tableName: "scores", schema: nil, adapter: adapter)

        let secondGrid = DataGridViewState()
        secondGrid.appState = appState
        await secondGrid.load(tableName: "scores", schema: nil, adapter: adapter)

        let fetchGate = adapter.holdNextFetchAndCaptureTableDescription()
        let secondPageLoad = Task { @MainActor in
            await secondGrid.loadPage(0)
        }
        await adapter.waitForHeldFetch()

        await firstGrid.reloadAfterStructureChange()
        fetchGate.open()
        await secondPageLoad.value

        XCTAssertFalse(secondGrid.isLoading)
        try await adapter.disconnect()
    }

    func test_metadataCache_distinguishesNilSchemaFromLiteralPublicSchema() async throws {
        let (defaultSchemaGrid, adapter) = try await makeCountingGrid()
        let appState = AppState()
        let config = ConnectionConfig(
            name: "shared-schema-cache-scope",
            databaseType: .sqlite,
            filePath: ":memory:"
        )
        appState.activeAdapter = adapter
        appState.activeConnectionId = config.id
        appState.activeConfig = config
        appState.currentDatabaseName = "main"
        defaultSchemaGrid.appState = appState
        await defaultSchemaGrid.load(tableName: "scores", schema: nil, adapter: adapter)

        adapter.setDescriptionPrimaryKey("rank", forSchema: "public")

        let literalPublicGrid = DataGridViewState()
        literalPublicGrid.appState = appState
        await literalPublicGrid.load(tableName: "scores", schema: "public", adapter: adapter)

        XCTAssertEqual(defaultSchemaGrid.primaryKeyColumns, ["id"])
        XCTAssertEqual(literalPublicGrid.primaryKeyColumns, ["rank"])
        try await adapter.disconnect()
    }

    func test_metadataCache_withoutAppState_usesPerGridFallbackScope() async throws {
        let (firstDatabaseGrid, adapter) = try await makeCountingGrid()
        adapter.selectLogicalMetadataDatabase(primaryKeyColumn: "rank")

        let secondDatabaseGrid = DataGridViewState()
        XCTAssertNil(firstDatabaseGrid.appState)
        XCTAssertNil(secondDatabaseGrid.appState)
        await secondDatabaseGrid.load(tableName: "scores", schema: nil, adapter: adapter)

        XCTAssertEqual(firstDatabaseGrid.primaryKeyColumns, ["id"])
        XCTAssertEqual(secondDatabaseGrid.primaryKeyColumns, ["rank"])
        try await adapter.disconnect()
    }

    func test_failedStructureRefreshSettlesSupersededPageLoad() async throws {
        let (grid, adapter) = try await makeCountingGrid()
        let fetchGate = adapter.holdNextFetchAndCaptureTableDescription()
        let pageLoad = Task { @MainActor in
            await grid.loadPage(0)
        }
        await adapter.waitForHeldFetch()

        adapter.failNextDescribeTable()
        await grid.reloadAfterStructureChange()
        fetchGate.open()
        await pageLoad.value

        XCTAssertFalse(grid.isLoading)
        try await adapter.disconnect()
    }

    func test_missedProgrammaticSortClearDoesNotSuppressLaterUserClear() async throws {
        let (grid, adapter) = try await makeCountingGrid()
        grid.sortColumn = "rank"
        await grid.loadPage(0)

        _ = try await adapter.executeRaw(sql: "ALTER TABLE scores RENAME COLUMN rank TO score")
        await grid.reloadAfterStructureChange()

        grid.sortColumn = "score"
        await grid.handleSortColumnChange("score")
        XCTAssertEqual(grid.rows.compactMap { $0[1].intValue }, [10, 20, 30])

        adapter.resetCounts()
        grid.sortColumn = nil
        await grid.handleSortColumnChange(nil)

        XCTAssertEqual(adapter.fetchRowsCount, 1)
        XCTAssertEqual(grid.rows.compactMap { $0[1].intValue }, [30, 10, 20])
        try await adapter.disconnect()
    }

    private func makeMetadataRaceDescription(
        primaryKey: String,
        defaultValue: String,
        referencedTable: String,
        referencedColumn: String
    ) -> TableDescription {
        TableDescription(
            name: "metadata_race",
            schema: nil,
            columns: [
                ColumnInfo(
                    name: "id",
                    dataType: "text",
                    isNullable: false,
                    defaultValue: nil,
                    isPrimaryKey: primaryKey == "id",
                    isAutoIncrement: false,
                    comment: nil,
                    ordinalPosition: 0,
                    characterMaxLength: nil,
                    checkConstraint: nil
                ),
                ColumnInfo(
                    name: "status",
                    dataType: "status_enum",
                    isNullable: true,
                    defaultValue: defaultValue,
                    isPrimaryKey: primaryKey == "status",
                    isAutoIncrement: false,
                    comment: nil,
                    ordinalPosition: 1,
                    characterMaxLength: nil,
                    checkConstraint: nil
                ),
                ColumnInfo(
                    name: "parent_id",
                    dataType: "text",
                    isNullable: true,
                    defaultValue: nil,
                    isPrimaryKey: primaryKey == "parent_id",
                    isAutoIncrement: false,
                    comment: nil,
                    ordinalPosition: 2,
                    characterMaxLength: nil,
                    checkConstraint: nil
                ),
            ],
            indexes: [],
            foreignKeys: [
                ForeignKeyInfo(
                    name: "fk_metadata_race_parent",
                    columns: ["parent_id"],
                    referencedTable: referencedTable,
                    referencedColumns: [referencedColumn],
                    onDelete: .noAction,
                    onUpdate: .noAction
                ),
            ],
            constraints: [],
            comment: nil,
            estimatedRowCount: 1
        )
    }

    private func makeGrid() async throws -> (DataGridViewState, SQLiteAdapter) {
        DataGridViewState.clearMetadataCache()
        let adapter = SQLiteAdapter()
        let config = ConnectionConfig(
            name: "refresh-test",
            databaseType: .sqlite,
            filePath: ":memory:"
        )
        try await adapter.connect(config: config, password: nil)
        _ = try await adapter.executeRaw(sql: "CREATE TABLE scores (id INTEGER PRIMARY KEY, rank INTEGER NOT NULL)")
        _ = try await adapter.executeRaw(sql: "INSERT INTO scores (id, rank) VALUES (1, 30), (2, 10), (3, 20)")
        let grid = DataGridViewState()
        await grid.load(tableName: "scores", schema: nil, adapter: adapter)
        return (grid, adapter)
    }

    private func makeTextPrimaryKeyGrid() async throws -> (DataGridViewState, SQLiteAdapter) {
        DataGridViewState.clearMetadataCache()
        let adapter = SQLiteAdapter()
        let config = ConnectionConfig(
            name: "refresh-text-primary-key-test",
            databaseType: .sqlite,
            filePath: ":memory:"
        )
        try await adapter.connect(config: config, password: nil)
        _ = try await adapter.executeRaw(sql: "CREATE TABLE text_scores (id TEXT PRIMARY KEY, score INTEGER NOT NULL)")
        _ = try await adapter.executeRaw(sql: "INSERT INTO text_scores (id, score) VALUES ('c', 30), ('a', 10), ('b', 20)")
        let grid = DataGridViewState()
        await grid.load(tableName: "text_scores", schema: nil, adapter: adapter)
        return (grid, adapter)
    }

    private func makeConnectionGrid(
        name: String,
        createTableSQL: String,
        insertSQL: String
    ) async throws -> (DataGridViewState, SQLiteAdapter) {
        let adapter = SQLiteAdapter()
        let config = ConnectionConfig(name: name, databaseType: .sqlite, filePath: ":memory:")
        try await adapter.connect(config: config, password: nil)
        _ = try await adapter.executeRaw(sql: createTableSQL)
        _ = try await adapter.executeRaw(sql: insertSQL)
        return (DataGridViewState(), adapter)
    }

    private func makeCountingGrid(clearingMetadataCache: Bool = true) async throws -> (DataGridViewState, CountingSQLiteAdapter) {
        if clearingMetadataCache {
            DataGridViewState.clearMetadataCache()
        }
        let adapter = CountingSQLiteAdapter()
        let config = ConnectionConfig(
            name: "refresh-counting-test",
            databaseType: .sqlite,
            filePath: ":memory:"
        )
        try await adapter.connect(config: config, password: nil)
        _ = try await adapter.executeRaw(sql: "CREATE TABLE scores (id INTEGER PRIMARY KEY, rank INTEGER NOT NULL)")
        _ = try await adapter.executeRaw(sql: "INSERT INTO scores (id, rank) VALUES (1, 30), (2, 10), (3, 20)")
        let grid = DataGridViewState()
        await grid.load(tableName: "scores", schema: nil, adapter: adapter)
        return (grid, adapter)
    }
}

private final class CountingSQLiteAdapter: DatabaseAdapter, @unchecked Sendable {
    private struct HeldDescriptionResult {
        let description: TableDescription
        let gate: AsyncGate
        let started: AsyncSignal
    }

    private struct HeldEnumResult {
        let values: [String: [String]]
        let gate: AsyncGate
        let started: AsyncSignal
    }

    private let base = SQLiteAdapter()
    private let reportedDatabaseType: DatabaseType
    private let lock = NSLock()
    private var fetchRowsCalls = 0
    private var describeTableCalls = 0
    private var fetchDelay: UInt64 = 0
    private var nextHeldFetchGate: AsyncGate?
    private var heldFetchStarted: AsyncSignal?
    private var heldFetchHasResumed = false
    private var heldTableDescription: TableDescription?
    private var nextHeldDescribeGate: AsyncGate?
    private var heldDescribeStarted: AsyncSignal?
    private var failNextDescribe = false
    private var descriptionPrimaryKeysBySchema: [String: String] = [:]
    private var logicalMetadataPrimaryKey: String?
    private var heldDescriptionResults: [HeldDescriptionResult] = []
    private var heldEnumResults: [HeldEnumResult] = []

    init(databaseType: DatabaseType = .sqlite) {
        reportedDatabaseType = databaseType
    }

    var databaseType: DatabaseType { reportedDatabaseType }
    var isConnected: Bool { base.isConnected }

    var fetchRowsCount: Int {
        withLock(lock) { fetchRowsCalls }
    }

    var describeTableCount: Int {
        withLock(lock) { describeTableCalls }
    }

    var fetchDelayNanoseconds: UInt64 {
        get { withLock(lock) { fetchDelay } }
        set { withLock(lock) { fetchDelay = newValue } }
    }

    func resetCounts() {
        withLock(lock) {
            fetchRowsCalls = 0
            describeTableCalls = 0
        }
    }

    func holdNextFetchAndCaptureTableDescription() -> AsyncGate {
        let gate = AsyncGate()
        let started = AsyncSignal()
        withLock(lock) {
            nextHeldFetchGate = gate
            heldFetchStarted = started
            heldFetchHasResumed = false
            heldTableDescription = nil
        }
        return gate
    }

    func waitForHeldFetch() async {
        let started = withLock(lock) { heldFetchStarted }
        await started?.wait()
    }

    func holdNextDescribeTable() -> AsyncGate {
        let gate = AsyncGate()
        let started = AsyncSignal()
        withLock(lock) {
            nextHeldDescribeGate = gate
            heldDescribeStarted = started
        }
        return gate
    }

    func waitForHeldDescribeTable() async {
        let started = withLock(lock) { heldDescribeStarted }
        await started?.wait()
    }

    func failNextDescribeTable() {
        withLock(lock) {
            failNextDescribe = true
        }
    }

    func enqueueHeldDescription(_ description: TableDescription) -> (gate: AsyncGate, started: AsyncSignal) {
        let result = HeldDescriptionResult(
            description: description,
            gate: AsyncGate(),
            started: AsyncSignal()
        )
        withLock(lock) {
            heldDescriptionResults.append(result)
        }
        return (result.gate, result.started)
    }

    func enqueueHeldEnumValues(_ values: [String: [String]]) -> (gate: AsyncGate, started: AsyncSignal) {
        let result = HeldEnumResult(
            values: values,
            gate: AsyncGate(),
            started: AsyncSignal()
        )
        withLock(lock) {
            heldEnumResults.append(result)
        }
        return (result.gate, result.started)
    }

    func setDescriptionPrimaryKey(_ column: String, forSchema schema: String) {
        withLock(lock) {
            descriptionPrimaryKeysBySchema[schema] = column
        }
    }

    func selectLogicalMetadataDatabase(primaryKeyColumn: String) {
        withLock(lock) {
            logicalMetadataPrimaryKey = primaryKeyColumn
        }
    }

    func connect(config: ConnectionConfig, password: String?) async throws {
        try await base.connect(config: config, password: password)
    }

    func disconnect() async throws {
        try await base.disconnect()
    }

    func testConnection(config: ConnectionConfig, password: String?) async throws -> Bool {
        try await base.testConnection(config: config, password: password)
    }

    func execute(query: String, parameters: [QueryParameter]?) async throws -> QueryResult {
        try await base.execute(query: query, parameters: parameters)
    }

    func executeRaw(sql: String) async throws -> QueryResult {
        try await base.executeRaw(sql: sql)
    }

    func executeWithRowValues(sql: String, parameters: [RowValue]) async throws -> QueryResult {
        if sql.contains("JOIN pg_enum") {
            let heldResult = withLock(lock) { () -> HeldEnumResult? in
                guard !heldEnumResults.isEmpty else { return nil }
                return heldEnumResults.removeFirst()
            }
            if let heldResult {
                heldResult.started.signal()
                await heldResult.gate.wait()
                let rows: [[RowValue]] = heldResult.values.keys.sorted().flatMap { column in
                    heldResult.values[column, default: []].map { value in
                        [.string(column), .string(value)]
                    }
                }
                return QueryResult(
                    columns: [ColumnHeader(name: "attname"), ColumnHeader(name: "enumlabel")],
                    rows: rows,
                    rowsAffected: rows.count,
                    executionTime: 0,
                    queryType: .select
                )
            }
        }
        return try await base.executeWithRowValues(sql: sql, parameters: parameters)
    }

    func listDatabases() async throws -> [String] {
        try await base.listDatabases()
    }

    func listSchemas(database: String?) async throws -> [String] {
        try await base.listSchemas(database: database)
    }

    func listTables(schema: String?) async throws -> [TableInfo] {
        try await base.listTables(schema: schema)
    }

    func listViews(schema: String?) async throws -> [ViewInfo] {
        try await base.listViews(schema: schema)
    }

    func describeTable(name: String, schema: String?) async throws -> TableDescription {
        let heldResult = withLock(lock) { () -> HeldDescriptionResult? in
            guard !heldDescriptionResults.isEmpty else { return nil }
            describeTableCalls += 1
            return heldDescriptionResults.removeFirst()
        }
        if let heldResult {
            heldResult.started.signal()
            await heldResult.gate.wait()
            return heldResult.description
        }

        let result = withLock(lock) { () -> (heldDescription: TableDescription?, shouldFail: Bool, primaryKeyOverride: String?, gate: AsyncGate?, started: AsyncSignal?) in
            describeTableCalls += 1
            if failNextDescribe {
                failNextDescribe = false
                return (nil, true, nil, nil, nil)
            }
            let primaryKeyOverride = schema.flatMap { descriptionPrimaryKeysBySchema[$0] }
                ?? logicalMetadataPrimaryKey
            let gate = nextHeldDescribeGate
            nextHeldDescribeGate = nil
            let started = gate == nil ? nil : heldDescribeStarted
            guard heldFetchHasResumed, let heldTableDescription else {
                return (nil, false, primaryKeyOverride, gate, started)
            }
            self.heldTableDescription = nil
            return (heldTableDescription, false, primaryKeyOverride, gate, started)
        }
        if result.shouldFail {
            throw CountingSQLiteAdapterError.describeFailed
        }
        let description: TableDescription
        if let heldDescription = result.heldDescription {
            description = heldDescription
        } else {
            description = try await base.describeTable(name: name, schema: schema)
        }
        if let gate = result.gate {
            result.started?.signal()
            await gate.wait()
        }
        guard let primaryKeyOverride = result.primaryKeyOverride else {
            return description
        }
        return TableDescription(
            name: description.name,
            schema: schema,
            columns: description.columns.map { column in
                ColumnInfo(
                    name: column.name,
                    dataType: column.dataType,
                    isNullable: column.isNullable,
                    defaultValue: column.defaultValue,
                    isPrimaryKey: column.name == primaryKeyOverride,
                    isAutoIncrement: column.isAutoIncrement && column.name == primaryKeyOverride,
                    comment: column.comment,
                    ordinalPosition: column.ordinalPosition,
                    characterMaxLength: column.characterMaxLength,
                    checkConstraint: column.checkConstraint
                )
            },
            indexes: description.indexes,
            foreignKeys: description.foreignKeys,
            constraints: description.constraints,
            comment: description.comment,
            estimatedRowCount: description.estimatedRowCount
        )
    }

    func listIndexes(table: String, schema: String?) async throws -> [IndexInfo] {
        try await base.listIndexes(table: table, schema: schema)
    }

    func listForeignKeys(table: String, schema: String?) async throws -> [ForeignKeyInfo] {
        try await base.listForeignKeys(table: table, schema: schema)
    }

    func listFunctions(schema: String?) async throws -> [String] {
        try await base.listFunctions(schema: schema)
    }

    func getFunctionSource(name: String, schema: String?) async throws -> String {
        try await base.getFunctionSource(name: name, schema: schema)
    }

    func insertRow(table: String, schema: String?, values: [String: RowValue]) async throws -> QueryResult {
        try await base.insertRow(table: table, schema: schema, values: values)
    }

    func updateRow(table: String, schema: String?, set values: [String: RowValue], where conditions: [String: RowValue]) async throws -> QueryResult {
        try await base.updateRow(table: table, schema: schema, set: values, where: conditions)
    }

    func deleteRow(table: String, schema: String?, where conditions: [String: RowValue]) async throws -> QueryResult {
        try await base.deleteRow(table: table, schema: schema, where: conditions)
    }

    func beginTransaction() async throws {
        try await base.beginTransaction()
    }

    func commitTransaction() async throws {
        try await base.commitTransaction()
    }

    func rollbackTransaction() async throws {
        try await base.rollbackTransaction()
    }

    func fetchRows(table: String, schema: String?, columns: [String]?, where filter: FilterExpression?, orderBy: [QuerySortDescriptor]?, limit: Int, offset: Int) async throws -> QueryResult {
        let heldFetchGate = withLock(lock) { () -> AsyncGate? in
            fetchRowsCalls += 1
            defer { nextHeldFetchGate = nil }
            return nextHeldFetchGate
        }
        if let heldFetchGate {
            let result = try await base.fetchRows(
                table: table,
                schema: schema,
                columns: columns,
                where: filter,
                orderBy: orderBy,
                limit: limit,
                offset: offset
            )
            let description = try await base.describeTable(name: table, schema: schema)
            let started = withLock(lock) { () -> AsyncSignal? in
                heldTableDescription = description
                return heldFetchStarted
            }
            started?.signal()
            await heldFetchGate.wait()
            withLock(lock) {
                heldFetchHasResumed = true
            }
            return result
        }

        let delay = withLock(lock) { fetchDelay }
        if delay > 0 {
            try await Task.sleep(nanoseconds: delay)
        }
        return try await base.fetchRows(
            table: table,
            schema: schema,
            columns: columns,
            where: filter,
            orderBy: orderBy,
            limit: limit,
            offset: offset
        )
    }

    func serverVersion() async throws -> String {
        try await base.serverVersion()
    }

    func currentDatabase() async throws -> String? {
        try await base.currentDatabase()
    }
}

private final class AsyncSignal: @unchecked Sendable {
    private let lock = NSLock()
    private var signaled = false
    private var waiters: [CheckedContinuation<Void, Never>] = []

    func signal() {
        let pendingWaiters = withLock(lock) { () -> [CheckedContinuation<Void, Never>] in
            signaled = true
            defer { waiters.removeAll() }
            return waiters
        }
        pendingWaiters.forEach { $0.resume() }
    }

    func wait() async {
        await withCheckedContinuation { continuation in
            let shouldResume = withLock(lock) { () -> Bool in
                guard !signaled else { return true }
                waiters.append(continuation)
                return false
            }
            if shouldResume {
                continuation.resume()
            }
        }
    }
}

private enum CountingSQLiteAdapterError: Error {
    case describeFailed
}

private final class AsyncGate: @unchecked Sendable {
    private let signal = AsyncSignal()

    func open() {
        signal.signal()
    }

    func wait() async {
        await signal.wait()
    }
}

private func withLock<T>(_ lock: NSLock, _ body: () throws -> T) rethrows -> T {
    lock.lock()
    defer { lock.unlock() }
    return try body()
}
