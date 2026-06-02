#pragma once

// 280px fixed-width sidebar container that swaps content driven by the
// activity bar. Mirrors `app.jsx`'s single side-panel composition where
// activity index selects which of five panels is visible:
//
//   0 — Connections   (placeholder empty-state for now)
//   1 — Schema        (existing WorkspaceSidebar, reparented in)
//   2 — History       (placeholder)
//   3 — Snippets      (placeholder)
//   4 — ERD           (placeholder)
//
// Placeholders render the design's gx-empty-state card (centered icon +
// headline + sub-copy) so the activity buttons never feel "dead".

#include <QWidget>
#include <memory>

class QListWidget;
class QStackedWidget;

namespace gridex {

class AppDatabase;
class WorkspaceSidebar;

class SidebarPanelStack : public QWidget {
    Q_OBJECT
public:
    explicit SidebarPanelStack(QWidget* parent = nullptr);

    // Inject the app's history database so the History panel can populate
    // itself with real rows from `listAllHistory()`. Pass before
    // setCurrentIndex(2) lands on the History panel for the first time.
    void setAppDatabase(std::shared_ptr<AppDatabase> db);

    // Refresh the History panel from the database. Call when a new query
    // has been logged (e.g. from WorkspaceSidebar::logQuery).
    void reloadHistory();

signals:
    // Emitted when the user clicks "Open ER diagram" in the ERD panel.
    // MainWindow forwards this to WorkspaceView::onNewErDiagramTab.
    void erdRequested();
    // Emitted when the user double-clicks a history row. Carries the SQL
    // for the caller to load into the current editor tab.
    void historyEntryActivated(const QString& sql);

public:

    // Inject the existing WorkspaceSidebar at index 1 (Schema). The widget
    // is reparented in; the caller (MainWindow) keeps its raw pointer to
    // wire signals against — the stack does NOT take ownership in any
    // memory sense beyond Qt's parent-child rules.
    void setSchemaWidget(WorkspaceSidebar* schema);

    // Inject a ConnectionSidebar (or any QWidget) into the Connections
    // slot (index 0). Replaces the default placeholder.
    void setConnectionsWidget(QWidget* w);

    WorkspaceSidebar* schemaWidget() const noexcept { return schema_; }

    // Drive panel index 0..4. Out-of-range values are clamped.
    void setCurrentIndex(int index);
    int  currentIndex() const noexcept;

private:
    QWidget* buildEmptyState(const QString& iconGlyph,
                             const QString& title,
                             const QString& subtitle);

    QStackedWidget*   stack_       = nullptr;
    WorkspaceSidebar* schema_      = nullptr;
    QWidget*          schemaSlot_  = nullptr;  // wrapper at index 1
    QListWidget*      historyList_ = nullptr;  // populated lazily
    std::shared_ptr<AppDatabase> appDb_;
};

}  // namespace gridex
