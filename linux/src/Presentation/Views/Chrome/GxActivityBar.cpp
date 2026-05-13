#include "Presentation/Views/Chrome/GxActivityBar.h"

#include <QButtonGroup>
#include <QToolButton>
#include <QVBoxLayout>

#include "Presentation/Views/Chrome/GxIcons.h"

namespace gridex {

GxActivityBar::GxActivityBar(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("gxActivityBar"));
    setFixedWidth(40);
    setAttribute(Qt::WA_StyledBackground, true);
    // Styling lives in resources/style-gx{,-light}.qss — see selectors
    // QWidget#gxActivityBar / QWidget#gxActivityBar QToolButton.
    group_ = new QButtonGroup(this);
    group_->setExclusive(true);
    buildUi();
}

QToolButton* GxActivityBar::addPanelButton(QVBoxLayout* layout, Panel p,
                                            const QString& iconName,
                                            const QString& tooltip) {
    auto* btn = new QToolButton(this);
    btn->setIcon(GxIcons::glyph(iconName));
    btn->setIconSize({16, 16});
    btn->setToolTip(tooltip);
    btn->setStatusTip(tooltip);
    btn->setAttribute(Qt::WA_Hover, true);
    btn->setAutoRaise(true);
    btn->setCheckable(true);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setFocusPolicy(Qt::NoFocus);
    btn->setProperty("gxRole", "activity-btn");
    group_->addButton(btn, static_cast<int>(p));
    layout->addWidget(btn, 0, Qt::AlignHCenter);
    buttons_[p] = btn;
    return btn;
}

void GxActivityBar::buildUi() {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 6, 0, 6);
    v->setSpacing(2);
    v->setAlignment(Qt::AlignTop);

    // Snippets is the only panel without a working backend on Linux yet,
    // so its button stays omitted. Others all route to live content.
    addPanelButton(v, Panel::Connections, QStringLiteral("db"),      tr("Connections"));
    addPanelButton(v, Panel::Schema,      QStringLiteral("schema"),  tr("Schema browser"));
    addPanelButton(v, Panel::History,     QStringLiteral("history"), tr("Query history"));
    addPanelButton(v, Panel::ERD,         QStringLiteral("erd"),     tr("ER diagram"));

    connect(group_, &QButtonGroup::idClicked, this, [this](int id) {
        setActivePanel(static_cast<Panel>(id));
    });

    v->addStretch(1);

    auto* cog = new QToolButton(this);
    cog->setIcon(GxIcons::glyph(QStringLiteral("cog")));
    cog->setIconSize({16, 16});
    cog->setToolTip(tr("Preferences"));
    cog->setStatusTip(tr("Preferences"));
    cog->setAttribute(Qt::WA_Hover, true);
    cog->setAutoRaise(true);
    cog->setCheckable(false);
    cog->setCursor(Qt::PointingHandCursor);
    cog->setFocusPolicy(Qt::NoFocus);
    connect(cog, &QToolButton::clicked, this, &GxActivityBar::preferencesRequested);
    v->addWidget(cog, 0, Qt::AlignHCenter);

    if (auto* first = buttons_.value(active_)) first->setChecked(true);
}

void GxActivityBar::setActivePanel(Panel p) {
    if (active_ == p && buttons_.value(p) && buttons_.value(p)->isChecked()) return;
    active_ = p;
    for (auto it = buttons_.constBegin(); it != buttons_.constEnd(); ++it) {
        it.value()->setChecked(it.key() == p);
    }
    emit panelChanged(p);
    emit activityChanged(static_cast<int>(p));
}

}  // namespace gridex
