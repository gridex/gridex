// AppKitDataGrid.swift
// Gridex
//
// High-performance NSTableView wrapper for the data grid.
// The Coordinator observes the viewModel directly via Combine,
// bypassing SwiftUI's update cycle to avoid unnecessary reloadData() calls.

import SwiftUI
import AppKit
import Combine
import ObjectiveC

struct AppKitDataGrid: NSViewRepresentable {
    let viewModel: DataGridViewState
    let onSelectRows: (Set<Int>) -> Void
    var onFKClick: ((_ refTable: String, _ refColumn: String, _ value: String) -> Void)?

    func makeNSView(context: Context) -> NSScrollView {
        let scrollView = NSScrollView()
        scrollView.hasVerticalScroller = true
        scrollView.hasHorizontalScroller = true
        scrollView.autohidesScrollers = false
        scrollView.borderType = .noBorder

        let tableView = DataGridTableView()
        tableView.style = .plain
        tableView.usesAlternatingRowBackgroundColors = true
        tableView.allowsMultipleSelection = true
        tableView.allowsColumnReordering = false
        tableView.allowsColumnResizing = true
        tableView.columnAutoresizingStyle = .noColumnAutoresizing
        tableView.intercellSpacing = NSSize(width: 0, height: 0)
        tableView.rowHeight = 28
        tableView.headerView = NSTableHeaderView()
        tableView.gridStyleMask = [.solidVerticalGridLineMask, .solidHorizontalGridLineMask]
        tableView.gridColor = NSColor.separatorColor.withAlphaComponent(0.3)
        tableView.selectionHighlightStyle = .regular
        tableView.doubleAction = #selector(context.coordinator.tableViewDoubleClick(_:))
        tableView.target = context.coordinator

        let menu = NSMenu()
        menu.autoenablesItems = false
        menu.delegate = context.coordinator
        let copyRowItem = NSMenuItem(title: "Copy Row", action: #selector(context.coordinator.copyRow(_:)), keyEquivalent: "")
        copyRowItem.target = context.coordinator
        let copyCellItem = NSMenuItem(title: "Copy Cell", action: #selector(context.coordinator.copyCell(_:)), keyEquivalent: "")
        copyCellItem.target = context.coordinator
        let deleteItem = NSMenuItem(title: "Delete Row(s)", action: #selector(context.coordinator.deleteRows(_:)), keyEquivalent: "")
        deleteItem.target = context.coordinator
        menu.addItem(copyRowItem)
        menu.addItem(copyCellItem)
        menu.addItem(NSMenuItem.separator())
        menu.addItem(deleteItem)
        tableView.menu = menu

        scrollView.documentView = tableView
        tableView.dataSource = context.coordinator
        tableView.delegate = context.coordinator

        context.coordinator.tableView = tableView
        context.coordinator.onSelectRows = onSelectRows
        tableView.gridCoordinator = context.coordinator
        context.coordinator.bind(to: viewModel)

        return scrollView
    }

    func updateNSView(_ scrollView: NSScrollView, context: Context) {
        context.coordinator.onSelectRows = onSelectRows
        context.coordinator.onFKClick = onFKClick
    }

    func makeCoordinator() -> Coordinator {
        Coordinator()
    }

    // MARK: - Coordinator

    @MainActor
    class Coordinator: NSObject, NSTableViewDataSource, NSTableViewDelegate, NSTextFieldDelegate, NSMenuDelegate {
        weak var tableView: NSTableView?
        var onSelectRows: ((Set<Int>) -> Void)?
        var onFKClick: ((_ refTable: String, _ refColumn: String, _ value: String) -> Void)?

        private(set) var columns: [ColumnHeader] = []
        private(set) var rows: [[RowValue]] = []
        private(set) var columnWidths: [String: CGFloat] = [:]
        private(set) var pendingChanges: [CellEdit] = []
        private(set) var insertedRowIndices: Set<Int> = []
        private(set) var foreignKeyColumns: [String: String] = [:]
        private(set) var foreignKeyRefColumns: [String: String] = [:]
        private(set) var columnDefaults: [String: String] = [:]
        private(set) var columnEnumValues: [String: [String]] = [:]

        private weak var viewModel: DataGridViewState?
        private var cancellables = Set<AnyCancellable>()
        private var isUpdating = false
        private var isEditing = false
        private var justFinishedEditing = false
        private var previousColumnNames: [String] = []
        private var previousRowCount = 0

        // Cached lookups for O(1) access
        private var deletedRows: Set<Int> = []
        private var modifiedCells: Set<String> = []  // "row:colName"
        private var columnNameToIndex: [String: Int] = [:]
        private var dateColumnIndices: Set<Int> = []
        private var boolColumnIndices: Set<Int> = []

        private var allowsMutations: Bool {
            viewModel?.allowsMutations == true
        }

        private func rebuildPendingChangeCaches() {
            deletedRows = []
            modifiedCells = []
            for change in pendingChanges {
                if change.editType == .delete {
                    deletedRows.insert(change.row)
                } else if change.editType == .update {
                    if let col = change.column {
                        modifiedCells.insert("\(change.row):\(col)")
                    }
                }
            }
        }

        private func rebuildColumnCaches() {
            columnNameToIndex = [:]
            dateColumnIndices = []
            boolColumnIndices = []
            for (idx, col) in columns.enumerated() {
                columnNameToIndex[col.name] = idx
                let dt = col.dataType.lowercased()
                if dt.contains("date") || dt.contains("time") || dt.contains("timestamp") {
                    dateColumnIndices.insert(idx)
                }
                if dt == "boolean" || dt == "bool" || dt == "tinyint(1)" {
                    boolColumnIndices.insert(idx)
                }
            }
        }

        static let cellFont = NSFont.monospacedSystemFont(ofSize: 12, weight: .regular)
        static let nullFont: NSFont = {
            let descriptor = cellFont.fontDescriptor.withSymbolicTraits(.italic)
            return NSFont(descriptor: descriptor, size: 12) ?? cellFont
        }()

        // MARK: - ViewModel Binding

        nonisolated func bind(to viewModel: DataGridViewState) {
            MainActor.assumeIsolated {
                self.viewModel = viewModel
                self.cancellables.removeAll()
                self.snapshotFromViewModel()
                self.rebuildColumns()

                viewModel.objectWillChange
                    .receive(on: RunLoop.main)
                    .debounce(for: .milliseconds(100), scheduler: RunLoop.main)
                    .sink { [weak self, weak viewModel] _ in
                        MainActor.assumeIsolated {
                            guard viewModel != nil else { return }
                            self?.handleViewModelUpdate()
                        }
                    }
                    .store(in: &self.cancellables)
            }
        }

        func releaseData() {
            cancellables.removeAll()
            rows = []
            columns = []
            viewModel = nil
        }

        private func snapshotFromViewModel() {
            guard let vm = viewModel else { return }
            columns = vm.columns
            rows = vm.rows
            columnWidths = vm.columnWidths
            pendingChanges = vm.changeTracker.pendingChanges
            insertedRowIndices = vm.insertedRowIndices
            foreignKeyColumns = vm.foreignKeyColumns
            foreignKeyRefColumns = vm.foreignKeyRefColumns
            columnDefaults = vm.columnDefaults
            columnEnumValues = vm.columnEnumValues
            rebuildColumnCaches()
            rebuildPendingChangeCaches()
        }

        private func handleViewModelUpdate() {
            guard let tableView, let vm = viewModel else { return }

            let newColumnNames = vm.columns.map(\.name)
            let newRowCount = vm.rows.count
            let columnsChanged = newColumnNames != previousColumnNames
            let rowsChanged = vm.rows.count != rows.count
                || (vm.rows.first != rows.first)
            columns = vm.columns
            rows = vm.rows
            columnWidths = vm.columnWidths
            foreignKeyColumns = vm.foreignKeyColumns
            foreignKeyRefColumns = vm.foreignKeyRefColumns
            columnDefaults = vm.columnDefaults
            columnEnumValues = vm.columnEnumValues

            // Always sync pending changes — edits to the same cell change content but not count
            let newPending = vm.changeTracker.pendingChanges
            let newInserted = vm.insertedRowIndices
            let pendingChanged = newPending.count != pendingChanges.count
                || newInserted != insertedRowIndices
                || newPending.map(\.column) != pendingChanges.map(\.column)
            pendingChanges = newPending
            insertedRowIndices = newInserted
            if pendingChanged {
                rebuildPendingChangeCaches()
            }
            if columnsChanged {
                rebuildColumnCaches()
            }

            guard !isEditing else {
                previousRowCount = newRowCount
                previousColumnNames = newColumnNames
                return
            }

            if justFinishedEditing {
                justFinishedEditing = false
                previousRowCount = newRowCount
                previousColumnNames = newColumnNames
                // Must rebuild caches since pending changes updated during edit
                pendingChanges = vm.changeTracker.pendingChanges
                insertedRowIndices = vm.insertedRowIndices
                rebuildPendingChangeCaches()
                // Force redraw all visible cells — cells may have been blanked
                // while the floating editor was active
                let visibleRange = tableView.rows(in: tableView.visibleRect)
                guard visibleRange.length > 0 else { return }
                for row in visibleRange.location..<(visibleRange.location + visibleRange.length) {
                    for colIdx in 0..<tableView.numberOfColumns {
                        if let cell = tableView.view(atColumn: colIdx, row: row, makeIfNecessary: false) as? DataGridCellView {
                            cell.isEditingActive = false
                            configureCell(cell, row: row, col: colIdx)
                            cell.needsDisplay = true
                        }
                    }
                }
                return
            }

            if columnsChanged {
                previousColumnNames = newColumnNames
                previousRowCount = newRowCount
                rebuildColumns()
                isUpdating = true
                tableView.reloadData()
                isUpdating = false
                return
            }

            if newRowCount != previousRowCount {
                let wasInsert = newRowCount == previousRowCount + 1 && vm.insertedRowIndices.contains(newRowCount - 1)
                previousRowCount = newRowCount
                isUpdating = true
                tableView.reloadData()
                isUpdating = false
                if wasInsert, newRowCount > 0 {
                    tableView.scrollRowToVisible(newRowCount - 1)
                }
                return
            }

            // Only refresh visible cells if data or pending changes actually changed
            guard rowsChanged || pendingChanged else { return }

            let visibleRange = tableView.rows(in: tableView.visibleRect)
            guard visibleRange.length > 0 else { return }
            for row in visibleRange.location..<(visibleRange.location + visibleRange.length) {
                refreshRow(row, in: tableView)
            }
        }

        private func rebuildColumns() {
            guard let tableView, let vm = viewModel else { return }

            for col in tableView.tableColumns.reversed() {
                tableView.removeTableColumn(col)
            }
            for colHeader in vm.columns {
                let col = NSTableColumn(identifier: NSUserInterfaceItemIdentifier(colHeader.name))
                col.title = colHeader.name
                col.width = vm.columnWidths[colHeader.name] ?? 120
                col.minWidth = 50
                col.maxWidth = 2000
                col.headerCell = SortableHeaderCell(textCell: colHeader.name)
                col.sortDescriptorPrototype = NSSortDescriptor(key: colHeader.name, ascending: true)
                tableView.addTableColumn(col)
            }
            previousColumnNames = vm.columns.map(\.name)
        }

        // MARK: - Cell Configuration

        private func configureCell(_ cell: DataGridCellView, row: Int, col: Int) {
            let value = col < rows[row].count ? rows[row][col] : .null
            let isInserted = insertedRowIndices.contains(row)
            var dirty = false

            let newText: String
            let newFont: NSFont
            let newColor: NSColor
            if value.isNull && isInserted {
                newText = "DEFAULT"
                newFont = Self.cellFont
                newColor = NSColor.secondaryLabelColor
            } else if value.isNull {
                newText = "NULL"
                newFont = Self.nullFont
                newColor = NSColor.Gridex.cellNull
            } else {
                // Use pre-computed display cache when available
                if let vm = viewModel,
                   row < vm.displayCache.count,
                   col < vm.displayCache[row].count {
                    newText = vm.displayCache[row][col]
                } else {
                    newText = value.displayString
                }
                newFont = Self.cellFont
                newColor = .labelColor
            }

            if cell.text != newText {
                cell.text = newText
                cell.toolTip = newText.count > 30 ? (newText.count > 200 ? String(newText.prefix(200)) + "…" : newText) : nil
                dirty = true
            }
            if cell.textFont != newFont { cell.textFont = newFont; dirty = true }
            if cell.textColor != newColor { cell.textColor = newColor; dirty = true }

            let newAlignment: NSTextAlignment = value.isNumeric ? .right : .left
            if cell.textAlignment != newAlignment { cell.textAlignment = newAlignment; dirty = true }
            cell.isDateColumn = allowsMutations && isDateColumn(col)
            cell.isBooleanColumn = allowsMutations && isBoolColumn(col)

            let colName = col < columns.count ? columns[col].name : ""
            cell.foreignKeyTarget = foreignKeyColumns[colName]
            cell.foreignKeyRefColumn = foreignKeyRefColumns[colName]
            cell.cellValue = value == .null ? "" : newText
            cell.onFKClick = onFKClick
            cell.hasDefaultValue = allowsMutations && columnDefaults[colName] != nil
            cell.hasEnumValues = allowsMutations && columnEnumValues[colName] != nil

            let isModified = modifiedCells.contains("\(row):\(colName)")
            let newBg = isModified ? NSColor.Gridex.cellModified : nil
            if cell.cellBackgroundColor != newBg { cell.cellBackgroundColor = newBg; dirty = true }
            if dirty { cell.needsDisplay = true }
        }

        private func refreshRow(_ row: Int, in tableView: NSTableView) {
            guard row < rows.count else { return }

            // Update row view background (deleted = red, inserted = blue)
            if let rowView = tableView.rowView(atRow: row, makeIfNecessary: false) as? DataGridRowView {
                let isDeleted = deletedRows.contains(row)
                let isInserted = insertedRowIndices.contains(row)
                if isDeleted {
                    rowView.overrideBackgroundColor = NSColor.Gridex.cellDeleted
                } else if isInserted {
                    rowView.overrideBackgroundColor = NSColor.systemBlue.withAlphaComponent(0.12)
                } else {
                    rowView.overrideBackgroundColor = nil
                }
                rowView.needsDisplay = true
            }

            for colIdx in 0..<tableView.numberOfColumns {
                guard let cell = tableView.view(atColumn: colIdx, row: row, makeIfNecessary: false) as? DataGridCellView,
                      !cell.isEditingActive else { continue }
                configureCell(cell, row: row, col: colIdx)
            }
        }

        // MARK: - DataSource

        nonisolated func numberOfRows(in tableView: NSTableView) -> Int {
            MainActor.assumeIsolated { rows.count }
        }

        nonisolated func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
            MainActor.assumeIsolated {
                guard let tableColumn,
                      let colIdx = columnNameToIndex[tableColumn.identifier.rawValue],
                      row < rows.count else { return nil }

                let cellId = NSUserInterfaceItemIdentifier("DataCell")
                let cell: DataGridCellView
                if let reused = tableView.makeView(withIdentifier: cellId, owner: self) as? DataGridCellView {
                    cell = reused
                    cell.isEditingActive = false
                } else {
                    cell = DataGridCellView()
                    cell.identifier = cellId
                }

                configureCell(cell, row: row, col: colIdx)
                return cell
            }
        }

        nonisolated func tableView(_ tableView: NSTableView, rowViewForRow row: Int) -> NSTableRowView? {
            MainActor.assumeIsolated {
                let rowId = NSUserInterfaceItemIdentifier("DataGridRow")
                let rowView = (tableView.makeView(withIdentifier: rowId, owner: self) as? DataGridRowView) ?? DataGridRowView()
                rowView.identifier = rowId
                let isDeleted = deletedRows.contains(row)
                let isInserted = insertedRowIndices.contains(row)
                if isDeleted {
                    rowView.overrideBackgroundColor = NSColor.Gridex.cellDeleted
                } else if isInserted {
                    rowView.overrideBackgroundColor = NSColor.systemBlue.withAlphaComponent(0.12)
                } else {
                    rowView.overrideBackgroundColor = nil
                }
                return rowView
            }
        }

        // MARK: - Delegate

        nonisolated func tableViewSelectionDidChange(_ notification: Notification) {
            MainActor.assumeIsolated {
                guard !isUpdating, let tableView else { return }
                onSelectRows?(Set(tableView.selectedRowIndexes.map { $0 }))
            }
        }

        nonisolated func tableView(_ tableView: NSTableView, didClick tableColumn: NSTableColumn) {
            MainActor.assumeIsolated {
                guard let vm = viewModel else { return }
                let colName = tableColumn.identifier.rawValue
                if vm.sortColumn == colName {
                    vm.sortAscending.toggle()
                } else {
                    vm.sortColumn = colName
                    vm.sortAscending = true
                }
            }
        }

        nonisolated func tableViewColumnDidResize(_ notification: Notification) {
            MainActor.assumeIsolated {
                guard let col = notification.userInfo?["NSTableColumn"] as? NSTableColumn else { return }
                // Update local cache only — don't fire objectWillChange to avoid full refresh
                columnWidths[col.identifier.rawValue] = col.width
            }
        }

        /// Sync column widths back to view model (called on resize end or when needed)
        private func syncColumnWidthsToViewModel() {
            viewModel?.columnWidths = columnWidths
        }

        // MARK: - Inline Editing

        @objc nonisolated func tableViewDoubleClick(_ sender: Any?) {
            MainActor.assumeIsolated {
                guard let tableView else { return }
                let row = tableView.clickedRow
                let col = tableView.clickedColumn
                guard row >= 0, col >= 0 else { return }
                beginEditing(row: row, col: col)
            }
        }

        private func isDateColumn(_ col: Int) -> Bool {
            dateColumnIndices.contains(col)
        }

        private func isBoolColumn(_ col: Int) -> Bool {
            boolColumnIndices.contains(col)
        }

        func beginEditing(row: Int, col: Int) {
            guard allowsMutations else { return }
            guard let tableView, col < columns.count, row < rows.count else { return }
            if tableView.subviews.contains(where: { $0 is EditContainerView }) { return }

            // Boolean columns show True/False picker
            if isBoolColumn(col) {
                showBooleanMenu(row: row, col: col)
                return
            }

            // Date/time columns show a popup menu instead of a text field
            if isDateColumn(col) {
                showDateMenu(row: row, col: col)
                return
            }

            // Enum columns show a popup menu with enum values
            let colName = columns[col].name
            if columnEnumValues[colName] != nil {
                showEnumMenu(row: row, col: col)
                return
            }

            beginTextEditing(row: row, col: col)
        }

        private func beginTextEditing(row: Int, col: Int) {
            guard allowsMutations else { return }
            guard let tableView, col < columns.count, row < rows.count else { return }

            isEditing = true

            let value = col < rows[row].count ? rows[row][col] : .null
            let initialText = value.isNull ? "" : value.description

            // Hide the cell's drawn text while editing
            if let cellView = tableView.view(atColumn: col, row: row, makeIfNecessary: false) as? DataGridCellView {
                cellView.isEditingActive = true
                cellView.needsDisplay = true
            }

            // Place editor as a direct child of the table view (not the cell)
            // to prevent first responder changes from disrupting the cell hierarchy.
            let cellRect = tableView.frameOfCell(atColumn: col, row: row)

            let container = EditContainerView()
            container.frame = cellRect
            container.wantsLayer = true
            container.layer?.borderColor = NSColor.controlAccentColor.cgColor
            container.layer?.borderWidth = 2
            container.layer?.cornerRadius = 2
            container.layer?.backgroundColor = NSColor.textBackgroundColor.cgColor

            let editor = NSTextField(string: initialText)
            editor.font = Self.cellFont
            editor.isEditable = true
            editor.isBordered = false
            editor.isBezeled = false
            editor.focusRingType = .none
            editor.textColor = .textColor
            editor.backgroundColor = .textBackgroundColor
            editor.drawsBackground = true
            editor.frame = CGRect(x: 4, y: 0, width: cellRect.width - 8, height: cellRect.height)
            editor.autoresizingMask = [.width, .height]
            editor.delegate = self

            let info = EditInfo(row: row, col: col)
            objc_setAssociatedObject(editor, &Self.editInfoKey, info, .OBJC_ASSOCIATION_RETAIN)

            container.addSubview(editor)
            tableView.addSubview(container)
            tableView.window?.makeFirstResponder(editor)
        }

        // MARK: - Date Column Menu

        private var pendingDateEdit: (row: Int, col: Int)?

        private func showDateMenu(row: Int, col: Int) {
            guard allowsMutations else { return }
            guard let tableView else { return }
            pendingDateEdit = (row, col)

            let menu = NSMenu()
            menu.autoenablesItems = false

            let nullItem = NSMenuItem(title: "NULL", action: #selector(dateMenuNull(_:)), keyEquivalent: "")
            nullItem.target = self
            menu.addItem(nullItem)

            if insertedRowIndices.contains(row) {
                let defaultItem = NSMenuItem(title: "DEFAULT", action: #selector(dateMenuDefault(_:)), keyEquivalent: "")
                defaultItem.target = self
                menu.addItem(defaultItem)
            }

            menu.addItem(NSMenuItem.separator())

            let nowItem = NSMenuItem(title: "NOW()", action: #selector(dateMenuNow(_:)), keyEquivalent: "")
            nowItem.target = self
            menu.addItem(nowItem)

            menu.addItem(NSMenuItem.separator())

            let pickerItem = NSMenuItem(title: "Date Picker...", action: #selector(dateMenuPicker(_:)), keyEquivalent: "")
            pickerItem.target = self
            menu.addItem(pickerItem)

            let manualItem = NSMenuItem(title: "Manual input...", action: #selector(dateMenuManual(_:)), keyEquivalent: "")
            manualItem.target = self
            menu.addItem(manualItem)

            let cellRect = tableView.frameOfCell(atColumn: col, row: row)
            let menuOrigin = NSPoint(x: cellRect.minX, y: cellRect.maxY)
            menu.popUp(positioning: nil, at: menuOrigin, in: tableView)
        }

        @objc nonisolated func dateMenuNull(_ sender: Any?) {
            MainActor.assumeIsolated {
                commitDateValue(.null)
            }
        }

        @objc nonisolated func dateMenuDefault(_ sender: Any?) {
            MainActor.assumeIsolated {
                // For new rows, NULL means let DB handle default
                commitDateValue(.null)
            }
        }

        @objc nonisolated func dateMenuNow(_ sender: Any?) {
            MainActor.assumeIsolated {
                commitDateValue(.date(Date()))
            }
        }

        @objc nonisolated func dateMenuManual(_ sender: Any?) {
            MainActor.assumeIsolated {
                guard let edit = pendingDateEdit else { return }
                pendingDateEdit = nil
                beginTextEditing(row: edit.row, col: edit.col)
            }
        }

        @objc nonisolated func dateMenuPicker(_ sender: Any?) {
            MainActor.assumeIsolated {
                guard let edit = pendingDateEdit, let tableView else {
                    pendingDateEdit = nil
                    return
                }

                let cellRect = tableView.frameOfCell(atColumn: edit.col, row: edit.row)
                let screenRect = tableView.convert(cellRect, to: nil)
                let windowRect = tableView.window?.convertToScreen(screenRect) ?? .zero

                // Current value as initial date
                var initialDate = Date()
                if edit.row < rows.count, edit.col < rows[edit.row].count {
                    if case .date(let d) = rows[edit.row][edit.col] {
                        initialDate = d
                    }
                }

                let panel = DatePickerPanel(date: initialDate) { [weak self] selectedDate in
                    self?.commitDateValue(.date(selectedDate))
                }
                panel.show(relativeTo: windowRect)
            }
        }

        private func commitDateValue(_ value: RowValue) {
            guard allowsMutations else {
                pendingDateEdit = nil
                return
            }
            guard let edit = pendingDateEdit else { return }
            pendingDateEdit = nil

            guard edit.row < rows.count, edit.col < columns.count else { return }

            let oldValue = rows[edit.row][edit.col]
            guard oldValue != value else { return }

            if insertedRowIndices.contains(edit.row) {
                viewModel?.commitDateEdit(rowIndex: edit.row, colIdx: edit.col, newValue: value)
            } else {
                viewModel?.commitDateEdit(rowIndex: edit.row, colIdx: edit.col, newValue: value)
            }

            if let vm = viewModel {
                rows = vm.rows
                pendingChanges = vm.changeTracker.pendingChanges
            }

            // Update cell appearance
            if let tableView,
               let cellView = tableView.view(atColumn: edit.col, row: edit.row, makeIfNecessary: false) as? DataGridCellView {
                configureCell(cellView, row: edit.row, col: edit.col)
                if oldValue != value {
                    cellView.cellBackgroundColor = NSColor.Gridex.cellModified
                    cellView.needsDisplay = true
                }
            }
        }

        // MARK: - Default Value Column Menu

        private var pendingDefaultEdit: (row: Int, col: Int)?

        private func showDefaultMenu(row: Int, col: Int) {
            guard allowsMutations else { return }
            guard let tableView else { return }
            pendingDefaultEdit = (row, col)

            let colName = columns[col].name
            let defaultExpr = columnDefaults[colName] ?? ""

            let menu = NSMenu()
            menu.autoenablesItems = false

            let nullItem = NSMenuItem(title: "NULL", action: #selector(defaultMenuNull(_:)), keyEquivalent: "")
            nullItem.target = self
            menu.addItem(nullItem)

            if insertedRowIndices.contains(row) {
                let defaultItem = NSMenuItem(title: "DEFAULT (\(defaultExpr))", action: #selector(defaultMenuDefault(_:)), keyEquivalent: "")
                defaultItem.target = self
                menu.addItem(defaultItem)
            }

            menu.addItem(NSMenuItem.separator())

            let manualItem = NSMenuItem(title: "Manual input...", action: #selector(defaultMenuManual(_:)), keyEquivalent: "")
            manualItem.target = self
            menu.addItem(manualItem)

            let cellRect = tableView.frameOfCell(atColumn: col, row: row)
            let menuOrigin = NSPoint(x: cellRect.minX, y: cellRect.maxY)
            menu.popUp(positioning: nil, at: menuOrigin, in: tableView)
        }

        @objc nonisolated func defaultMenuNull(_ sender: Any?) {
            MainActor.assumeIsolated {
                guard let edit = pendingDefaultEdit else { return }
                pendingDefaultEdit = nil
                commitDefaultValue(.null, edit: edit)
            }
        }

        @objc nonisolated func defaultMenuDefault(_ sender: Any?) {
            MainActor.assumeIsolated {
                guard let edit = pendingDefaultEdit else { return }
                pendingDefaultEdit = nil
                // NULL for inserted rows = let DB handle the default
                commitDefaultValue(.null, edit: edit)
            }
        }

        @objc nonisolated func defaultMenuManual(_ sender: Any?) {
            MainActor.assumeIsolated {
                guard let edit = pendingDefaultEdit else { return }
                pendingDefaultEdit = nil
                beginTextEditing(row: edit.row, col: edit.col)
            }
        }

        private func commitDefaultValue(_ value: RowValue, edit: (row: Int, col: Int)) {
            guard allowsMutations else { return }
            guard edit.row < rows.count, edit.col < columns.count else { return }

            let oldValue = rows[edit.row][edit.col]
            guard oldValue != value else { return }

            if insertedRowIndices.contains(edit.row) {
                viewModel?.commitNewRowEdit(rowIndex: edit.row, colIdx: edit.col, newText: value.isNull ? "NULL" : value.description)
            } else {
                viewModel?.commitCellEdit(rowIndex: edit.row, colIdx: edit.col, newText: value.isNull ? "NULL" : value.description)
            }

            if let vm = viewModel {
                rows = vm.rows
                pendingChanges = vm.changeTracker.pendingChanges
            }

            if let tableView,
               let cellView = tableView.view(atColumn: edit.col, row: edit.row, makeIfNecessary: false) as? DataGridCellView {
                configureCell(cellView, row: edit.row, col: edit.col)
                if oldValue != value {
                    cellView.cellBackgroundColor = NSColor.Gridex.cellModified
                    cellView.needsDisplay = true
                }
            }
        }

        // MARK: - Enum Column Menu

        private var pendingEnumEdit: (row: Int, col: Int)?

        private func showEnumMenu(row: Int, col: Int) {
            guard allowsMutations else { return }
            guard let tableView else { return }
            pendingEnumEdit = (row, col)

            let colName = columns[col].name
            guard let values = columnEnumValues[colName] else { return }

            let menu = NSMenu()
            menu.autoenablesItems = false

            let nullItem = NSMenuItem(title: "NULL", action: #selector(enumMenuNull(_:)), keyEquivalent: "")
            nullItem.target = self
            menu.addItem(nullItem)

            menu.addItem(NSMenuItem.separator())

            for value in values {
                let item = NSMenuItem(title: value, action: #selector(enumMenuSelect(_:)), keyEquivalent: "")
                item.target = self
                item.representedObject = value
                // Check-mark the current value
                let currentValue = rows[row][col]
                if case .string(let s) = currentValue, s == value {
                    item.state = .on
                }
                menu.addItem(item)
            }

            menu.addItem(NSMenuItem.separator())

            let manualItem = NSMenuItem(title: "Manual input...", action: #selector(enumMenuManual(_:)), keyEquivalent: "")
            manualItem.target = self
            menu.addItem(manualItem)

            let cellRect = tableView.frameOfCell(atColumn: col, row: row)
            let menuOrigin = NSPoint(x: cellRect.minX, y: cellRect.maxY)
            menu.popUp(positioning: nil, at: menuOrigin, in: tableView)
        }

        @objc nonisolated func enumMenuNull(_ sender: Any?) {
            MainActor.assumeIsolated {
                guard let edit = pendingEnumEdit else { return }
                pendingEnumEdit = nil
                commitEnumValue(.null, edit: edit)
            }
        }

        @objc nonisolated func enumMenuSelect(_ sender: Any?) {
            MainActor.assumeIsolated {
                guard let edit = pendingEnumEdit,
                      let item = sender as? NSMenuItem,
                      let value = item.representedObject as? String else { return }
                pendingEnumEdit = nil
                commitEnumValue(.string(value), edit: edit)
            }
        }

        @objc nonisolated func enumMenuManual(_ sender: Any?) {
            MainActor.assumeIsolated {
                guard let edit = pendingEnumEdit else { return }
                pendingEnumEdit = nil
                beginTextEditing(row: edit.row, col: edit.col)
            }
        }

        private func commitEnumValue(_ value: RowValue, edit: (row: Int, col: Int)) {
            guard allowsMutations else { return }
            guard edit.row < rows.count, edit.col < columns.count else { return }

            let oldValue = rows[edit.row][edit.col]
            guard oldValue != value else { return }

            if insertedRowIndices.contains(edit.row) {
                viewModel?.commitNewRowEdit(rowIndex: edit.row, colIdx: edit.col, newText: value.isNull ? "NULL" : value.description)
            } else {
                viewModel?.commitCellEdit(rowIndex: edit.row, colIdx: edit.col, newText: value.isNull ? "NULL" : value.description)
            }

            if let vm = viewModel {
                rows = vm.rows
                pendingChanges = vm.changeTracker.pendingChanges
            }

            if let tableView,
               let cellView = tableView.view(atColumn: edit.col, row: edit.row, makeIfNecessary: false) as? DataGridCellView {
                configureCell(cellView, row: edit.row, col: edit.col)
                if oldValue != value {
                    cellView.cellBackgroundColor = NSColor.Gridex.cellModified
                    cellView.needsDisplay = true
                }
            }
        }

        // MARK: - Boolean Column Menu

        private var pendingBoolEdit: (row: Int, col: Int)?

        private func showBooleanMenu(row: Int, col: Int) {
            guard allowsMutations else { return }
            guard let tableView else { return }
            pendingBoolEdit = (row, col)

            let currentValue = rows[row][col]
            let menu = NSMenu()
            menu.autoenablesItems = false

            let nullItem = NSMenuItem(title: "NULL", action: #selector(boolMenuNull(_:)), keyEquivalent: "")
            nullItem.target = self
            if currentValue.isNull { nullItem.state = .on }
            menu.addItem(nullItem)

            menu.addItem(NSMenuItem.separator())

            let trueItem = NSMenuItem(title: "true", action: #selector(boolMenuTrue(_:)), keyEquivalent: "")
            trueItem.target = self
            if case .boolean(true) = currentValue { trueItem.state = .on }
            else if case .string(let s) = currentValue, s.lowercased() == "true" || s == "t" { trueItem.state = .on }
            menu.addItem(trueItem)

            let falseItem = NSMenuItem(title: "false", action: #selector(boolMenuFalse(_:)), keyEquivalent: "")
            falseItem.target = self
            if case .boolean(false) = currentValue { falseItem.state = .on }
            else if case .string(let s) = currentValue, s.lowercased() == "false" || s == "f" { falseItem.state = .on }
            menu.addItem(falseItem)

            let cellRect = tableView.frameOfCell(atColumn: col, row: row)
            let menuOrigin = NSPoint(x: cellRect.minX, y: cellRect.maxY)
            menu.popUp(positioning: nil, at: menuOrigin, in: tableView)
        }

        @objc nonisolated func boolMenuNull(_ sender: Any?) {
            MainActor.assumeIsolated {
                guard let edit = pendingBoolEdit else { return }
                pendingBoolEdit = nil
                commitBoolValue(.null, edit: edit)
            }
        }

        @objc nonisolated func boolMenuTrue(_ sender: Any?) {
            MainActor.assumeIsolated {
                guard let edit = pendingBoolEdit else { return }
                pendingBoolEdit = nil
                commitBoolValue(.boolean(true), edit: edit)
            }
        }

        @objc nonisolated func boolMenuFalse(_ sender: Any?) {
            MainActor.assumeIsolated {
                guard let edit = pendingBoolEdit else { return }
                pendingBoolEdit = nil
                commitBoolValue(.boolean(false), edit: edit)
            }
        }

        private func commitBoolValue(_ value: RowValue, edit: (row: Int, col: Int)) {
            guard allowsMutations else { return }
            guard edit.row < rows.count, edit.col < columns.count else { return }

            let oldValue = rows[edit.row][edit.col]
            guard oldValue != value else { return }

            let text: String
            switch value {
            case .null: text = "NULL"
            case .boolean(let b): text = b ? "true" : "false"
            default: text = value.description
            }

            if insertedRowIndices.contains(edit.row) {
                viewModel?.commitNewRowEdit(rowIndex: edit.row, colIdx: edit.col, newText: text)
            } else {
                viewModel?.commitCellEdit(rowIndex: edit.row, colIdx: edit.col, newText: text)
            }

            if let vm = viewModel {
                rows = vm.rows
                pendingChanges = vm.changeTracker.pendingChanges
            }

            if let tableView,
               let cellView = tableView.view(atColumn: edit.col, row: edit.row, makeIfNecessary: false) as? DataGridCellView {
                configureCell(cellView, row: edit.row, col: edit.col)
                if oldValue != value {
                    cellView.cellBackgroundColor = NSColor.Gridex.cellModified
                    cellView.needsDisplay = true
                }
            }
        }

        private static var editInfoKey: UInt8 = 0

        private struct EditInfo {
            let row: Int
            let col: Int
        }

        private func finishEditing(_ editor: NSTextField, commit: Bool, nextCol: Bool = false) {
            guard let info = objc_getAssociatedObject(editor, &Self.editInfoKey) as? EditInfo else { return }
            objc_setAssociatedObject(editor, &Self.editInfoKey, nil, .OBJC_ASSOCIATION_RETAIN)
            let shouldCommit = commit && allowsMutations

            // Save scroll position before any changes — removing the editor
            // and changing first responder can cause NSTableView to auto-scroll.
            let savedContentOffset = tableView?.enclosingScrollView?.contentView.bounds.origin

            if shouldCommit {
                if insertedRowIndices.contains(info.row) {
                    viewModel?.commitNewRowEdit(rowIndex: info.row, colIdx: info.col, newText: editor.stringValue)
                } else {
                    viewModel?.commitCellEdit(rowIndex: info.row, colIdx: info.col, newText: editor.stringValue)
                }
            }

            // Remove the floating editor from the table view
            (editor.superview as? EditContainerView)?.removeFromSuperview()

            // Restore the cell underneath and update its content
            if let tableView,
               let cellView = tableView.view(atColumn: info.col, row: info.row, makeIfNecessary: false) as? DataGridCellView {
                cellView.isEditingActive = false
                if shouldCommit, let vm = viewModel {
                    rows = vm.rows
                    pendingChanges = vm.changeTracker.pendingChanges
                    rebuildPendingChangeCaches()
                }
                configureCell(cellView, row: info.row, col: info.col)
                cellView.needsDisplay = true
            }

            // Restore scroll position using the enclosing scroll view directly.
            // Deferred to ensure it runs after any pending layout triggered by
            // removing the editor or changing first responder.
            if let scrollView = tableView?.enclosingScrollView, let offset = savedContentOffset {
                scrollView.contentView.scroll(to: offset)
                scrollView.reflectScrolledClipView(scrollView.contentView)
            }

            justFinishedEditing = true
            isEditing = false

            // Tab → open next column for editing
            if nextCol, shouldCommit {
                let nextColIdx = info.col + 1
                if nextColIdx < columns.count {
                    beginEditing(row: info.row, col: nextColIdx)
                }
            }
        }

        nonisolated func control(_ control: NSControl, textView: NSTextView, doCommandBy commandSelector: Selector) -> Bool {
            MainActor.assumeIsolated {
                guard let editor = control as? NSTextField else { return false }

                if commandSelector == #selector(NSResponder.insertNewline(_:)) {
                    finishEditing(editor, commit: true)
                    return true
                } else if commandSelector == #selector(NSResponder.insertTab(_:)) {
                    finishEditing(editor, commit: true, nextCol: true)
                    return true
                } else if commandSelector == #selector(NSResponder.cancelOperation(_:)) {
                    finishEditing(editor, commit: false)
                    return true
                }
                return false
            }
        }

        nonisolated func controlTextDidEndEditing(_ obj: Notification) {
            MainActor.assumeIsolated {
                guard let editor = obj.object as? NSTextField else { return }
                finishEditing(editor, commit: true)
            }
        }

        nonisolated func menuWillOpen(_ menu: NSMenu) {
            MainActor.assumeIsolated {
                menu.items.first(where: { $0.action == #selector(deleteRows(_:)) })?
                    .isEnabled = allowsMutations
            }
        }

        // MARK: - Copy

        @objc nonisolated func copyRow(_ sender: Any?) {
            MainActor.assumeIsolated {
                guard let tableView else { return }
                let row = tableView.clickedRow
                guard row >= 0, row < rows.count else { return }
                let text = rows[row].map { $0.isNull ? "NULL" : $0.description }.joined(separator: "\t")
                NSPasteboard.general.clearContents()
                NSPasteboard.general.setString(text, forType: .string)
            }
        }

        @objc nonisolated func copyCell(_ sender: Any?) {
            MainActor.assumeIsolated {
                guard let tableView else { return }
                let row = tableView.clickedRow
                let col = tableView.clickedColumn
                guard row >= 0, col >= 0, col < columns.count, row < rows.count else { return }
                let value = col < rows[row].count ? rows[row][col] : .null
                let text = value.isNull ? "NULL" : value.description
                NSPasteboard.general.clearContents()
                NSPasteboard.general.setString(text, forType: .string)
            }
        }

        @objc nonisolated func deleteRows(_ sender: Any?) {
            MainActor.assumeIsolated {
                guard allowsMutations, let vm = viewModel, let tableView else { return }
                // If right-clicked row is not in selection, delete just that row
                let clickedRow = tableView.clickedRow
                if clickedRow >= 0, !tableView.selectedRowIndexes.contains(clickedRow) {
                    vm.selectedRows = [clickedRow]
                }
                vm.deleteSelectedRows()
            }
        }
    }
}

// MARK: - Key-aware Table View (Delete key support)

private class DataGridTableView: NSTableView {
    weak var gridCoordinator: AppKitDataGrid.Coordinator?

    override func keyDown(with event: NSEvent) {
        let flags = event.modifierFlags.intersection(.deviceIndependentFlagsMask)

        switch event.keyCode {
        case 51, 117: // Delete, Forward Delete
            gridCoordinator?.deleteRows(nil)
        case 36: // Enter → edit selected cell
            if selectedRow >= 0, selectedColumn >= 0 {
                gridCoordinator?.beginEditing(row: selectedRow, col: selectedColumn)
            } else if selectedRow >= 0 {
                gridCoordinator?.beginEditing(row: selectedRow, col: 0)
            }
        case 8 where flags == .command: // ⌘C → copy
            gridCoordinator?.copyCell(nil)
        default:
            super.keyDown(with: event)
        }
    }
}

// MARK: - Custom Row View

private class DataGridRowView: NSTableRowView {
    var overrideBackgroundColor: NSColor?

    override func drawBackground(in dirtyRect: NSRect) {
        if let color = overrideBackgroundColor {
            color.setFill()
            bounds.fill()
        } else {
            super.drawBackground(in: dirtyRect)
        }
    }
}

// MARK: - Cell View (Direct Text Drawing)

private class DataGridCellView: NSView {
    var cellBackgroundColor: NSColor?
    var textFont: NSFont = AppKitDataGrid.Coordinator.cellFont
    var textColor: NSColor = .labelColor
    var textAlignment: NSTextAlignment = .left
    var isEditingActive: Bool = false
    var isDateColumn: Bool = false
    var hasDefaultValue: Bool = false
    var hasEnumValues: Bool = false
    var foreignKeyTarget: String? = nil      // referenced table name
    var foreignKeyRefColumn: String? = nil   // referenced column name
    var cellValue: String = ""               // current cell value (for FK click)
    var onFKClick: ((_ refTable: String, _ refColumn: String, _ value: String) -> Void)?
    var isBooleanColumn: Bool = false

    // Text with cached attributed string — invalidated on change
    private var _text: String = ""
    private var _cachedAttrString: NSAttributedString?
    private var _cacheFont: NSFont?
    private var _cacheColor: NSColor?
    private var _cacheAlignment: NSTextAlignment?

    var text: String {
        get { _text }
        set {
            if _text != newValue {
                _text = newValue
                _cachedAttrString = nil
            }
        }
    }

    var showChevron: Bool { isDateColumn || hasEnumValues || isBooleanColumn }

    // Cached objects — avoid allocations in draw()
    private static let fkArrowAttrs: [NSAttributedString.Key: Any] = [
        .font: NSFont.systemFont(ofSize: 10, weight: .medium),
        .foregroundColor: NSColor.secondaryLabelColor
    ]
    private static let fkArrow = NSAttributedString(string: "→", attributes: fkArrowAttrs)
    private static let fkArrowSize = fkArrow.size()

    private static let chevronImage: NSImage? = {
        guard let img = NSImage(systemSymbolName: "chevron.up.chevron.down", accessibilityDescription: nil) else { return nil }
        let config = NSImage.SymbolConfiguration(pointSize: 10, weight: .regular)
            .applying(.init(paletteColors: [.secondaryLabelColor]))
        return img.withSymbolConfiguration(config) ?? img
    }()

    private let paragraphStyle: NSMutableParagraphStyle = {
        let ps = NSMutableParagraphStyle()
        ps.lineBreakMode = .byTruncatingTail
        return ps
    }()

    private func cachedAttributedString() -> NSAttributedString {
        if let cached = _cachedAttrString,
           _cacheFont === textFont,
           _cacheColor == textColor,
           _cacheAlignment == textAlignment {
            return cached
        }
        paragraphStyle.alignment = textAlignment
        // Pre-truncate long strings — cell can only show ~200 chars max at any width
        let drawText = _text.count > 300 ? String(_text.prefix(300)) + "…" : _text
        let str = NSAttributedString(string: drawText, attributes: [
            .font: textFont,
            .foregroundColor: textColor,
            .paragraphStyle: paragraphStyle
        ])
        _cachedAttrString = str
        _cacheFont = textFont
        _cacheColor = textColor
        _cacheAlignment = textAlignment
        return str
    }

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        wantsLayer = true
        layerContentsRedrawPolicy = .onSetNeedsDisplay
        canDrawSubviewsIntoLayer = true
    }

    required init?(coder: NSCoder) { fatalError() }

    override var isFlipped: Bool { true }

    override func draw(_ dirtyRect: NSRect) {
        if let color = cellBackgroundColor {
            color.setFill()
            bounds.fill()
        }

        guard !isEditingActive else { return }

        let hasFK = foreignKeyTarget != nil
        let chevronWidth: CGFloat = showChevron ? 18 : 0
        let fkWidth: CGFloat = hasFK ? 16 : 0
        let rightIconWidth = chevronWidth + fkWidth

        let attrStr = cachedAttributedString()

        var textRect = bounds.insetBy(dx: 6, dy: 0)
        textRect.size.width -= rightIconWidth
        let textHeight = textFont.ascender - textFont.descender + textFont.leading
        textRect.origin.y = max(0, (bounds.height - textHeight) / 2)
        textRect.size.height = textHeight + 2
        attrStr.draw(with: textRect, options: [.truncatesLastVisibleLine, .usesLineFragmentOrigin], context: nil)

        // FK arrow on the right (before chevron)
        if hasFK {
            let s = Self.fkArrowSize
            let fkX = bounds.maxX - chevronWidth - s.width - 3
            let fkY = (bounds.height - s.height) / 2
            Self.fkArrow.draw(at: NSPoint(x: fkX, y: fkY))
        }

        // Chevron icon on the far right
        if showChevron, let img = Self.chevronImage {
            let iconSize = img.size
            let iconX = bounds.maxX - iconSize.width - 5
            let iconY = (bounds.height - iconSize.height) / 2
            img.draw(in: NSRect(x: iconX, y: iconY, width: iconSize.width, height: iconSize.height))
        }
    }

    override func mouseDown(with event: NSEvent) {
        guard let fkTable = foreignKeyTarget, let fkCol = foreignKeyRefColumn, !cellValue.isEmpty else {
            super.mouseDown(with: event)
            return
        }
        let loc = convert(event.locationInWindow, from: nil)
        let chevronWidth: CGFloat = showChevron ? 18 : 0
        let fkHitX = bounds.maxX - chevronWidth - Self.fkArrowSize.width - 6
        if loc.x >= fkHitX && loc.x <= bounds.maxX - chevronWidth {
            onFKClick?(fkTable, fkCol, cellValue)
        } else {
            super.mouseDown(with: event)
        }
    }

    override func prepareForReuse() {
        super.prepareForReuse()
        cellBackgroundColor = nil
        text = ""
        textColor = .labelColor
        textAlignment = .left
        isEditingActive = false
        isDateColumn = false
        hasDefaultValue = false
        hasEnumValues = false
        isBooleanColumn = false
        foreignKeyTarget = nil
        foreignKeyRefColumn = nil
        cellValue = ""
        onFKClick = nil
    }
}

// MARK: - Edit Container (marker class for type detection)

private class EditContainerView: NSView {}

// MARK: - Sortable Header Cell

private class SortableHeaderCell: NSTableHeaderCell {
    override func drawInterior(withFrame cellFrame: NSRect, in controlView: NSView) {
        let attrs: [NSAttributedString.Key: Any] = [
            .font: NSFont.systemFont(ofSize: 12, weight: .medium),
            .foregroundColor: NSColor.secondaryLabelColor
        ]
        let str = NSAttributedString(string: stringValue, attributes: attrs)
        let rect = cellFrame.insetBy(dx: 6, dy: 0)
        str.draw(with: rect, options: [.truncatesLastVisibleLine, .usesLineFragmentOrigin], context: nil)
    }
}

// MARK: - Date Picker Panel

@MainActor
private class DatePickerPanel: NSObject {
    private let panel: NSPanel
    private let datePicker: NSDatePicker
    private let onSelect: (Date) -> Void

    init(date: Date, onSelect: @escaping (Date) -> Void) {
        self.onSelect = onSelect

        panel = NSPanel(
            contentRect: NSRect(x: 0, y: 0, width: 240, height: 180),
            styleMask: [.titled, .closable, .fullSizeContentView],
            backing: .buffered,
            defer: false
        )
        panel.isFloatingPanel = true
        panel.level = .floating
        panel.titlebarAppearsTransparent = true
        panel.titleVisibility = .hidden
        panel.isMovableByWindowBackground = true
        panel.hasShadow = true
        panel.becomesKeyOnlyIfNeeded = false

        datePicker = NSDatePicker()
        datePicker.datePickerStyle = .clockAndCalendar
        datePicker.datePickerElements = [.yearMonthDay, .hourMinuteSecond]
        datePicker.dateValue = date
        datePicker.timeZone = TimeZone(identifier: "UTC")

        super.init()

        let contentView = NSView(frame: NSRect(x: 0, y: 0, width: 240, height: 180))

        datePicker.translatesAutoresizingMaskIntoConstraints = false
        contentView.addSubview(datePicker)

        let okButton = NSButton(title: "OK", target: self, action: #selector(confirmDate(_:)))
        okButton.bezelStyle = .rounded
        okButton.keyEquivalent = "\r"
        okButton.translatesAutoresizingMaskIntoConstraints = false
        contentView.addSubview(okButton)

        let cancelButton = NSButton(title: "Cancel", target: self, action: #selector(cancelDate(_:)))
        cancelButton.bezelStyle = .rounded
        cancelButton.keyEquivalent = "\u{1b}"
        cancelButton.translatesAutoresizingMaskIntoConstraints = false
        contentView.addSubview(cancelButton)

        NSLayoutConstraint.activate([
            datePicker.topAnchor.constraint(equalTo: contentView.topAnchor, constant: 12),
            datePicker.centerXAnchor.constraint(equalTo: contentView.centerXAnchor),

            okButton.trailingAnchor.constraint(equalTo: contentView.trailingAnchor, constant: -12),
            okButton.bottomAnchor.constraint(equalTo: contentView.bottomAnchor, constant: -12),
            okButton.widthAnchor.constraint(greaterThanOrEqualToConstant: 60),

            cancelButton.trailingAnchor.constraint(equalTo: okButton.leadingAnchor, constant: -8),
            cancelButton.bottomAnchor.constraint(equalTo: contentView.bottomAnchor, constant: -12),
            cancelButton.widthAnchor.constraint(greaterThanOrEqualToConstant: 60),
        ])

        panel.contentView = contentView
    }

    func show(relativeTo rect: NSRect) {
        let x = rect.minX
        let y = rect.minY - panel.frame.height - 4
        panel.setFrameOrigin(NSPoint(x: x, y: y))
        panel.makeKeyAndOrderFront(nil)
        // Keep self alive while panel is visible
        objc_setAssociatedObject(panel, "datePickerOwner", self, .OBJC_ASSOCIATION_RETAIN)
    }

    @objc private func confirmDate(_ sender: Any?) {
        let date = datePicker.dateValue
        panel.close()
        onSelect(date)
    }

    @objc private func cancelDate(_ sender: Any?) {
        panel.close()
    }
}
