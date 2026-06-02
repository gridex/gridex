#include "Presentation/Views/Sidebar/SidebarPanelStack.h"

#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "Data/Persistence/AppDatabase.h"
#include "Presentation/Views/Chrome/GxIcons.h"
#include "Presentation/Views/Sidebar/WorkspaceSidebar.h"

namespace gridex {

SidebarPanelStack::SidebarPanelStack(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("gxSidebarPanelStack"));
    setFixedWidth(280);
    setAttribute(Qt::WA_StyledBackground, true);
    // Background + right border live in the theme QSS — QWidget#gxSidebarPanelStack.

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    stack_ = new QStackedWidget(this);

    // 0 — Connections (placeholder)
    stack_->addWidget(buildEmptyState(
        QStringLiteral("plug"),
        tr("Connections"),
        tr("Manage saved servers and databases here.\n"
           "(Move from the legacy connections panel — coming next.)")));

    // 1 — Schema slot. Empty wrapper until setSchemaWidget() is called.
    schemaSlot_ = new QWidget(stack_);
    auto* slotLayout = new QVBoxLayout(schemaSlot_);
    slotLayout->setContentsMargins(0, 0, 0, 0);
    slotLayout->setSpacing(0);
    stack_->addWidget(schemaSlot_);

    // 2 — History (real list; rows arrive via reloadHistory()).
    {
        auto* w = new QWidget(stack_);
        w->setObjectName(QStringLiteral("gxHistoryPanel"));
        w->setAttribute(Qt::WA_StyledBackground, true);
        auto* v = new QVBoxLayout(w);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(0);

        // Header strip matching .gx-side-hd
        auto* header = new QLabel(tr("QUERY HISTORY"), w);
        header->setObjectName(QStringLiteral("gxHistoryHeader"));
        header->setFixedHeight(28);
        header->setContentsMargins(10, 0, 10, 0);
        v->addWidget(header);

        historyList_ = new QListWidget(w);
        historyList_->setObjectName(QStringLiteral("gxHistoryList"));
        historyList_->setFrameShape(QFrame::NoFrame);
        v->addWidget(historyList_, 1);

        connect(historyList_, &QListWidget::itemDoubleClicked, this,
                [this](QListWidgetItem* it) {
                    if (!it) return;
                    const QString sql = it->data(Qt::UserRole).toString();
                    if (!sql.isEmpty()) emit historyEntryActivated(sql);
                });
        stack_->addWidget(w);
    }

    // 3 — Snippets (placeholder)
    stack_->addWidget(buildEmptyState(
        QStringLiteral("snippets"),
        tr("Snippets"),
        tr("Save reusable SQL fragments and recall them with Ctrl+Shift+P.")));

    // 4 — ERD (functional button)
    {
        auto* w = new QWidget(stack_);
        w->setObjectName(QStringLiteral("gxErdPanel"));
        w->setAttribute(Qt::WA_StyledBackground, true);
        auto* v = new QVBoxLayout(w);
        v->setContentsMargins(24, 24, 24, 24);
        v->setSpacing(12);
        v->addStretch();

        auto* iconLabel = new QLabel(w);
        iconLabel->setAlignment(Qt::AlignCenter);
        iconLabel->setPixmap(GxIcons::glyph(QStringLiteral("erd")).pixmap(36, 36));
        v->addWidget(iconLabel);

        auto* title = new QLabel(tr("ER diagrams"), w);
        title->setObjectName(QStringLiteral("gxEmptyStateTitle"));
        title->setAlignment(Qt::AlignCenter);
        v->addWidget(title);

        auto* sub = new QLabel(
            tr("Generate a relationship diagram for the\n"
                "currently connected schema."), w);
        sub->setObjectName(QStringLiteral("gxEmptyStateSub"));
        sub->setAlignment(Qt::AlignCenter);
        sub->setWordWrap(true);
        v->addWidget(sub);

        auto* btn = new QPushButton(tr("Open ER diagram"), w);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setProperty("gxKind", "primary");
        connect(btn, &QPushButton::clicked, this,
                &SidebarPanelStack::erdRequested);
        v->addWidget(btn, 0, Qt::AlignCenter);

        v->addStretch();
        stack_->addWidget(w);
    }

    stack_->setCurrentIndex(1);
    root->addWidget(stack_, 1);
}

void SidebarPanelStack::setConnectionsWidget(QWidget* w) {
    if (!stack_ || !w) return;
    auto* old = stack_->widget(0);
    stack_->insertWidget(0, w);
    if (old) {
        stack_->removeWidget(old);
        old->deleteLater();
    }
    w->show();
}

void SidebarPanelStack::setSchemaWidget(WorkspaceSidebar* schema) {
    if (!schemaSlot_ || !schema) return;
    schema_ = schema;
    schema->setParent(schemaSlot_);
    schema->setMinimumWidth(0);
    schema->setMaximumWidth(QWIDGETSIZE_MAX);
    auto* layout = qobject_cast<QVBoxLayout*>(schemaSlot_->layout());
    if (layout) layout->addWidget(schema);
    schema->show();
}

void SidebarPanelStack::setAppDatabase(std::shared_ptr<AppDatabase> db) {
    appDb_ = std::move(db);
    reloadHistory();
}

void SidebarPanelStack::reloadHistory() {
    if (!historyList_ || !appDb_) return;
    historyList_->clear();
    try {
        const auto entries = appDb_->listAllHistory(200);
        for (const auto& h : entries) {
            const QString sql = QString::fromStdString(h.sql);
            const QString preview = sql.simplified().left(64);
            const QString label = QStringLiteral("%1 rows · %2 ms\n%3")
                                      .arg(h.rowCount)
                                      .arg(h.durationMs)
                                      .arg(preview);
            auto* item = new QListWidgetItem(label);
            item->setData(Qt::UserRole, sql);
            item->setToolTip(sql);
            historyList_->addItem(item);
        }
    } catch (...) {
        // App DB unavailable — leave the list empty.
    }
}

void SidebarPanelStack::setCurrentIndex(int index) {
    if (!stack_) return;
    if (index < 0) index = 0;
    if (index >= stack_->count()) index = stack_->count() - 1;
    // Lazy-refresh the history panel each time the user lands on it.
    if (index == 2) reloadHistory();
    stack_->setCurrentIndex(index);
}

int SidebarPanelStack::currentIndex() const noexcept {
    return stack_ ? stack_->currentIndex() : 0;
}

QWidget* SidebarPanelStack::buildEmptyState(const QString& iconGlyph,
                                            const QString& title,
                                            const QString& subtitle) {
    auto* w = new QWidget(stack_);
    w->setObjectName(QStringLiteral("gxEmptyState"));
    w->setAttribute(Qt::WA_StyledBackground, true);

    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(24, 24, 24, 24);
    v->setSpacing(10);
    v->addStretch();

    auto* iconLabel = new QLabel(w);
    iconLabel->setAlignment(Qt::AlignCenter);
    iconLabel->setPixmap(GxIcons::glyph(iconGlyph).pixmap(36, 36));
    v->addWidget(iconLabel);

    auto* titleLabel = new QLabel(title, w);
    titleLabel->setObjectName(QStringLiteral("gxEmptyStateTitle"));
    titleLabel->setAlignment(Qt::AlignCenter);
    v->addWidget(titleLabel);

    auto* subLabel = new QLabel(subtitle, w);
    subLabel->setObjectName(QStringLiteral("gxEmptyStateSub"));
    subLabel->setAlignment(Qt::AlignCenter);
    subLabel->setWordWrap(true);
    v->addWidget(subLabel);

    v->addStretch();
    return w;
}

}  // namespace gridex
