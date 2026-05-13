#include "Presentation/Views/Chrome/GxIcons.h"

#include <QHash>
#include <QPainter>
#include <QPixmap>
#include <QSvgRenderer>

#include "Presentation/Theme/ThemeManager.h"

namespace gridex {

namespace {
// Resolve the implicit "text-2" tint based on the active theme so glyphs
// stay legible after a light/dark switch. Caller-supplied colors win.
QString resolveDefaultColor() {
    return ThemeManager::instance().isDark()
        ? QStringLiteral("#b4b8bc")
        : QStringLiteral("#4a4f56");
}
}  // namespace

namespace {

// Inline SVG bodies — `{c}` is replaced with the requested colour.
// Source: linux/Gridex _standalone_.html chrome.jsx SvgIcon switch table.
const QHash<QString, QString> kGlyphs = {
    {"plug",
     "<g stroke='{c}' stroke-width='1.4' fill='none' stroke-linecap='round' stroke-linejoin='round'>"
     "<path d='M5 3v3M11 3v3'/><rect x='3.5' y='6' width='9' height='3.5' rx='.3'/>"
     "<path d='M8 9.5v2.5M6.5 14h3'/></g>"},
    {"sql-new",
     "<g stroke='{c}' stroke-width='1.4' fill='none' stroke-linecap='round' stroke-linejoin='round'>"
     "<path d='M4 2.5h5l3 3V13a.5.5 0 0 1-.5.5h-7A.5.5 0 0 1 4 13z'/>"
     "<path d='M9 2.5V5.5h3'/>"
     "<path d='M6.5 9.5h3M6.5 11.5h3'/></g>"},
    {"folder",
     "<g stroke='{c}' stroke-width='1.4' fill='none' stroke-linecap='round' stroke-linejoin='round'>"
     "<path d='M2.5 4.5h3l1 1.5h7v6.5a.5.5 0 0 1-.5.5h-10A.5.5 0 0 1 2.5 12.5z'/></g>"},
    {"save",
     "<g stroke='{c}' stroke-width='1.4' fill='none' stroke-linecap='round' stroke-linejoin='round'>"
     "<path d='M3 3h8l2 2v8a.5.5 0 0 1-.5.5h-9.5A.5.5 0 0 1 2.5 13V3.5z'/>"
     "<rect x='5' y='3' width='5' height='3'/><rect x='5' y='9' width='6' height='4'/></g>"},
    {"play",
     "<path d='M4.5 3v10l8.5-5z' fill='{c}'/>"},
    {"play-all",
     "<path d='M3 3v10l6-5zM9 3v10l5-5z' fill='{c}'/>"},
    {"stop",
     "<rect x='3.5' y='3.5' width='9' height='9' rx='.5' fill='{c}'/>"},
    {"commit",
     "<g stroke='{c}' stroke-width='1.4' fill='none' stroke-linecap='round' stroke-linejoin='round'>"
     "<circle cx='8' cy='8' r='2.5'/><path d='M2 8h3.5M10.5 8H14'/></g>"},
    {"rollback",
     "<g stroke='{c}' stroke-width='1.4' fill='none' stroke-linecap='round' stroke-linejoin='round'>"
     "<path d='M5 5a4 4 0 1 1-1.5 3.1'/><path d='M2.5 3v3.5h3.5'/></g>"},
    {"explain",
     "<g stroke='{c}' stroke-width='1.4' fill='none' stroke-linecap='round' stroke-linejoin='round'>"
     "<circle cx='6' cy='6' r='2.5'/><path d='M7.8 7.8L13 13'/></g>"},
    {"refresh",
     "<g stroke='{c}' stroke-width='1.4' fill='none' stroke-linecap='round' stroke-linejoin='round'>"
     "<path d='M3 8a5 5 0 0 1 8.5-3.5L13 6'/><path d='M13 3v3h-3'/>"
     "<path d='M13 8a5 5 0 0 1-8.5 3.5L3 10'/><path d='M3 13v-3h3'/></g>"},
    {"export",
     "<g stroke='{c}' stroke-width='1.4' fill='none' stroke-linecap='round' stroke-linejoin='round'>"
     "<path d='M8 11V3M5 6l3-3 3 3'/><path d='M3 11v2.5h10V11'/></g>"},
    {"erd",
     "<g stroke='{c}' stroke-width='1.4' fill='none' stroke-linecap='round' stroke-linejoin='round'>"
     "<rect x='2' y='3' width='4.5' height='3'/><rect x='9.5' y='3' width='4.5' height='3'/>"
     "<rect x='5.75' y='10' width='4.5' height='3'/>"
     "<path d='M4.25 6v2h3.5v2M11.75 6v2h-3.5v2'/></g>"},
    {"search",
     "<g stroke='{c}' stroke-width='1.4' fill='none' stroke-linecap='round' stroke-linejoin='round'>"
     "<circle cx='7' cy='7' r='3.5'/><path d='M9.5 9.5L13 13'/></g>"},
    {"db",
     "<g stroke='{c}' stroke-width='1.4' fill='none' stroke-linecap='round' stroke-linejoin='round'>"
     "<ellipse cx='8' cy='4' rx='5' ry='1.6'/>"
     "<path d='M3 4v8c0 .9 2.2 1.6 5 1.6s5-.7 5-1.6V4'/>"
     "<path d='M3 8c0 .9 2.2 1.6 5 1.6s5-.7 5-1.6'/></g>"},
    {"schema",
     "<g stroke='{c}' stroke-width='1.4' fill='none' stroke-linecap='round' stroke-linejoin='round'>"
     "<path d='M2.5 4h11v3h-11zM2.5 9.5h11v3h-11z'/>"
     "<circle cx='4.5' cy='5.5' r='.6' fill='{c}'/>"
     "<circle cx='4.5' cy='11' r='.6' fill='{c}'/></g>"},
    {"history",
     "<g stroke='{c}' stroke-width='1.4' fill='none' stroke-linecap='round' stroke-linejoin='round'>"
     "<circle cx='8' cy='8' r='5.5'/><path d='M8 5v3.2l2 1.3'/></g>"},
    {"snippets",
     "<g stroke='{c}' stroke-width='1.4' fill='none' stroke-linecap='round' stroke-linejoin='round'>"
     "<rect x='3' y='2.5' width='9' height='11'/>"
     "<path d='M5 5h5M5 7.5h5M5 10h3'/></g>"},
    {"cog",
     "<g stroke='{c}' stroke-width='1.4' fill='none' stroke-linecap='round' stroke-linejoin='round'>"
     "<circle cx='8' cy='8' r='2'/><circle cx='8' cy='8' r='5'/></g>"},
    {"table",
     "<g stroke='{c}' stroke-width='1.4' fill='none' stroke-linecap='round' stroke-linejoin='round'>"
     "<rect x='2.5' y='3' width='11' height='10'/>"
     "<path d='M2.5 6h11M2.5 9h11M6 3v10'/></g>"},
    {"view",
     "<g stroke='{c}' stroke-width='1.4' fill='none' stroke-linecap='round' stroke-linejoin='round'>"
     "<ellipse cx='8' cy='8' rx='5.5' ry='3'/><circle cx='8' cy='8' r='1.2'/></g>"},
    {"fn",
     "<g stroke='{c}' stroke-width='1.4' fill='none' stroke-linecap='round' stroke-linejoin='round'>"
     "<path d='M5 13c1 0 1.5-.6 1.7-2L8 4c.2-1.4.8-2 1.8-2'/>"
     "<path d='M4.5 7.5h4.5'/></g>"},
    {"col",
     "<g stroke='{c}' stroke-width='1.4' fill='none' stroke-linecap='round' stroke-linejoin='round'>"
     "<rect x='2.5' y='3' width='11' height='10'/><path d='M2.5 6h11'/></g>"},
    {"key",
     "<g stroke='{c}' stroke-width='1.4' fill='none' stroke-linecap='round' stroke-linejoin='round'>"
     "<circle cx='5.5' cy='8' r='2.5'/><path d='M8 8h5.5M11 8v2M13 8v2'/></g>"},
    {"idx",
     "<g stroke='{c}' stroke-width='1.4' fill='none' stroke-linecap='round' stroke-linejoin='round'>"
     "<path d='M3 4h10M3 8h7M3 12h10'/><circle cx='12' cy='8' r='1.2'/></g>"},
    {"fk",
     "<g stroke='{c}' stroke-width='1.4' fill='none' stroke-linecap='round' stroke-linejoin='round'>"
     "<circle cx='5' cy='8' r='1.8'/><circle cx='11' cy='8' r='1.8'/>"
     "<path d='M6.8 8h2.4'/></g>"},
    {"x",
     "<g stroke='{c}' stroke-width='1.4' fill='none' stroke-linecap='round' stroke-linejoin='round'>"
     "<path d='M4 4l8 8M12 4l-8 8'/></g>"},
    {"close",
     "<g stroke='{c}' stroke-width='1.4' fill='none' stroke-linecap='round' stroke-linejoin='round'>"
     "<path d='M4 4l8 8M12 4l-8 8'/></g>"},
    {"tri",
     "<path d='M5 4l5 4-5 4z' fill='{c}'/>"},
    {"tri-d",
     "<path d='M4 5l4 5 4-5z' fill='{c}'/>"},
    {"caret-r",
     "<path d='M6 4l4 4-4 4z' fill='{c}'/>"},
    {"caret-d",
     "<path d='M4 6l4 4 4-4z' fill='{c}'/>"},
};

QPixmap renderToPixmap(const QString& name, const QString& color, int size) {
    auto it = kGlyphs.find(name);
    if (it == kGlyphs.end()) return {};

    QString body = it.value();
    body.replace(QStringLiteral("{c}"), color);
    QString svg = QStringLiteral(
        "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 16 16' width='16' height='16'>"
        "%1</svg>").arg(body);

    QSvgRenderer renderer(svg.toUtf8());
    if (!renderer.isValid()) return {};

    // Super-sample at 4× the requested size, then downscale with smooth
    // transformation. This gives sharp small glyphs (the design's 1.2-1.4
    // strokes at viewBox 16 are sub-pixel at native size — direct render
    // produces aliased corners that look like "broken brackets" in the
    // toolbar).
    const int srcRes = qMax(16, size * 4);
    QPixmap hi(srcRes, srcRes);
    hi.fill(Qt::transparent);
    {
        QPainter painter(&hi);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        renderer.render(&painter);
    }

    const qreal dpr = 2.0;
    const int target = static_cast<int>(size * dpr);
    QPixmap pm = hi.scaled(target, target,
                            Qt::IgnoreAspectRatio,
                            Qt::SmoothTransformation);
    pm.setDevicePixelRatio(dpr);
    return pm;
}

}  // namespace

QIcon GxIcons::glyph(const QString& name, const QString& color, int sizePx) {
    const QString c = color.isEmpty() ? resolveDefaultColor() : color;
    const QPixmap pm = renderToPixmap(name, c, sizePx);
    if (pm.isNull()) return {};
    return QIcon(pm);
}

QPixmap GxIcons::pixmap(const QString& name, const QString& color, int size) {
    const QString c = color.isEmpty() ? resolveDefaultColor() : color;
    return renderToPixmap(name, c, size);
}

}  // namespace gridex
