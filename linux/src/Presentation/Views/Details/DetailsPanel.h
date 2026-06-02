#pragma once

#include <QPushButton>
#include <QStackedWidget>
#include <QWidget>
#include <string>
#include <vector>

class QLabel;
class QLineEdit;
class QScrollArea;
class QVBoxLayout;

namespace gridex {

class AIChatView;
class SecretStore;
class WorkspaceState;

// Right inspector panel matching .gx-inspect in gridex.css.
//
// Layout (mirrors panels.jsx Inspector):
//   gx-insp-hd        header strip (bg-2): icon + title + sub
//   gx-insp-tabs      5 tabs: Columns / Indexes / Keys / Triggers / DDL
//                     (plus an extra "Assistant" tab for the AI chat that
//                     replaced the previous two-tab Details/Assistant
//                     layout)
//   gx-insp-body      stacked content for the active tab
//
// Public API is preserved verbatim: setSelectedRow / clearSelectedRow /
// fieldEdited still drive the row inspector — which now lives under the
// "Columns" tab as the row-mode view. Indexes/Keys/Triggers/DDL render
// placeholders until their schema-introspection wiring lands.
class DetailsPanel : public QWidget {
    Q_OBJECT

public:
    explicit DetailsPanel(SecretStore* secretStore,
                          WorkspaceState* state,
                          QWidget* parent = nullptr);

    struct FieldEntry {
        std::string column;
        std::string value;
    };

public slots:
    void setSelectedRow(const std::vector<FieldEntry>& fields);
    void clearSelectedRow();

signals:
    // Emitted when user edits a field value in the row inspector.
    // columnIndex is the position in the current fields array.
    void fieldEdited(int columnIndex, const QString& newValue);

private slots:
    void onTabClicked(int index);
    void onSearchChanged(const QString& text);

private:
    void buildUi();
    void rebuildDetailsList();

    QPushButton* makeTabButton(const QString& title, int index, QWidget* parent);

    int activeTab_ = 0;

    // Tab bar (6 tabs: 5 inspector tabs + Assistant)
    QPushButton* colsTabBtn_      = nullptr;
    QPushButton* idxTabBtn_       = nullptr;
    QPushButton* keysTabBtn_      = nullptr;
    QPushButton* trigTabBtn_      = nullptr;
    QPushButton* ddlTabBtn_       = nullptr;
    QPushButton* assistantTabBtn_ = nullptr;

    // Stacked content
    QStackedWidget* stack_ = nullptr;

    // Page 0 — Columns / row inspector
    QWidget*     detailsPage_  = nullptr;
    QLineEdit*   searchEdit_   = nullptr;
    QScrollArea* scrollArea_   = nullptr;
    QWidget*     fieldsHost_   = nullptr;
    QVBoxLayout* fieldsLayout_ = nullptr;
    QLabel*      emptyLabel_   = nullptr;

    // Page 5 — AI chat
    AIChatView* chatView_ = nullptr;

    std::vector<FieldEntry> currentFields_;
};

}
