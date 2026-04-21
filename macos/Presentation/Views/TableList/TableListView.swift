// TableListView.swift
// Gridex
//
// Shows all tables in a schema with statistics (name, schema, kind, owner, sizes, etc.)

import SwiftUI

struct TableListView: View {
    let schema: String?

    @EnvironmentObject private var appState: AppState
    @StateObject private var viewModel = TableListViewModel()
    @State private var lastClickTime: Date = .distantPast
    @State private var lastClickedRow: Int = -1

    var body: some View {
        VStack(spacing: 0) {
            if viewModel.isLoading && viewModel.rows.isEmpty {
                ProgressView("Loading...")
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else if let err = viewModel.loadError {
                VStack(spacing: 8) {
                    Text("Failed to load tables").foregroundStyle(.secondary)
                    Text(err).font(.system(size: 11)).foregroundStyle(.red).textSelection(.enabled)
                    Button("Retry") { Task { await viewModel.load(adapter: appState.activeAdapter, schema: schema) } }
                        .controlSize(.small)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else if viewModel.rows.isEmpty {
                Text("No tables")
                    .foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                GeometryReader { geo in
                    ScrollView([.horizontal, .vertical]) {
                        let minContentWidth = max(totalColumnsWidth, geo.size.width)
                        LazyVStack(alignment: .leading, spacing: 0, pinnedViews: [.sectionHeaders]) {
                            Section {
                                ForEach(Array(viewModel.rows.enumerated()), id: \.offset) { rowIndex, row in
                                    tableListRow(rowIndex: rowIndex, row: row)
                                }
                            } header: {
                                tableListHeader
                            }
                        }
                        .frame(minWidth: minContentWidth, minHeight: geo.size.height, alignment: .topLeading)
                    }
                }
                .background(Color(nsColor: .textBackgroundColor))
            }
        }
        .task {
            await viewModel.load(adapter: appState.activeAdapter, schema: schema)
        }
    }

    // MARK: - Header

    private var tableListHeader: some View {
        HStack(spacing: 0) {
            ForEach(viewModel.columns, id: \.self) { col in
                HStack(spacing: 4) {
                    Text(col)
                        .font(.system(size: 11, weight: .semibold))
                        .foregroundStyle(.secondary)
                    Spacer()
                }
                .padding(.horizontal, 8)
                .frame(width: viewModel.columnWidths[col] ?? 120, height: 26)
                .frame(maxHeight: .infinity)
            }
        }
        .background(Color(nsColor: .controlBackgroundColor))
        .overlay(alignment: .bottom) {
            Divider()
        }
    }

    // MARK: - Row

    private func tableListRow(rowIndex: Int, row: [String]) -> some View {
        let isSelected = viewModel.selectedRow == rowIndex

        return HStack(spacing: 0) {
            // Table icon
            ForEach(Array(row.enumerated()), id: \.offset) { colIdx, value in
                HStack(spacing: 4) {
                    if colIdx == 0 {
                        Image(systemName: "tablecells")
                            .font(.system(size: 10))
                            .foregroundStyle(Color(red: 0.8, green: 0.5, blue: 0.2))
                    }
                    Text(value)
                        .font(.system(size: 12, weight: .regular, design: .default))
                        .lineLimit(1)
                        .foregroundStyle(value == "NULL" || value == "EMPTY" ? .tertiary : .primary)
                    Spacer()
                }
                .padding(.horizontal, 8)
                .frame(width: viewModel.columnWidths[viewModel.columns[safe: colIdx] ?? ""] ?? 120)
            }
        }
        .frame(height: 28)
        .background(isSelected ? Color.accentColor.opacity(0.2) : (rowIndex % 2 == 0 ? Color.clear : Color(nsColor: .controlBackgroundColor).opacity(0.3)))
        .contentShape(Rectangle())
        .onTapGesture {
            let now = Date()
            if lastClickedRow == rowIndex && now.timeIntervalSince(lastClickTime) < 0.3 {
                // Double click — open the table
                if let tableName = row.first {
                    appState.openTable(name: tableName, schema: schema)
                }
                lastClickTime = .distantPast
            } else {
                // Single click — select and show details
                viewModel.selectedRow = rowIndex
                if rowIndex < viewModel.rows.count {
                    let rowData = viewModel.rows[rowIndex]
                    appState.selectedRowDetails = viewModel.columns.enumerated().map { idx, col in
                        (column: col, value: idx < rowData.count ? rowData[idx] : "")
                    }
                    appState.onDetailFieldEdit = nil
                }
                lastClickedRow = rowIndex
                lastClickTime = now
            }
        }
        .overlay(alignment: .bottom) {
            Divider().padding(.leading, 8)
        }
    }

    private var totalColumnsWidth: CGFloat {
        viewModel.columns.reduce(CGFloat(0)) { sum, col in
            sum + (viewModel.columnWidths[col] ?? 120)
        }
    }
}

// MARK: - Safe subscript

private extension Array {
    subscript(safe index: Index) -> Element? {
        indices.contains(index) ? self[index] : nil
    }
}

// MARK: - View Model

@MainActor
final class TableListViewModel: ObservableObject {
    @Published var columns: [String] = []
    @Published var rows: [[String]] = []
    @Published var columnWidths: [String: CGFloat] = [:]
    @Published var isLoading = false
    @Published var selectedRow: Int?
    @Published var loadError: String?

    func load(adapter: (any DatabaseAdapter)?, schema: String?) async {
        guard let adapter else { return }
        isLoading = true
        defer { isLoading = false }

        do {
            let result: QueryResult
            switch adapter.databaseType {
            case .postgresql:
                result = try await adapter.executeRaw(sql: tableStatsQueryPostgres(schema: schema ?? "public"))
            case .mysql:
                result = try await adapter.executeRaw(sql: tableStatsQueryMySQL(schema: schema))
            case .sqlite:
                result = try await adapter.executeRaw(sql: tableStatsQuerySQLite())
            case .redis:
                result = try await adapter.executeRaw(sql: "INFO keyspace")
            case .mongodb:
                // MongoDB: list collections via adapter.listTables
                let tables = try await adapter.listTables(schema: nil)
                let cols = [
                    ColumnHeader(name: "name", dataType: "string"),
                    ColumnHeader(name: "rows", dataType: "integer"),
                ]
                let rows: [[RowValue]] = tables.map { t in
                    [.string(t.name), .integer(Int64(t.estimatedRowCount ?? 0))]
                }
                result = QueryResult(columns: cols, rows: rows, rowsAffected: 0, executionTime: 0, queryType: .select)
            case .mssql:
                // SQL Server: list tables via INFORMATION_SCHEMA (portable across SQL Server / Azure SQL Edge)
                result = try await adapter.executeRaw(sql: """
                    SELECT TABLE_NAME, TABLE_TYPE
                    FROM INFORMATION_SCHEMA.TABLES
                    WHERE TABLE_SCHEMA = '\(schema ?? "dbo")' AND TABLE_TYPE = 'BASE TABLE'
                    ORDER BY TABLE_NAME
                    """)
            case .clickhouse:
                let db: String
                if let schema, !schema.isEmpty {
                    db = schema
                } else {
                    db = (try? await adapter.currentDatabase()) ?? "default"
                }
                let safe = db.replacingOccurrences(of: "'", with: "\\'")
                result = try await adapter.executeRaw(sql: """
                    SELECT name, total_rows, total_bytes, engine
                    FROM system.tables
                    WHERE database = '\(safe)' AND engine NOT LIKE '%View'
                    ORDER BY name
                    """)
            }

            columns = result.columns.map(\.name)
            rows = result.rows.map { row in
                row.map { $0.description }
            }

            // Set column widths based on header names
            let widthMap: [String: CGFloat] = [
                "name": 150,
                "schema": 80,
                "kind": 80,
                "owner": 100,
                "columns": 80,
                "indexes": 80,
                "has_pk": 70,
                "estimated_row": 120,
                "total_size": 100,
                "data_size": 100,
                "index_size": 100,
                "toast_size": 100,
                "last_vacuum": 140,
                "last_analyze": 140,
                "live_tuples": 110,
                "dead_tuples": 110,
                "modifications": 120,
                "comment": 160,
                "type": 80,
                "rows": 100,
                "engine": 90,
                "collation": 110,
                "auto_increment": 120,
                "create_time": 140,
                "update_time": 140,
            ]
            for col in columns {
                columnWidths[col] = widthMap[col] ?? 120
            }
        } catch {
            loadError = error.localizedDescription
        }
    }

    private func tableStatsQueryPostgres(schema: String) -> String {
        """
        SELECT
            c.relname AS name,
            n.nspname AS schema,
            CASE c.relkind
                WHEN 'r' THEN 'TABLE'
                WHEN 'v' THEN 'VIEW'
                WHEN 'm' THEN 'MAT VIEW'
                WHEN 'f' THEN 'FOREIGN'
                ELSE 'OTHER'
            END AS kind,
            pg_get_userbyid(c.relowner) AS owner,
            (SELECT count(*) FROM information_schema.columns ic
             WHERE ic.table_schema = n.nspname AND ic.table_name = c.relname) AS columns,
            (SELECT count(*) FROM pg_indexes pi
             WHERE pi.schemaname = n.nspname AND pi.tablename = c.relname) AS indexes,
            CASE WHEN EXISTS (
                SELECT 1 FROM pg_constraint pc
                WHERE pc.conrelid = c.oid AND pc.contype = 'p'
            ) THEN 'YES' ELSE 'NO' END AS has_pk,
            c.reltuples::bigint AS estimated_row,
            pg_size_pretty(pg_total_relation_size(c.oid)) AS total_size,
            pg_size_pretty(pg_relation_size(c.oid)) AS data_size,
            pg_size_pretty(pg_indexes_size(c.oid)) AS index_size,
            pg_size_pretty(COALESCE(pg_total_relation_size(c.oid) - pg_relation_size(c.oid) - pg_indexes_size(c.oid), 0)) AS toast_size,
            COALESCE(to_char(s.last_vacuum, 'YYYY-MM-DD HH24:MI'), 'Never') AS last_vacuum,
            COALESCE(to_char(s.last_analyze, 'YYYY-MM-DD HH24:MI'), 'Never') AS last_analyze,
            COALESCE(s.n_live_tup, 0) AS live_tuples,
            COALESCE(s.n_dead_tup, 0) AS dead_tuples,
            COALESCE(s.n_mod_since_analyze, 0) AS modifications,
            COALESCE(obj_description(c.oid, 'pg_class'), 'EMPTY') AS comment
        FROM pg_class c
        JOIN pg_namespace n ON n.oid = c.relnamespace
        LEFT JOIN pg_stat_user_tables s ON s.relid = c.oid
        WHERE n.nspname = '\(schema)'
          AND c.relkind IN ('r', 'f')
        ORDER BY c.relname
        """
    }

    private func tableStatsQueryMySQL(schema: String?) -> String {
        let db = schema.map { "'\($0)'" } ?? "DATABASE()"
        return """
        SELECT
            TABLE_NAME AS name,
            TABLE_SCHEMA AS `schema`,
            TABLE_TYPE AS kind,
            ENGINE AS engine,
            TABLE_ROWS AS estimated_row,
            CONCAT(ROUND((DATA_LENGTH + INDEX_LENGTH) / 1024), ' KB') AS total_size,
            CONCAT(ROUND(DATA_LENGTH / 1024), ' KB') AS data_size,
            CONCAT(ROUND(INDEX_LENGTH / 1024), ' KB') AS index_size,
            TABLE_COLLATION AS collation,
            COALESCE(AUTO_INCREMENT, 0) AS auto_increment,
            COALESCE(CREATE_TIME, '') AS create_time,
            COALESCE(UPDATE_TIME, '') AS update_time,
            COALESCE(NULLIF(TABLE_COMMENT, ''), 'EMPTY') AS comment
        FROM information_schema.TABLES
        WHERE TABLE_SCHEMA = \(db)
        ORDER BY TABLE_NAME
        """
    }

    private func tableStatsQuerySQLite() -> String {
        """
        SELECT
            name,
            'main' AS schema,
            type AS kind,
            '' AS owner,
            '' AS estimated_row
        FROM sqlite_master
        WHERE type = 'table' AND name NOT LIKE 'sqlite_%'
        ORDER BY name
        """
    }
}
