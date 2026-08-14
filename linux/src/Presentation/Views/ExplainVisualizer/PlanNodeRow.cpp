#include "Presentation/Views/ExplainVisualizer/PlanNodeRow.h"
#include "Presentation/Views/ExplainVisualizer/ExplainColors.h"
#include "Presentation/Views/ExplainVisualizer/ExplainFormat.h"

#include <QEnterEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>

namespace gridex::explain {

// ── HeatBar ────────────────────────────────────────────────────────────
HeatBar::HeatBar(::gridex::explainviz::NodeLevel level, double pctFill, QWidget* parent)
    : QFrame(parent), level_(level), pct_(qBound(0.0, pctFill, 100.0))
{
    setFixedHeight(7);
    setMinimumWidth(60);
    setAttribute(Qt::WA_TransparentForMouseEvents);
}

void HeatBar::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor track = isDarkPalette() ? QColor(255, 255, 255, 30)
                                          : QColor(0, 0, 0, 20);
    QPainterPath tp;
    tp.addRoundedRect(rect(), 2, 2);
    p.fillPath(tp, track);
    // Fill — minimum 2px so even ~0% nodes are visible.
    const double w = std::max(2.0, width() * pct_ / 100.0);
    QRectF fillRect(0, 0, w, height());
    QPainterPath fp;
    fp.addRoundedRect(fillRect, 2, 2);
    p.fillPath(fp, heatColor(level_));
}

// ── PlanNodeRow ────────────────────────────────────────────────────────
PlanNodeRow::PlanNodeRow(std::shared_ptr<::gridex::explainviz::PlanNode> node,
                          int depth,
                          QWidget* parent)
    : QFrame(parent), level_(node ? node->level : ::gridex::explainviz::NodeLevel::Good)
{
    nodeId_ = toQ(node ? node->id : std::wstring());
    setFrameShape(QFrame::NoFrame);
    setCursor(Qt::PointingHandCursor);
    applyStyle();

    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(8 + depth * 18, 6, 8, 6);
    outer->setSpacing(10);

    // Glyph badge — 34×28 rounded, heat tint background, heat accent text.
    auto* glyph = new QLabel(node ? kindGlyph(node->kind) : QStringLiteral("•"), this);
    glyph->setFixedSize(34, 28);
    glyph->setAlignment(Qt::AlignCenter);
    {
        auto gf = glyph->font();
        gf.setPointSize(gf.pointSize() + 2);
        gf.setBold(true);
        glyph->setFont(gf);
    }
    glyph->setStyleSheet(QStringLiteral(
        "background: %1; color: %2; border-radius: 5px;")
        .arg(heatBgRgba(level_, 64), heatColor(level_).name()));
    outer->addWidget(glyph, 0, Qt::AlignVCenter);

    // Body column: head row (op + relation + pill) + detail (elided) + cost line.
    auto* col = new QVBoxLayout();
    col->setSpacing(2);
    col->setContentsMargins(0, 0, 0, 0);

    auto* headRow = new QHBoxLayout();
    headRow->setSpacing(8);
    headRow->setContentsMargins(0, 0, 0, 0);
    auto* opLbl = new QLabel(node ? toQ(node->op) : QString(), this);
    {
        auto of = opLbl->font();
        of.setBold(true);
        of.setPointSize(of.pointSize() + 1);
        opLbl->setFont(of);
    }
    headRow->addWidget(opLbl);
    if (node && !node->relation.empty()) {
        QString rel = toQ(node->relation);
        if (!node->alias.empty() && node->alias != node->relation)
            rel += QStringLiteral(" ") + toQ(node->alias);
        auto* relLbl = new QLabel(rel, this);
        relLbl->setFont(monospaceFont(opLbl->font().pointSize() - 1));
        relLbl->setStyleSheet(mutedTextCss());
        headRow->addWidget(relLbl);
    }
    using L = ::gridex::explainviz::NodeLevel;
    if (node && (node->level == L::Bad || node->level == L::Warn)) {
        const bool bad = node->level == L::Bad;
        auto* pill = new QLabel(bad ? tr("hot path") : tr("watch"), this);
        pill->setAlignment(Qt::AlignCenter);
        pill->setContentsMargins(8, 1, 8, 1);
        auto pf = pill->font();
        pf.setPointSize(qMax(8, pf.pointSize() - 2));
        pf.setBold(true);
        pill->setFont(pf);
        pill->setStyleSheet(QStringLiteral(
            "background: %1; color: %2; border-radius: 8px;")
            .arg(heatBgRgba(node->level, 90), heatColor(node->level).name()));
        headRow->addWidget(pill);
    }
    headRow->addStretch(1);
    col->addLayout(headRow);

    if (node && !node->detail.empty()) {
        auto* det = new QLabel(this);
        det->setText(toQ(node->detail));
        det->setStyleSheet(mutedTextCss());
        auto df = det->font(); df.setPointSize(qMax(8, df.pointSize() - 1)); det->setFont(df);
        // Match Windows: NoWrap + CharacterEllipsis at width.
        det->setTextFormat(Qt::PlainText);
        det->setWordWrap(false);
        det->setMinimumWidth(100);
        det->setMaximumWidth(600);
        det->setTextInteractionFlags(Qt::NoTextInteraction);
        // Qt doesn't have ellipsis on QLabel out-of-the-box; clip via
        // size policy + style. Force shrink with sizeHint horizontal.
        det->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        col->addWidget(det);
    }

    // Cost line: monospace, gray.
    if (node) {
        QString cost;
        cost += QStringLiteral("cost %1…%2  ·  rows %3  ·  width %4")
                    .arg(node->startupCost, 0, 'f', 2)
                    .arg(node->totalCost, 0, 'f', 2)
                    .arg(fmtNum(node->actualRows))
                    .arg(node->planWidth);
        if (node->analyzed) {
            cost += QStringLiteral("  ·  actual %1 ms").arg(fmtMs(node->actualTotal));
            if (node->actualLoops > 1)
                cost += QStringLiteral(" × %1 loops").arg(node->actualLoops);
        }
        auto* costLbl = new QLabel(cost, this);
        costLbl->setFont(monospaceFont(opLbl->font().pointSize() - 2));
        costLbl->setStyleSheet(mutedTextCss());
        costLbl->setWordWrap(false);
        costLbl->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        col->addWidget(costLbl);
    }

    outer->addLayout(col, 1);

    // Right column: %time number (bold, heat color) + heat bar (140px).
    if (node && node->analyzed) {
        auto* heatCol = new QVBoxLayout();
        heatCol->setSpacing(4);
        heatCol->setContentsMargins(0, 0, 0, 0);

        auto* pct = new QLabel(QStringLiteral("%1%").arg(node->pctOfTotal, 0, 'f', 1), this);
        pct->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        {
            auto pf = pct->font();
            pf.setBold(true);
            pf.setPointSize(pf.pointSize() + 1);
            pct->setFont(pf);
        }
        pct->setStyleSheet(QStringLiteral("color: %1;").arg(heatColor(level_).name()));
        heatCol->addWidget(pct);

        auto* bar = new HeatBar(level_, node->pctOfTotal, this);
        bar->setFixedWidth(140);
        heatCol->addWidget(bar);

        auto* heatHost = new QWidget(this);
        heatHost->setFixedWidth(140);
        heatHost->setLayout(heatCol);
        outer->addWidget(heatHost, 0, Qt::AlignVCenter);
    }
}

void PlanNodeRow::applyStyle() {
    QString bg;
    if (selected_) {
        bg = heatBgRgba(level_, 40);
    } else if (hovered_) {
        bg = isDarkPalette() ? QStringLiteral("rgba(255,255,255,0.06)")
                              : QStringLiteral("rgba(0,0,0,0.04)");
    } else {
        bg = QStringLiteral("transparent");
    }
    QString border = selected_
        ? QStringLiteral("border: 1px solid %1;").arg(heatColor(level_).name())
        : QStringLiteral("border: 1px solid transparent;");
    setStyleSheet(QStringLiteral(
        "PlanNodeRow { background: %1; %2 border-radius: 6px; }")
        .arg(bg, border));
}

void PlanNodeRow::setSelected(bool on) {
    if (selected_ == on) return;
    selected_ = on;
    applyStyle();
}

void PlanNodeRow::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) emit clicked(nodeId_);
    QFrame::mousePressEvent(e);
}

void PlanNodeRow::enterEvent(QEnterEvent*) { hovered_ = true; applyStyle(); }
void PlanNodeRow::leaveEvent(QEvent*)        { hovered_ = false; applyStyle(); }

}
