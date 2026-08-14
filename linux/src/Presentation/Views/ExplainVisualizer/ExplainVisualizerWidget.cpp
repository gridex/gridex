#include "Presentation/Views/ExplainVisualizer/ExplainVisualizerWidget.h"
#include "Presentation/Views/ExplainVisualizer/ExplainCards.h"
#include "Presentation/Views/ExplainVisualizer/ExplainColors.h"
#include "Presentation/Views/ExplainVisualizer/ExplainFormat.h"
#include "Presentation/Views/ExplainVisualizer/PlanNodeRow.h"

#include <QCheckBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>
#include <vector>

namespace gridex::explain {

namespace {
using namespace ::gridex::explainviz;

// Walk plan tree.
template <typename Fn>
void walkPlan(const std::shared_ptr<PlanNode>& n, Fn&& fn) {
    if (!n) return;
    fn(*n);
    for (const auto& c : n->children) walkPlan(c, fn);
}

bool hasHotDescendant(const std::shared_ptr<PlanNode>& n) {
    if (!n) return false;
    if (n->level != NodeLevel::Good) return true;
    for (const auto& c : n->children)
        if (hasHotDescendant(c)) return true;
    return false;
}

NodeLevel toneFromMs(double ms) {
    if (ms > 1000.0) return NodeLevel::Bad;
    if (ms > 250.0)  return NodeLevel::Warn;
    return NodeLevel::Good;
}

NodeLevel toneFromFactor(double f) {
    if (f >= 5.0) return NodeLevel::Bad;
    if (f >= 2.0) return NodeLevel::Warn;
    return NodeLevel::Good;
}

}  // namespace

ExplainVisualizerWidget::ExplainVisualizerWidget(QWidget* parent) : QWidget(parent) {
    buildUi();
    showEmpty();
}

// ── State scaffolding ────────────────────────────────────────────────
void ExplainVisualizerWidget::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    stack_ = new QStackedWidget(this);
    stack_->addWidget(buildEmptyState());     // 0
    stack_->addWidget(buildLoadingState());   // 1
    stack_->addWidget(buildErrorState());     // 2
    stack_->addWidget(buildLoadedState());    // 3
    root->addWidget(stack_);
}

QWidget* ExplainVisualizerWidget::buildEmptyState() {
    auto* w = new QWidget();
    auto* outer = new QVBoxLayout(w);
    outer->addStretch(1);
    auto* inner = new QVBoxLayout();
    inner->setSpacing(12);
    inner->setAlignment(Qt::AlignCenter);

    auto* glyph = new QLabel(QStringLiteral("📊"), w);
    auto gf = glyph->font(); gf.setPointSize(gf.pointSize() + 14); glyph->setFont(gf);
    glyph->setAlignment(Qt::AlignCenter);

    auto* title = new QLabel(tr("Visualize execution plan"), w);
    auto tf = title->font(); tf.setPointSize(tf.pointSize() + 4); tf.setBold(true); title->setFont(tf);
    title->setAlignment(Qt::AlignCenter);

    auto* body = new QLabel(
        tr("Run your query with EXPLAIN ANALYZE and we'll render the plan tree, hot paths, and tuning recommendations."), w);
    body->setWordWrap(true);
    body->setAlignment(Qt::AlignCenter);
    body->setMaximumWidth(520);
    body->setStyleSheet(mutedTextCss());

    auto* runBtn = new QPushButton(tr("▶  Run with Explain Analyze"), w);
    connect(runBtn, &QPushButton::clicked, this, &ExplainVisualizerWidget::runRequested);

    auto* hint = new QLabel(tr("Postgres only — MySQL coming soon."), w);
    hint->setAlignment(Qt::AlignCenter);
    hint->setStyleSheet(mutedTextCss());

    inner->addWidget(glyph);
    inner->addWidget(title);
    inner->addWidget(body);
    inner->addWidget(runBtn, 0, Qt::AlignCenter);
    inner->addWidget(hint);
    outer->addLayout(inner);
    outer->addStretch(1);
    return w;
}

QWidget* ExplainVisualizerWidget::buildLoadingState() {
    auto* w = new QWidget();
    auto* lay = new QVBoxLayout(w);
    lay->setAlignment(Qt::AlignCenter);
    loadingLbl_ = new QLabel(tr("Running EXPLAIN ANALYZE…"), w);
    auto f = loadingLbl_->font(); f.setPointSize(f.pointSize() + 1); loadingLbl_->setFont(f);
    loadingLbl_->setStyleSheet(mutedTextCss());
    loadingLbl_->setAlignment(Qt::AlignCenter);
    lay->addWidget(loadingLbl_);
    return w;
}

QWidget* ExplainVisualizerWidget::buildErrorState() {
    auto* w = new QWidget();
    auto* outer = new QVBoxLayout(w);
    outer->addStretch(1);
    auto* inner = new QVBoxLayout();
    inner->setAlignment(Qt::AlignCenter);
    inner->setSpacing(10);

    auto* glyph = new QLabel(QStringLiteral("⚠"), w);
    auto gf = glyph->font(); gf.setPointSize(gf.pointSize() + 10); glyph->setFont(gf);
    glyph->setAlignment(Qt::AlignCenter);
    glyph->setStyleSheet(QStringLiteral("color: %1;")
        .arg(heatColor(::gridex::explainviz::NodeLevel::Bad).name()));

    auto* title = new QLabel(tr("Couldn't run EXPLAIN ANALYZE"), w);
    auto tf = title->font(); tf.setBold(true); tf.setPointSize(tf.pointSize() + 2); title->setFont(tf);
    title->setAlignment(Qt::AlignCenter);

    errorLbl_ = new QLabel(w);
    errorLbl_->setWordWrap(true);
    errorLbl_->setAlignment(Qt::AlignCenter);
    errorLbl_->setMaximumWidth(520);
    errorLbl_->setStyleSheet(mutedTextCss());

    auto* tryBtn = new QPushButton(tr("Try again"), w);
    connect(tryBtn, &QPushButton::clicked, this, &ExplainVisualizerWidget::runRequested);

    inner->addWidget(glyph);
    inner->addWidget(title);
    inner->addWidget(errorLbl_);
    inner->addWidget(tryBtn, 0, Qt::AlignCenter);
    outer->addLayout(inner);
    outer->addStretch(1);
    return w;
}

QWidget* ExplainVisualizerWidget::buildLoadedState() {
    auto* scroll = new QScrollArea();
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* body = new QWidget();
    auto* lay = new QVBoxLayout(body);
    lay->setContentsMargins(20, 16, 20, 28);
    lay->setSpacing(18);

    // Toolbar.
    {
        auto* row = new QHBoxLayout();
        row->setSpacing(8);
        auto* rerunBtn = new QPushButton(tr("⟳  Re-run"), body);
        connect(rerunBtn, &QPushButton::clicked, this, &ExplainVisualizerWidget::runRequested);
        auto* copyBtn = new QPushButton(tr("⧉  Copy plan"), body);
        connect(copyBtn, &QPushButton::clicked, this, [this]() {
            if (doc_) emit copyPlanRequested(toQ(doc_->rawJson));
        });
        metaLbl_ = new QLabel(body);
        metaLbl_->setStyleSheet(mutedTextCss());
        row->addWidget(rerunBtn);
        row->addWidget(copyBtn);
        row->addSpacing(8);
        row->addWidget(metaLbl_, 1);
        lay->addLayout(row);
    }

    // InfoBar.
    infoBar_ = new InfoBarLite(body);
    lay->addWidget(infoBar_);

    // Metrics row.
    metricsCard_ = new QFrame(body);
    metricsLay_ = new QVBoxLayout(metricsCard_);
    metricsLay_->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(metricsCard_);

    // Plan section header.
    {
        auto* row = new QHBoxLayout();
        auto* title = new QLabel(tr("Execution plan"), body);
        auto tf = title->font(); tf.setBold(true); tf.setPointSize(tf.pointSize() + 2); title->setFont(tf);
        planCountLbl_ = new QLabel(body);
        planCountLbl_->setStyleSheet(mutedTextCss());
        hotOnlyChk_ = new QCheckBox(tr("Show only hot"), body);
        connect(hotOnlyChk_, &QCheckBox::toggled, this, [this](bool) { renderPlanTree(); });
        row->addWidget(title);
        row->addSpacing(12);
        row->addWidget(planCountLbl_);
        row->addStretch(1);
        row->addWidget(hotOnlyChk_);
        lay->addLayout(row);
    }

    // Plan tree + flyout (two cols).
    {
        auto* row = new QHBoxLayout();
        row->setSpacing(12);
        treeCard_ = new QFrame(body);
        treeCard_->setFrameShape(QFrame::StyledPanel);
        treeCard_->setStyleSheet(QStringLiteral(
            "QFrame { %1 %2 border-radius: 8px; }")
            .arg(cardBgCss(), cardBorderCss()));
        treeLay_ = new QVBoxLayout(treeCard_);
        treeLay_->setContentsMargins(6, 8, 6, 8);
        treeLay_->setSpacing(2);

        flyoutCard_ = new QFrame(body);
        flyoutCard_->setFrameShape(QFrame::StyledPanel);
        flyoutCard_->setFixedWidth(340);
        flyoutCard_->setStyleSheet(QStringLiteral(
            "QFrame { %1 %2 border-radius: 8px; }")
            .arg(cardBgCss(), cardBorderCss()));
        flyoutLay_ = new QVBoxLayout(flyoutCard_);
        flyoutLay_->setContentsMargins(14, 12, 14, 12);
        flyoutLay_->setSpacing(8);

        row->addWidget(treeCard_, 1);
        row->addWidget(flyoutCard_);
        lay->addLayout(row);
    }

    // Waterfall.
    {
        auto* title = new QLabel(tr("Timing waterfall"), body);
        auto tf = title->font(); tf.setBold(true); tf.setPointSize(tf.pointSize() + 2); title->setFont(tf);
        auto* hint = new QLabel(tr("Actual time per node, in execution order."), body);
        hint->setStyleSheet(mutedTextCss());
        auto* card = new QFrame(body);
        card->setFrameShape(QFrame::StyledPanel);
        card->setStyleSheet(QStringLiteral(
            "QFrame { %1 %2 border-radius: 8px; }")
            .arg(cardBgCss(), cardBorderCss()));
        waterfallLay_ = new QVBoxLayout(card);
        waterfallLay_->setContentsMargins(12, 12, 12, 12);
        waterfallLay_->setSpacing(4);
        lay->addWidget(title);
        lay->addWidget(hint);
        lay->addWidget(card);
    }

    // Recommendations.
    {
        auto* title = new QLabel(tr("Recommendations"), body);
        auto tf = title->font(); tf.setBold(true); tf.setPointSize(tf.pointSize() + 2); title->setFont(tf);
        auto* hint = new QLabel(
            tr("Suggestions are derived from the plan above. Click Preview to copy the SQL."), body);
        hint->setStyleSheet(mutedTextCss());
        auto* host = new QWidget(body);
        recsLay_ = new QVBoxLayout(host);
        recsLay_->setContentsMargins(0, 0, 0, 0);
        recsLay_->setSpacing(8);
        lay->addWidget(title);
        lay->addWidget(hint);
        lay->addWidget(host);
    }

    scroll->setWidget(body);
    return scroll;
}

// ── Public API ────────────────────────────────────────────────────────
void ExplainVisualizerWidget::showEmpty() { stack_->setCurrentIndex(0); }

void ExplainVisualizerWidget::showLoading(const QString& text) {
    if (loadingLbl_ && !text.isEmpty()) loadingLbl_->setText(text);
    stack_->setCurrentIndex(1);
}

void ExplainVisualizerWidget::showError(const QString& message) {
    if (errorLbl_) errorLbl_->setText(message);
    stack_->setCurrentIndex(2);
}

void ExplainVisualizerWidget::setPlan(std::shared_ptr<::gridex::explainviz::PlanDocument> doc) {
    doc_ = std::move(doc);
    issues_.clear();
    recs_.clear();
    selectedNodeId_.clear();
    if (doc_) {
        issues_ = ::gridex::explainviz::ExplainPlanHeuristics::analyze(*doc_);
        recs_   = ::gridex::explainviz::ExplainPlanHeuristics::recommend(*doc_, issues_);
    }
    renderInfoBar();
    renderMetrics();
    renderPlanTree();
    renderFlyout();
    renderWaterfall();
    renderRecs();

    if (doc_ && metaLbl_) {
        metaLbl_->setText(
            tr("Postgres · execution %1 ms · planning %2 ms")
                .arg(fmtMs(doc_->executionMs))
                .arg(fmtMs(doc_->planningMs)));
    }
    stack_->setCurrentIndex(3);
}

// ── Render passes ────────────────────────────────────────────────────
void ExplainVisualizerWidget::clearLayout(QVBoxLayout* lay) {
    if (!lay) return;
    while (auto* item = lay->takeAt(0)) {
        if (auto* w = item->widget()) w->deleteLater();
        delete item;
    }
}

void ExplainVisualizerWidget::renderInfoBar() {
    if (!infoBar_) return;
    if (issues_.empty()) { infoBar_->clear(); return; }
    // Pick the most severe issue for the InfoBar.
    const auto* pick = &issues_.front();
    for (const auto& i : issues_)
        if (int(i.severity) > int(pick->severity)) pick = &i;
    infoBar_->setMessage(toQ(pick->title), toQ(pick->detail), pick->severity);
}

void ExplainVisualizerWidget::renderMetrics() {
    clearLayout(metricsLay_);
    if (!doc_ || !doc_->root) return;
    using namespace ::gridex::explainviz;

    const double total = doc_->executionMs > 0 ? doc_->executionMs : 0.0;
    const double planning = doc_->planningMs;

    // ── Total time ───────────────────────────────
    const NodeLevel timeTone = toneFromMs(total);
    QString timeDelta = planning > 0
        ? tr("planning %1 ms · execution %2 ms").arg(fmtMs(planning), fmtMs(total))
        : tr("execution %1 ms").arg(fmtMs(total));

    // ── Rows ─────────────────────────────────────
    double rowsActual = doc_->root->actualRows;
    double rowsScanned = 0.0;
    walkPlan(doc_->root, [&](const PlanNode& n) {
        if (n.kind == NodeKind::SeqScan || n.kind == NodeKind::IndexScan ||
            n.kind == NodeKind::IndexOnlyScan || n.kind == NodeKind::BitmapHeapScan)
            rowsScanned += n.actualRows + n.rowsRemovedByFilter;
    });
    QString rowsUnit = rowsScanned > 0
        ? tr(" / %1 scanned").arg(fmtNum(rowsScanned)) : QString();
    QString rowsDelta = rowsScanned > 0
        ? tr("selectivity %1%").arg(fmtMs(rowsActual / rowsScanned * 100)) : QString();
    double rowsBar = rowsScanned > 0
        ? std::min(1.0, rowsActual / rowsScanned) : 0.0;

    // ── Buffers ──────────────────────────────────
    quint64 totalHit = 0, totalRead = 0;
    walkPlan(doc_->root, [&](const PlanNode& n) {
        totalHit += n.sharedHit;
        totalRead += n.sharedRead;
    });
    quint64 bufBytes = (totalHit + totalRead) * 8ULL * 1024;  // 8KB pages
    QString bufDelta = tr("%1 hit · %2 read").arg(totalHit).arg(totalRead);
    NodeLevel bufTone = totalRead > 1000 ? NodeLevel::Warn : NodeLevel::Good;
    double bufBar = totalRead > 1000 ? 0.7 : 0.3;

    // ── Accuracy (worst-case est/actual ratio) ──
    double worst = 1.0;
    walkPlan(doc_->root, [&](const PlanNode& n) {
        if (n.planRows >= 100 && n.actualRows >= 100) {
            double f = std::max(n.planRows, n.actualRows) /
                       std::max(1.0, std::min(n.planRows, n.actualRows));
            if (f > worst) worst = f;
        }
    });
    NodeLevel accTone = toneFromFactor(worst);
    QString accDelta = tr("%1× worst-case error").arg(QString::number(worst, 'f', 1));
    double accBar = std::min(1.0, (worst - 1) / 5.0);

    // Build the 4 cards in a row.
    auto* host = new QWidget(metricsCard_);
    auto* row = new QHBoxLayout(host);
    row->setSpacing(8);
    row->setContentsMargins(0, 0, 0, 0);
    row->addWidget(new MetricCard(
        QStringLiteral("⏱"), tr("Total time"), fmtMs(total), QStringLiteral("ms"),
        timeDelta, std::min(1.0, total / 1000.0 * 0.5), timeTone));
    row->addWidget(new MetricCard(
        QStringLiteral("≡"), tr("Rows returned"), fmtNum(rowsActual), rowsUnit,
        rowsDelta, rowsBar, NodeLevel::Good));
    row->addWidget(new MetricCard(
        QStringLiteral("▢"), tr("Buffers"), fmtBytes(bufBytes), QString(),
        bufDelta, bufBar, bufTone));
    row->addWidget(new MetricCard(
        QStringLiteral("◎"), tr("Planner accuracy"),
        QString::number(worst, 'f', 1), QStringLiteral("×"),
        accDelta, accBar, accTone));
    metricsLay_->addWidget(host);
}

void ExplainVisualizerWidget::buildNodeRowRecursive(
    const std::shared_ptr<::gridex::explainviz::PlanNode>& node,
    int depth)
{
    if (!node) return;
    using L = ::gridex::explainviz::NodeLevel;
    bool hotOnly = hotOnlyChk_ && hotOnlyChk_->isChecked();
    // Keep nodes that are hot themselves OR have hot descendants (matches Win).
    bool render = !hotOnly || node->level != L::Good || hasHotDescendant(node);
    if (render) {
        auto* row = new PlanNodeRow(node, depth, treeCard_);
        connect(row, &PlanNodeRow::clicked, this, &ExplainVisualizerWidget::selectNode);
        treeLay_->addWidget(row);
        treeRows_.push_back(row);
    }
    for (const auto& c : node->children) buildNodeRowRecursive(c, depth + 1);
}

void ExplainVisualizerWidget::renderPlanTree() {
    clearLayout(treeLay_);
    treeRows_.clear();
    if (!doc_ || !doc_->root) return;

    int count = 0;
    walkPlan(doc_->root, [&](const ::gridex::explainviz::PlanNode&) { ++count; });
    if (planCountLbl_) {
        planCountLbl_->setText(tr("%1 nodes · planner cost %2")
                                .arg(count).arg(fmtMs(doc_->root->totalCost)));
    }

    buildNodeRowRecursive(doc_->root, 0);
    treeLay_->addStretch(1);

    if (!doc_->root->id.empty()) selectNode(toQ(doc_->root->id));
}

void ExplainVisualizerWidget::renderFlyout() {
    clearLayout(flyoutLay_);
    if (!doc_) return;
    using namespace ::gridex::explainviz;
    std::function<std::shared_ptr<PlanNode>(const std::shared_ptr<PlanNode>&)> find =
        [&](const std::shared_ptr<PlanNode>& n) -> std::shared_ptr<PlanNode> {
            if (!n) return nullptr;
            if (toQ(n->id) == selectedNodeId_) return n;
            for (const auto& c : n->children) if (auto m = find(c)) return m;
            return nullptr;
        };
    auto sel = find(doc_->root);
    if (!sel) sel = doc_->root;
    if (!sel) {
        auto* hint = new QLabel(tr("Click a node to inspect."), flyoutCard_);
        hint->setStyleSheet(mutedTextCss());
        hint->setWordWrap(true);
        flyoutLay_->addWidget(hint);
        return;
    }

    // Header: glyph badge (42×36) + title + detail.
    auto* head = new QHBoxLayout();
    head->setSpacing(10);
    auto* glyph = new QLabel(kindGlyph(sel->kind), flyoutCard_);
    glyph->setFixedSize(42, 36);
    glyph->setAlignment(Qt::AlignCenter);
    { auto gf = glyph->font(); gf.setBold(true); gf.setPointSize(gf.pointSize() + 4); glyph->setFont(gf); }
    glyph->setStyleSheet(QStringLiteral(
        "background: %1; color: %2; border-radius: 7px;")
        .arg(heatBgRgba(sel->level, 64), heatColor(sel->level).name()));
    auto* htext = new QVBoxLayout();
    htext->setSpacing(0);
    QString title = toQ(sel->op);
    if (!sel->relation.empty()) title += QStringLiteral(" · ") + toQ(sel->relation);
    auto* titleLbl = new QLabel(title, flyoutCard_);
    auto tf = titleLbl->font(); tf.setBold(true); tf.setPointSize(tf.pointSize() + 2); titleLbl->setFont(tf);
    htext->addWidget(titleLbl);
    if (!sel->detail.empty()) {
        auto* det = new QLabel(toQ(sel->detail), flyoutCard_);
        det->setStyleSheet(mutedTextCss());
        det->setWordWrap(true);
        htext->addWidget(det);
    }
    head->addWidget(glyph, 0, Qt::AlignTop);
    head->addLayout(htext, 1);
    auto* headHost = new QWidget(flyoutCard_);
    headHost->setLayout(head);
    flyoutLay_->addWidget(headHost);

    // Section helper.
    auto addSection = [&](const QString& title,
                          const std::vector<std::pair<QString, QString>>& kv) {
        auto* s = new QWidget(flyoutCard_);
        auto* sl = new QVBoxLayout(s);
        sl->setContentsMargins(0, 8, 0, 0);
        sl->setSpacing(2);
        auto* t = new QLabel(title.toUpper(), s);
        auto tff = t->font(); tff.setBold(true); tff.setPointSize(qMax(8, tff.pointSize() - 2)); t->setFont(tff);
        t->setStyleSheet(mutedTextCss() + QStringLiteral(" letter-spacing: 1px;"));
        sl->addWidget(t);
        for (const auto& [k, v] : kv) {
            auto* row = new QHBoxLayout();
            row->setContentsMargins(0, 2, 0, 2);
            row->setSpacing(8);
            auto* kl = new QLabel(k, s);
            kl->setStyleSheet(mutedTextCss());
            kl->setFixedWidth(120);
            auto* vl = new QLabel(v, s);
            vl->setWordWrap(true);
            row->addWidget(kl);
            row->addWidget(vl, 1);
            sl->addLayout(row);
        }
        flyoutLay_->addWidget(s);
    };

    // TIMING
    addSection(tr("Timing"), {
        { tr("Startup cost"), fmtMs(sel->startupCost) },
        { tr("Total cost"),   fmtMs(sel->totalCost) },
        { tr("Actual time"),  fmtMs(sel->actualTotal) + QStringLiteral(" ms") },
        { tr("Loops"),        QString::number(sel->actualLoops) },
        { tr("% of total"),   QStringLiteral("%1%").arg(sel->pctOfTotal, 0, 'f', 1) },
    });

    // ROWS
    QString estErr = QStringLiteral("—");
    if (sel->planRows >= 1 && sel->actualRows >= 1) {
        double f = std::max(sel->planRows, sel->actualRows) /
                   std::max(1.0, std::min(sel->planRows, sel->actualRows));
        estErr = QStringLiteral("%1× %2")
            .arg(QString::number(f, 'f', 1),
                 sel->actualRows > sel->planRows ? tr("under") : tr("over"));
    }
    addSection(tr("Rows"), {
        { tr("Estimated"), fmtNum(sel->planRows) },
        { tr("Actual"),    fmtNum(sel->actualRows) },
        { tr("Estimation error"), estErr },
        { tr("Rows removed"),     fmtNum(sel->rowsRemovedByFilter) },
        { tr("Width"),     QString::number(sel->planWidth) + QStringLiteral(" bytes") },
    });

    // BUFFERS — only if non-zero.
    if (sel->sharedHit > 0 || sel->sharedRead > 0 || sel->tempRead > 0 || sel->tempWritten > 0) {
        addSection(tr("Buffers"), {
            { tr("Shared hit"),  fmtNum(sel->sharedHit) + QStringLiteral(" blocks") },
            { tr("Shared read"), fmtNum(sel->sharedRead) + QStringLiteral(" blocks") },
            { tr("Shared dirtied"), fmtNum(sel->sharedDirtied) },
            { tr("Temp read"),   fmtNum(sel->tempRead) },
            { tr("Temp written"),fmtNum(sel->tempWritten) },
        });
    }

    // OUTPUT — chips.
    if (!sel->outputCols.empty()) {
        auto* s = new QWidget(flyoutCard_);
        auto* sl = new QVBoxLayout(s);
        sl->setContentsMargins(0, 8, 0, 0);
        sl->setSpacing(4);
        auto* t = new QLabel(tr("OUTPUT"), s);
        auto tff = t->font(); tff.setBold(true); tff.setPointSize(qMax(8, tff.pointSize() - 2)); t->setFont(tff);
        t->setStyleSheet(mutedTextCss() + QStringLiteral(" letter-spacing: 1px;"));
        sl->addWidget(t);
        // Chips in a wrapping row via QGridLayout.
        auto* chipHost = new QWidget(s);
        auto* fl = new QGridLayout(chipHost);
        fl->setContentsMargins(0, 0, 0, 0);
        fl->setHorizontalSpacing(4);
        fl->setVerticalSpacing(4);
        const size_t cap = std::min<size_t>(sel->outputCols.size(), 12);
        for (size_t i = 0; i < cap; ++i) {
            auto* chip = new QLabel(toQ(sel->outputCols[i]), chipHost);
            chip->setContentsMargins(6, 2, 6, 2);
            auto cf = chip->font(); cf.setPointSize(qMax(8, cf.pointSize() - 2)); chip->setFont(cf);
            chip->setStyleSheet(QStringLiteral(
                "background: %1; border-radius: 8px; color: %2;")
                .arg(isDarkPalette() ? QStringLiteral("rgba(255,255,255,0.08)")
                                      : QStringLiteral("rgba(0,0,0,0.05)"),
                     isDarkPalette() ? QStringLiteral("#cdd6f4") : QStringLiteral("#3c3c3c")));
            fl->addWidget(chip, int(i / 3), int(i % 3));
        }
        sl->addWidget(chipHost);
        flyoutLay_->addWidget(s);
    }

    flyoutLay_->addStretch(1);
}

void ExplainVisualizerWidget::renderWaterfall() {
    clearLayout(waterfallLay_);
    if (!doc_ || !doc_->root) return;
    using namespace ::gridex::explainviz;

    // Build steps in execution order: post-order DFS, children before parents.
    // Each step gets cumulative start = sum of preceding step durations.
    struct Step {
        QString label;
        QString glyph;
        double startMs;
        double durMs;
        NodeLevel level;
    };
    std::vector<Step> seq;
    double cursor = 0.0;
    std::function<double(const std::shared_ptr<PlanNode>&)> visit =
        [&](const std::shared_ptr<PlanNode>& n) -> double {
            if (!n) return 0.0;
            double childrenSum = 0.0;
            for (const auto& c : n->children) {
                visit(c);
                if (c) childrenSum += c->actualTotal * c->actualLoops;
            }
            double dur = std::max(0.0, n->actualTotal * n->actualLoops - childrenSum);
            QString label = toQ(n->op);
            if (!n->relation.empty()) label += QStringLiteral(" · ") + toQ(n->relation);
            seq.push_back({ label, kindGlyph(n->kind), cursor, dur, n->level });
            cursor += dur;
            return dur;
        };
    visit(doc_->root);

    const double maxMs = std::max(1.0, doc_->executionMs > 0 ? doc_->executionMs : cursor);

    // Axis row: 220px "Node" | star (5 tick labels) | 70px "Duration".
    {
        auto* row = new QWidget();
        auto* lay = new QHBoxLayout(row);
        lay->setContentsMargins(0, 0, 0, 4);
        lay->setSpacing(0);
        auto* nlbl = new QLabel(tr("Node"));
        nlbl->setFixedWidth(220);
        nlbl->setStyleSheet(mutedTextCss());
        lay->addWidget(nlbl);
        auto* tickHost = new QWidget();
        auto* tickLay = new QHBoxLayout(tickHost);
        tickLay->setContentsMargins(0, 0, 0, 0);
        tickLay->setSpacing(0);
        for (int i = 0; i <= 4; ++i) {
            auto* t = new QLabel(QStringLiteral("%1 ms").arg(fmtMs(maxMs * i / 4)));
            t->setStyleSheet(mutedTextCss());
            if (i == 0)      t->setAlignment(Qt::AlignLeft);
            else if (i == 4) t->setAlignment(Qt::AlignRight);
            else             t->setAlignment(Qt::AlignCenter);
            tickLay->addWidget(t, 1);
        }
        lay->addWidget(tickHost, 1);
        auto* dlbl = new QLabel(tr("Duration"));
        dlbl->setFixedWidth(70);
        dlbl->setAlignment(Qt::AlignRight);
        dlbl->setStyleSheet(mutedTextCss());
        lay->addWidget(dlbl);
        waterfallLay_->addWidget(row);
    }

    for (const auto& s : seq) {
        waterfallLay_->addWidget(new WaterfallRow(
            s.glyph, s.label, s.startMs, s.durMs, maxMs, s.level));
    }
}

void ExplainVisualizerWidget::renderRecs() {
    clearLayout(recsLay_);
    if (recs_.empty()) {
        auto* hint = new QLabel(tr("No tuning suggestions — the planner looks healthy."));
        hint->setStyleSheet(mutedTextCss());
        recsLay_->addWidget(hint);
        return;
    }
    for (const auto& r : recs_) {
        auto* card = new RecCard(r);
        connect(card, &RecCard::previewRequested, this, &ExplainVisualizerWidget::previewSqlRequested);
        recsLay_->addWidget(card);
    }
}

void ExplainVisualizerWidget::selectNode(const QString& id) {
    selectedNodeId_ = id;
    for (auto* r : treeRows_) r->setSelected(r->nodeId() == id);
    renderFlyout();
}

}
