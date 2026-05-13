#include "Presentation/Views/Sidebar/ConnectionRowWidget.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QString>
#include <QStyle>

#include "Presentation/Theme/ThemeManager.h"

namespace gridex {

namespace {

// Two-letter engine badge matching panels.jsx ENGINE_LABEL / ENGINE_COLOR.
struct EngineSpec { QString bg; QString fg; QString label; };
EngineSpec engineSpec(DatabaseType t) {
    switch (t) {
        case DatabaseType::PostgreSQL: return {"#5c8ec5", "#0a0d10", "PG"};
        case DatabaseType::MySQL:      return {"#e0a347", "#0a0d10", "MY"};
        case DatabaseType::SQLite:     return {"#6dbf6b", "#0a0d10", "SL"};
        case DatabaseType::Redis:      return {"#dc382d", "#fff",    "RD"};
        case DatabaseType::MongoDB:    return {"#4dab51", "#0a0d10", "MG"};
        case DatabaseType::MSSQL:      return {"#cc2927", "#fff",    "MS"};
        case DatabaseType::ClickHouse: return {"#f8c842", "#0a0d10", "CH"};
    }
    return {"#7d8185", "#fff", "??"};
}

QString tooltipFor(const ConnectionConfig& c) {
    QString host = QString::fromUtf8(c.displayHost().c_str());
    QString db   = c.database ? QString::fromUtf8(c.database->c_str()) : QString();
    QString type = QString::fromUtf8(std::string(displayName(c.databaseType)).c_str());
    // Tooltip subtle text follows the theme's muted token so the popup
    // stays readable in both light and dark.
    const QString mutedHex = ThemeManager::instance().isDark()
        ? QStringLiteral("#7d8185") : QStringLiteral("#6a6e74");
    QString lines = QString("<b>%1</b><br><span style='color:%2'>%3</span>")
                        .arg(QString::fromUtf8(c.name.c_str()), mutedHex, type);
    if (!host.isEmpty()) lines += QString("<br><tt>%1</tt>").arg(host);
    if (!db.isEmpty())   lines += QString("<br><tt>db: %1</tt>").arg(db);
    return lines;
}

}  // namespace

ConnectionRowWidget::ConnectionRowWidget(const ConnectionConfig& config, QWidget* parent)
    : QWidget(parent), connectionId_(QString::fromUtf8(config.id.c_str())) {
    buildUi(config);
    applyPalette();
}

void ConnectionRowWidget::buildUi(const ConnectionConfig& config) {
    setAutoFillBackground(true);
    setAttribute(Qt::WA_StyledBackground, true);
    setProperty("gxRole", QStringLiteral("conn-row"));
    setProperty("gxActive", false);
    setToolTip(tooltipFor(config));

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(4, 2, 8, 2);
    root->setSpacing(8);

    // 1) Thin colour bar (3×16) — env hint when colorTag is set.
    colorBar_ = new QLabel(this);
    colorBar_->setFixedSize(3, 16);
    if (config.colorTag) {
        const auto rgb = rgbColor(*config.colorTag);
        colorBar_->setStyleSheet(QString("background-color: rgb(%1,%2,%3); border-radius: 1px;")
                                     .arg(rgb.r).arg(rgb.g).arg(rgb.b));
    } else {
        colorBar_->setStyleSheet("background: transparent;");
    }
    root->addWidget(colorBar_);

    // 2) Engine badge (20×14 chip, two-letter code).
    const auto spec = engineSpec(config.databaseType);
    engineBadge_ = new QLabel(spec.label, this);
    engineBadge_->setFixedSize(20, 14);
    engineBadge_->setAlignment(Qt::AlignCenter);
    engineBadge_->setStyleSheet(QString(
        "background-color: %1; color: %2; "
        "font-family: 'JetBrains Mono', monospace; "
        "font-size: 9px; font-weight: 700; letter-spacing: 0.02em; "
        "border-radius: 2px;").arg(spec.bg, spec.fg));
    root->addWidget(engineBadge_);

    // 3) Name — flex, ellipsis-truncated.
    nameLabel_ = new QLabel(QString::fromUtf8(config.name.c_str()), this);
    nameLabel_->setObjectName(QStringLiteral("gxConnRowName"));
    nameLabel_->setTextFormat(Qt::PlainText);
    nameLabel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    nameLabel_->setMinimumWidth(50);
    root->addWidget(nameLabel_, 1);

    // 4) Connection-status dot (grey when offline; PR A2 hooks live status).
    statusDot_ = new QLabel(this);
    statusDot_->setObjectName(QStringLiteral("gxConnRowDot"));
    statusDot_->setFixedSize(7, 7);
    root->addWidget(statusDot_, 0, Qt::AlignVCenter);
}

void ConnectionRowWidget::setSelected(bool selected) {
    if (selected_ == selected) return;
    selected_ = selected;
    applyPalette();
}

void ConnectionRowWidget::applyPalette() {
    setProperty("gxActive", selected_);
    style()->unpolish(this);
    style()->polish(this);
}

}  // namespace gridex
