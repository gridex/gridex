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
    var fetchDelayNanoseconds: UInt64 = 0

    var databaseType: DatabaseType { base.databaseType }
    var isConnected: Bool { base.isConnected }

    var fetchRowsCount: Int {
        lock.lock()
        defer { lock.unlock() }
        return fetchRowsCalls
    }

    var describeTableCount: Int {
        lock.lock()
        defer { lock.unlock() }
        return describeTableCalls
    }

    func resetCounts() {
        lock.lock()
        fetchRowsCalls = 0
        describeTableCalls = 0
        lock.unlock()
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
        lock.lock()
        describeTableCalls += 1
        lock.unlock()
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
        lock.lock()
        fetchRowsCalls += 1
        lock.unlock()
        if fetchDelayNanoseconds > 0 {
            try await Task.sleep(nanoseconds: fetchDelayNanoseconds)
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
