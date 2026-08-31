import Foundation
import XCTest
@testable import Gridex

@MainActor
final class SidebarLoadPublicationTests: XCTestCase {
    func test_staleSchemaLoadCannotReplaceNewerSidebarContents() async {
        let gate = SidebarLoadGate()
        let adapter = RecordingDatabaseAdapter(blockedSchema: "tenant_a", gate: gate)
        let appState = AppState()
        let config = makePostgreSQLConfig()

        appState.selectedSidebarSchema = "tenant_a"
        let staleLoad = Task {
            await appState.loadSidebar(config: config, adapter: adapter, schema: "tenant_a")
        }
        await gate.waitUntilBlocked()

        appState.selectedSidebarSchema = "tenant_b"
        await appState.loadSidebar(config: config, adapter: adapter, schema: "tenant_b")

        await gate.release()
        await staleLoad.value

        let visibleTableNames = appState.sidebarItems
            .first(where: { $0.title == "Tables" })?
            .children
            .map(\.title)

        XCTAssertEqual(visibleTableNames, ["orders_tenant_b"])
    }
}

@MainActor
final class PostgreSQLTableListQueryTests: XCTestCase {
    func test_quotedSchemaIsBoundAsParameter() async {
        let adapter = RecordingDatabaseAdapter()
        let viewModel = TableListViewModel()
        let schema = "tenant's"

        await viewModel.load(adapter: adapter, schema: schema)

        XCTAssertTrue(adapter.rawExecutions.isEmpty)
        XCTAssertEqual(adapter.parameterizedExecutions.count, 1)

        guard let execution = adapter.parameterizedExecutions.first else {
            return XCTFail("Expected PostgreSQL table statistics to use parameter binding")
        }
        XCTAssertTrue(execution.sql.contains("n.nspname = $1"))
        XCTAssertFalse(execution.sql.contains(schema))
        XCTAssertEqual(execution.parameters, [.string(schema)])
    }
}

final class PostgreSQLQualifiedDDLTests: XCTestCase {
    func test_createTableDDLQualifiesTableWithItsSchema() {
        let table = TableDescription(
            name: "orders",
            schema: "tenant_a",
            columns: [
                ColumnInfo(
                    name: "id",
                    dataType: "bigint",
                    isNullable: false,
                    defaultValue: nil,
                    isPrimaryKey: true,
                    isAutoIncrement: false,
                    comment: nil,
                    ordinalPosition: 1,
                    characterMaxLength: nil,
                    checkConstraint: nil
                )
            ],
            indexes: [],
            foreignKeys: [],
            constraints: [],
            comment: nil,
            estimatedRowCount: nil
        )

        let ddl = table.toDDL(dialect: .postgresql)

        XCTAssertTrue(ddl.hasPrefix("CREATE TABLE \"tenant_a\".\"orders\" ("), ddl)
    }
}

final class PostgreSQLSQLDumpScopeTests: XCTestCase {
    func test_dumpRunsUnchangedStatementsInsideSelectedSchemaTransaction() async {
        let adapter = RecordingDatabaseAdapter()
        let statements = [
            "CREATE TABLE orders (id bigint)",
            "INSERT INTO orders VALUES (1)",
        ]

        _ = await SQLDumpExecutor.execute(
            statements: statements,
            schema: "tenant_a",
            using: adapter
        )

        XCTAssertEqual(
            adapter.transactionEvents,
            [
                .begin,
                .statement("SET LOCAL search_path TO \"tenant_a\""),
                .statement(statements[0]),
                .statement(statements[1]),
                .commit,
            ]
        )
    }
}

private func makePostgreSQLConfig() -> ConnectionConfig {
    ConnectionConfig(
        name: "PostgreSQL test",
        databaseType: .postgresql,
        host: "127.0.0.1",
        port: 5432,
        database: "postgres",
        username: "postgres"
    )
}

private actor SidebarLoadGate {
    private var isBlocked = false
    private var continuation: CheckedContinuation<Void, Never>?

    func block() async {
        isBlocked = true
        await withCheckedContinuation { continuation in
            self.continuation = continuation
        }
    }

    func waitUntilBlocked() async {
        while !isBlocked {
            await Task.yield()
        }
    }

    func release() {
        continuation?.resume()
        continuation = nil
    }
}

private final class RecordingDatabaseAdapter: DatabaseAdapter, @unchecked Sendable {
    enum TransactionEvent: Equatable {
        case begin
        case statement(String)
        case commit
        case rollback
    }

    let databaseType: DatabaseType = .postgresql
    private(set) var isConnected = true
    private(set) var rawExecutions: [String] = []
    private(set) var parameterizedExecutions: [(sql: String, parameters: [RowValue])] = []
    private(set) var transactionEvents: [TransactionEvent] = []

    private let blockedSchema: String?
    private let gate: SidebarLoadGate?

    init(blockedSchema: String? = nil, gate: SidebarLoadGate? = nil) {
        self.blockedSchema = blockedSchema
        self.gate = gate
    }

    func connect(config: ConnectionConfig, password: String?) async throws {
        isConnected = true
    }

    func disconnect() async throws {
        isConnected = false
    }

    func testConnection(config: ConnectionConfig, password: String?) async throws -> Bool {
        true
    }

    func execute(query: String, parameters: [QueryParameter]?) async throws -> QueryResult {
        emptyResult
    }

    func executeRaw(sql: String) async throws -> QueryResult {
        rawExecutions.append(sql)
        transactionEvents.append(.statement(sql))
        return emptyResult
    }

    func executeWithRowValues(sql: String, parameters: [RowValue]) async throws -> QueryResult {
        parameterizedExecutions.append((sql, parameters))
        return emptyResult
    }

    func listDatabases() async throws -> [String] { [] }

    func listSchemas(database: String?) async throws -> [String] { [] }

    func listTables(schema: String?) async throws -> [TableInfo] {
        if schema == blockedSchema, let gate {
            await gate.block()
        }
        return [
            TableInfo(
                name: "orders_\(schema ?? "none")",
                schema: schema,
                type: .table,
                estimatedRowCount: nil
            )
        ]
    }

    func listViews(schema: String?) async throws -> [ViewInfo] { [] }

    func describeTable(name: String, schema: String?) async throws -> TableDescription {
        TableDescription(
            name: name,
            schema: schema,
            columns: [],
            indexes: [],
            foreignKeys: [],
            constraints: [],
            comment: nil,
            estimatedRowCount: nil
        )
    }

    func listIndexes(table: String, schema: String?) async throws -> [IndexInfo] { [] }

    func listForeignKeys(table: String, schema: String?) async throws -> [ForeignKeyInfo] { [] }

    func listFunctions(schema: String?) async throws -> [String] { [] }

    func getFunctionSource(name: String, schema: String?) async throws -> String { "" }

    func insertRow(table: String, schema: String?, values: [String: RowValue]) async throws -> QueryResult {
        emptyResult
    }

    func updateRow(
        table: String,
        schema: String?,
        set: [String: RowValue],
        where: [String: RowValue]
    ) async throws -> QueryResult {
        emptyResult
    }

    func deleteRow(table: String, schema: String?, where: [String: RowValue]) async throws -> QueryResult {
        emptyResult
    }

    func beginTransaction() async throws {
        transactionEvents.append(.begin)
    }

    func commitTransaction() async throws {
        transactionEvents.append(.commit)
    }

    func rollbackTransaction() async throws {
        transactionEvents.append(.rollback)
    }

    func fetchRows(
        table: String,
        schema: String?,
        columns: [String]?,
        where: FilterExpression?,
        orderBy: [QuerySortDescriptor]?,
        limit: Int,
        offset: Int
    ) async throws -> QueryResult {
        emptyResult
    }

    func serverVersion() async throws -> String { "test" }

    func currentDatabase() async throws -> String? { "postgres" }

    private var emptyResult: QueryResult {
        QueryResult(
            columns: [],
            rows: [],
            rowsAffected: 0,
            executionTime: 0,
            queryType: .select
        )
    }
}
