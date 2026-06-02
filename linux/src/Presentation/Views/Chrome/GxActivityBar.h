#pragma once

// 40px vertical activity bar — VSCode-style nav strip on the far left of
// the IDE body. Five panel switchers (Connections / Schema / History /
// Snippets / ERD) with a 2px accent left border on the active button,
// plus a cog at the bottom for Preferences.
//
// Mirrors panels.jsx's `ActivityBar` component. Drives a sibling
// SidebarPanelStack via the `activityChanged(int)` signal — indices
// 0..4 in declaration order match the stack's panel ordering.

#include <QHash>
#include <QWidget>

class QButtonGroup;
class QVBoxLayout;
class QToolButton;

namespace gridex {

class GxActivityBar : public QWidget {
    Q_OBJECT
public:
    enum class Panel {
        Connections = 0,
        Schema      = 1,
        History     = 2,
        Snippets    = 3,
        ERD         = 4,
    };

    explicit GxActivityBar(QWidget* parent = nullptr);

    Panel activePanel() const noexcept { return active_; }
    void setActivePanel(Panel p);

signals:
    void panelChanged(Panel p);
    // Integer mirror of panelChanged for SidebarPanelStack::setCurrentIndex.
    void activityChanged(int index);
    void preferencesRequested();

private:
    void buildUi();
    QToolButton* addPanelButton(QVBoxLayout* layout,
                                Panel p,
                                const QString& iconName,
                                const QString& tooltip);

    QHash<Panel, QToolButton*> buttons_;
    QButtonGroup* group_ = nullptr;
    Panel active_ = Panel::Schema;
};

}  // namespace gridex
