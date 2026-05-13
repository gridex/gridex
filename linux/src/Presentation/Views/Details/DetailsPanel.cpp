#include "Presentation/Views/Details/DetailsPanel.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "Presentation/Views/AIChat/AIChatView.h"
#include "Presentation/Views/Chrome/GxIcons.h"

namespace gridex {

DetailsPanel::DetailsPanel(SecretStore* secretStore, WorkspaceState* state, QWidget* parent)
    : QWidget(parent) {
    buildUi();
    chatView_ = new AIChatView(secretStore, state, stack_);
    stack_->addWidget(chatView_);  // index 1 — Assistant
}

QPushButton* DetailsPanel::makeTabButton(const QString& title, int index, QWidget* parent) {
    // .gx-insp-tab — selectors live in resources/style-gx{,-light}.qss
    // under QPushButton[gxRole="insp-tab"].
    auto* btn = new QPushButton(title, parent);
    btn->setFlat(true);
    btn->setCheckable(true);
    btn->setAutoExclusive(true);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setProperty("gxRole", "insp-tab");
    connect(btn, &QPushButton::clicked, this, [this, index] { onTabClicked(index); });
    return btn;
}

void DetailsPanel::buildUi() {
    setObjectName(QStringLiteral("gxInspect"));
    setAttribute(Qt::WA_StyledBackground, true);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ── .gx-insp-tabs ────────────────────────────────────────────────
    auto* tabBar = new QWidget(this);
    tabBar->setObjectName(QStringLiteral("gxInspTabs"));
    tabBar->setAttribute(Qt::WA_StyledBackground, true);
    auto* tabH = new QHBoxLayout(tabBar);
    tabH->setContentsMargins(0, 0, 0, 0);
    tabH->setSpacing(0);

    colsTabBtn_      = makeTabButton(tr("Details"),   0, tabBar);
    assistantTabBtn_ = makeTabButton(tr("Assistant"), 1, tabBar);
    tabH->addWidget(colsTabBtn_);
    tabH->addWidget(assistantTabBtn_);
    tabH->addStretch(1);
    root->addWidget(tabBar);

    // ── Stacked body ─────────────────────────────────────────────────
    stack_ = new QStackedWidget(this);
    stack_->setObjectName(QStringLiteral("gxInspStack"));

    // Page 0 — Columns / row inspector.
    detailsPage_ = new QWidget(stack_);
    detailsPage_->setObjectName(QStringLiteral("gxInspBody"));
    detailsPage_->setAttribute(Qt::WA_StyledBackground, true);
    auto* dv = new QVBoxLayout(detailsPage_);
    dv->setContentsMargins(0, 0, 0, 0);
    dv->setSpacing(0);

    auto* searchRow = new QWidget(detailsPage_);
    auto* sh = new QHBoxLayout(searchRow);
    sh->setContentsMargins(8, 6, 8, 6);
    searchEdit_ = new QLineEdit(searchRow);
    searchEdit_->setObjectName(QStringLiteral("gxInspSearch"));
    searchEdit_->setPlaceholderText(tr("Filter fields…"));
    searchEdit_->setClearButtonEnabled(false);
    connect(searchEdit_, &QLineEdit::textChanged, this, &DetailsPanel::onSearchChanged);
    sh->addWidget(searchEdit_);
    dv->addWidget(searchRow);

    scrollArea_ = new QScrollArea(detailsPage_);
    scrollArea_->setObjectName(QStringLiteral("gxInspScroll"));
    scrollArea_->setWidgetResizable(true);
    scrollArea_->setFrameShape(QFrame::NoFrame);
    fieldsHost_ = new QWidget(scrollArea_);
    fieldsHost_->setObjectName(QStringLiteral("gxInspFieldsHost"));
    fieldsHost_->setAttribute(Qt::WA_StyledBackground, true);
    fieldsLayout_ = new QVBoxLayout(fieldsHost_);
    fieldsLayout_->setContentsMargins(0, 0, 0, 0);
    fieldsLayout_->setSpacing(0);
    fieldsLayout_->addStretch();
    scrollArea_->setWidget(fieldsHost_);
    dv->addWidget(scrollArea_, 1);

    emptyLabel_ = new QLabel(tr("No row selected"), detailsPage_);
    emptyLabel_->setObjectName(QStringLiteral("gxInspEmpty"));
    emptyLabel_->setAlignment(Qt::AlignCenter);
    dv->addWidget(emptyLabel_, 1);
    scrollArea_->setVisible(false);

    stack_->addWidget(detailsPage_);

    root->addWidget(stack_, 1);

    // Default tab: Columns / row inspector.
    onTabClicked(0);
}

void DetailsPanel::onTabClicked(int index) {
    // Only Details (0) and Assistant (1) are real. Older callers may pass
    // 2..5 (Indexes/Keys/Triggers/DDL) — clamp them to 0.
    if (index < 0 || index > 1) index = 0;
    activeTab_ = index;
    stack_->setCurrentIndex(index);
    if (colsTabBtn_)      colsTabBtn_->setChecked(index == 0);
    if (assistantTabBtn_) assistantTabBtn_->setChecked(index == 1);
}

void DetailsPanel::setSelectedRow(const std::vector<FieldEntry>& fields) {
    currentFields_ = fields;
    emptyLabel_->setVisible(false);
    scrollArea_->setVisible(true);
    rebuildDetailsList();
}

void DetailsPanel::clearSelectedRow() {
    currentFields_.clear();
    emptyLabel_->setVisible(true);
    scrollArea_->setVisible(false);
    while (fieldsLayout_->count() > 1) {
        auto* item = fieldsLayout_->takeAt(0);
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }
}

void DetailsPanel::onSearchChanged(const QString&) {
    rebuildDetailsList();
}

void DetailsPanel::rebuildDetailsList() {
    while (fieldsLayout_->count() > 1) {
        auto* item = fieldsLayout_->takeAt(0);
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }

    const auto filter = searchEdit_->text().trimmed().toLower();

    // Section header — .gx-insp-section
    if (!currentFields_.empty()) {
        auto* section = new QLabel(tr("FIELDS"), fieldsHost_);
        section->setObjectName(QStringLiteral("gxInspSection"));
        fieldsLayout_->insertWidget(fieldsLayout_->count() - 1, section);
    }

    for (std::size_t fi = 0; fi < currentFields_.size(); ++fi) {
        const auto& f = currentFields_[fi];
        const auto col = QString::fromUtf8(f.column.c_str());
        const auto val = QString::fromUtf8(f.value.c_str());

        if (!filter.isEmpty() &&
            !col.toLower().contains(filter) &&
            !val.toLower().contains(filter)) {
            continue;
        }

        // .gx-col-row — 18px icon | name | type/value (two-line)
        auto* row = new QWidget(fieldsHost_);
        row->setObjectName(QStringLiteral("gxInspRow"));
        row->setAttribute(Qt::WA_StyledBackground, true);
        auto* rh = new QHBoxLayout(row);
        rh->setContentsMargins(10, 5, 14, 5);
        rh->setSpacing(6);

        auto* iconLbl = new QLabel(row);
        iconLbl->setPixmap(GxIcons::pixmap(QStringLiteral("col"), QString(), 10));
        iconLbl->setFixedSize(18, 18);
        iconLbl->setAlignment(Qt::AlignCenter);
        iconLbl->setAttribute(Qt::WA_TranslucentBackground);
        rh->addWidget(iconLbl);

        auto* col2 = new QVBoxLayout();
        col2->setContentsMargins(0, 0, 0, 0);
        col2->setSpacing(1);

        auto* colLabel = new QLabel(col, row);
        colLabel->setObjectName(QStringLiteral("gxInspRowName"));
        col2->addWidget(colLabel);

        auto* valEdit = new QLineEdit(val, row);
        valEdit->setObjectName(QStringLiteral("gxInspRowValue"));
        valEdit->setFrame(false);
        const int colIdx = static_cast<int>(fi);
        connect(valEdit, &QLineEdit::editingFinished, this,
                [this, valEdit, colIdx, val] {
                    const auto newVal = valEdit->text();
                    if (newVal != val) {
                        emit fieldEdited(colIdx, newVal);
                    }
                });
        col2->addWidget(valEdit);

        rh->addLayout(col2, 1);

        fieldsLayout_->insertWidget(fieldsLayout_->count() - 1, row);
    }
}

}
