#pragma once

#include <memory>
#include <QWidget>
#include <string>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QMenu;
class QStackedWidget;
class QStandardItem;
class QStandardItemModel;
class QTreeView;
class QTreeWidget;
class QTreeWidgetItem;
class QToolButton;
class QPushButton;

namespace gridex { class TableGridView; }

namespace gridex {

class AppDatabase;
class SavedQueryRepository;
class WorkspaceState;

// Left sidebar shown inside WorkspaceView once a connection is open.
//
// PR A2 rewrite (2026-05): Visual structure mirrors the design's
// FlatSidebar from project/panels.jsx:
//   - 280px fixed width, gx-bg-1 (#11151a) background
//   - Header strip: "SCHEMA" label + plug / refresh / collapse buttons
//   - Activity-tab strip (Items / Queries / History / Saved)
//   - Filter row with search-glyph
//   - QTreeView with custom delegate painting engine pill + status dot
//     on the root row, GxIcons on db/schema/table/view/fn nodes, and
//     row-count badges trailing each table row
//   - Bottom strip: schema combo + db info + new-table / disconnect
//
// Public API (constructor, methods, signals, slots) is preserved verbatim
// — WorkspaceView.cpp consumes this surface and must not change.
class WorkspaceSidebar : public QWidget {
    Q_OBJECT

public:
    explicit WorkspaceSidebar(WorkspaceState* state,
                              std::shared_ptr<AppDatabase> appDb,
                              QWidget* parent = nullptr);
    ~WorkspaceSidebar();

    void refreshTree();
    void logQuery(const QString& sql, int rowCount, int elapsedMs);
    void promptSaveQuery(const QString& sql);

signals:
    void tableSelected(const QString& schema, const QString& table);
    void tableDeleted(const QString& schema, const QString& table);
    void newTableRequested(const QString& schema);
    void disconnectRequested();
    void loadSavedQueryRequested(const QString& sql);
    void functionSelected(const QString& schema, const QString& name);
    void procedureSelected(const QString& schema, const QString& name);

private slots:
    void onConnectionOpened();
    void onConnectionClosed();
    void onItemExpanded(const QModelIndex& index);
    void onItemDoubleClicked(const QModelIndex& index);
    void onSearchChanged(const QString& text);
    void onSchemaChanged(int index);
    void onContextMenuRequested(const QPoint& pos);
    void onSavedQueryContextMenu(const QPoint& pos);

private:
    void buildUi();
    QWidget* buildItemsPage();
    QWidget* buildHeaderStrip();
    QWidget* buildFilterRow(QWidget* parent);
    QWidget* buildBottomBar(QWidget* parent);

    void loadSchemas();
    void loadTablesForSchema(QStandardItem* schemaItem, const QString& schemaName);
    void loadFunctionsForSchema(QStandardItem* parent, const QString& schemaName);
    void loadProceduresForSchema(QStandardItem* parent, const QString& schemaName);
    void reloadActiveSchema();
    void loadHistoryFromDb();
    void reloadSavedQueriesTree();

    // Add a "Tables (N)" / "Views (N)" / "Functions (N)" header row.
    QStandardItem* appendFolderRow(QStandardItem* parent, const QString& label, int count);

    // Data import / backup actions (wired via context menu).
    void runSqlFile();
    void importCsv(const QString& schema, const QString& table);
    void backupDatabase();
    void restoreDatabase();

    WorkspaceState* state_;
    std::shared_ptr<AppDatabase> appDb_;
    std::unique_ptr<SavedQueryRepository> savedQueryRepo_;

    // Header strip
    QToolButton* hdNewConnBtn_  = nullptr;
    QToolButton* hdRefreshBtn_  = nullptr;
    QToolButton* hdCollapseBtn_ = nullptr;

    // Items (Schema) page
    QLineEdit*      searchEdit_     = nullptr;
    QStackedWidget* itemsViewStack_ = nullptr;   // 0=tree, 1=grid
    QToolButton*    gridToggleBtn_  = nullptr;
    QTreeView*      tree_           = nullptr;
    QStandardItemModel* model_      = nullptr;
    TableGridView*  tableGrid_      = nullptr;

    // Bottom bar
    QComboBox*   schemaCombo_    = nullptr;
    QLabel*      dbInfoLabel_    = nullptr;
    QPushButton* disconnectBtn_  = nullptr;
    QPushButton* newTableBtn_    = nullptr;

    // History / Saved queries: data sinks kept alive but not in the
    // visible layout — the visible UI moved up to the activity-bar
    // History/Snippets panels. logQuery / promptSaveQuery still write
    // into these so the public API on this class doesn't regress while
    // SidebarPanelStack's history/snippets panels are still placeholders.
    QListWidget* historyList_    = nullptr;
    QTreeWidget* savedTree_      = nullptr;
};

}
