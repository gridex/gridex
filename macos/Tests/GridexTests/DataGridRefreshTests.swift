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
}
