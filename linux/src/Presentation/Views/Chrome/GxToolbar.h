#pragma once

// 36px QToolBar matching .gx-toolbar in gridex.css.
//
// Layout per chrome.jsx (left-to-right):
//   [plug · sql-new · folder · save]  divider
//   [play (primary) · play-all · stop (danger)]  divider
//   [commit · rollback · explain]  divider
//   [refresh · export · erd]  spacer
//   [search field, 280px wide]
//
// All 13 buttons render unconditionally; ones whose feature isn't wired
// yet are disabled (setEnabled(false)), not hidden. The styling is fully
// scoped under QToolBar#gxToolbar in style-gx.qss so this toolbar does
// not influence QToolButtons elsewhere in the app.

#include <QToolBar>

class QAction;
class QLineEdit;
class QToolButton;

namespace gridex {

class GxToolbar : public QToolBar {
    Q_OBJECT
public:
    explicit GxToolbar(QWidget* parent = nullptr);

    // File group
    QAction* newConnectionAction() const noexcept { return newConnAction_; }
    QAction* newQueryAction()      const noexcept { return newQueryAction_; }
    QAction* openFileAction()      const noexcept { return openFileAction_; }
    QAction* saveAction()          const noexcept { return saveAction_; }

    // Run group
    QAction* runAction()       const noexcept { return runAction_; }
    QAction* runAllAction()    const noexcept { return runAllAction_; }
    QAction* stopAction()      const noexcept { return stopAction_; }

    // Transaction group
    QAction* commitAction()    const noexcept { return commitAction_; }
    QAction* rollbackAction()  const noexcept { return rollbackAction_; }
    QAction* explainAction()   const noexcept { return explainAction_; }

    // Schema/export group
    QAction* refreshAction()   const noexcept { return refreshAction_; }
    QAction* exportAction()    const noexcept { return exportAction_; }
    QAction* erdAction()       const noexcept { return erdAction_; }

    // Database switcher (popup picks which DB to use on the same server)
    QAction* switchDbAction()  const noexcept { return switchDbAction_; }

    QLineEdit* search()        const noexcept { return search_; }

signals:
    void searchSubmitted(const QString& text);

private:
    QAction* newConnAction_   = nullptr;
    QAction* newQueryAction_  = nullptr;
    QAction* openFileAction_  = nullptr;
    QAction* saveAction_      = nullptr;

    QAction* runAction_       = nullptr;
    QAction* runAllAction_    = nullptr;
    QAction* stopAction_      = nullptr;

    QAction* commitAction_    = nullptr;
    QAction* rollbackAction_  = nullptr;
    QAction* explainAction_   = nullptr;

    QAction* refreshAction_   = nullptr;
    QAction* exportAction_    = nullptr;
    QAction* erdAction_       = nullptr;
    QAction* switchDbAction_  = nullptr;

    QLineEdit* search_        = nullptr;
};

}  // namespace gridex
