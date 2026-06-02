#include "Presentation/Views/FilterBar/FilterBarView.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>

#include "Core/Models/Database/RowValue.h"

namespace gridex {

namespace {

// Ordered operator labels and matching FilterOperator values.
struct OpEntry { const char* label; FilterOperator op; };
constexpr OpEntry kOps[] = {
    { "=",           FilterOperator::Equal          },
    { "!=",          FilterOperator::NotEqual        },
    { ">",           FilterOperator::GreaterThan     },
    { "<",           FilterOperator::LessThan        },
    { ">=",          FilterOperator::GreaterOrEqual  },
    { "<=",          FilterOperator::LessOrEqual     },
    { "LIKE",        FilterOperator::Like            },
    { "IS NULL",     FilterOperator::IsNull          },
    { "IS NOT NULL", FilterOperator::IsNotNull       },
};
constexpr int kOpsCount = static_cast<int>(sizeof(kOps) / sizeof(kOps[0]));

// Operators that don't need a value input.
bool needsValue(FilterOperator op) {
    return op != FilterOperator::IsNull && op != FilterOperator::IsNotNull;
}

}

// FilterBarView chrome lives in resources/style-gx{,-light}.qss under the
// "Filter bar" section.

FilterBarView::FilterBarView(QWidget* parent) : QWidget(parent) {
    buildUi();
}

void FilterBarView::buildUi() {
    setObjectName(QStringLiteral("FilterBarView"));
    setAttribute(Qt::WA_StyledBackground, true);
    setProperty("compact", true);  // shrinks descendant buttons/inputs
    auto* h = new QHBoxLayout(this);
    h->setContentsMargins(10, 4, 10, 4);
    h->setSpacing(6);

    columnCombo_ = new QComboBox(this);
    columnCombo_->setObjectName(QStringLiteral("FilterColumn"));
    columnCombo_->setMinimumWidth(130);
    columnCombo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    columnCombo_->addItem(tr("Any column"));
    h->addWidget(columnCombo_);

    operatorCombo_ = new QComboBox(this);
    operatorCombo_->setObjectName(QStringLiteral("FilterOperator"));
    operatorCombo_->setFixedWidth(110);
    for (int i = 0; i < kOpsCount; ++i) {
        operatorCombo_->addItem(QString::fromUtf8(kOps[i].label));
    }
    connect(operatorCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &FilterBarView::onOperatorChanged);
    h->addWidget(operatorCombo_);

    valueEdit_ = new QLineEdit(this);
    valueEdit_->setObjectName(QStringLiteral("FilterValue"));
    valueEdit_->setPlaceholderText(tr("value"));
    valueEdit_->setMinimumWidth(160);
    connect(valueEdit_, &QLineEdit::returnPressed, this, &FilterBarView::onApply);
    h->addWidget(valueEdit_, 1);

    applyBtn_ = new QPushButton(tr("Apply"), this);
    applyBtn_->setObjectName(QStringLiteral("FilterApply"));
    applyBtn_->setCursor(Qt::PointingHandCursor);
    connect(applyBtn_, &QPushButton::clicked, this, &FilterBarView::onApply);
    h->addWidget(applyBtn_);

    clearBtn_ = new QPushButton(tr("Clear"), this);
    clearBtn_->setObjectName(QStringLiteral("FilterClear"));
    clearBtn_->setCursor(Qt::PointingHandCursor);
    connect(clearBtn_, &QPushButton::clicked, this, &FilterBarView::onClear);
    h->addWidget(clearBtn_);
}

void FilterBarView::setColumns(const std::vector<std::string>& columns) {
    const QString current = columnCombo_->currentText();
    columnCombo_->clear();
    columnCombo_->addItem(tr("Any column"));
    for (const auto& c : columns) {
        columnCombo_->addItem(QString::fromUtf8(c.c_str()));
    }
    // Restore previous selection if still present.
    const int idx = columnCombo_->findText(current);
    if (idx >= 0) columnCombo_->setCurrentIndex(idx);
}

void FilterBarView::reset() {
    columnCombo_->setCurrentIndex(0);
    operatorCombo_->setCurrentIndex(0);
    valueEdit_->clear();
    valueEdit_->setEnabled(true);
}

void FilterBarView::onOperatorChanged(int index) {
    if (index < 0 || index >= kOpsCount) return;
    const bool needs = needsValue(kOps[index].op);
    valueEdit_->setEnabled(needs);
    if (!needs) valueEdit_->clear();
}

void FilterBarView::onApply() {
    const int opIdx = operatorCombo_->currentIndex();
    if (opIdx < 0 || opIdx >= kOpsCount) return;

    const FilterOperator op = kOps[opIdx].op;
    const QString colText = columnCombo_->currentIndex() <= 0
                                ? QString{}
                                : columnCombo_->currentText();

    // Build expression: if no column chosen we emit filterCleared instead.
    if (colText.isEmpty()) {
        emit filterCleared();
        return;
    }

    FilterCondition cond;
    cond.column = colText.toStdString();
    cond.op     = op;
    if (needsValue(op)) {
        cond.value = RowValue::makeString(valueEdit_->text().toStdString());
    }

    FilterExpression expr;
    expr.conditions.push_back(std::move(cond));
    emit filterApplied(expr);
}

void FilterBarView::onClear() {
    reset();
    emit filterCleared();
}

}
