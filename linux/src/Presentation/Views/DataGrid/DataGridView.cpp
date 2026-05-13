#include "Presentation/Views/DataGrid/DataGridView.h"

#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollBar>
#include <QShortcut>
#include <QWheelEvent>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include "Core/Enums/DatabaseType.h"
#include "Core/Enums/SQLDialect.h"
#include "Core/Errors/GridexError.h"
#include "Core/Protocols/Database/IDatabaseAdapter.h"
#include "Presentation/Views/DataGrid/QueryResultModel.h"
#include "Presentation/Views/FilterBar/FilterBarView.h"
#include "Presentation/Views/QueryLog/QueryLogPanel.h"
#include "Presentation/Views/TableStructure/TableStructureView.h"
#include "Presentation/Views/Chrome/GxIcons.h"
#include "Services/Export/ExportService.h"

namespace gridex {

// All DataGrid chrome lives in resources/style-gx{,-light}.qss under the
// "DataGrid" section.

DataGridView::DataGridView(QWidget* parent) : QWidget(parent) {
    buildUi();
}

void DataGridView::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ===================== Top 28px DataGridToolbar =====================
    auto* topBar = new QWidget(this);
    topBar->setObjectName(QStringLiteral("DataGridTopBar"));
    topBar->setAttribute(Qt::WA_StyledBackground, true);
    topBar->setFixedHeight(28);
    topBar->setProperty("compact", true);  // shrinks descendant buttons/inputs
    auto* topH = new QHBoxLayout(topBar);
    topH->setContentsMargins(12, 0, 12, 0);
    topH->setSpacing(8);

    pendingStub_ = new QLabel(QString{}, topBar);
    pendingStub_->setObjectName(QStringLiteral("gxDataGridPending"));
    pendingStub_->setProperty("gxText", QStringLiteral("warning"));
    pendingStub_->hide();
    topH->addWidget(pendingStub_);

    discardBtn_ = new QPushButton(tr("Discard"), topBar);
    discardBtn_->setToolTip(tr("Discard all pending changes"));
    discardBtn_->setObjectName(QStringLiteral("discardButton"));
    discardBtn_->setFixedHeight(22);
    discardBtn_->hide();
    connect(discardBtn_, &QPushButton::clicked, this, &DataGridView::onDiscard);
    topH->addWidget(discardBtn_);

    commitBtn_ = new QPushButton(tr("Commit"), topBar);
    commitBtn_->setToolTip(tr("Commit pending changes to database (Ctrl+S)"));
    commitBtn_->setObjectName(QStringLiteral("commitButton"));
    commitBtn_->setFixedHeight(22);
    commitBtn_->hide();
    connect(commitBtn_, &QPushButton::clicked, this, &DataGridView::onCommit);
    topH->addWidget(commitBtn_);

    // Ctrl+S — commit pending changes whenever this view has focus.
    auto* commitShortcut = new QShortcut(QKeySequence::Save, this);
    commitShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(commitShortcut, &QShortcut::activated, this, &DataGridView::onCommit);

    topH->addStretch();

    reloadBtn_ = new QPushButton(topBar);
    reloadBtn_->setObjectName(QStringLiteral("dataGridReload"));
    reloadBtn_->setIcon(GxIcons::glyph(QStringLiteral("refresh")));
    reloadBtn_->setIconSize(QSize(13, 13));
    reloadBtn_->setToolTip(tr("Reload"));
    reloadBtn_->setFixedSize(24, 22);
    connect(reloadBtn_, &QPushButton::clicked, this, &DataGridView::reload);
    topH->addWidget(reloadBtn_);

    root->addWidget(topBar);

    // ===================== Filter bar (hidden until toggled) =====================
    filterBar_ = new FilterBarView(this);
    filterBar_->setVisible(false);
    filterBar_->setAutoFillBackground(true);

    connect(filterBar_, &FilterBarView::filterApplied, this,
            [this](const FilterExpression& expr) {
                activeFilter_  = expr;
                offset_        = 0;
                totalRowCount_ = -1;
                fetchAndRender();
            });
    connect(filterBar_, &FilterBarView::filterCleared, this,
            [this] {
                activeFilter_  = std::nullopt;
                filterBar_->reset();
                offset_        = 0;
                totalRowCount_ = -1;
                fetchAndRender();
            });

    filterBarDiv_ = new QFrame(this);
    filterBarDiv_->setObjectName(QStringLiteral("gxFilterBarDiv"));
    filterBarDiv_->setFrameShape(QFrame::HLine);
    filterBarDiv_->setVisible(false);

    root->addWidget(filterBar_);
    root->addWidget(filterBarDiv_);

    // ===================== Center: QStackedWidget (data | structure) =====================
    centerStack_ = new QStackedWidget(this);

    // Page 0 — data table
    tableView_ = new QTableView(this);
    tableView_->setObjectName(QStringLiteral("DataGridTable"));
    tableView_->setFrameShape(QFrame::NoFrame);
    tableView_->setAlternatingRowColors(true);
    tableView_->setShowGrid(false);
    tableView_->horizontalHeader()->setObjectName(QStringLiteral("DataGridHeader"));
    tableView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tableView_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    // Task 6: enable cell editing via double-click or F2
    tableView_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    tableView_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    tableView_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    tableView_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    tableView_->horizontalHeader()->setStretchLastSection(true);
    tableView_->horizontalHeader()->setSortIndicatorShown(true);
    tableView_->setSortingEnabled(true);
    tableView_->verticalHeader()->setDefaultSectionSize(30);

    // Task 3/5: right-click context menu
    tableView_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tableView_, &QTableView::customContextMenuRequested,
            this, &DataGridView::onTableContextMenu);

    // Delete key — mark selected row(s) for deletion (same as context menu).
    auto* deleteShortcut = new QShortcut(QKeySequence(Qt::Key_Delete), tableView_);
    deleteShortcut->setContext(Qt::WidgetShortcut);
    connect(deleteShortcut, &QShortcut::activated, this, &DataGridView::onDeleteRows);

    model_ = new QueryResultModel(this);
    tableView_->setModel(model_);
    // Shift+scroll → horizontal scroll.
    tableView_->viewport()->installEventFilter(this);
    connect(tableView_->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, &DataGridView::onRowSelectionChanged);
    connect(model_, &QueryResultModel::pendingChangesChanged,
            this, &DataGridView::onPendingChangesChanged);

    centerStack_->addWidget(tableView_);   // index 0

    // Page 1 — structure view
    structureView_ = new TableStructureView(this);
    centerStack_->addWidget(structureView_);  // index 1

    centerStack_->setCurrentIndex(0);

    // Vertical splitter: [table/structure] over [query log] — user can drag
    // the handle to grow the log panel.
    logPanel_ = new QueryLogPanel(this);
    auto* vSplit = new QSplitter(Qt::Vertical, this);
    vSplit->setChildrenCollapsible(false);
    vSplit->setHandleWidth(4);
    vSplit->addWidget(centerStack_);
    vSplit->addWidget(logPanel_);
    vSplit->setStretchFactor(0, 1);
    vSplit->setStretchFactor(1, 0);
    vSplit->setSizes({700, 140});
    root->addWidget(vSplit, 1);

    // ===================== Bottom 34px BottomTabBar =====================
    auto* bot = new QWidget(this);
    bot->setObjectName(QStringLiteral("DataGridBotBar"));
    bot->setAttribute(Qt::WA_StyledBackground, true);
    bot->setFixedHeight(34);
    bot->setProperty("compact", true);  // shrinks descendant buttons/inputs
    auto* botH = new QHBoxLayout(bot);
    botH->setContentsMargins(10, 5, 10, 5);
    botH->setSpacing(8);

    // Data / Structure pill group
    auto* pillGroup = new QWidget(bot);
    auto* pillH = new QHBoxLayout(pillGroup);
    pillH->setContentsMargins(0, 0, 0, 0);
    pillH->setSpacing(0);

    dataTabBtn_ = new QPushButton(tr("Data"), pillGroup);
    dataTabBtn_->setCheckable(true);
    dataTabBtn_->setChecked(true);
    dataTabBtn_->setAutoExclusive(true);
    dataTabBtn_->setProperty("tab", true);
    connect(dataTabBtn_, &QPushButton::clicked, this, [this] { onModeClicked(0); });
    pillH->addWidget(dataTabBtn_);

    structTabBtn_ = new QPushButton(tr("Structure"), pillGroup);
    structTabBtn_->setCheckable(true);
    structTabBtn_->setAutoExclusive(true);
    structTabBtn_->setProperty("tab", true);
    connect(structTabBtn_, &QPushButton::clicked, this, [this] { onModeClicked(1); });
    pillH->addWidget(structTabBtn_);

    botH->addWidget(pillGroup);
    botH->addSpacing(8);

    // Task 2: [+ Row] button — enabled
    addRowBtn_ = new QPushButton(QStringLiteral("+ Row"), bot);
    addRowBtn_->setObjectName(QStringLiteral("dataGridGenericBtn"));
    addRowBtn_->setToolTip(tr("Insert a new empty row"));
    addRowBtn_->setEnabled(true);
    connect(addRowBtn_, &QPushButton::clicked, this, &DataGridView::onAddRow);
    botH->addWidget(addRowBtn_);

    botH->addSpacing(16);

    // Row count label
    pageLabel_ = new QLabel(tr("No rows"), bot);
    pageLabel_->setObjectName(QStringLiteral("dataGridPageLabel"));
    botH->addWidget(pageLabel_);

    botH->addStretch();

    // Filters button — now functional
    filtersBtn_ = new QPushButton(tr("Filters"), bot);
    filtersBtn_->setObjectName(QStringLiteral("dataGridGenericBtn"));
    filtersBtn_->setIcon(GxIcons::glyph(QStringLiteral("search")));
    filtersBtn_->setIconSize(QSize(11, 11));
    filtersBtn_->setCheckable(true);
    filtersBtn_->setToolTip(tr("Toggle filter bar"));
    connect(filtersBtn_, &QPushButton::clicked, this, &DataGridView::onFiltersToggled);
    botH->addWidget(filtersBtn_);

    // Export button with drop-down menu
    exportMenu_ = new QMenu(this);
    exportMenu_->addAction(tr("CSV…"),  this, &DataGridView::onExportCsv);
    exportMenu_->addAction(tr("JSON…"), this, &DataGridView::onExportJson);
    exportMenu_->addAction(tr("SQL…"),  this, &DataGridView::onExportSql);

    exportBtn_ = new QPushButton(tr("Export"), bot);
    exportBtn_->setObjectName(QStringLiteral("dataGridGenericBtn"));
    exportBtn_->setIcon(GxIcons::glyph(QStringLiteral("export")));
    exportBtn_->setIconSize(QSize(11, 11));
    exportBtn_->setToolTip(tr("Export current data"));
    exportBtn_->setMenu(exportMenu_);
    botH->addWidget(exportBtn_);

    // Pagination
    prevBtn_ = new QPushButton(QStringLiteral("‹"), bot);
    prevBtn_->setObjectName(QStringLiteral("dataGridGenericBtn"));
    prevBtn_->setFixedSize(28, 22);
    prevBtn_->setToolTip(tr("Previous page"));
    connect(prevBtn_, &QPushButton::clicked, this, &DataGridView::onPrevPage);
    botH->addWidget(prevBtn_);

    nextBtn_ = new QPushButton(QStringLiteral("›"), bot);
    nextBtn_->setObjectName(QStringLiteral("dataGridGenericBtn"));
    nextBtn_->setFixedSize(28, 22);
    nextBtn_->setToolTip(tr("Next page"));
    connect(nextBtn_, &QPushButton::clicked, this, &DataGridView::onNextPage);
    botH->addWidget(nextBtn_);

    // Rows-per-page spin
    auto* rowsLbl = new QLabel(tr("Rows"), bot);
    rowsLbl->setObjectName(QStringLiteral("dataGridPageLabel"));
    botH->addWidget(rowsLbl);
    limitSpin_ = new QSpinBox(bot);
    limitSpin_->setObjectName(QStringLiteral("dataGridLimit"));
    limitSpin_->setRange(100, 100000);
    limitSpin_->setSingleStep(100);
    limitSpin_->setValue(1000);
    limitSpin_->setFixedWidth(80);
    botH->addWidget(limitSpin_);

    root->addWidget(bot);

    // Initial disabled nav state
    prevBtn_->setEnabled(false);
    nextBtn_->setEnabled(false);

    // Build context menu (populated lazily in onTableContextMenu)
    tableContextMenu_ = new QMenu(this);
}

// ===================== Adapter & Table loading =====================

void DataGridView::setAdapter(IDatabaseAdapter* adapter) {
    adapter_ = adapter;
    pkColumns_.clear();
    if (!adapter_) {
        model_->clear();
        pageLabel_->setText(tr("No rows"));
        updatePendingLabel();
    }
}

void DataGridView::loadTable(const QString& schema, const QString& table) {
    currentSchema_ = schema.isEmpty() ? std::optional<std::string>{}
                                       : std::optional<std::string>(schema.toStdString());
    currentTable_  = table.toStdString();
    offset_        = 0;
    totalRowCount_ = -1;
    activeFilter_  = std::nullopt;

    // Reset to Data tab whenever a new table is selected.
    dataTabBtn_->setChecked(true);
    structTabBtn_->setChecked(false);
    centerStack_->setCurrentIndex(0);
    structureView_->clear();

    // Reset filter bar column list when table changes.
    if (filterBar_) {
        filterBar_->reset();
    }

    // Cache PK columns for the new table
    cachePrimaryKeys();

    fetchAndRender();
}

void DataGridView::cachePrimaryKeys() {
    pkColumns_.clear();
    if (!adapter_ || currentTable_.empty()) return;
    try {
        const auto desc = adapter_->describeTable(currentTable_, currentSchema_);
        for (const auto& col : desc.columns) {
            if (col.isPrimaryKey) pkColumns_.push_back(col.name);
        }
    } catch (...) {
        // Non-fatal: commit will fall back to full-row match or fail gracefully.
    }
}

void DataGridView::reload() {
    if (currentTable_.empty()) return;
    offset_        = 0;
    totalRowCount_ = -1;
    fetchAndRender();
}

// ===================== Pending changes UI =====================

void DataGridView::onPendingChangesChanged() {
    updatePendingLabel();
}

void DataGridView::updatePendingLabel() {
    const bool hasChanges = model_ && model_->hasPendingChanges();
    pendingStub_->setVisible(hasChanges);
    discardBtn_->setVisible(hasChanges);
    commitBtn_->setVisible(hasChanges);
    if (!hasChanges) {
        pendingStub_->setText(QString{});
        return;
    }
    const int edits   = static_cast<int>(model_->pendingEdits().size());
    const int inserts = static_cast<int>(model_->insertedRows().size());
    const int deletes = static_cast<int>(model_->deletedRows().size());
    QStringList parts;
    if (edits   > 0) parts << tr("%1 edit(s)").arg(edits);
    if (inserts > 0) parts << tr("%1 new").arg(inserts);
    if (deletes > 0) parts << tr("%1 delete(s)").arg(deletes);
    pendingStub_->setText(parts.join(QStringLiteral(", ")) + tr(" pending"));
    discardBtn_->setEnabled(true);
    commitBtn_->setEnabled(true);
}

// ===================== Add Row (Task 2) =====================

void DataGridView::onAddRow() {
    if (!model_ || currentTable_.empty()) return;
    model_->addEmptyRow();
    // Scroll to and select the new row
    const int newRow = model_->rowCount() - 1;
    const auto newIdx = model_->index(newRow, 0);
    tableView_->scrollTo(newIdx);
    tableView_->setCurrentIndex(newIdx);
    tableView_->edit(newIdx);
}

// ===================== Pending SQL preview helper =====================

QStringList DataGridView::buildPendingStatements() const {
    if (!model_ || currentTable_.empty()) return {};

    const SQLDialect dialect = adapter_ ? sqlDialect(adapter_->databaseType()) : SQLDialect::PostgreSQL;

    const std::string qualifiedTable = [&] {
        if (currentSchema_) {
            return quoteIdentifier(dialect, *currentSchema_) + "." +
                   quoteIdentifier(dialect, currentTable_);
        }
        return quoteIdentifier(dialect, currentTable_);
    }();

    const auto& result   = model_->result();
    const auto& cols     = result.columns;
    const int   colCount = static_cast<int>(cols.size());

    auto quoteLiteral = [](const RowValue& v) -> std::string {
        if (v.isNull())    return "NULL";
        if (v.isNumeric()) return v.displayString();
        std::string s = v.displayString();
        std::string out;
        out.reserve(s.size() + 2);
        out.push_back('\'');
        for (char c : s) {
            if (c == '\'') out.push_back('\'');
            out.push_back(c);
        }
        out.push_back('\'');
        return out;
    };

    auto buildWhereClause = [&](int row) -> std::string {
        std::string clause;
        auto append = [&](const std::string& col, const RowValue& val) {
            if (!clause.empty()) clause += " AND ";
            clause += quoteIdentifier(dialect, col) + " = " + quoteLiteral(val);
        };
        if (pkColumns_.empty()) {
            for (int c = 0; c < colCount; ++c) {
                append(cols[c].name, model_->effectiveCellValue(row, c));
            }
        } else {
            for (const auto& pk : pkColumns_) {
                for (int c = 0; c < colCount; ++c) {
                    if (cols[c].name == pk) {
                        const RowValue origVal = (row < static_cast<int>(result.rows.size()) &&
                                                  c  < static_cast<int>(result.rows[row].size()))
                                                     ? result.rows[row][c]
                                                     : RowValue::makeNull();
                        append(pk, origVal);
                        break;
                    }
                }
            }
        }
        return clause;
    };

    QStringList stmts;

    // Updates
    std::map<int, std::unordered_map<std::string, RowValue>> editsByRow;
    for (const auto& [key, val] : model_->pendingEdits()) {
        const int c = key.second;
        if (c < colCount) editsByRow[key.first][cols[c].name] = val;
    }
    for (const auto& [row, setMap] : editsByRow) {
        if (model_->isRowDeleted(row)) continue;
        std::string setPart;
        for (const auto& [col, val] : setMap) {
            if (!setPart.empty()) setPart += ", ";
            setPart += quoteIdentifier(dialect, col) + " = " + quoteLiteral(val);
        }
        const std::string where = buildWhereClause(row);
        stmts << QString::fromStdString(
            "UPDATE " + qualifiedTable + " SET " + setPart +
            (where.empty() ? "" : " WHERE " + where) + ";");
    }

    // Inserts
    for (const auto& insertedRow : model_->insertedRows()) {
        std::string colPart, valPart;
        for (int c = 0; c < colCount && c < static_cast<int>(insertedRow.size()); ++c) {
            if (insertedRow[c].isNull()) continue;
            if (!colPart.empty()) { colPart += ", "; valPart += ", "; }
            colPart += quoteIdentifier(dialect, cols[c].name);
            valPart += quoteLiteral(insertedRow[c]);
        }
        if (!colPart.empty()) {
            stmts << QString::fromStdString(
                "INSERT INTO " + qualifiedTable + " (" + colPart + ") VALUES (" + valPart + ");");
        }
    }

    // Deletes
    std::vector<int> toDelete(model_->deletedRows().rbegin(), model_->deletedRows().rend());
    for (const int row : toDelete) {
        const std::string where = buildWhereClause(row);
        stmts << QString::fromStdString(
            "DELETE FROM " + qualifiedTable +
            (where.empty() ? "" : " WHERE " + where) + ";");
    }

    return stmts;
}

// ===================== Commit (Task 4) =====================

void DataGridView::onCommit() {
    if (!adapter_ || !model_ || currentTable_.empty()) return;
    if (!model_->hasPendingChanges()) return;

    // Pre-flight dry-run: show SQL preview and require explicit Apply.
    {
        const QStringList stmts = buildPendingStatements();
        const int updates = static_cast<int>(
            std::count_if(stmts.begin(), stmts.end(),
                          [](const QString& s){ return s.startsWith(QLatin1String("UPDATE")); }));
        const int inserts = static_cast<int>(
            std::count_if(stmts.begin(), stmts.end(),
                          [](const QString& s){ return s.startsWith(QLatin1String("INSERT")); }));
        const int deletes = static_cast<int>(
            std::count_if(stmts.begin(), stmts.end(),
                          [](const QString& s){ return s.startsWith(QLatin1String("DELETE")); }));

        QStringList summaryParts;
        if (updates > 0) summaryParts << tr("%1 UPDATE").arg(updates);
        if (inserts > 0) summaryParts << tr("%1 INSERT").arg(inserts);
        if (deletes > 0) summaryParts << tr("%1 DELETE").arg(deletes);

        QString preview;
        preview.reserve(stmts.size() * 80);
        for (int i = 0; i < stmts.size(); ++i) {
            preview += QString::number(i + 1) + QLatin1String(". ") + stmts[i] + QLatin1Char('\n');
        }
        preview += QLatin1Char('\n') + tr("— %1 —").arg(summaryParts.join(QStringLiteral(" · ")));

        auto* dlg = new QDialog(this);
        dlg->setWindowTitle(tr("Preview Changes"));
        dlg->resize(640, 400);
        auto* vlay = new QVBoxLayout(dlg);
        auto* edit = new QPlainTextEdit(dlg);
        edit->setReadOnly(true);
        edit->setPlainText(preview);
        edit->setFont(QFont(QStringLiteral("Monospace"), 10));
        vlay->addWidget(edit);
        auto* btns = new QDialogButtonBox(dlg);
        auto* applyBtn  = btns->addButton(tr("Apply"), QDialogButtonBox::AcceptRole);
        auto* cancelBtn = btns->addButton(tr("Cancel"), QDialogButtonBox::RejectRole);
        Q_UNUSED(applyBtn); Q_UNUSED(cancelBtn);
        connect(btns, &QDialogButtonBox::accepted, dlg, &QDialog::accept);
        connect(btns, &QDialogButtonBox::rejected, dlg, &QDialog::reject);
        vlay->addWidget(btns);

        if (dlg->exec() != QDialog::Accepted) {
            dlg->deleteLater();
            return;
        }
        dlg->deleteLater();
    }

    const auto& result   = model_->result();
    const auto& cols     = result.columns;
    const int   colCount = static_cast<int>(cols.size());

    // Helper: build WHERE clause from PK columns for a given original row.
    auto buildPkWhere = [&](int row) -> std::unordered_map<std::string, RowValue> {
        std::unordered_map<std::string, RowValue> where;
        if (pkColumns_.empty()) {
            // No PK info — use all columns as identity (best-effort).
            for (int c = 0; c < colCount; ++c) {
                where[cols[c].name] = model_->effectiveCellValue(row, c);
            }
        } else {
            for (const auto& pk : pkColumns_) {
                for (int c = 0; c < colCount; ++c) {
                    if (cols[c].name == pk) {
                        // Use the ORIGINAL value from result_ for the WHERE clause.
                        const RowValue origVal = (row < static_cast<int>(result.rows.size()) &&
                                                  c  < static_cast<int>(result.rows[row].size()))
                                                     ? result.rows[row][c]
                                                     : RowValue::makeNull();
                        where[pk] = origVal;
                        break;
                    }
                }
            }
        }
        return where;
    };

    QStringList errors;

    try {
        adapter_->beginTransaction();

        // 1. Apply edits to original rows (grouped by row).
        std::map<int, std::unordered_map<std::string, RowValue>> editsByRow;
        for (const auto& [key, val] : model_->pendingEdits()) {
            const int r = key.first;
            const int c = key.second;
            if (c < colCount) {
                editsByRow[r][cols[c].name] = val;
            }
        }
        for (const auto& [row, setMap] : editsByRow) {
            if (model_->isRowDeleted(row)) continue;  // will be handled by delete pass
            try {
                adapter_->updateRow(currentTable_, currentSchema_, setMap, buildPkWhere(row));
            } catch (const GridexError& e) {
                errors << tr("Update row %1: %2").arg(row + 1).arg(QString::fromUtf8(e.what()));
            }
        }

        // 2. Insert new rows.
        for (const auto& insertedRow : model_->insertedRows()) {
            std::unordered_map<std::string, RowValue> values;
            for (int c = 0; c < colCount && c < static_cast<int>(insertedRow.size()); ++c) {
                if (!insertedRow[c].isNull()) {
                    values[cols[c].name] = insertedRow[c];
                }
            }
            if (values.empty()) continue;  // skip fully-null rows
            try {
                adapter_->insertRow(currentTable_, currentSchema_, values);
            } catch (const GridexError& e) {
                errors << tr("Insert row: %1").arg(QString::fromUtf8(e.what()));
            }
        }

        // 3. Delete marked rows (process in reverse order to keep indices stable).
        std::vector<int> toDelete(model_->deletedRows().rbegin(), model_->deletedRows().rend());
        for (const int row : toDelete) {
            try {
                adapter_->deleteRow(currentTable_, currentSchema_, buildPkWhere(row));
            } catch (const GridexError& e) {
                errors << tr("Delete row %1: %2").arg(row + 1).arg(QString::fromUtf8(e.what()));
            }
        }

        if (errors.isEmpty()) {
            adapter_->commitTransaction();
        } else {
            adapter_->rollbackTransaction();
            QMessageBox::critical(this, tr("Commit Failed"),
                                  tr("Some operations failed — changes rolled back:\n\n") +
                                  errors.join(QStringLiteral("\n")));
            return;
        }
    } catch (const GridexError& e) {
        try { adapter_->rollbackTransaction(); } catch (...) {}
        QMessageBox::critical(this, tr("Commit Failed"), QString::fromUtf8(e.what()));
        return;
    }

    // Commit summary for the user (transient — auto-hides).
    const int edits   = static_cast<int>(model_->pendingEdits().size());
    const int inserts = static_cast<int>(model_->insertedRows().size());
    const int deletes = static_cast<int>(model_->deletedRows().size());
    QStringList parts;
    if (edits   > 0) parts << tr("%1 updated").arg(edits);
    if (inserts > 0) parts << tr("%1 inserted").arg(inserts);
    if (deletes > 0) parts << tr("%1 deleted").arg(deletes);

    // Reload to reflect committed state.
    model_->clearPendingChanges();
    fetchAndRender();

    // Show transient ✓ confirmation in the top toolbar for 3 seconds.
    auto setPendingState = [this](const char* state) {
        pendingStub_->setProperty("gxText", QString::fromLatin1(state));
        pendingStub_->style()->unpolish(pendingStub_);
        pendingStub_->style()->polish(pendingStub_);
    };
    setPendingState("success");
    pendingStub_->setText(tr("✓ Committed — %1").arg(parts.join(QStringLiteral(", "))));
    pendingStub_->setVisible(true);
    QTimer::singleShot(3000, this, [this, setPendingState] {
        pendingStub_->setText(QString{});
        pendingStub_->setVisible(false);
        setPendingState("warning");
    });
}

// ===================== Discard (Task 4) =====================

void DataGridView::onDiscard() {
    if (!model_) return;
    model_->clearPendingChanges();
    fetchAndRender();
}

// ===================== Context menu (Tasks 3 & 5) =====================

void DataGridView::onTableContextMenu(const QPoint& pos) {
    const QModelIndex idx = tableView_->indexAt(pos);

    tableContextMenu_->clear();

    // Refresh — reload current table from adapter. Always enabled when a
    // table is loaded; works even on blank area (no cell under cursor).
    auto* refreshAction = tableContextMenu_->addAction(
        tr("Refresh"), this, &DataGridView::reload);
    refreshAction->setShortcut(QKeySequence::Refresh);
    refreshAction->setEnabled(!currentTable_.empty());

    tableContextMenu_->addSeparator();

    // Copy actions (Task 5)
    auto* copyCellAction    = tableContextMenu_->addAction(tr("Copy Cell"),      this, &DataGridView::onCopyCell);
    auto* copyRowAction     = tableContextMenu_->addAction(tr("Copy Row"),       this, &DataGridView::onCopyRow);
    auto* copyInsertAction  = tableContextMenu_->addAction(tr("Copy as INSERT"), this, &DataGridView::onCopyAsInsert);

    copyCellAction->setEnabled(idx.isValid());
    copyRowAction->setEnabled(idx.isValid());
    copyInsertAction->setEnabled(idx.isValid() && !currentTable_.empty());

    tableContextMenu_->addSeparator();

    // Delete action (Task 3)
    const auto selected = tableView_->selectionModel()->selectedRows();
    auto* deleteAction = tableContextMenu_->addAction(tr("Delete Row(s)\tDel"), this, &DataGridView::onDeleteRows);
    deleteAction->setEnabled(!selected.isEmpty());

    tableContextMenu_->exec(tableView_->viewport()->mapToGlobal(pos));
}

void DataGridView::onCopyCell() {
    const QModelIndex idx = tableView_->currentIndex();
    if (!idx.isValid() || !model_) return;
    const RowValue cell = model_->effectiveCellValue(idx.row(), idx.column());
    const QString text  = QString::fromUtf8(cell.displayString().c_str());
    QApplication::clipboard()->setText(text);
}

void DataGridView::onCopyRow() {
    const QModelIndex idx = tableView_->currentIndex();
    if (!idx.isValid() || !model_) return;
    const int row = idx.row();
    const int cols = model_->columnCount();
    QStringList parts;
    parts.reserve(cols);
    for (int c = 0; c < cols; ++c) {
        parts << QString::fromUtf8(model_->effectiveCellValue(row, c).displayString().c_str());
    }
    QApplication::clipboard()->setText(parts.join(QLatin1Char('\t')));
}

void DataGridView::onCopyAsInsert() {
    const QModelIndex idx = tableView_->currentIndex();
    if (!idx.isValid() || !model_ || currentTable_.empty()) return;
    const int row  = idx.row();
    const int cols = model_->columnCount();
    const auto& columns = model_->result().columns;

    QStringList colNames, values;
    colNames.reserve(cols);
    values.reserve(cols);

    for (int c = 0; c < cols; ++c) {
        const RowValue cell = model_->effectiveCellValue(row, c);
        colNames << QString::fromUtf8(columns[c].name.c_str());
        if (cell.isNull()) {
            values << QStringLiteral("NULL");
        } else {
            // Quote as string literal; numeric types inline without quotes
            if (cell.isNumeric()) {
                values << QString::fromUtf8(cell.displayString().c_str());
            } else {
                QString escaped = QString::fromUtf8(cell.displayString().c_str());
                escaped.replace(QLatin1Char('\''), QStringLiteral("''"));
                values << (QLatin1Char('\'') + escaped + QLatin1Char('\''));
            }
        }
    }

    const QString sql = QStringLiteral("INSERT INTO %1 (%2) VALUES (%3);")
                            .arg(QString::fromStdString(currentTable_))
                            .arg(colNames.join(QStringLiteral(", ")))
                            .arg(values.join(QStringLiteral(", ")));
    QApplication::clipboard()->setText(sql);
}

void DataGridView::onDeleteRows() {
    if (!model_) return;
    const auto selected = tableView_->selectionModel()->selectedRows();
    for (const auto& idx : selected) {
        model_->markRowDeleted(idx.row());
    }
}

// ===================== Navigation & paging =====================

void DataGridView::onPrevPage() {
    if (currentTable_.empty()) return;
    offset_ = std::max(0, offset_ - limitSpin_->value());
    fetchAndRender();
}

void DataGridView::onNextPage() {
    if (currentTable_.empty()) return;
    offset_ += limitSpin_->value();
    fetchAndRender();
}

void DataGridView::onFiltersToggled() {
    const bool show = filtersBtn_->isChecked();
    filterBar_->setVisible(show);
    filterBarDiv_->setVisible(show);
    if (!show) {
        if (activeFilter_.has_value()) {
            activeFilter_  = std::nullopt;
            filterBar_->reset();
            offset_        = 0;
            totalRowCount_ = -1;
            fetchAndRender();
        }
    }
}

void DataGridView::onModeClicked(int mode) {
    dataTabBtn_->setChecked(mode == 0);
    structTabBtn_->setChecked(mode == 1);

    if (mode == 1) {
        centerStack_->setCurrentIndex(1);
        loadStructure();
    } else {
        centerStack_->setCurrentIndex(0);
    }
}

void DataGridView::loadStructure() {
    if (!adapter_ || currentTable_.empty()) {
        structureView_->clear();
        return;
    }
    try {
        const auto desc = adapter_->describeTable(currentTable_, currentSchema_);
        structureView_->loadStructure(desc);
    } catch (const GridexError& e) {
        structureView_->clear();
        emit loadFailed(QString::fromUtf8(e.what()));
    }
}

void DataGridView::fetchAndRender() {
    if (!adapter_ || currentTable_.empty()) return;
    QElapsedTimer timer; timer.start();
    try {
        auto result = adapter_->fetchRows(currentTable_,
                                          currentSchema_,
                                          std::nullopt,
                                          activeFilter_,
                                          std::nullopt,
                                          limitSpin_->value(),
                                          offset_);
        const int n  = static_cast<int>(result.rows.size());
        const int ms = static_cast<int>(timer.elapsed());

        // Fetch total row count using COUNT(*) so the label can show "X-Y of Z".
        // Only re-fetch when offset_ == 0 (table/filter just changed) or unknown.
        if (offset_ == 0 || totalRowCount_ < 0) {
            try {
                const SQLDialect dialect = sqlDialect(adapter_->databaseType());
                std::string qualTable = currentSchema_
                    ? quoteIdentifier(dialect, *currentSchema_) + "." + quoteIdentifier(dialect, currentTable_)
                    : quoteIdentifier(dialect, currentTable_);
                std::string countSql = "SELECT COUNT(*) FROM " + qualTable;
                if (activeFilter_.has_value() && !activeFilter_->conditions.empty()) {
                    countSql += " WHERE " + activeFilter_->toSQL(dialect);
                }
                const auto countResult = adapter_->executeRaw(countSql);
                if (!countResult.rows.empty() && !countResult.rows[0].empty()) {
                    const auto& cell = countResult.rows[0][0];
                    totalRowCount_ = static_cast<int>(cell.tryIntValue().value_or(0));
                }
            } catch (...) {
                totalRowCount_ = -1;
            }
        }

        // Capture column list before moving result into model.
        std::vector<std::string> colNames;
        colNames.reserve(result.columns.size());
        for (const auto& h : result.columns) colNames.push_back(h.name);

        model_->setResult(std::move(result));
        // Update filter bar column list after data loads.
        if (filterBar_ && !colNames.empty()) {
            filterBar_->setColumns(colNames);
        }
        tableView_->resizeColumnsToContents();
        fitColumnsToViewport();

        const int limit = limitSpin_->value();
        setPagingLabel(n, ms);
        prevBtn_->setEnabled(offset_ > 0);
        nextBtn_->setEnabled(totalRowCount_ < 0
                                 ? n >= limit
                                 : offset_ + n < totalRowCount_);
        emit rowCountChanged(n, ms);

        // Log the query to the SQL log panel.
        if (logPanel_) {
            const QString schema = currentSchema_
                ? QString::fromStdString(*currentSchema_) : QString{};
            const QString table  = QString::fromStdString(currentTable_);
            const QString qualified = schema.isEmpty() ? table : (schema + QStringLiteral(".") + table);
            QString logSql = QStringLiteral("SELECT * FROM %1").arg(qualified);
            if (activeFilter_.has_value() && !activeFilter_->conditions.empty()) {
                logSql += QStringLiteral(" WHERE [filter]");
            }
            logSql += QStringLiteral(" LIMIT %1 OFFSET %2").arg(limitSpin_->value()).arg(offset_);
            logPanel_->appendQuery(logSql, ms);
        }
    } catch (const GridexError& e) {
        model_->clear();
        pageLabel_->setText(QString::fromUtf8(e.what()));
        emit loadFailed(QString::fromUtf8(e.what()));
    }
    // Reset pending UI after a fresh load
    updatePendingLabel();
}

void DataGridView::fitColumnsToViewport() {
    if (!tableView_ || !model_) return;
    auto* h = tableView_->horizontalHeader();
    const int cols = model_->columnCount();
    if (!h || cols == 0) return;
    int used = 0;
    for (int i = 0; i < cols - 1; ++i) used += h->sectionSize(i);
    const int avail = tableView_->viewport()->width() - used;
    if (avail > h->sectionSize(cols - 1)) {
        h->resizeSection(cols - 1, avail);
    }
}

bool DataGridView::eventFilter(QObject* obj, QEvent* event) {
    // Shift+wheel → horizontal scroll on the table viewport.
    if (obj == tableView_->viewport() && event->type() == QEvent::Wheel) {
        auto* we = static_cast<QWheelEvent*>(event);
        if (we->modifiers() & Qt::ShiftModifier) {
            // Forward as horizontal scroll.
            auto* hBar = tableView_->horizontalScrollBar();
            if (hBar) {
                const int delta = we->angleDelta().y();  // vertical wheel value
                hBar->setValue(hBar->value() - delta);
                we->accept();
                return true;
            }
        }
    }
    return QWidget::eventFilter(obj, event);
}

void DataGridView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    QTimer::singleShot(0, this, [this] { fitColumnsToViewport(); });
}

void DataGridView::onRowSelectionChanged() {
    const auto idx = tableView_->currentIndex();
    if (!idx.isValid() || !model_) return;
    const int row = idx.row();
    const auto& result = model_->result();
    if (row < 0 || row >= static_cast<int>(result.rows.size())) return;

    std::vector<std::pair<std::string, std::string>> fields;
    fields.reserve(result.columns.size());
    for (std::size_t c = 0; c < result.columns.size(); ++c) {
        const auto& col = result.columns[c].name;
        const auto& val = (c < result.rows[row].size())
                              ? result.rows[row][c].displayString()
                              : std::string{"NULL"};
        fields.emplace_back(col, val);
    }
    emit rowSelected(fields);
}

void DataGridView::setPagingLabel(int rowsShown, int /*durationMs*/) {
    if (rowsShown == 0) {
        pageLabel_->setText(tr("No rows"));
        return;
    }
    const int start = offset_ + 1;
    const int end   = offset_ + rowsShown;
    if (totalRowCount_ >= 0) {
        pageLabel_->setText(tr("Showing %1–%2 of %3")
                                .arg(start)
                                .arg(end)
                                .arg(QLocale().toString(totalRowCount_)));
    } else {
        pageLabel_->setText(tr("%1–%2").arg(start).arg(end));
    }
}

// ===================== Export =====================

void DataGridView::onExportCsv() {
    if (!model_ || model_->result().isEmpty()) return;
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export as CSV"),
        currentTable_.empty() ? QStringLiteral("export.csv")
                              : QString::fromStdString(currentTable_) + QStringLiteral(".csv"),
        tr("CSV files (*.csv);;All files (*)"));
    if (path.isEmpty()) return;
    try {
        ExportService::exportToCsv(model_->result(), path.toStdString());
    } catch (const std::exception& e) {
        QMessageBox::critical(this, tr("Export Failed"), QString::fromUtf8(e.what()));
    }
}

void DataGridView::onExportJson() {
    if (!model_ || model_->result().isEmpty()) return;
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export as JSON"),
        currentTable_.empty() ? QStringLiteral("export.json")
                              : QString::fromStdString(currentTable_) + QStringLiteral(".json"),
        tr("JSON files (*.json);;All files (*)"));
    if (path.isEmpty()) return;
    try {
        ExportService::exportToJson(model_->result(), path.toStdString());
    } catch (const std::exception& e) {
        QMessageBox::critical(this, tr("Export Failed"), QString::fromUtf8(e.what()));
    }
}

void DataGridView::onExportSql() {
    if (!model_ || model_->result().isEmpty()) return;
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export as SQL"),
        currentTable_.empty() ? QStringLiteral("export.sql")
                              : QString::fromStdString(currentTable_) + QStringLiteral(".sql"),
        tr("SQL files (*.sql);;All files (*)"));
    if (path.isEmpty()) return;
    try {
        ExportService::exportToSql(model_->result(), currentTable_, path.toStdString());
    } catch (const std::exception& e) {
        QMessageBox::critical(this, tr("Export Failed"), QString::fromUtf8(e.what()));
    }
}

}  // namespace gridex
