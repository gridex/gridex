// CreateTableView.swift
// Gridex
//
// View for creating a new database table with columns, primary key, indexes.
// UI matches the Structure editor (columns: #, name, type, nullable, check,
// foreign_key, default, comment; indexes: name, algorithm, unique, columns,
// condition, include, comment).

import SwiftUI

struct CreateTableView: View {
    let schema: String?

    @EnvironmentObject private var appState: AppState
    @State private var tableName: String = "new_table"
    @State private var columns: [NewColumn] = [
        NewColumn(name: "id", dataType: "int4", isPrimaryKey: true, isNullable: false, defaultValue: ""),
    ]
    @State private var indexes: [NewIndex] = []
    @State private var isCreating = false
    @State private var errorMessage: String?
    @State private var cachedDataTypes: [String] = []

    enum ColumnField: Hashable {
        case name(UUID), dataType(UUID), comment(UUID)
    }
    @FocusState private var focusedField: ColumnField?
    @State private var selectedColumnId: UUID?
    @State private var selectedIndexId: UUID?

    // FK editor state
    @State private var fkEditColumnId: UUID?
    @State private var fkRefTable: String = ""
    @State private var fkRefColumn: String = ""
    @State private var availableTables: [String] = []

    // Default value editor state
    @State private var defaultEditColumnId: UUID?
    @State private var defaultTab: DefaultTab = .string
    @State private var defaultStringValue: String = ""
    @State private var defaultExpressionValue: String = ""

    enum DefaultTab: String, CaseIterable { case string = "String", expression = "Expression" }

    private var isMongoDB: Bool {
        appState.activeAdapter?.databaseType == .mongodb
    }

    private var defaultSchema: String {
        switch appState.activeAdapter?.databaseType {
        case .mssql: return "dbo"
        case .clickhouse: return appState.currentDatabaseName ?? "default"
        default: return "public"
        }
    }

    var body: some View {
        VStack(spacing: 0) {
            toolbar
            Divider()
            if isMongoDB {
                mongoCollectionInfo
            } else {
                VSplitView {
                    columnsSection
                    indexesSection
                }
            }
        }
        .onAppear {
            rebuildDataTypes()
            // Fix default column data type for current adapter (the @State initializer
            // defaults to "int4" which is PostgreSQL-specific).
            if !isMongoDB, let adapter = appState.activeAdapter {
                let idType: String
                switch adapter.databaseType {
                case .postgresql: idType = "int4"
                case .mysql: idType = "int"
                case .sqlite: idType = "INTEGER"
                case .mssql: idType = "INT"
                case .clickhouse: idType = "UInt64"
                default: idType = "int"
                }
                if columns.count == 1 && columns[0].dataType == "int4" {
                    columns[0].dataType = idType
                }
            }
            // Select the first column so it's immediately editable
            if isMongoDB {
                tableName = "new_collection"
            } else if selectedColumnId == nil, let first = columns.first {
                selectedColumnId = first.id
            }
        }
    }

    private var mongoCollectionInfo: some View {
        VStack(spacing: 16) {
            Spacer()
            Image(systemName: "doc.text.below.ecg")
                .font(.system(size: 48))
                .foregroundStyle(.secondary)
            Text("MongoDB Collection")
                .font(.system(size: 16, weight: .semibold))
            Text("MongoDB collections are schemaless — no column definitions are needed.\nClick \"Create\" to add the collection. Documents can be inserted later with any structure.")
                .font(.system(size: 12))
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .padding(.horizontal, 40)
            Spacer()
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    // MARK: - Toolbar

    private var toolbar: some View {
        HStack(spacing: 12) {
            Image(systemName: isMongoDB ? "doc.text" : "tablecells")
                .foregroundStyle(.secondary)
            TextField(isMongoDB ? "Collection name" : "Table name", text: $tableName)
                .textFieldStyle(.squareBorder)
                .frame(maxWidth: 250)

            if !isMongoDB {
                Text("Schema: \(schema ?? defaultSchema)")
                    .font(.system(size: 12))
                    .foregroundStyle(.secondary)
            } else {
                Text("MongoDB · \(appState.currentDatabaseName ?? "")")
                    .font(.system(size: 12))
                    .foregroundStyle(.secondary)
            }

            Spacer()

            if let errorMessage {
                Text(errorMessage)
                    .font(.system(size: 11))
                    .foregroundStyle(.red)
                    .lineLimit(2)
                    .frame(maxWidth: 300, alignment: .trailing)
            }

            Button {
                Task { await createTable() }
            } label: {
                HStack(spacing: 3) {
                    if isCreating {
                        ProgressView().controlSize(.small)
                    }
                    Image(systemName: "checkmark").font(.system(size: 10, weight: .semibold))
                    Text("Create").font(.system(size: 11, weight: .medium))
                }
            }
            .buttonStyle(.borderedProminent)
            .pointerCursor()
            .disabled(tableName.isEmpty || (!isMongoDB && columns.isEmpty) || isCreating)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
    }

    // MARK: - Columns Section (matches Structure editor)

    private var columnsSection: some View {
        VStack(spacing: 0) {
            Table($columns, selection: $selectedColumnId) {
                TableColumn("#") { $col in
                    Text("\((columns.firstIndex(where: { $0.id == col.id }) ?? 0) + 1)")
                        .foregroundStyle(.secondary)
                }
                .width(30)

                TableColumn("column_name") { $col in
                    columnNameCell(col: $col)
                }

                TableColumn("data_type") { $col in
                    dataTypeCell(col: $col)
                }

                TableColumn("is_nullable") { $col in
                    nullableCell(col: $col)
                }
                .width(80)

                TableColumn("check") { $col in
                    if selectedColumnId == col.id {
                        TextField("", text: $col.checkConstraint)
                            .textFieldStyle(.squareBorder)
                    } else {
                        Text(col.checkConstraint.isEmpty ? "" : col.checkConstraint)
                            .foregroundStyle(.secondary)
                            .lineLimit(1)
                            .help(col.checkConstraint)
                    }
                }

                TableColumn("foreign_key") { $col in
                    fkCell(col: $col)
                }

                TableColumn("column_default") { $col in
                    defaultCell(col: $col)
                }

                TableColumn("comment") { $col in
                    commentCell(col: $col)
                }
            }
            .contextMenu(forSelectionType: UUID.self) { ids in
                if let id = ids.first {
                    if let col = columns.first(where: { $0.id == id }) {
                        Button(col.isPrimaryKey ? "Unset Primary Key" : "Set as Primary Key") {
                            togglePrimaryKey(id)
                        }
                        Divider()
                    }
                    Button("Delete Column", role: .destructive) {
                        columns.removeAll { $0.id == id }
                    }
                }
            } primaryAction: { _ in }
            .onChange(of: selectedColumnId) { _, _ in
                focusedField = nil
            }
            .environment(\.defaultMinListRowHeight, 34)

            // Inline "+ New column" button (matches Structure editor)
            HStack {
                Button { addNewColumn() } label: {
                    HStack(spacing: 4) {
                        Image(systemName: "plus").font(.system(size: 9))
                        Text("New column").font(.system(size: 11))
                    }
                    .padding(.horizontal, 10)
                    .padding(.vertical, 6)
                }
                .buttonStyle(.plain)
                .pointerCursor()
                .foregroundStyle(.tertiary)
                Spacer()
            }
            .padding(.leading, 8)
            .padding(.top, 4)
        }
        .frame(minHeight: 120)
    }

    // MARK: - Column Cell Views (mirrors Structure editor)

    @ViewBuilder
    private func columnNameCell(col: Binding<NewColumn>) -> some View {
        let c = col.wrappedValue
        if selectedColumnId == c.id {
            HStack(spacing: 4) {
                if c.isPrimaryKey {
                    Image(systemName: "key.fill").font(.system(size: 9)).foregroundStyle(.yellow)
                }
                TextField("name", text: col.name)
                    .textFieldStyle(.squareBorder)
                    .fontWeight(c.isPrimaryKey ? .medium : .regular)
                    .focused($focusedField, equals: .name(c.id))
            }
        } else {
            HStack(spacing: 4) {
                if c.isPrimaryKey {
                    Image(systemName: "key.fill").font(.system(size: 9)).foregroundStyle(.yellow)
                }
                Text(c.name)
                    .fontWeight(c.isPrimaryKey ? .medium : .regular)
                    .lineLimit(1)
            }
        }
    }

    @ViewBuilder
    private func dataTypeCell(col: Binding<NewColumn>) -> some View {
        let c = col.wrappedValue
        if selectedColumnId == c.id {
            HStack(spacing: 2) {
                TextField("type", text: col.dataType)
                    .textFieldStyle(.squareBorder)
                    .focused($focusedField, equals: .dataType(c.id))
                Menu {
                    ForEach(cachedDataTypes, id: \.self) { t in
                        Button(t) { col.wrappedValue.dataType = t }
                    }
                } label: {
                    Image(systemName: "chevron.down")
                        .font(.system(size: 8, weight: .semibold))
                        .foregroundStyle(.secondary)
                }
                .menuStyle(.borderlessButton)
                .menuIndicator(.hidden)
                .fixedSize()
            }
        } else {
            Text(c.dataType).lineLimit(1)
        }
    }

    @ViewBuilder
    private func nullableCell(col: Binding<NewColumn>) -> some View {
        let c = col.wrappedValue
        if selectedColumnId == c.id {
            Picker("", selection: col.isNullable) {
                Text("NO").tag(false)
                Text("YES").tag(true)
            }
            .pickerStyle(.menu)
            .labelsHidden()
            .focusable(false)
        } else {
            Text(c.isNullable ? "YES" : "NO")
                .foregroundStyle(c.isNullable ? .primary : .secondary)
                .lineLimit(1)
        }
    }

    @ViewBuilder
    private func fkCell(col: Binding<NewColumn>) -> some View {
        let c = col.wrappedValue
        HStack(spacing: 4) {
            Text(c.foreignKey.isEmpty ? "EMPTY" : c.foreignKey)
                .foregroundStyle(c.foreignKey.isEmpty ? .tertiary : .primary)
                .lineLimit(1)
            Spacer()
            Image(systemName: "arrow.right")
                .font(.system(size: 9))
                .foregroundStyle(.tertiary)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .contentShape(Rectangle())
        .pointerCursor()
        .onTapGesture { openFKEditor(c) }
        .popover(isPresented: Binding(
            get: { fkEditColumnId == c.id },
            set: { if !$0 { fkEditColumnId = nil } }
        )) {
            foreignKeyPopover(c)
        }
    }

    @ViewBuilder
    private func defaultCell(col: Binding<NewColumn>) -> some View {
        let c = col.wrappedValue
        HStack(spacing: 4) {
            Text(c.defaultValue.isEmpty ? "NULL" : c.defaultValue)
                .foregroundStyle(c.defaultValue.isEmpty ? .tertiary : .primary)
                .lineLimit(1)
            Image(systemName: "chevron.down")
                .font(.system(size: 8))
                .foregroundStyle(selectedColumnId == c.id ? Color.gray.opacity(0.4) : Color.clear)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .contentShape(Rectangle())
        .pointerCursor()
        .onTapGesture { openDefaultEditor(c) }
        .popover(isPresented: Binding(
            get: { defaultEditColumnId == c.id },
            set: { if !$0 { defaultEditColumnId = nil } }
        )) {
            defaultValuePopover(c)
        }
    }

    @ViewBuilder
    private func commentCell(col: Binding<NewColumn>) -> some View {
        let c = col.wrappedValue
        if selectedColumnId == c.id {
            TextField("NULL", text: col.comment)
                .textFieldStyle(.squareBorder)
                .focused($focusedField, equals: .comment(c.id))
        } else {
            Text(c.comment.isEmpty ? "NULL" : c.comment)
                .foregroundStyle(c.comment.isEmpty ? .tertiary : .primary)
                .lineLimit(1)
        }
    }

    // MARK: - Indexes Section (matches Structure editor)

    private var indexesSection: some View {
        VStack(spacing: 0) {
            Rectangle()
                .fill(Color.accentColor.opacity(0.6))
                .frame(height: 2)

            if indexes.isEmpty {
                VStack {
                    Text("No indexes")
                        .foregroundStyle(.secondary)
                    HStack {
                        Button { addNewIndex() } label: {
                            HStack(spacing: 4) {
                                Image(systemName: "plus").font(.system(size: 9))
                                Text("New index").font(.system(size: 11))
                            }
                            .padding(.horizontal, 10)
                            .padding(.vertical, 6)
                        }
                        .buttonStyle(.plain)
                        .pointerCursor()
                        .foregroundStyle(.tertiary)
                    }
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                Table($indexes, selection: $selectedIndexId) {
                    TableColumn("index_name") { $idx in
                        if selectedIndexId == idx.id {
                            TextField("name", text: $idx.name)
                                .textFieldStyle(.squareBorder)
                        } else {
                            Text(idx.name).lineLimit(1)
                        }
                    }

                    TableColumn("index_algorithm") { $idx in
                        if selectedIndexId == idx.id {
                            Picker("", selection: $idx.algorithm) {
                                Text("BTREE").tag("BTREE")
                                Text("HASH").tag("HASH")
                                Text("GIST").tag("GIST")
                                Text("SPGIST").tag("SPGIST")
                                Text("GIN").tag("GIN")
                                Text("BRIN").tag("BRIN")
                            }
                            .pickerStyle(.menu)
                            .labelsHidden()
                        } else {
                            Text(idx.algorithm).lineLimit(1)
                        }
                    }.width(120)

                    TableColumn("is_unique") { $idx in
                        if selectedIndexId == idx.id {
                            Picker("", selection: $idx.isUnique) {
                                Text("FALSE").tag(false)
                                Text("TRUE").tag(true)
                            }
                            .pickerStyle(.menu)
                            .labelsHidden()
                        } else {
                            Text(idx.isUnique ? "TRUE" : "FALSE")
                                .foregroundStyle(idx.isUnique ? .primary : .secondary)
                        }
                    }.width(80)

                    TableColumn("column_name") { $idx in
                        if selectedIndexId == idx.id {
                            TextField("col1, col2", text: $idx.columns)
                                .textFieldStyle(.squareBorder)
                        } else {
                            Text(idx.columns).lineLimit(1)
                        }
                    }

                    TableColumn("condition") { $idx in
                        if selectedIndexId == idx.id {
                            TextField("EMPTY", text: $idx.condition)
                                .textFieldStyle(.squareBorder)
                        } else {
                            Text(idx.condition.isEmpty ? "EMPTY" : idx.condition)
                                .foregroundStyle(idx.condition.isEmpty ? .tertiary : .secondary)
                                .lineLimit(1)
                        }
                    }

                    TableColumn("include") { $idx in
                        if selectedIndexId == idx.id {
                            TextField("EMPTY", text: $idx.include)
                                .textFieldStyle(.squareBorder)
                        } else {
                            Text(idx.include.isEmpty ? "EMPTY" : idx.include)
                                .foregroundStyle(idx.include.isEmpty ? .tertiary : .secondary)
                                .lineLimit(1)
                        }
                    }

                    TableColumn("comment") { $idx in
                        if selectedIndexId == idx.id {
                            TextField("NULL", text: $idx.comment)
                                .textFieldStyle(.squareBorder)
                        } else {
                            Text(idx.comment.isEmpty ? "NULL" : idx.comment)
                                .foregroundStyle(idx.comment.isEmpty ? .tertiary : .secondary)
                                .lineLimit(1)
                        }
                    }
                }
                .contextMenu(forSelectionType: UUID.self) { ids in
                    if let id = ids.first {
                        Button("Delete Index", role: .destructive) {
                            indexes.removeAll { $0.id == id }
                        }
                    }
                } primaryAction: { _ in }
                .environment(\.defaultMinListRowHeight, 34)

                // Inline "+ New index" button
                HStack {
                    Button { addNewIndex() } label: {
                        HStack(spacing: 4) {
                            Image(systemName: "plus").font(.system(size: 9))
                            Text("New index").font(.system(size: 11))
                        }
                        .padding(.horizontal, 10)
                        .padding(.vertical, 6)
                    }
                    .buttonStyle(.plain)
                    .pointerCursor()
                    .foregroundStyle(.tertiary)
                    Spacer()
                }
                .padding(.leading, 8)
                .padding(.top, 4)
            }
        }
        .frame(minHeight: 120)
    }

    // MARK: - Foreign Key Editor

    private func openFKEditor(_ col: NewColumn) {
        // Parse existing "table(col)" format
        let fk = col.foreignKey
        if let parenIdx = fk.firstIndex(of: "(") {
            fkRefTable = String(fk[..<parenIdx])
            let rest = fk[fk.index(after: parenIdx)...]
            fkRefColumn = String(rest.dropLast()) // drop closing ')'
        } else {
            fkRefTable = ""
            fkRefColumn = ""
        }
        fkEditColumnId = col.id
        // Load available tables
        if availableTables.isEmpty, let adapter = appState.activeAdapter {
            Task {
                if let tables = try? await adapter.listTables(schema: schema) {
                    availableTables = tables.filter { $0.type == .table }.map(\.name).sorted()
                }
            }
        }
    }

    private func saveForeignKey(_ col: NewColumn) {
        guard let idx = columns.firstIndex(where: { $0.id == col.id }) else { return }
        columns[idx].foreignKey = "\(fkRefTable)(\(fkRefColumn))"
        fkEditColumnId = nil
    }

    private func clearForeignKey(_ col: NewColumn) {
        guard let idx = columns.firstIndex(where: { $0.id == col.id }) else { return }
        columns[idx].foreignKey = ""
        fkEditColumnId = nil
    }

    @ViewBuilder
    private func foreignKeyPopover(_ col: NewColumn) -> some View {
        VStack(spacing: 12) {
            HStack {
                Text("Column").frame(width: 130, alignment: .trailing).foregroundStyle(.secondary)
                Text(col.name)
                    .padding(.horizontal, 8).padding(.vertical, 4)
                    .background(Color.accentColor.opacity(0.2))
                    .cornerRadius(4)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }

            HStack {
                Text("Referenced Table").frame(width: 130, alignment: .trailing).foregroundStyle(.secondary)
                Picker("", selection: $fkRefTable) {
                    Text("Select a table...").tag("")
                    ForEach(availableTables, id: \.self) { t in
                        Text(t).tag(t)
                    }
                }
                .labelsHidden()
            }

            HStack {
                Text("Referenced Column").frame(width: 130, alignment: .trailing).foregroundStyle(.secondary)
                TextField("Column name...", text: $fkRefColumn)
                    .textFieldStyle(.roundedBorder)
            }

            Divider()

            HStack {
                if !col.foreignKey.isEmpty {
                    Button("Clear", role: .destructive) { clearForeignKey(col) }
                }
                Spacer()
                Button("Cancel") { fkEditColumnId = nil }
                Button("OK") { saveForeignKey(col) }
                    .disabled(fkRefTable.isEmpty || fkRefColumn.isEmpty)
                    .buttonStyle(.borderedProminent)
            }
        }
        .padding(16)
        .frame(width: 380)
    }

    // MARK: - Default Value Editor

    private func openDefaultEditor(_ col: NewColumn) {
        let current = col.defaultValue
        if current.contains("(") || current == "CURRENT_TIMESTAMP" || current == "NOW()" ||
            current == "true" || current == "false" || current == "gen_random_uuid()" {
            defaultTab = .expression
            defaultExpressionValue = current
            defaultStringValue = ""
        } else {
            defaultTab = .string
            var str = current
            if str.hasPrefix("'") && str.hasSuffix("'") && str.count >= 2 {
                str = String(str.dropFirst().dropLast())
            }
            defaultStringValue = str
            defaultExpressionValue = ""
        }
        defaultEditColumnId = col.id
    }

    private func saveDefaultValue(_ col: NewColumn) {
        guard let idx = columns.firstIndex(where: { $0.id == col.id }) else { return }
        switch defaultTab {
        case .string:
            columns[idx].defaultValue = defaultStringValue.isEmpty
                ? ""
                : "'\(defaultStringValue.replacingOccurrences(of: "'", with: "''"))'"
        case .expression:
            columns[idx].defaultValue = defaultExpressionValue
        }
        defaultEditColumnId = nil
    }

    @ViewBuilder
    private func defaultValuePopover(_ col: NewColumn) -> some View {
        VStack(spacing: 12) {
            Picker("", selection: $defaultTab) {
                ForEach(DefaultTab.allCases, id: \.self) { tab in
                    Text(tab.rawValue).tag(tab)
                }
            }
            .pickerStyle(.segmented)
            .labelsHidden()

            switch defaultTab {
            case .string:
                VStack(alignment: .leading, spacing: 6) {
                    Text("String value").font(.caption).foregroundStyle(.secondary)
                    TextField("Enter string value...", text: $defaultStringValue)
                        .textFieldStyle(.roundedBorder)
                }
            case .expression:
                VStack(alignment: .leading, spacing: 6) {
                    Text("SQL expression").font(.caption).foregroundStyle(.secondary)
                    TextField("e.g. CURRENT_TIMESTAMP, gen_random_uuid()", text: $defaultExpressionValue)
                        .textFieldStyle(.roundedBorder)
                    HStack(spacing: 4) {
                        ForEach(["CURRENT_TIMESTAMP", "NOW()", "gen_random_uuid()", "true", "false"], id: \.self) { expr in
                            Button(expr) { defaultExpressionValue = expr }
                                .buttonStyle(.plain)
                                .font(.system(size: 10, design: .monospaced))
                                .padding(.horizontal, 6)
                                .padding(.vertical, 2)
                                .background(Color(nsColor: .controlBackgroundColor))
                                .cornerRadius(4)
                        }
                    }
                }
            }

            Divider()

            HStack {
                if !col.defaultValue.isEmpty {
                    Button("Clear", role: .destructive) {
                        guard let idx = columns.firstIndex(where: { $0.id == col.id }) else { return }
                        columns[idx].defaultValue = ""
                        defaultEditColumnId = nil
                    }
                }
                Spacer()
                Button("Cancel") { defaultEditColumnId = nil }
                Button("OK") { saveDefaultValue(col) }
                    .buttonStyle(.borderedProminent)
            }
        }
        .padding(16)
        .frame(width: 360)
    }

    // MARK: - Actions

    private func addNewColumn() {
        let newCol = NewColumn(
            name: "column_\(columns.count + 1)",
            dataType: defaultDataType,
            isPrimaryKey: false,
            isNullable: true,
            defaultValue: ""
        )
        columns.append(newCol)
        selectedColumnId = newCol.id
    }

    private func addNewIndex() {
        let newIdx = NewIndex(
            name: "idx_\(tableName.isEmpty ? "table" : tableName)_\(indexes.count + 1)",
            columns: "",
            isUnique: false
        )
        indexes.append(newIdx)
        selectedIndexId = newIdx.id
    }

    private func togglePrimaryKey(_ id: UUID) {
        if let idx = columns.firstIndex(where: { $0.id == id }) {
            columns[idx].isPrimaryKey.toggle()
        }
    }

    // MARK: - SQL Generation & Execution

    private func createTable() async {
        guard let adapter = appState.activeAdapter else { return }
        isCreating = true
        errorMessage = nil
        defer { isCreating = false }

        // MongoDB: schemaless collection creation
        if let mongo = adapter as? MongoDBAdapter {
            do {
                try await mongo.createCollection(name: tableName)
            } catch {
                errorMessage = "Failed: \(DataGridViewState.detailedErrorMessage(error))"
                return
            }
            let createdName = tableName
            let createTabId = appState.tabs.first(where: {
                $0.type == .createTable && $0.id == appState.activeTabId
            })?.id
            appState.refreshSidebar()
            appState.openTable(name: createdName, schema: schema)
            if let createTabId {
                appState.closeTab(id: createTabId)
            }
            return
        }

        let d = adapter.databaseType.sqlDialect
        let schemaName = schema ?? defaultSchema
        let qualifiedTable = adapter.databaseType == .sqlite
            ? d.quoteIdentifier(tableName)
            : "\(d.quoteIdentifier(schemaName)).\(d.quoteIdentifier(tableName))"

        // Build CREATE TABLE
        var colDefs: [String] = []
        var pkCols: [String] = []

        for col in columns {
            var def = "\(d.quoteIdentifier(col.name)) \(col.dataType)"
            if !col.isNullable { def += " NOT NULL" }
            if !col.defaultValue.isEmpty { def += " DEFAULT \(col.defaultValue)" }
            if !col.checkConstraint.isEmpty { def += " CHECK (\(col.checkConstraint))" }
            colDefs.append(def)
            if col.isPrimaryKey { pkCols.append(d.quoteIdentifier(col.name)) }
        }

        if !pkCols.isEmpty {
            colDefs.append("PRIMARY KEY (\(pkCols.joined(separator: ", ")))")
        }

        // Foreign keys (basic support: "referenced_table(col)")
        for col in columns where !col.foreignKey.isEmpty {
            colDefs.append("FOREIGN KEY (\(d.quoteIdentifier(col.name))) REFERENCES \(col.foreignKey)")
        }

        let createSQL = "CREATE TABLE \(qualifiedTable) (\n  \(colDefs.joined(separator: ",\n  "))\n)"

        do {
            _ = try await adapter.executeRaw(sql: createSQL)
        } catch {
            errorMessage = "Failed: \(DataGridViewState.detailedErrorMessage(error))"
            return
        }

        // Create indexes
        for idx in indexes where !idx.columns.isEmpty {
            let uniqueStr = idx.isUnique ? "UNIQUE " : ""
            var idxSQL: String
            if adapter.databaseType == .postgresql {
                idxSQL = "CREATE \(uniqueStr)INDEX \(d.quoteIdentifier(idx.name)) ON \(qualifiedTable) USING \(idx.algorithm.lowercased()) (\(idx.columns))"
            } else {
                idxSQL = "CREATE \(uniqueStr)INDEX \(d.quoteIdentifier(idx.name)) ON \(qualifiedTable) (\(idx.columns))"
            }
            if !idx.include.isEmpty && adapter.databaseType == .postgresql {
                idxSQL += " INCLUDE (\(idx.include))"
            }
            if !idx.condition.isEmpty {
                idxSQL += " WHERE \(idx.condition)"
            }
            do {
                _ = try await adapter.executeRaw(sql: idxSQL)
            } catch {
                errorMessage = "Table created, but index failed: \(DataGridViewState.detailedErrorMessage(error))"
                return
            }
        }

        // Add comments (PostgreSQL only)
        if adapter.databaseType == .postgresql {
            for col in columns where !col.comment.isEmpty {
                let commentSQL = "COMMENT ON COLUMN \(qualifiedTable).\(d.quoteIdentifier(col.name)) IS '\(col.comment.replacingOccurrences(of: "'", with: "''"))'"
                _ = try? await adapter.executeRaw(sql: commentSQL)
            }
        }

        // Success — refresh sidebar, close this Create Table tab, open the new table
        let createdName = tableName
        let createTabId = appState.tabs.first(where: {
            $0.type == .createTable && $0.id == appState.activeTabId
        })?.id
        appState.refreshSidebar()
        appState.openTable(name: createdName, schema: schema)
        if let createTabId {
            appState.closeTab(id: createTabId)
        }
    }

    // MARK: - Helpers

    private var defaultDataType: String {
        guard let adapter = appState.activeAdapter else { return "text" }
        switch adapter.databaseType {
        case .postgresql: return "text"
        case .mysql: return "varchar(255)"
        case .sqlite: return "TEXT"
        case .mssql: return "NVARCHAR(255)"
        case .clickhouse: return "String"
        case .redis: return "string"
        case .mongodb: return "string"
        }
    }

    private func rebuildDataTypes() {
        guard let adapter = appState.activeAdapter else {
            cachedDataTypes = postgresDataTypes
            return
        }
        switch adapter.databaseType {
        case .postgresql: cachedDataTypes = postgresDataTypes
        case .mysql: cachedDataTypes = mysqlDataTypes
        case .sqlite: cachedDataTypes = sqliteDataTypes
        case .mssql: cachedDataTypes = mssqlDataTypes
        case .clickhouse: cachedDataTypes = clickhouseDataTypes
        case .redis: cachedDataTypes = ["string", "list", "set", "zset", "hash", "stream"]
        case .mongodb: cachedDataTypes = ["string", "integer", "double", "boolean", "date", "objectId", "document", "array"]
        }
    }

    /// ClickHouse data types grouped by family. Nullable/Array wrappers are applied
    /// per-column by the user (the form doesn't try to auto-wrap).
    private var clickhouseDataTypes: [String] {
        [
            // Integers
            "UInt8", "UInt16", "UInt32", "UInt64", "UInt128", "UInt256",
            "Int8", "Int16", "Int32", "Int64", "Int128", "Int256",
            // Floats + decimals
            "Float32", "Float64", "Decimal(18, 4)", "Decimal(38, 4)", "Decimal(76, 4)",
            // Strings
            "String", "FixedString(32)", "UUID",
            // Date/time
            "Date", "Date32", "DateTime", "DateTime64(3)", "DateTime64(6)",
            // Other
            "Boolean", "IPv4", "IPv6", "JSON",
            // Common wrappers (users can edit the inner type)
            "Nullable(String)", "Array(String)", "Array(UInt64)", "Map(String, String)",
        ]
    }

    /// SQL Server data types ordered by category (exact numerics → approximate
    /// numerics → date/time → char strings → unicode strings → binary → other).
    /// Modern variants (VARCHAR(MAX), NVARCHAR(MAX), VARBINARY(MAX)) replace
    /// the deprecated TEXT/NTEXT/IMAGE types but those are kept for compatibility.
    /// Source: https://learn.microsoft.com/sql/t-sql/data-types/data-types-transact-sql
    private var mssqlDataTypes: [String] {
        [
            // Exact numerics
            "INT", "BIGINT", "SMALLINT", "TINYINT", "BIT",
            "DECIMAL", "NUMERIC", "MONEY", "SMALLMONEY",
            // Approximate numerics
            "FLOAT", "REAL",
            // Date and time
            "DATE", "TIME", "DATETIME", "DATETIME2", "DATETIMEOFFSET", "SMALLDATETIME",
            // Character strings
            "CHAR", "VARCHAR", "VARCHAR(MAX)", "TEXT",
            // Unicode character strings
            "NCHAR", "NVARCHAR", "NVARCHAR(MAX)", "NTEXT",
            // Binary strings
            "BINARY", "VARBINARY", "VARBINARY(MAX)", "IMAGE",
            // Other
            "UNIQUEIDENTIFIER", "XML", "JSON", "ROWVERSION", "HIERARCHYID",
            "GEOGRAPHY", "GEOMETRY", "SQL_VARIANT",
        ]
    }
}

// MARK: - Models

struct NewColumn: Identifiable {
    let id = UUID()
    var name: String
    var dataType: String
    var isPrimaryKey: Bool
    var isNullable: Bool
    var defaultValue: String
    var comment: String = ""
    var checkConstraint: String = ""
    var foreignKey: String = ""  // "referenced_table(referenced_col)"
}

struct NewIndex: Identifiable {
    let id = UUID()
    var name: String
    var columns: String
    var isUnique: Bool
    var algorithm: String = "BTREE"
    var condition: String = ""
    var include: String = ""
    var comment: String = ""
}
