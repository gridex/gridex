#include "Presentation/Views/Chrome/GxStatusBar.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QStyle>
#include <QWidget>

#include "Presentation/Theme/ThemeManager.h"

namespace gridex {

namespace {
constexpr int kDotSize = 7;
}

GxStatusBar::GxStatusBar(QWidget* parent) : QStatusBar(parent) {
    setObjectName(QStringLiteral("gxStatusBar"));
    setSizeGripEnabled(false);
    setFixedHeight(22);
    setContentsMargins(10, 0, 10, 0);
    buildSegments();
    clearAll();
}

QLabel* GxStatusBar::makeSegment(const QString& gxText) {
    auto* l = new QLabel(this);
    if (!gxText.isEmpty()) l->setProperty("gxText", gxText);
    l->setContentsMargins(0, 0, 0, 0);
    l->setStyleSheet(QStringLiteral("background: transparent;"));
    return l;
}

void GxStatusBar::buildSegments() {
    // ── Left cluster ──────────────────────────────────────────────
    // Green pulsing dot + connection name.
    connDot_ = new QLabel(this);
    connDot_->setFixedSize(kDotSize, kDotSize);
    // Initial tint set by setConnection() once it knows the connection
    // state; default to the theme-appropriate "offline" shade.
    const QString initDot = ThemeManager::instance().isDark()
        ? QStringLiteral("#55585c") : QStringLiteral("#9398a0");
    connDot_->setStyleSheet(QStringLiteral(
        "background: %1; border-radius: %2px;").arg(initDot).arg(kDotSize / 2));
    addWidget(connDot_);

    connLabel_ = makeSegment(QStringLiteral("success"));
    addWidget(connLabel_);

    txLabel_ = makeSegment();
    addWidget(txLabel_);

    rowsLabel_ = makeSegment();
    addWidget(rowsLabel_);

    timeLabel_ = makeSegment();
    addWidget(timeLabel_);

    // ── Right cluster ─────────────────────────────────────────────
    langLabel_   = makeSegment();
    encLabel_    = makeSegment();
    leLabel_     = makeSegment();
    indentLabel_ = makeSegment();
    cursorLabel_ = makeSegment();
    dirtyLabel_  = makeSegment();

    addPermanentWidget(langLabel_);
    addPermanentWidget(encLabel_);
    addPermanentWidget(leLabel_);
    addPermanentWidget(indentLabel_);
    addPermanentWidget(cursorLabel_);
    addPermanentWidget(dirtyLabel_);
}

void GxStatusBar::clearAll() {
    setConnection(tr("not connected"));
    setTxState(QStringLiteral("—"));
    setRowCount(0);
    setQueryTime(0);
    setLanguage(QStringLiteral("SQL · PostgreSQL"));
    setEncoding(QStringLiteral("UTF-8"));
    setLineEnding(QStringLiteral("LF"));
    setIndent(QStringLiteral("Spaces: 4"));
    setCursorPos(1, 1);
    setDirty(false);
}

void GxStatusBar::setConnection(const QString& text) {
    connLabel_->setText(text);
    // Dim the dot when not connected. Offline tint follows the theme's
    // faint token; success tint is the shared green from the QSS pill set.
    const bool offline = text.contains(tr("not connected"));
    const QString offlineColor = ThemeManager::instance().isDark()
        ? QStringLiteral("#55585c") : QStringLiteral("#9398a0");
    const QString onlineColor  = ThemeManager::instance().isDark()
        ? QStringLiteral("#6dd47e") : QStringLiteral("#2e7d32");
    connDot_->setStyleSheet(QStringLiteral(
        "background: %1; border-radius: %2px;")
        .arg(offline ? offlineColor : onlineColor)
        .arg(kDotSize / 2));
}

void GxStatusBar::setTxState(const QString& tx) {
    txLabel_->setText(QStringLiteral("tx: <b>%1</b>").arg(tx));
    txLabel_->setTextFormat(Qt::RichText);
}

void GxStatusBar::setRowCount(int rows) {
    rowsLabel_->setText(QStringLiteral("%1 rows")
                            .arg(QLocale::system().toString(rows)));
}

void GxStatusBar::setQueryTime(int milliseconds) {
    timeLabel_->setText(QStringLiteral("%1 ms").arg(milliseconds));
}

void GxStatusBar::setLanguage(const QString& language)   { langLabel_->setText(language); }
void GxStatusBar::setEncoding(const QString& enc)        { encLabel_->setText(enc); }
void GxStatusBar::setLineEnding(const QString& le)       { leLabel_->setText(le); }
void GxStatusBar::setIndent(const QString& s)            { indentLabel_->setText(s); }

void GxStatusBar::setCursorPos(int line, int col) {
    cursorLabel_->setText(tr("Ln %1, Col %2").arg(line).arg(col));
}

void GxStatusBar::setDirty(bool dirty) {
    dirtyLabel_->setText(dirty ? QStringLiteral("● Modified") : QStringLiteral("Saved"));
    dirtyLabel_->setProperty("gxText", dirty ? QStringLiteral("warning") : QString());
    dirtyLabel_->style()->unpolish(dirtyLabel_);
    dirtyLabel_->style()->polish(dirtyLabel_);
}

}  // namespace gridex
