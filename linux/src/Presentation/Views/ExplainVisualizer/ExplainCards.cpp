#include "Presentation/Views/ExplainVisualizer/ExplainCards.h"
#include "Presentation/Views/ExplainVisualizer/ExplainColors.h"
#include "Presentation/Views/ExplainVisualizer/ExplainFormat.h"

#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSizePolicy>
#include <QVBoxLayout>

namespace gridex::explain {

// ── MetricCard ────────────────────────────────────────────────────────
MetricCard::MetricCard(const QString& glyph,
                       const QString& label,
                       const QString& value,
                       const QString& unit,
                       const QString& delta,
                       double barPct,
                       ::gridex::explainviz::NodeLevel tone,
                       QWidget* parent)
    : QFrame(parent)
{
    setFrameShape(QFrame::StyledPanel);
    setLineWidth(1);
    setStyleSheet(QStringLiteral(
        "MetricCard { %1 %2 border-radius: 8px; }")
        .arg(cardBgCss(), cardBorderCss()));

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(12, 10, 12, 10);
    lay->setSpacing(4);

    auto* header = new QHBoxLayout();
    header->setSpacing(6);
    auto* glyphLbl = new QLabel(glyph, this);
    auto f = glyphLbl->font(); f.setPointSize(f.pointSize() + 1); glyphLbl->setFont(f);
    auto* lblLbl = new QLabel(label, this);
    lblLbl->setStyleSheet(mutedTextCss());
    header->addWidget(glyphLbl);
    header->addWidget(lblLbl, 1);
    lay->addLayout(header);

    auto* valueRow = new QHBoxLayout();
    valueRow->setSpacing(4);
    auto* valLbl = new QLabel(value, this);
    auto vf = valLbl->font(); vf.setPointSize(vf.pointSize() + 4); vf.setBold(true);
    valLbl->setFont(vf);
    valueRow->addWidget(valLbl);
    if (!unit.isEmpty()) {
        auto* unitLbl = new QLabel(unit, this);
        unitLbl->setStyleSheet(mutedTextCss());
        valueRow->addWidget(unitLbl);
    }
    valueRow->addStretch(1);
    if (!delta.isEmpty()) {
        auto* deltaLbl = new QLabel(delta, this);
        deltaLbl->setStyleSheet(QStringLiteral("color: %1;").arg(heatColor(tone).name()));
        valueRow->addWidget(deltaLbl);
    }
    lay->addLayout(valueRow);

    if (barPct > 0.0) {
        auto* bar = new QProgressBar(this);
        bar->setRange(0, 100);
        bar->setValue(qBound(0, int(barPct * 100), 100));
        bar->setTextVisible(false);
        bar->setFixedHeight(4);
        bar->setStyleSheet(QStringLiteral(
            "QProgressBar { background: %1; border: 0; border-radius: 2px; }"
            "QProgressBar::chunk { background: %2; border-radius: 2px; }")
            .arg(trackBgHex(), heatColor(tone).name()));
        lay->addWidget(bar);
    }
}

// ── InfoBarLite ───────────────────────────────────────────────────────
InfoBarLite::InfoBarLite(QWidget* parent) : QFrame(parent) {
    setFrameShape(QFrame::StyledPanel);
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(12, 8, 12, 8);
    lay->setSpacing(2);
    titleLbl_ = new QLabel(this);
    auto tf = titleLbl_->font(); tf.setBold(true); titleLbl_->setFont(tf);
    detailLbl_ = new QLabel(this);
    detailLbl_->setWordWrap(true);
    lay->addWidget(titleLbl_);
    lay->addWidget(detailLbl_);
    setMessage({}, {}, ::gridex::explainviz::IssueSeverity::Warn);
    hide();
}

void InfoBarLite::setMessage(const QString& title,
                              const QString& detail,
                              ::gridex::explainviz::IssueSeverity sev) {
    using S = ::gridex::explainviz::IssueSeverity;
    using L = ::gridex::explainviz::NodeLevel;
    L tone = (sev == S::Bad) ? L::Bad : (sev == S::Info ? L::Good : L::Warn);
    setStyleSheet(QStringLiteral(
        "InfoBarLite { background: %1; border: 1px solid %2; border-radius: 6px; color: %2; }")
        .arg(heatBgRgba(tone, 64), heatColor(tone).name()));
    if (titleLbl_) titleLbl_->setStyleSheet(QStringLiteral("color: %1;").arg(heatColor(tone).name()));
    if (detailLbl_) detailLbl_->setStyleSheet(QStringLiteral("color: %1;").arg(heatColor(tone).name()));
    titleLbl_->setText(title);
    detailLbl_->setText(detail);
    detailLbl_->setVisible(!detail.isEmpty());
    setVisible(!title.isEmpty());
}

void InfoBarLite::clear() { hide(); }

// ── RecCard ───────────────────────────────────────────────────────────
RecCard::RecCard(const ::gridex::explainviz::Recommendation& r, QWidget* parent)
    : QFrame(parent), sql_(toQ(r.previewSql))
{
    setFrameShape(QFrame::StyledPanel);
    using L = ::gridex::explainviz::NodeLevel;
    using S = ::gridex::explainviz::IssueSeverity;
    L tone = (r.severity == S::Bad) ? L::Bad : (r.severity == S::Info ? L::Good : L::Warn);
    setStyleSheet(QStringLiteral(
        "RecCard { %1 border-left: 4px solid %2; %3 border-radius: 6px; }")
        .arg(cardBgCss(), heatColor(tone).name(), cardBorderCss()));

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(14, 10, 14, 10);
    lay->setSpacing(4);

    auto* titleLbl = new QLabel(toQ(r.title), this);
    auto tf = titleLbl->font(); tf.setBold(true); titleLbl->setFont(tf);
    titleLbl->setWordWrap(true);
    auto* rationaleLbl = new QLabel(toQ(r.rationale), this);
    rationaleLbl->setWordWrap(true);
    rationaleLbl->setStyleSheet(mutedTextCss());
    lay->addWidget(titleLbl);
    lay->addWidget(rationaleLbl);

    if (!sql_.isEmpty()) {
        auto* btn = new QPushButton(tr("Preview SQL"), this);
        btn->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
        connect(btn, &QPushButton::clicked, this, [this]() { emit previewRequested(sql_); });
        lay->addWidget(btn, 0, Qt::AlignLeft);
    }
}

// ── WaterfallRow ──────────────────────────────────────────────────────
WaterfallRow::WaterfallRow(const QString& glyph,
                            const QString& label,
                            double startMs,
                            double durMs,
                            double maxMs,
                            ::gridex::explainviz::NodeLevel level,
                            QWidget* parent)
    : QFrame(parent)
{
    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(0, 3, 0, 3);
    lay->setSpacing(0);

    // 220px name column = glyph (24x20 tinted badge) + label (ellipsized).
    auto* name = new QWidget(this);
    name->setFixedWidth(220);
    auto* nameLay = new QHBoxLayout(name);
    nameLay->setContentsMargins(0, 0, 8, 0);
    nameLay->setSpacing(6);
    auto* glyphBox = new QLabel(glyph, name);
    glyphBox->setFixedSize(24, 20);
    glyphBox->setAlignment(Qt::AlignCenter);
    {
        auto gf = glyphBox->font(); gf.setBold(true);
        gf.setPointSize(qMax(8, gf.pointSize() - 1));
        glyphBox->setFont(gf);
    }
    glyphBox->setStyleSheet(QStringLiteral(
        "background: %1; color: %2; border-radius: 4px;")
        .arg(heatBgRgba(level, 64), heatColor(level).name()));
    auto* labelLbl = new QLabel(label, name);
    auto lf = labelLbl->font();
    lf.setPointSize(qMax(8, lf.pointSize() - 1));
    labelLbl->setFont(lf);
    labelLbl->setStyleSheet(mutedTextCss());
    labelLbl->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    nameLay->addWidget(glyphBox);
    nameLay->addWidget(labelLbl, 1);
    lay->addWidget(name);

    // Star bar track: 16px track with neutral bg + 10px colored bar
    // positioned by margins for start offset and width for duration.
    double total = maxMs > 0.0 ? maxMs : 1.0;
    double startFrac = qBound(0.0, startMs / total, 1.0);
    double durFrac   = qBound(0.001, durMs / total, 1.0);

    auto* track = new QWidget(this);
    track->setFixedHeight(16);
    track->setStyleSheet(QStringLiteral(
        "background: %1; border-radius: 3px;")
        .arg(isDarkPalette() ? QStringLiteral("rgba(255,255,255,0.06)")
                              : QStringLiteral("rgba(0,0,0,0.05)")));
    auto* trackLay = new QHBoxLayout(track);
    trackLay->setContentsMargins(0, 0, 0, 0);
    trackLay->setSpacing(0);
    // Use spacer/widget ratios — startFrac : durFrac : (1-startFrac-durFrac).
    int s = qMax(0, int(startFrac * 1000));
    int d = qMax(2, int(durFrac * 1000));
    int t = qMax(0, 1000 - s - d);
    if (s > 0) trackLay->addStretch(s);
    auto* fill = new QFrame(track);
    fill->setMinimumHeight(10);
    fill->setMaximumHeight(10);
    fill->setStyleSheet(QStringLiteral(
        "background: %1; border-radius: 2px;")
        .arg(heatColor(level).name()));
    trackLay->addWidget(fill, d, Qt::AlignVCenter);
    if (t > 0) trackLay->addStretch(t);
    lay->addWidget(track, 1);

    // 70px duration column right-aligned.
    auto* msLbl = new QLabel(QStringLiteral("%1 ms").arg(fmtMs(durMs)), this);
    msLbl->setFixedWidth(70);
    msLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    msLbl->setStyleSheet(mutedTextCss());
    auto mf = msLbl->font();
    mf.setPointSize(qMax(8, mf.pointSize() - 1));
    msLbl->setFont(mf);
    lay->addWidget(msLbl);
}

}
