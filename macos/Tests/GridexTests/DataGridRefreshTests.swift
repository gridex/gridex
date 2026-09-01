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
            .sink { _ in
                Task { @MainActor in
                    await grid.loadPage(0)
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

    func test_reloadAfterStructureChange_discardsOlderLoadResultsAndMetadataCache() async throws {
        let (grid, adapter) = try await makeCountingGrid()
        DataGridViewState.clearMetadataCache()

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
        await cacheProbe.load(tableName: "scores", schema: nil, adapter: adapter)
        XCTAssertEqual(cacheProbe.primaryKeyColumns, ["record_id"])
        XCTAssertEqual(cacheProbe.tableDescription?.columns.map(\.name), ["record_id", "rank"])
        try await adapter.disconnect()
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

    private func makeCountingGrid() async throws -> (DataGridViewState, CountingSQLiteAdapter) {
        DataGridViewState.clearMetadataCache()
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
    private let base = SQLiteAdapter()
    private let lock = NSLock()
    private var fetchRowsCalls = 0
    private var describeTableCalls = 0
    private var fetchDelay: UInt64 = 0
    private var nextHeldFetchGate: AsyncGate?
    private var heldFetchStarted: AsyncSignal?
    private var heldFetchHasResumed = false
    private var heldTableDescription: TableDescription?

    var databaseType: DatabaseType { base.databaseType }
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
        try await base.executeWithRowValues(sql: sql, parameters: parameters)
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
        let heldDescription = withLock(lock) { () -> TableDescription? in
            describeTableCalls += 1
            guard heldFetchHasResumed, let heldTableDescription else { return nil }
            self.heldTableDescription = nil
            return heldTableDescription
        }
        if let heldDescription {
            return heldDescription
        }
        return try await base.describeTable(name: name, schema: schema)
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
