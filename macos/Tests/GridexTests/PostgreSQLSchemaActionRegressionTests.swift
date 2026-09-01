import AppKit
import Foundation
import SwiftUI
import XCTest
@testable import Gridex
@testable import PostgresNIO

@MainActor
final class SidebarLoadPublicationTests: XCTestCase {
    func test_staleSchemaLoadCannotReplaceNewerSidebarContents() async {
        let gate = SidebarLoadGate()
        let adapter = RecordingDatabaseAdapter(
            schemas: ["tenant_a", "tenant_b"],
            blockedSchema: "tenant_a",
            gate: gate
        )
        let appState = AppState()
        let config = makePostgreSQLConfig()

        appState.activeConnectionId = config.id
        appState.activeConfig = config
        appState.activeAdapter = adapter
        appState.selectedSidebarSchema = "tenant_a"
        let staleLoad = Task {
            let reload = await appState.loadSidebarSchemas(
                config: config,
                adapter: adapter,
                preferredSchema: "tenant_a"
            )
            await appState.loadSidebar(
                config: config,
                adapter: adapter,
                schema: "tenant_a",
                using: reload
            )
        }
        await gate.waitUntilBlocked()

        appState.selectedSidebarSchema = "tenant_b"
        let latestReload = await appState.loadSidebarSchemas(
            config: config,
            adapter: adapter,
            preferredSchema: "tenant_b"
        )
        await appState.loadSidebar(
            config: config,
            adapter: adapter,
            schema: "tenant_b",
            using: latestReload
        )

        await gate.release()
        await staleLoad.value

        let visibleTableNames = appState.sidebarItems
            .first(where: { $0.title == "Tables" })?
            .children
            .map(\.title)

        XCTAssertEqual(visibleTableNames, ["orders_tenant_b"])
    }

    func test_connectionThatFinishesSchemaDiscoveryLateCannotReplaceActiveSidebar() async {
        let staleSchemaGate = SidebarLoadGate()
        let staleAdapter = RecordingDatabaseAdapter(
            schemas: ["legacy_a", "public"],
            tableName: "orders_from_a",
            schemaGate: staleSchemaGate
        )
        let activeAdapter = RecordingDatabaseAdapter(
            schemas: ["public", "tenant_b"],
            tableName: "orders_from_b"
        )
        let staleConfig = makePostgreSQLConfig(name: "Connection A", database: "database_a")
        let activeConfig = makePostgreSQLConfig(name: "Connection B", database: "database_b")
        let appState = AppState()

        appState.activeConnectionId = staleConfig.id
        appState.activeConfig = staleConfig
        appState.activeAdapter = staleAdapter
        let staleLoad = Task {
            let reload = await appState.loadSidebarSchemas(config: staleConfig, adapter: staleAdapter)
            await appState.loadSidebar(
                config: staleConfig,
                adapter: staleAdapter,
                using: reload
            )
        }
        await staleSchemaGate.waitUntilBlocked()

        appState.activeConnectionId = activeConfig.id
        appState.activeConfig = activeConfig
        appState.activeAdapter = activeAdapter
        let activeReload = await appState.loadSidebarSchemas(config: activeConfig, adapter: activeAdapter)
        await appState.loadSidebar(
            config: activeConfig,
            adapter: activeAdapter,
            using: activeReload
        )

        await staleSchemaGate.release()
        await staleLoad.value

        XCTAssertEqual(appState.activeConnectionId, activeConfig.id)
        XCTAssertEqual(appState.sidebarSchemas, ["public", "tenant_b"])
        XCTAssertEqual(visibleTableNames(in: appState), ["orders_from_b"])
    }

    func test_disconnectInvalidatesRefreshBlockedInSchemaDiscovery() async {
        let schemaGate = SidebarLoadGate()
        let adapter = RecordingDatabaseAdapter(
            schemas: ["public", "tenant_a"],
            tableName: "orders_from_disconnected_database",
            schemaGate: schemaGate
        )
        let config = makePostgreSQLConfig(name: "Connection A", database: "database_a")
        let appState = AppState()

        appState.activeConnectionId = config.id
        appState.activeConfig = config
        appState.activeAdapter = adapter
        let staleLoad = Task {
            let reload = await appState.loadSidebarSchemas(config: config, adapter: adapter)
            await appState.loadSidebar(
                config: config,
                adapter: adapter,
                using: reload
            )
        }
        await schemaGate.waitUntilBlocked()

        appState.disconnect()
        await schemaGate.release()
        await staleLoad.value

        XCTAssertNil(appState.activeConnectionId)
        XCTAssertTrue(appState.sidebarSchemas.isEmpty)
        XCTAssertNil(appState.selectedSidebarSchema)
        XCTAssertTrue(appState.sidebarItems.isEmpty)
    }

    func test_failedSchemaItemLoadDoesNotPairNewSelectionWithPreviousTree() async {
        let adapter = RecordingDatabaseAdapter(
            schemas: ["tenant_a", "tenant_b"],
            failingSidebarSchema: "tenant_b"
        )
        let config = makePostgreSQLConfig()
        let appState = AppState()
        appState.activeConnectionId = config.id
        appState.activeConfig = config
        appState.activeAdapter = adapter

        let initialReload = await appState.loadSidebarSchemas(config: config, adapter: adapter)
        await appState.loadSidebar(
            config: config,
            adapter: adapter,
            using: initialReload
        )
        XCTAssertEqual(appState.selectedSidebarSchema, "tenant_a")
        XCTAssertEqual(visibleTableNames(in: appState), ["orders_tenant_a"])

        appState.selectedSidebarSchema = "tenant_b"
        let failingReload = await appState.loadSidebarSchemas(config: config, adapter: adapter)
        await appState.loadSidebar(
            config: config,
            adapter: adapter,
            using: failingReload
        )

        XCTAssertTrue(
            appState.sidebarItems.isEmpty
                || sidebarItems(appState.sidebarItems, allMatch: appState.selectedSidebarSchema),
            "The selected schema and every visible sidebar item must describe one coherent schema"
        )
    }

    func test_explicitItemSchemaCannotConsumePendingDiscoveryForAnotherSchema() async {
        let adapter = RecordingDatabaseAdapter(schemas: ["tenant_a", "tenant_b"])
        let config = makePostgreSQLConfig()
        let appState = AppState()
        appState.activeConnectionId = config.id
        appState.activeConfig = config
        appState.activeAdapter = adapter

        let reload = await appState.loadSidebarSchemas(
            config: config,
            adapter: adapter,
            preferredSchema: "tenant_a"
        )
        await appState.loadSidebar(
            config: config,
            adapter: adapter,
            schema: "tenant_b",
            using: reload
        )

        XCTAssertEqual(appState.selectedSidebarSchema, "tenant_b")
        XCTAssertEqual(visibleTableNames(in: appState), ["orders_tenant_b"])
    }

    func test_olderSplitReloadCannotOverwriteNewerCompletedSplitReload() async {
        let olderItemsGate = SidebarLoadGate()
        let adapter = RecordingDatabaseAdapter(schemas: ["tenant_a", "tenant_b"])
        let config = makePostgreSQLConfig()
        let appState = AppState()
        appState.activeConnectionId = config.id
        appState.activeConfig = config
        appState.activeAdapter = adapter

        let olderReload = Task {
            let reload = await appState.loadSidebarSchemas(
                config: config,
                adapter: adapter,
                preferredSchema: "tenant_a"
            )
            await olderItemsGate.block()
            await appState.loadSidebar(
                config: config,
                adapter: adapter,
                schema: "tenant_a",
                using: reload
            )
        }
        await olderItemsGate.waitUntilBlocked()

        let newerReload = await appState.loadSidebarSchemas(
            config: config,
            adapter: adapter,
            preferredSchema: "tenant_b"
        )
        await appState.loadSidebar(
            config: config,
            adapter: adapter,
            schema: "tenant_b",
            using: newerReload
        )
        XCTAssertEqual(appState.selectedSidebarSchema, "tenant_b")
        XCTAssertEqual(visibleTableNames(in: appState), ["orders_tenant_b"])

        await olderItemsGate.release()
        await olderReload.value

        XCTAssertEqual(appState.selectedSidebarSchema, "tenant_b")
        XCTAssertEqual(visibleTableNames(in: appState), ["orders_tenant_b"])
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

    func test_failedDumpRollsBackWithoutCommitting() async {
        let failingStatement = "INSERT INTO orders VALUES (1)"
        let adapter = RecordingDatabaseAdapter(failingStatement: failingStatement)
        let statements = [
            failingStatement,
            "INSERT INTO orders VALUES (2)",
        ]

        let result = await SQLDumpExecutor.execute(
            statements: statements,
            schema: "tenant_a",
            using: adapter
        )

        XCTAssertEqual(result.success, 0)
        XCTAssertEqual(result.total, 2)
        XCTAssertEqual(result.firstError, RecordingDatabaseAdapter.Failure.statement.localizedDescription)
        XCTAssertEqual(
            adapter.transactionEvents,
            [
                .begin,
                .statement("SET LOCAL search_path TO \"tenant_a\""),
                .statement(failingStatement),
                .rollback,
            ]
        )
        XCTAssertFalse(adapter.transactionEvents.contains(.commit))
    }
}

final class PostgreSQLTransactionErrorFormattingTests: XCTestCase {
    func test_transactionWrapperPreservesNestedServerMessageAndRollbackContext() {
        let serverError = PSQLError.server(
            .init(fields: [
                .localizedSeverity: "ERROR",
                .severity: "ERROR",
                .sqlState: "23505",
                .message: "duplicate key value violates unique constraint \"orders_pkey\"",
                .detail: "Key (id)=(1) already exists.",
            ])
        )
        let transactionError = PostgresTransactionError(
            file: #fileID,
            line: #line,
            closureError: serverError,
            rollbackError: TransactionRollbackFailure()
        )

        let message = PostgreSQLAdapter.formatPostgresError(transactionError)

        XCTAssertTrue(
            message.contains("duplicate key value violates unique constraint \"orders_pkey\""),
            message
        )
        XCTAssertTrue(message.contains("Detail: Key (id)=(1) already exists."), message)
        XCTAssertTrue(message.contains("SQLSTATE: 23505"), message)
        XCTAssertTrue(message.contains("Rollback failed: connection closed during rollback"), message)
    }
}

final class PostgreSQLParameterizedQueryErrorTests: XCTestCase {
    func test_boundQueryErrorBoundaryPreservesServerDiagnostics() async {
        let serverError = PSQLError.server(
            .init(fields: [
                .localizedSeverity: "ERROR",
                .severity: "ERROR",
                .sqlState: "42501",
                .message: "permission denied for relation orders",
                .detail: "Role reporting_reader cannot read tenant_b.orders.",
            ])
        )
        let failingBoundQuery: () async throws -> Gridex.QueryResult = {
            throw serverError
        }

        do {
            _ = try await PostgreSQLAdapter.withFormattedQueryErrors(failingBoundQuery)
            XCTFail("Expected the bound PostgreSQL query to fail")
        } catch GridexError.queryExecutionFailed(let message) {
            XCTAssertTrue(message.contains("permission denied for relation orders"), message)
            XCTAssertTrue(
                message.contains("Detail: Role reporting_reader cannot read tenant_b.orders."),
                message
            )
            XCTAssertTrue(message.contains("SQLSTATE: 42501"), message)
        } catch {
            XCTFail("Expected GridexError.queryExecutionFailed, got \(error)")
        }
    }
}

@MainActor
final class SidebarActiveHighlightTests: XCTestCase {
    func test_sameNameTableInAnotherSchemaIsNotRenderedAsActive() throws {
        let displayedItem = SidebarItem(
            title: "orders",
            type: .table("orders"),
            schema: "tenant_b"
        )

        let sameSchemaState = AppState()
        sameSchemaState.openTable(name: "orders", schema: "tenant_b")
        sameSchemaState.selectedSidebarSchema = "tenant_b"

        let otherSchemaState = AppState()
        otherSchemaState.openTable(name: "orders", schema: "tenant_a")
        let tenantATab = try XCTUnwrap(otherSchemaState.activeTabId)
        otherSchemaState.openTable(name: "orders", schema: "tenant_b")
        otherSchemaState.selectedSidebarSchema = "tenant_b"
        otherSchemaState.activeTabId = tenantATab

        let inactiveState = AppState()
        inactiveState.openTable(name: "customers", schema: "tenant_a")
        inactiveState.selectedSidebarSchema = "tenant_b"

        let activeColor = try renderedSidebarRowBackground(
            item: displayedItem,
            appState: sameSchemaState
        )
        let otherSchemaColor = try renderedSidebarRowBackground(
            item: displayedItem,
            appState: otherSchemaState
        )
        let inactiveColor = try renderedSidebarRowBackground(
            item: displayedItem,
            appState: inactiveState
        )

        XCTAssertGreaterThan(
            colorDistance(activeColor, inactiveColor),
            0.2,
            "Render control must distinguish an active row from an inactive row"
        )
        XCTAssertLessThan(
            colorDistance(otherSchemaColor, inactiveColor),
            0.02,
            "tenant_b.orders must look inactive while the active tab is tenant_a.orders"
        )
    }
}

private struct TransactionRollbackFailure: LocalizedError {
    var errorDescription: String? { "connection closed during rollback" }
}

private func makePostgreSQLConfig(
    name: String = "PostgreSQL test",
    database: String = "postgres"
) -> ConnectionConfig {
    ConnectionConfig(
        name: name,
        databaseType: .postgresql,
        host: "127.0.0.1",
        port: 5432,
        database: database,
        username: "postgres"
    )
}

@MainActor
private func visibleTableNames(in appState: AppState) -> [String]? {
    appState.sidebarItems
        .first(where: { $0.title == "Tables" })?
        .children
        .map(\.title)
}

private func sidebarItems(_ items: [SidebarItem], allMatch schema: String?) -> Bool {
    items.allSatisfy { item in
        item.schema == schema && sidebarItems(item.children, allMatch: schema)
    }
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

@MainActor
private func renderedSidebarRowBackground(
    item: SidebarItem,
    appState: AppState
) throws -> NSColor {
    let rootView = ZStack {
        Color.white
        SidebarItemRow(item: item, searchText: "")
            .environmentObject(appState)
    }
    .frame(width: 240, height: 28)

    let hostingView = NSHostingView(rootView: rootView)
    hostingView.appearance = NSAppearance(named: .aqua)
    hostingView.frame = NSRect(x: 0, y: 0, width: 240, height: 28)
    hostingView.layoutSubtreeIfNeeded()

    let bitmap = try XCTUnwrap(hostingView.bitmapImageRepForCachingDisplay(in: hostingView.bounds))
    hostingView.cacheDisplay(in: hostingView.bounds, to: bitmap)
    let sample = try XCTUnwrap(
        bitmap.colorAt(
            x: max(0, bitmap.pixelsWide - 8),
            y: bitmap.pixelsHigh / 2
        )
    )
    return try XCTUnwrap(sample.usingColorSpace(.deviceRGB))
}

private func colorDistance(_ lhs: NSColor, _ rhs: NSColor) -> CGFloat {
    abs(lhs.redComponent - rhs.redComponent)
        + abs(lhs.greenComponent - rhs.greenComponent)
        + abs(lhs.blueComponent - rhs.blueComponent)
        + abs(lhs.alphaComponent - rhs.alphaComponent)
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
    private let failingStatement: String?
    private let schemas: [String]
    private let tableName: String?
    private let schemaGate: SidebarLoadGate?
    private let failingSidebarSchema: String?

    init(
        blockedSchema: String? = nil,
        gate: SidebarLoadGate? = nil,
        failingStatement: String? = nil,
        schemas: [String] = [],
        tableName: String? = nil,
        schemaGate: SidebarLoadGate? = nil,
        failingSidebarSchema: String? = nil
    ) {
        self.blockedSchema = blockedSchema
        self.gate = gate
        self.failingStatement = failingStatement
        self.schemas = schemas
        self.tableName = tableName
        self.schemaGate = schemaGate
        self.failingSidebarSchema = failingSidebarSchema
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

    func execute(query: String, parameters: [QueryParameter]?) async throws -> Gridex.QueryResult {
        emptyResult
    }

    func executeRaw(sql: String) async throws -> Gridex.QueryResult {
        rawExecutions.append(sql)
        transactionEvents.append(.statement(sql))
        if sql == failingStatement {
            throw Failure.statement
        }
        return emptyResult
    }

    func executeWithRowValues(sql: String, parameters: [RowValue]) async throws -> Gridex.QueryResult {
        parameterizedExecutions.append((sql, parameters))
        return emptyResult
    }

    func listDatabases() async throws -> [String] { [] }

    func listSchemas(database: String?) async throws -> [String] {
        if let schemaGate {
            await schemaGate.block()
        }
        return schemas
    }

    func listTables(schema: String?) async throws -> [TableInfo] {
        if schema == failingSidebarSchema {
            throw Failure.sidebarItems(schema ?? "default")
        }
        if schema == blockedSchema, let gate {
            await gate.block()
        }
        return [
            TableInfo(
                name: tableName ?? "orders_\(schema ?? "none")",
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

    func insertRow(table: String, schema: String?, values: [String: RowValue]) async throws -> Gridex.QueryResult {
        emptyResult
    }

    func updateRow(
        table: String,
        schema: String?,
        set: [String: RowValue],
        where: [String: RowValue]
    ) async throws -> Gridex.QueryResult {
        emptyResult
    }

    func deleteRow(table: String, schema: String?, where: [String: RowValue]) async throws -> Gridex.QueryResult {
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
    ) async throws -> Gridex.QueryResult {
        emptyResult
    }

    func serverVersion() async throws -> String { "test" }

    func currentDatabase() async throws -> String? { "postgres" }

    private var emptyResult: Gridex.QueryResult {
        Gridex.QueryResult(
            columns: [],
            rows: [],
            rowsAffected: 0,
            executionTime: 0,
            queryType: .select
        )
    }

    enum Failure: LocalizedError {
        case statement
        case sidebarItems(String)

        var errorDescription: String? {
            switch self {
            case .statement:
                return "statement failed"
            case .sidebarItems(let schema):
                return "failed to load sidebar items for \(schema)"
            }
        }
    }
}
