#include "Presentation/Views/TabBar/WorkspaceTabBar.h"

#include <QEnterEvent>
#include <QEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QStyle>
#include <QUuid>
#include <functional>

#include "Presentation/Views/Chrome/GxIcons.h"

// Faithful port of workspace.jsx + gridex.css `.gx-tabs`. Each tab is a
// QWidget with:
//   [icon 11px] gap [title …ellipsis…] gap [close 14×14]
// Layout sizes are *constant* regardless of hover/active state so the
// strip never shifts when the user moves the mouse over a tab. The close
// button stays in the layout at all times — only its icon flips between
// the "x" glyph (hover or active) and a transparent placeholder.

namespace gridex {

namespace {

// All chrome styles live in resources/style-gx{,-light}.qss. See the
// "Workspace tab bar" section in those sheets.

QIcon iconCloseVisible() {
    return GxIcons::glyph(QStringLiteral("close"), QString(), 9);
}

QIcon iconCloseHidden() {
    // Empty icon — keeps the QPushButton at 14×14 so the row never resizes
    // when the user hovers in or out.
    return QIcon();
}

class TabItem : public QWidget {
public:
    TabItem(const QString& id, const QString& label, bool active, QWidget* parent)
        : QWidget(parent), id_(id) {
        setAttribute(Qt::WA_StyledBackground, true);
        setAttribute(Qt::WA_Hover, true);
        setProperty("gxTab", true);
        setProperty("gxActive", active);
        setCursor(Qt::PointingHandCursor);
        setFixedHeight(30);
        setMaximumWidth(220);

        auto* h = new QHBoxLayout(this);
        // Top inset matches the 2px border-top; padding mirrors .gx-tab
        // (0 8 0 10).
        h->setContentsMargins(10, 2, 8, 0);
        h->setSpacing(6);

        icon_ = new QLabel(this);
        icon_->setFixedSize(13, 13);
        icon_->setPixmap(GxIcons::pixmap(QStringLiteral("sql-new"), QString(), 13));
        icon_->setAttribute(Qt::WA_TransparentForMouseEvents);
        h->addWidget(icon_);

        title_ = new QLabel(label, this);
        title_->setProperty("gxTabTitle", true);
        title_->setProperty("gxActive", active);
        title_->setAttribute(Qt::WA_TransparentForMouseEvents);
        title_->setMinimumWidth(0);
        title_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        h->addWidget(title_, 1);

        close_ = new QPushButton(this);
        close_->setProperty("gxTabClose", true);
        close_->setFlat(true);
        close_->setCursor(Qt::PointingHandCursor);
        close_->setFixedSize(14, 14);
        close_->setIconSize(QSize(9, 9));
        close_->setToolTip(QObject::tr("Close"));
        // X visible on active immediately; hidden on inactive until hover.
        close_->setIcon(active ? iconCloseVisible() : iconCloseHidden());
        h->addWidget(close_);
    }

    QString id() const { return id_; }
    QPushButton* closeBtn() const { return close_; }

    void setActive(bool a) {
        setProperty("gxActive", a);
        if (title_) title_->setProperty("gxActive", a);
        if (close_) close_->setIcon(a || hovered_ ? iconCloseVisible()
                                                  : iconCloseHidden());
        repolish();
    }

    using ActivateCb = std::function<void()>;
    void setActivateCallback(ActivateCb cb) { activateCb_ = std::move(cb); }

protected:
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton && activateCb_) activateCb_();
        QWidget::mousePressEvent(e);
    }
    void enterEvent(QEnterEvent* e) override {
        hovered_ = true;
        if (close_) close_->setIcon(iconCloseVisible());
        QWidget::enterEvent(e);
    }
    void leaveEvent(QEvent* e) override {
        hovered_ = false;
        const bool active = property("gxActive").toBool();
        if (close_) close_->setIcon(active ? iconCloseVisible() : iconCloseHidden());
        QWidget::leaveEvent(e);
    }

private:
    void repolish() {
        style()->unpolish(this); style()->polish(this); update();
        for (auto* w : {static_cast<QWidget*>(title_), static_cast<QWidget*>(close_)}) {
            if (!w) continue;
            w->style()->unpolish(w); w->style()->polish(w); w->update();
        }
    }

    QString id_;
    bool hovered_ = false;
    QLabel* icon_  = nullptr;
    QLabel* title_ = nullptr;
    QPushButton* close_ = nullptr;
    ActivateCb activateCb_;
};

}  // namespace

WorkspaceTabBar::WorkspaceTabBar(QWidget* parent) : QWidget(parent) {
    buildUi();
}

void WorkspaceTabBar::buildUi() {
    setObjectName(QStringLiteral("WorkspaceTabBar"));
    setFixedHeight(30);
    setAttribute(Qt::WA_StyledBackground, true);

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    tabsLayout_ = new QHBoxLayout();
    tabsLayout_->setContentsMargins(0, 0, 0, 0);
    tabsLayout_->setSpacing(0);
    root->addLayout(tabsLayout_);

    plusBtn_ = new QPushButton(this);
    plusBtn_->setObjectName(QStringLiteral("gxNewTab"));
    plusBtn_->setIcon(GxIcons::glyph(QStringLiteral("sql-new"), QString(), 11));
    plusBtn_->setIconSize(QSize(11, 11));
    plusBtn_->setFixedSize(28, 30);
    plusBtn_->setCursor(Qt::PointingHandCursor);
    plusBtn_->setFlat(true);
    plusBtn_->setToolTip(tr("New Query Tab (Ctrl+Shift+N)"));
    connect(plusBtn_, &QPushButton::clicked, this, &WorkspaceTabBar::newTabRequested);
    root->addWidget(plusBtn_);

    root->addStretch(1);
}

QString WorkspaceTabBar::addTab(const QString& label) {
    TabInfo t;
    t.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    t.label = label;
    tabs_.push_back(t);
    setActiveTab(t.id);
    return t.id;
}

void WorkspaceTabBar::removeTab(const QString& id) {
    auto it = std::find_if(tabs_.begin(), tabs_.end(),
                            [&](const TabInfo& t) { return t.id == id; });
    if (it == tabs_.end()) return;
    const bool wasActive = activeId_ == id;
    const auto idx = std::distance(tabs_.begin(), it);
    tabs_.erase(it);

    if (wasActive && !tabs_.empty()) {
        const int next = std::min(static_cast<int>(idx),
                                   static_cast<int>(tabs_.size()) - 1);
        setActiveTab(tabs_[next].id);
    } else if (tabs_.empty()) {
        activeId_.clear();
        rebuildTabs();
    } else {
        rebuildTabs();
    }
}

void WorkspaceTabBar::setActiveTab(const QString& id) {
    activeId_ = id;
    rebuildTabs();
    emit tabSelected(id);
}

void WorkspaceTabBar::renameTab(const QString& id, const QString& label) {
    for (auto& t : tabs_) {
        if (t.id == id) { t.label = label; break; }
    }
    rebuildTabs();
}

void WorkspaceTabBar::rebuildTabs() {
    while (tabsLayout_->count() > 0) {
        auto* item = tabsLayout_->takeAt(0);
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }

    for (const auto& tab : tabs_) {
        const bool active = tab.id == activeId_;
        auto* item = new TabItem(tab.id, tab.label, active, this);
        const QString tabId = tab.id;
        item->setActivateCallback([this, tabId] { setActiveTab(tabId); });
        connect(item->closeBtn(), &QPushButton::clicked, this, [this, tabId] {
            emit tabCloseRequested(tabId);
        });
        tabsLayout_->addWidget(item);
    }
}

}  // namespace gridex
