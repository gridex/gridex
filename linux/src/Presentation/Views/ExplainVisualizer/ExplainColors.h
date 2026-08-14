#pragma once
// Theme-aware color palette + glyph mapping for the Explain Visualizer.
// The strategy:
//   • heat accents (red/amber/green) stay saturated so they read on any
//     theme — they're the focal point of the visualization
//   • backgrounds for tinted regions use the accent color at ~18% alpha
//     so they blend over whatever palette base is set by the active qss
//   • text on tinted backgrounds uses the saturated accent — high
//     contrast on both light pastels and dark translucent fills
//   • neutral surfaces (cards, borders, muted text) use the gx skin
//     tokens — same hex values the style-gx[-light].qss files use — so
//     the visualizer blends into the rest of the IDE chrome

#include "Presentation/Views/ExplainVisualizer/ExplainPlanTypes.h"

#include <QColor>
#include <QGuiApplication>
#include <QSettings>
#include <QString>
#include <QStyleHints>

namespace gridex::explain {

// QSS doesn't update QPalette, so checking palette(Window) drifts from
// what the user actually sees. Mirror ThemeManager's source of truth:
// the `ui/theme` QSettings key (set by ThemeManager::setMode), falling
// back to the system color scheme when Auto.
inline bool isDarkPalette() {
    const QString saved = QSettings().value(QStringLiteral("ui/theme"),
                                            QStringLiteral("Auto")).toString();
    if (saved == QLatin1String("Light")) return false;
    if (saved == QLatin1String("Dark"))  return true;
    const Qt::ColorScheme s = QGuiApplication::styleHints()->colorScheme();
    return (s == Qt::ColorScheme::Dark) || (s == Qt::ColorScheme::Unknown);
}

// Saturated accent. Picked separately for light vs dark so the contrast
// against the page background stays readable.
inline QColor heatColor(::gridex::explainviz::NodeLevel l) {
    using L = ::gridex::explainviz::NodeLevel;
    if (isDarkPalette()) {
        switch (l) {
            case L::Bad:  return QColor(0xF3, 0x8B, 0xA8); // pink (Catppuccin Mocha red)
            case L::Warn: return QColor(0xF9, 0xE2, 0xAF); // peach
            case L::Good: return QColor(0xA6, 0xE3, 0xA1); // green
        }
        return QColor(0xBA, 0xC2, 0xDE);
    }
    switch (l) {
        case L::Bad:  return QColor(0xC4, 0x2B, 0x1C);
        case L::Warn: return QColor(0xCA, 0x5C, 0x00);
        case L::Good: return QColor(0x10, 0x7C, 0x10);
    }
    return QColor(0x70, 0x70, 0x70);
}

// Translucent fill — sits on top of whatever the qss sets as the panel
// background. Returns an rgba() css string so widgets can drop it
// straight into setStyleSheet without computing rgba themselves.
inline QString heatBgRgba(::gridex::explainviz::NodeLevel l, int alpha = 46) {
    QColor c = heatColor(l);
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(c.red()).arg(c.green()).arg(c.blue()).arg(alpha);
}

// Muted text — secondary label color from the gx skin (--gx-text-2 dark,
// equivalent low-contrast gray on light).
inline QString mutedTextCss() {
    return isDarkPalette()
        ? QStringLiteral("color: #7d8185;")     // gx --gx-text-3
        : QStringLiteral("color: #5c6066;");
}

// Card-style background — matches QFrame[gxRole="card"] in the gx qss
// (--gx-bg-2 dark / light) so the visualizer surfaces blend with the
// rest of the IDE chrome.
inline QString cardBgCss() {
    return isDarkPalette()
        ? QStringLiteral("background: #171c22;")  // gx --gx-bg-2 dark
        : QStringLiteral("background: #e9ebef;"); // gx --gx-bg-2 light
}

inline QString cardBorderCss() {
    return isDarkPalette()
        ? QStringLiteral("border: 1px solid #2e3339;")  // gx border-1 dark
        : QStringLiteral("border: 1px solid #c5cad1;"); // gx border-1 light
}

// Page background — what QWidget {} resolves to in the gx qss. Use as
// the track color for progress bars / inline meters so the chunk pops
// against a surface one step darker than the card.
inline QString trackBgHex() {
    return isDarkPalette()
        ? QStringLiteral("#11151a")   // gx --gx-bg-1 dark
        : QStringLiteral("#f4f5f7");  // gx --gx-bg-1 light
}

// One-char Unicode glyph per node kind. Windows uses Segoe MDL2 private-
// use area; on Linux we pick widely-available Unicode symbols instead so
// the visualizer renders cleanly with the system default font.
inline QString kindGlyph(::gridex::explainviz::NodeKind k) {
    using K = ::gridex::explainviz::NodeKind;
    switch (k) {
        case K::Limit:           return QStringLiteral("↤");
        case K::Sort:            return QStringLiteral("⇵");
        case K::HashJoin:        return QStringLiteral("⋈");
        case K::MergeJoin:       return QStringLiteral("⋈");
        case K::NestedLoop:      return QStringLiteral("↻");
        case K::SeqScan:         return QStringLiteral("☰");
        case K::IndexScan:       return QStringLiteral("⧉");
        case K::IndexOnlyScan:   return QStringLiteral("⧉");
        case K::BitmapHeapScan:  return QStringLiteral("▦");
        case K::BitmapIndexScan: return QStringLiteral("▦");
        case K::Hash:            return QStringLiteral("⌗");
        case K::Aggregate:       return QStringLiteral("∑");
        case K::GroupAggregate:  return QStringLiteral("∑");
        case K::HashAggregate:   return QStringLiteral("∑");
        case K::WindowAgg:       return QStringLiteral("▣");
        case K::Materialize:     return QStringLiteral("□");
        case K::Gather:          return QStringLiteral("▼");
        case K::GatherMerge:     return QStringLiteral("▼");
        case K::Append:          return QStringLiteral("⊕");
        case K::Result:          return QStringLiteral("↳");
        case K::SubqueryScan:    return QStringLiteral("⁐");
        case K::CteScan:         return QStringLiteral("⦵");
        case K::Unique:          return QStringLiteral("♠");
        case K::Memoize:         return QStringLiteral("◉");
        case K::Other:           return QStringLiteral("•");
    }
    return QStringLiteral("•");
}

inline QString toQ(const std::wstring& w) {
    return QString::fromWCharArray(w.data(), static_cast<int>(w.size()));
}

}
