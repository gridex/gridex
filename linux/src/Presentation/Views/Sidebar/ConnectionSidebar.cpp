#include "Presentation/Views/Sidebar/ConnectionSidebar.h"

#include "Presentation/Views/Chrome/GxIcons.h"

#include <QAction>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QVariant>

#include "Data/Persistence/AppDatabase.h"
#include "Presentation/ViewModels/ConnectionListViewModel.h"
#include "Presentation/Views/Sidebar/ConnectionRowWidget.h"

namespace gridex {

namespace {
constexpr int kRoleConnectionId = Qt::UserRole + 1;
constexpr int kRoleIsGroup      = Qt::UserRole + 2;
constexpr int kPageEmpty = 0;
constexpr int kPageTree  = 1;

QPushButton* makeToolbarButton(const QString& iconName, const QString& tooltip, QWidget* parent) {
    auto* btn = new QPushButton(parent);
    btn->setToolTip(tooltip);
    btn->setFixedSize(28, 28);
    btn->setFlat(true);
    btn->setIcon(GxIcons::glyph(iconName));
    btn->setIconSize({14, 14});
    return btn;
}
}

ConnectionSidebar::ConnectionSidebar(ConnectionListViewModel* viewModel,
                                     std::shared_ptr<AppDatabase> appDb,
                                     QWidget* parent)
    : QWidget(parent), viewModel_(viewModel), appDb_(std::move(appDb)) {
    buildUi();
    if (viewModel_) {
        connect(viewModel_, &ConnectionListViewModel::connectionsChanged,
                this, &ConnectionSidebar::onViewModelChanged);
        onViewModelChanged();
    }
}

void ConnectionSidebar::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* toolbar = new QWidget(this);
    auto* h = new QHBoxLayout(toolbar);
    h->setContentsMargins(16, 10, 16, 10);
    h->setSpacing(10);

    searchEdit_ = new QLineEdit(toolbar);
    searchEdit_->setPlaceholderText(tr("Search connections..."));
    searchEdit_->setClearButtonEnabled(true);
    connect(searchEdit_, &QLineEdit::textChanged,
            this, &ConnectionSidebar::onSearchChanged);
    h->addWidget(searchEdit_, 1);

    auto* addBtn = makeToolbarButton(QStringLiteral("plug"), tr("New Connection"), toolbar);
    connect(addBtn, &QPushButton::clicked, this, &ConnectionSidebar::addConnectionRequested);
    h->addWidget(addBtn);

    auto* groupBtn = makeToolbarButton(QStringLiteral("folder"), tr("New Group"), toolbar);
    connect(groupBtn, &QPushButton::clicked, this, &ConnectionSidebar::newGroupRequested);
    h->addWidget(groupBtn);

    root->addWidget(toolbar);

    auto* div = new QFrame(this);
    div->setFrameShape(QFrame::HLine);
    root->addWidget(div);

    body_ = new QStackedWidget(this);

    // Page 0: empty state
    auto* empty = new QWidget(body_);
    auto* ev = new QVBoxLayout(empty);
    ev->setContentsMargins(24, 24, 24, 24);
    ev->addStretch();
    auto* emptyIcon = new QLabel(QStringLiteral("🗄"), empty);
    emptyIcon->setAlignment(Qt::AlignCenter);
    ev->addWidget(emptyIcon);
    auto* emptyTitle = new QLabel(tr("No connections yet"), empty);
    emptyTitle->setAlignment(Qt::AlignCenter);
    ev->addWidget(emptyTitle);
    auto* emptySub = new QLabel(tr("Create your first database connection to get started"), empty);
    emptySub->setAlignment(Qt::AlignCenter);
    emptySub->setWordWrap(true);
    ev->addWidget(emptySub);
    auto* createBtn = new QPushButton(tr("Create Connection"), empty);
    connect(createBtn, &QPushButton::clicked, this, &ConnectionSidebar::addConnectionRequested);
    auto* createRow = new QHBoxLayout();
    createRow->addStretch();
    createRow->addWidget(createBtn);
    createRow->addStretch();
    ev->addSpacing(8);
    ev->addLayout(createRow);
    ev->addStretch();
    body_->addWidget(empty);

    // Page 1: grouped tree
    tree_ = new QTreeWidget(body_);
    tree_->setHeaderHidden(true);
    tree_->setFrameShape(QFrame::NoFrame);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->setRootIsDecorated(true);
    connect(tree_, &QTreeWidget::itemDoubleClicked,
            this, &ConnectionSidebar::onItemDoubleClicked);
    connect(tree_, &QTreeWidget::customContextMenuRequested,
            this, &ConnectionSidebar::onContextMenuRequested);
    body_->addWidget(tree_);

    body_->setCurrentIndex(kPageEmpty);
    root->addWidget(body_, 1);
}

void ConnectionSidebar::onViewModelChanged() {
    tree_->clear();
    if (!viewModel_) return;

    const auto search = searchEdit_->text().toLower();
    const auto& connections = viewModel_->connections();

    QMap<QString, QTreeWidgetItem*> groupItems;
    int shownCount = 0;

    auto getOrCreateGroup = [&](const QString& groupName) -> QTreeWidgetItem* {
        if (groupItems.contains(groupName)) return groupItems[groupName];
        auto* gi = new QTreeWidgetItem(tree_);
        gi->setText(0, groupName);
        gi->setData(0, kRoleIsGroup, true);
        gi->setData(0, kRoleConnectionId, QString{});
        gi->setFlags(gi->flags() & ~Qt::ItemIsSelectable);
        groupItems[groupName] = gi;
        return gi;
    };

    for (const auto& c : connections) {
        const QString name = QString::fromUtf8(c.name.c_str());
        if (!search.isEmpty() && !name.toLower().contains(search)) continue;

        const QString groupName = c.group
            ? QString::fromUtf8(c.group->c_str())
            : tr("Ungrouped");

        auto* parent = getOrCreateGroup(groupName);

        auto* item = new QTreeWidgetItem(parent);
        item->setData(0, kRoleConnectionId, QString::fromUtf8(c.id.c_str()));
        item->setData(0, kRoleIsGroup, false);
        item->setSizeHint(0, QSize(0, 52));

        auto* row = new ConnectionRowWidget(c, tree_);
        tree_->setItemWidget(item, 0, row);
        ++shownCount;
    }

    tree_->expandAll();

    const bool hasAny = !connections.empty();
    body_->setCurrentIndex((hasAny || !search.isEmpty()) ? kPageTree : kPageEmpty);
}

void ConnectionSidebar::onSearchChanged(const QString&) {
    onViewModelChanged();
}

QString ConnectionSidebar::idOfItem(QTreeWidgetItem* item) const {
    if (!item) return {};
    return item->data(0, kRoleConnectionId).toString();
}

void ConnectionSidebar::onItemDoubleClicked(QTreeWidgetItem* item, int) {
    if (!item || item->data(0, kRoleIsGroup).toBool()) return;
    const auto id = idOfItem(item);
    if (!id.isEmpty()) emit connectionSelected(id);
}

void ConnectionSidebar::onContextMenuRequested(const QPoint& pos) {
    auto* item = tree_->itemAt(pos);
    QMenu menu(this);

    if (item && item->data(0, kRoleIsGroup).toBool()) {
        const QString groupName = item->text(0);

        auto* renameAct = menu.addAction(tr("Rename Group..."));
        connect(renameAct, &QAction::triggered, this, [this, groupName] {
            bool ok = false;
            const QString newName = QInputDialog::getText(
                this, tr("Rename Group"), tr("New name:"),
                QLineEdit::Normal, groupName, &ok);
            if (!ok || newName.trimmed().isEmpty()) return;
            if (!appDb_) return;
            // Move all connections in this group to newName
            if (viewModel_) {
                for (const auto& c : viewModel_->connections()) {
                    const QString cGroup = c.group
                        ? QString::fromUtf8(c.group->c_str()) : tr("Ungrouped");
                    if (cGroup == groupName) {
                        try {
                            appDb_->setConnectionGroup(c.id, newName.trimmed().toStdString());
                        } catch (...) {}
                    }
                }
                viewModel_->reload();
            }
        });

        auto* deleteGroupAct = menu.addAction(tr("Delete Group"));
        connect(deleteGroupAct, &QAction::triggered, this, [this, groupName] {
            if (!appDb_ || !viewModel_) return;
            for (const auto& c : viewModel_->connections()) {
                const QString cGroup = c.group
                    ? QString::fromUtf8(c.group->c_str()) : tr("Ungrouped");
                if (cGroup == groupName) {
                    try { appDb_->setConnectionGroup(c.id, {}); } catch (...) {}
                }
            }
            viewModel_->reload();
        });

        menu.addSeparator();

    } else if (item && !item->data(0, kRoleIsGroup).toBool()) {
        const auto id = idOfItem(item);

        auto* openAct = menu.addAction(tr("Connect"));
        connect(openAct, &QAction::triggered, this,
                [this, id] { emit connectionSelected(id); });

        auto* editAct = menu.addAction(tr("Edit…"));
        connect(editAct, &QAction::triggered, this,
                [this, id] { emit editConnectionRequested(id); });

        auto* moveAct = menu.addAction(tr("Move to Group..."));
        connect(moveAct, &QAction::triggered, this, [this, id] {
            if (!appDb_ || !viewModel_) return;
            // Collect existing group names
            QStringList groups;
            groups << tr("(Ungrouped)");
            for (const auto& c : viewModel_->connections()) {
                if (c.group) {
                    const QString g = QString::fromUtf8(c.group->c_str());
                    if (!groups.contains(g)) groups << g;
                }
            }
            bool ok = false;
            const QString chosen = QInputDialog::getItem(
                this, tr("Move to Group"), tr("Select or type a group:"),
                groups, 0, /*editable*/ true, &ok);
            if (!ok) return;
            const std::string newGroup = (chosen == tr("(Ungrouped)"))
                ? std::string{} : chosen.trimmed().toStdString();
            try {
                appDb_->setConnectionGroup(id.toStdString(), newGroup);
                viewModel_->reload();
            } catch (const std::exception& e) {
                (void)e;
            }
        });

        menu.addSeparator();

        auto* delAct = menu.addAction(tr("Delete"));
        connect(delAct, &QAction::triggered, this,
                [this, id] { emit removeConnectionRequested(id); });

        menu.addSeparator();
    }

    auto* addAct = menu.addAction(tr("New Connection…"));
    connect(addAct, &QAction::triggered, this, &ConnectionSidebar::addConnectionRequested);

    menu.exec(tree_->viewport()->mapToGlobal(pos));
}

}
