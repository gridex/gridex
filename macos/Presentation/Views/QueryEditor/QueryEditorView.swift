// QueryEditorView.swift
// Gridex
//
// SwiftUI query editor: SQL editor (top) + results grid (bottom).
// Uses NSViewRepresentable to wrap NSTextView for syntax highlighting.

import SwiftUI
import AppKit

struct QueryEditorView: View {
    let tabId: UUID
    @EnvironmentObject private var appState: AppState
    @State private var sqlText = ""
    @State private var cursorOffset: Int = 0
    @StateObject private var resultViewModel = DataGridViewState()
    @State private var resultRowCount: Int = 0
    @State private var errorMessage: String?
    @State private var statusMessage: String?
    @State private var isExecuting = false
    @State private var splitRatio: CGFloat = 0.5

    /// EXPLAIN options surfaced in the dropdown next to the Explain button.
    /// Persisted per-connection via UserDefaults so a fix applied in one
    /// session sticks across restarts.
    @State private var explainOptions: ExplainOptions = .default

    /// PG server major version (`SHOW server_version_num` / 10000), used to
    /// grey out options the server doesn't support (Memory needs PG 17+, etc.)
    @State private var pgServerMajorVersion: Int?

    /// Raw EXPLAIN output (text or JSON, depending on `explainOutputFormat`).
    /// Non-nil means the results pane should render the read-only Explain
    /// browser instead of the data grid. Cleared by every regular Run.
    @State private var explainOutput: String?
    @State private var explainOutputFormat: ExplainOptions.Format = .text

    private var isRedis: Bool { appState.activeConfig?.databaseType == .redis }
    private var isPostgres: Bool { appState.activeConfig?.databaseType == .postgresql }

    var body: some View {
        VSplitView {
            // SQL Editor pane
            VStack(spacing: 0) {
                HStack(spacing: 6) {
                    Text(isRedis ? "Redis CLI" : "SQL Editor")
                        .font(.system(size: 11, weight: .medium))
                        .foregroundStyle(.secondary)
                    Spacer()

                    if !isRedis {
                        Button(action: beautifySQL) {
                            HStack(spacing: 4) {
                                Image(systemName: "wand.and.stars")
                                Text("Format")
                            }
                        }
                        .controlSize(.small)
                        .pointerCursor()
                        .help("Beautify SQL (Ctrl+Shift+F)")
                        .keyboardShortcut("f", modifiers: [.control, .shift])

                        Button(action: minifySQL) {
                            HStack(spacing: 4) {
                                Image(systemName: "arrow.down.right.and.arrow.up.left")
                                Text("Minify")
                            }
                        }
                        .controlSize(.small)
                        .pointerCursor()
                        .help("Minify SQL to single line")

                        Divider().frame(height: 14)
                    }

                    Button(action: executeQuery) {
                        HStack(spacing: 4) {
                            Image(systemName: "play.fill")
                            Text("Run")
                        }
                    }
                    .controlSize(.small)
                    .pointerCursor()
                    .keyboardShortcut("r", modifiers: .command)
                    .disabled(isExecuting || sqlText.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                    .help(isRedis ? "Run Redis command (⌘R)" : "Run statement at cursor (⌘R)")

                    if !isRedis {
                        // Split: primary button runs EXPLAIN with the current
                        // options; the chevron beside opens the toggle menu.
                        // For non-PG engines the menu is hidden — they have
                        // no per-option syntax to surface.
                        HStack(spacing: 0) {
                            Button(action: explainQuery) {
                                HStack(spacing: 4) {
                                    Image(systemName: "lightbulb")
                                    Text("Explain")
                                }
                            }
                            .controlSize(.small)
                            .pointerCursor()

                            if isPostgres {
                                ExplainOptionsMenu(
                                    options: $explainOptions,
                                    serverMajorVersion: pgServerMajorVersion
                                )
                                .padding(.leading, 2)
                            }
                        }
                    }
                }
                .padding(.horizontal, 8)
                .padding(.vertical, 4)
                .background(Color(nsColor: .windowBackgroundColor))

                SQLEditorView(text: $sqlText, cursorOffset: $cursorOffset, adapter: appState.activeAdapter)
                    .frame(maxHeight: .infinity)
            }

            // Results pane
            VStack(spacing: 0) {
                HStack {
                    Text("Results")
                        .font(.system(size: 11, weight: .medium))
                        .foregroundStyle(.secondary)
                    if resultRowCount > 0 {
                        Text("(\(resultRowCount) rows)")
                            .font(.system(size: 11))
                            .foregroundStyle(.tertiary)
                    }
                    Spacer()
                }
                .padding(.horizontal, 8)
                .padding(.vertical, 4)
                .background(Color(nsColor: .windowBackgroundColor))

                if isExecuting {
                    ProgressView("Executing...")
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                } else if let error = errorMessage {
                    VStack {
                        Image(systemName: "exclamationmark.triangle")
                            .font(.title)
                            .foregroundStyle(.red)
                        Text(error)
                            .foregroundStyle(.red)
                            .font(.system(size: 13, design: .monospaced))
                            .textSelection(.enabled)
                    }
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .padding()
                } else if let status = statusMessage {
                    VStack(spacing: 8) {
                        Image(systemName: "checkmark.circle")
                            .font(.title)
                            .foregroundStyle(.green)
                        Text(status)
                            .foregroundStyle(.secondary)
                            .font(.system(size: 13, design: .monospaced))
                            .textSelection(.enabled)
                    }
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .padding()
                } else if let output = explainOutput {
                    ExplainOutputView(raw: output, format: explainOutputFormat)
                } else if !resultViewModel.columns.isEmpty {
                    AppKitDataGrid(
                        viewModel: resultViewModel,
                        onSelectRows: { _ in },
                        onFKClick: nil
                    )
                } else {
                    Text("Run a query to see results")
                        .foregroundStyle(.secondary)
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                }
            }
        }
        .onReceive(NotificationCenter.default.publisher(for: .executeQuery)) { _ in
            executeQuery()
        }
        .onReceive(NotificationCenter.default.publisher(for: .explainQuery)) { _ in
            explainQuery()
        }
        .onAppear {
            // Restore persisted text for this tab
            if let saved = appState.queryEditorText[tabId] {
                sqlText = saved
            }
            loadExplainOptionsForActiveConnection()
        }
        .onChange(of: sqlText) { _, newValue in
            // Persist so tab switches don't lose the query
            appState.queryEditorText[tabId] = newValue
        }
        .onChange(of: appState.activeConnectionId) { _, _ in
            loadExplainOptionsForActiveConnection()
        }
        .onReceive(NotificationCenter.default.publisher(for: .init("pasteQueryToEditor"))) { notif in
            // Only the currently active query editor tab handles the paste
            guard appState.activeTabId == tabId,
                  let sql = notif.userInfo?["sql"] as? String else { return }
            sqlText = sql
        }
    }

    // MARK: - Cursor-aware statement extraction

    /// Returns the SQL statement at the current cursor position.
    /// Falls back to the full text if there's only one statement or selection is empty.
    private func statementToRun() -> String {
        let trimmed = sqlText.trimmingCharacters(in: .whitespacesAndNewlines)
        // If no semicolons present, just use the whole text
        if !sqlText.contains(";") { return trimmed }
        let stmt = SQLFormatter.statementAt(sql: sqlText, cursorOffset: cursorOffset)
        return stmt.isEmpty ? trimmed : stmt
    }

    // MARK: - Format / Minify

    private func beautifySQL() {
        sqlText = SQLFormatter.beautify(sqlText)
    }

    private func minifySQL() {
        sqlText = SQLFormatter.minify(sqlText)
    }

    private func executeQuery() {
        guard let adapter = appState.activeAdapter else { return }

        // A regular Run wipes any previous EXPLAIN output so the data grid
        // can take over the results pane again.
        explainOutput = nil

        // Decide: run a single statement at cursor, or a full multi-statement script.
        // Heuristic: if the text contains `GO` batch separators (SQL Server) or
        // multiple non-empty statements separated by `;`, treat it as a script.
        let fullText = sqlText.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !fullText.isEmpty else { return }

        let batches = splitIntoBatches(fullText, databaseType: adapter.databaseType)

        // If only one statement, use the cursor-aware behavior (backwards compat)
        let toRun: [String]
        if batches.count <= 1 {
            let single = statementToRun()
            guard !single.isEmpty else { return }
            toRun = [single]
        } else {
            toRun = batches
        }

        isExecuting = true
        errorMessage = nil
        statusMessage = nil

        // For MSSQL multi-batch scripts, use a dedicated connection so that
        // USE statements persist across batches.
        let isMultiBatchMSSQL = adapter.databaseType == .mssql && toRun.count > 1
        let mssqlAdapter = adapter as? MSSQLAdapter

        Task {
            if isMultiBatchMSSQL, let mssql = mssqlAdapter {
                do {
                    try await mssql.beginScript()
                } catch {
                    errorMessage = error.localizedDescription
                    isExecuting = false
                    return
                }
            }
            var lastResult: QueryResult?
            var totalRowsAffected = 0
            let overallStart = Date()
            for stmt in toRun {
                let start = Date()
                do {
                    let result = try await adapter.executeRaw(sql: stmt)
                    let duration = Date().timeIntervalSince(start)
                    appState.logQuery(sql: stmt, duration: duration)
                    appState.recordQueryHistory(sql: stmt, duration: duration, rowCount: result.rowCount)
                    // Remember the most recent result that has rows (SELECT) to display
                    if !result.columns.isEmpty {
                        lastResult = result
                    }
                    totalRowsAffected += result.rowsAffected
                } catch {
                    let duration = Date().timeIntervalSince(start)
                    appState.logQuery(sql: stmt, duration: duration)
                    appState.recordQueryHistory(sql: stmt, duration: duration, error: error.localizedDescription)
                    errorMessage = error.localizedDescription
                    if isMultiBatchMSSQL, let mssql = mssqlAdapter {
                        await mssql.endScript()
                    }
                    isExecuting = false
                    // Reload sidebar so DDL effects show up even on partial failure
                    appState.refreshSidebar()
                    return
                }
            }
            if isMultiBatchMSSQL, let mssql = mssqlAdapter {
                await mssql.endScript()
            }

            if let result = lastResult {
                applyResult(result)
                appState.statusRowCount = result.rowCount
                appState.statusQueryTime = result.executionTime
            } else if toRun.count > 1 {
                // Script with only DDL/DML — show a success status (not an error)
                let totalDuration = Date().timeIntervalSince(overallStart)
                statusMessage = "\(toRun.count) statement(s) executed · \(totalRowsAffected) row(s) affected · \(String(format: "%.2fs", totalDuration))"
            }

            // If the script changed database context (USE) or created/dropped databases,
            // sync AppState so the sidebar and DB picker reflect the new state.
            let upperScript = fullText.uppercased()
            let changedDBContext = upperScript.contains("USE ")
                || upperScript.contains("CREATE DATABASE")
                || upperScript.contains("DROP DATABASE")
            if changedDBContext {
                // Re-fetch current database from the server and update AppState
                if let newDB = try? await adapter.currentDatabase() {
                    appState.currentDatabaseName = newDB
                }
                // Refresh the available databases list (picker shows new/removed DBs)
                await appState.refreshAvailableDatabases()
            }

            // Script may have created/altered/dropped tables — refresh the sidebar
            appState.refreshSidebar()
            isExecuting = false
        }
    }

    /// Split a SQL script into individual statements/batches. SQL Server uses
    /// `GO` on its own line as a batch separator; other databases use `;`.
    private func splitIntoBatches(_ script: String, databaseType: DatabaseType) -> [String] {
        if databaseType == .mssql {
            // Split on lines containing only "GO" (case-insensitive, allow whitespace)
            let lines = script.components(separatedBy: .newlines)
            var batches: [String] = []
            var current: [String] = []
            for line in lines {
                let trimmed = line.trimmingCharacters(in: .whitespaces)
                if trimmed.uppercased() == "GO" {
                    let batch = current.joined(separator: "\n").trimmingCharacters(in: .whitespacesAndNewlines)
                    if !batch.isEmpty { batches.append(batch) }
                    current = []
                } else {
                    current.append(line)
                }
            }
            let last = current.joined(separator: "\n").trimmingCharacters(in: .whitespacesAndNewlines)
            if !last.isEmpty { batches.append(last) }
            return batches
        }

        // Non-MSSQL: split by `;`, respecting quoted strings and comments.
        return splitSQLStatements(script)
    }

    /// Split SQL by `;` terminator, respecting single/double quoted strings and line comments.
    private func splitSQLStatements(_ sql: String) -> [String] {
        var statements: [String] = []
        var current = ""
        var inSingle = false
        var inDouble = false
        var inLineComment = false
        var i = sql.startIndex

        while i < sql.endIndex {
            let ch = sql[i]
            let next = sql.index(after: i) < sql.endIndex ? sql[sql.index(after: i)] : " "

            if inLineComment {
                current.append(ch)
                if ch == "\n" { inLineComment = false }
            } else if inSingle {
                current.append(ch)
                if ch == "'" { inSingle = false }
            } else if inDouble {
                current.append(ch)
                if ch == "\"" { inDouble = false }
            } else if ch == "-" && next == "-" {
                inLineComment = true
                current.append(ch)
            } else if ch == "'" {
                inSingle = true
                current.append(ch)
            } else if ch == "\"" {
                inDouble = true
                current.append(ch)
            } else if ch == ";" {
                let stmt = current.trimmingCharacters(in: .whitespacesAndNewlines)
                if !stmt.isEmpty { statements.append(stmt) }
                current = ""
            } else {
                current.append(ch)
            }
            i = sql.index(after: i)
        }
        let tail = current.trimmingCharacters(in: .whitespacesAndNewlines)
        if !tail.isEmpty { statements.append(tail) }
        return statements
    }

    private func explainQuery() {
        guard let adapter = appState.activeAdapter else { return }
        let sql = statementToRun()
        guard !sql.isEmpty else { return }

        isExecuting = true
        errorMessage = nil

        // Options apply only to PG (other engines ignore the param).
        let optsForBuild = isPostgres ? explainOptions : .default
        guard let explainSQL = adapter.databaseType.explainSQL(for: sql, options: optsForBuild) else {
            errorMessage = "EXPLAIN is not supported for \(adapter.databaseType.displayName) connections."
            isExecuting = false
            return
        }

        // Persist whatever the user just used so the next session restores it.
        if isPostgres, let connId = appState.activeConnectionId {
            persistExplainOptions(explainOptions, for: connId)
        }

        Task {
            let start = Date()
            do {
                let result = try await adapter.executeRaw(sql: explainSQL)
                let duration = Date().timeIntervalSince(start)
                appState.logQuery(sql: explainSQL, duration: duration)
                appState.recordQueryHistory(sql: explainSQL, duration: duration, rowCount: result.rowCount)

                // PG returns the plan as one row per text line (FORMAT TEXT)
                // or one row containing the whole document (FORMAT JSON).
                // Either way, joining the first cell of every row yields the
                // raw output we hand to ExplainOutputView.
                let raw = result.rows
                    .compactMap { $0.first?.stringValue }
                    .joined(separator: "\n")
                explainOutput = raw
                explainOutputFormat = isPostgres ? optsForBuild.format : .text
                resultViewModel.columns = []  // hide grid; show explain panel
                resultViewModel.rows = []
                resultRowCount = 0
            } catch {
                let duration = Date().timeIntervalSince(start)
                appState.logQuery(sql: explainSQL, duration: duration)
                appState.recordQueryHistory(sql: explainSQL, duration: duration, error: error.localizedDescription)
                errorMessage = error.localizedDescription
                explainOutput = nil
            }
            isExecuting = false
        }
    }

    // MARK: - EXPLAIN options persistence + version detection

    /// Load saved options for the active connection (and probe server version
    /// for PG, so the menu can grey-out unavailable options).
    private func loadExplainOptionsForActiveConnection() {
        guard let connId = appState.activeConnectionId else {
            explainOptions = .default
            pgServerMajorVersion = nil
            return
        }
        let key = ExplainOptions.userDefaultsKey(connectionId: connId)
        if let data = UserDefaults.standard.data(forKey: key),
           let saved = try? JSONDecoder().decode(ExplainOptions.self, from: data) {
            explainOptions = saved
        } else {
            explainOptions = .default
        }

        guard isPostgres, let adapter = appState.activeAdapter else {
            pgServerMajorVersion = nil
            return
        }
        Task {
            // `serverVersion()` returns the long banner; extract the major.
            // Failure → leave nil → menu permits everything (server enforces).
            let version = (try? await adapter.serverVersion()) ?? ""
            await MainActor.run {
                pgServerMajorVersion = Self.parsePostgresMajor(from: version)
            }
        }
    }

    private func persistExplainOptions(_ opts: ExplainOptions, for connId: UUID) {
        guard let data = try? JSONEncoder().encode(opts) else { return }
        UserDefaults.standard.set(data, forKey: ExplainOptions.userDefaultsKey(connectionId: connId))
    }

    /// Pull the major from the `version()` banner.
    /// Examples: "PostgreSQL 16.2 on …" → 16; "PostgreSQL 9.6.24 …" → 9.
    static func parsePostgresMajor(from banner: String) -> Int? {
        // Look for the first integer after "PostgreSQL ".
        guard let range = banner.range(of: "PostgreSQL ") else { return nil }
        let tail = banner[range.upperBound...]
        let digits = tail.prefix { $0.isNumber }
        return Int(digits)
    }

    /// Populate the DataGridViewState with query results so AppKitDataGrid can render them
    /// with the same look & feel as the table data view.
    private func applyResult(_ result: QueryResult) {
        resultViewModel.columns = result.columns
        resultViewModel.rows = result.rows
        resultViewModel.totalRows = result.rowCount
        resultViewModel.primaryKeyColumns = []
        resultViewModel.foreignKeyColumns = [:]
        resultViewModel.foreignKeyRefColumns = [:]
        resultViewModel.columnDefaults = [:]
        resultViewModel.columnEnumValues = [:]
        resultViewModel.sortColumn = nil
        resultViewModel.insertedRowIndices = []
        resultViewModel.selectedRows = []
        resultViewModel.editingCell = nil
        // Reset column widths so the grid auto-sizes for the new result set
        resultViewModel.columnWidths = [:]
        resultRowCount = result.rowCount
    }
}

// MARK: - NSViewRepresentable SQL Editor with Syntax Highlighting + Autocomplete

struct SQLEditorView: NSViewRepresentable {
    @Binding var text: String
    @Binding var cursorOffset: Int
    let adapter: (any DatabaseAdapter)?

    func makeNSView(context: Context) -> NSScrollView {
        let scrollView = NSScrollView()
        scrollView.hasVerticalScroller = true
        scrollView.hasHorizontalScroller = false

        let textView = SQLTextView()
        textView.isEditable = true
        textView.isRichText = false
        textView.allowsUndo = true
        textView.delegate = context.coordinator
        textView.autoresizingMask = [.width]
        textView.isVerticallyResizable = true
        textView.isHorizontallyResizable = false
        textView.textContainer?.widthTracksTextView = true
        textView.textContainer?.containerSize = NSSize(width: 0, height: CGFloat.greatestFiniteMagnitude)

        scrollView.documentView = textView
        context.coordinator.textView = textView
        context.coordinator.highlighter = SyntaxHighlighter(textView: textView)
        context.coordinator.setupCompletion(textView: textView)

        // Load schema for autocomplete
        context.coordinator.loadSchema(adapter: adapter)

        return scrollView
    }

    func updateNSView(_ scrollView: NSScrollView, context: Context) {
        guard let textView = scrollView.documentView as? NSTextView else { return }
        if textView.string != text {
            textView.string = text
            if let storage = textView.textStorage {
                context.coordinator.highlighter?.highlight(storage)
            }
        }
        // Refresh schema if adapter changed
        context.coordinator.loadSchema(adapter: adapter)
    }

    func makeCoordinator() -> Coordinator {
        Coordinator(text: $text, cursorOffset: $cursorOffset)
    }

    class Coordinator: NSObject, NSTextViewDelegate {
        @Binding var text: String
        @Binding var cursorOffset: Int
        weak var textView: NSTextView?
        var highlighter: SyntaxHighlighter?

        private let completionProvider = AutocompleteProvider()
        private let contextParser = SQLContextParser()
        private let completionWindow = CompletionWindow()
        private var schemaLoaded = false
        private var debounceWork: DispatchWorkItem?
        private var lastInsertedChar: Character?

        init(text: Binding<String>, cursorOffset: Binding<Int>) {
            _text = text
            _cursorOffset = cursorOffset
            super.init()
            completionWindow.onSelect = { [weak self] item in
                self?.insertCompletion(item)
            }
        }

        /// Called by NSTextView when the selection (cursor) changes.
        func textViewDidChangeSelection(_ notification: Notification) {
            guard let textView = notification.object as? NSTextView else { return }
            cursorOffset = textView.selectedRange().location
        }

        func setupCompletion(textView: NSTextView) {
            if let sqlTextView = textView as? SQLTextView {
                sqlTextView.completionCoordinator = self
            }
        }

        func loadSchema(adapter: (any DatabaseAdapter)?) {
            guard !schemaLoaded, let adapter else { return }
            schemaLoaded = true
            Task { @MainActor in
                do {
                    let tables = try await adapter.listTables(schema: nil)
                    var descriptions: [TableDescription] = []
                    for table in tables {
                        if let desc = try? await adapter.describeTable(name: table.name, schema: table.schema) {
                            descriptions.append(desc)
                        }
                    }
                    completionProvider.updateSchema(descriptions)
                } catch {
                    schemaLoaded = false
                }
            }
        }

        func textDidChange(_ notification: Notification) {
            guard let textView = notification.object as? NSTextView else { return }
            text = textView.string
            cursorOffset = textView.selectedRange().location

            // Detect what character was just typed
            let sql = textView.string
            let cursor = textView.selectedRange().location
            if cursor > 0, cursor <= sql.utf16.count {
                let idx = sql.index(sql.startIndex, offsetBy: cursor - 1, limitedBy: sql.endIndex) ?? sql.endIndex
                if idx < sql.endIndex {
                    lastInsertedChar = sql[idx]
                }
            }

            // Dismiss on space, semicolon, or when the current word is empty.
            // Only show completions when user is actively typing an identifier/keyword.
            if let ch = lastInsertedChar {
                if ch == " " || ch == ";" || ch == "\n" || ch == "\t" {
                    dismissCompletion()
                    return
                }
                // Instant trigger on dot (alias.column)
                if ch == "." {
                    triggerCompletionNow()
                    return
                }
            }

            // Only trigger if current word is non-empty (user is typing a word)
            let currentWordAtCursor = wordAtCursor(sql: sql, cursor: cursor)
            if currentWordAtCursor.isEmpty {
                dismissCompletion()
                return
            }

            // Debounced trigger (80ms)
            debounceWork?.cancel()
            let work = DispatchWorkItem { [weak self] in
                self?.triggerCompletionNow()
            }
            debounceWork = work
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.08, execute: work)
        }

        private func wordAtCursor(sql: String, cursor: Int) -> String {
            let safeCursor = max(0, min(cursor, sql.count))
            let before = String(sql.prefix(safeCursor))
            var start = before.endIndex
            while start > before.startIndex {
                let prev = before.index(before: start)
                let ch = before[prev]
                if ch.isLetter || ch.isNumber || ch == "_" || ch == "." {
                    start = prev
                } else {
                    break
                }
            }
            return String(before[start..<before.endIndex])
        }

        private func lastWordBefore(_ text: String) -> String {
            var end = text.endIndex
            // Skip trailing spaces
            while end > text.startIndex && text[text.index(before: end)] == " " {
                end = text.index(before: end)
            }
            var start = end
            while start > text.startIndex {
                let prev = text.index(before: start)
                let ch = text[prev]
                if ch.isLetter || ch.isNumber || ch == "_" {
                    start = prev
                } else {
                    break
                }
            }
            return String(text[start..<end])
        }

        // MARK: - Completion Logic

        func triggerCompletionNow() {
            guard let textView else { return }
            let sql = textView.string
            let cursorLocation = textView.selectedRange().location
            guard cursorLocation <= sql.utf16.count else { return }

            // Simple mode: take current word as prefix, search everything
            let prefix = wordAtCursor(sql: sql, cursor: cursorLocation)
            guard !prefix.isEmpty else {
                dismissCompletion()
                return
            }

            let context = CompletionContext(
                trigger: .general,
                prefix: prefix,
                scopeTables: []
            )
            let items = completionProvider.suggestions(for: context)

            if items.isEmpty {
                dismissCompletion()
                return
            }

            // Don't show if only 1 item and it exactly matches the prefix
            if items.count == 1 && items[0].text.lowercased() == context.prefix.lowercased() {
                dismissCompletion()
                return
            }

            showCompletionWindow(items: items, cursorLocation: cursorLocation)
        }

        private func showCompletionWindow(items: [CompletionItem], cursorLocation: Int) {
            guard let textView else { return }

            let glyphRange = textView.layoutManager?.glyphRange(
                forCharacterRange: NSRange(location: cursorLocation, length: 0),
                actualCharacterRange: nil
            ) ?? NSRange(location: cursorLocation, length: 0)

            var rect = textView.layoutManager?.boundingRect(forGlyphRange: glyphRange, in: textView.textContainer!) ?? .zero
            rect.origin.x += textView.textContainerInset.width
            rect.origin.y += textView.textContainerInset.height

            let windowPoint = textView.convert(NSPoint(x: rect.origin.x, y: rect.origin.y + rect.height + 4), to: nil)
            let screenPoint = textView.window?.convertPoint(toScreen: windowPoint) ?? windowPoint

            completionWindow.show(items: items, at: screenPoint)
        }

        func dismissCompletion() {
            debounceWork?.cancel()
            completionWindow.dismiss()
        }

        func insertCompletion(_ item: CompletionItem) {
            guard let textView else { return }

            let sql = textView.string
            let cursorLocation = textView.selectedRange().location
            let context = contextParser.parse(sql: sql, cursorOffset: cursorLocation)
            let prefix = context.prefix

            // For JOIN completions, replace the whole typed prefix (e.g., "j" or "jo")
            let prefixLength: Int
            if item.type == .join {
                // Replace everything the user typed for the join
                prefixLength = prefix.utf16.count
            } else if prefix.contains(".") {
                // For alias.column, only replace after the dot
                let afterDot = prefix.split(separator: ".", maxSplits: 1)
                prefixLength = afterDot.count > 1 ? afterDot[1].utf16.count : 0
            } else {
                prefixLength = prefix.utf16.count
            }

            let safePrefixLength = max(0, min(prefixLength, cursorLocation))
            let replaceRange = NSRange(location: cursorLocation - safePrefixLength, length: safePrefixLength)

            if textView.shouldChangeText(in: replaceRange, replacementString: item.insertText) {
                textView.replaceCharacters(in: replaceRange, with: item.insertText)
                textView.didChangeText()
            }

            completionProvider.trackUsed(item.text)
            dismissCompletion()
        }

        // MARK: - Key Handling

        func handleKeyForCompletion(_ event: NSEvent) -> Bool {
            guard completionWindow.isActive else { return false }

            switch event.keyCode {
            case 125: // Down arrow
                completionWindow.moveSelectionDown()
                return true
            case 126: // Up arrow
                completionWindow.moveSelectionUp()
                return true
            case 36: // Return/Enter
                if let item = completionWindow.selectedItem {
                    insertCompletion(item)
                    return true
                }
            case 48: // Tab
                if let item = completionWindow.selectedItem {
                    insertCompletion(item)
                    return true
                }
            case 53: // Escape
                dismissCompletion()
                return true
            default:
                break
            }

            return false
        }
    }
}
