#pragma once

// Inline SVG icon factory matching the design's stroked 16×16 glyph set.
// Builds QIcon from a small SVG string at runtime — no asset files to ship,
// re-tinted automatically by the active QSS palette via currentColor.
//
// Mirrors panels.jsx's SvgIcon component one-to-one.

#include <QIcon>
#include <QPixmap>
#include <QString>

namespace gridex {

class GxIcons {
public:
    // Returns a QIcon rendered at exactly `sizePx` logical pixels with
    // `color`. When `color` is empty the icon uses the theme's text-2 token
    // (#b4b8bc on dark, #4a4f56 on light) so glyphs remain legible after a
    // light/dark switch.
    static QIcon glyph(const QString& name,
                       const QString& color = QString(),
                       int sizePx = 16);

    // Returns a pixmap of the requested pixel size (default 14). HiDPI-aware
    // — renderer doubles the backing-store resolution. Use this when you
    // need a QLabel or custom-paint code rather than an action icon.
    static QPixmap pixmap(const QString& name,
                          const QString& color = QString(),
                          int size = 14);
};

}  // namespace gridex
