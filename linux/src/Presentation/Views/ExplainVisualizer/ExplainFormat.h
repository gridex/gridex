#pragma once
// Number / time / byte formatters used by the visualizer. Match the
// Windows control's helpers exactly so labels line up across platforms.

#include <QFontDatabase>
#include <QString>
#include <cmath>
#include <cstdint>

namespace gridex::explain {

// Time: 2 decimals up to 100ms, then 1 decimal, then integer.
inline QString fmtMs(double ms) {
    if (ms < 10.0)   return QString::number(ms, 'f', 3);
    if (ms < 100.0)  return QString::number(ms, 'f', 2);
    if (ms < 10000)  return QString::number(ms, 'f', 1);
    return QString::number(ms, 'f', 0);
}
// Row counts / generic numbers — thousands suffix.
inline QString fmtNum(double n) {
    double abs = std::abs(n);
    if (abs >= 1e9)  return QString::number(n / 1e9, 'f', 1) + QStringLiteral("B");
    if (abs >= 1e6)  return QString::number(n / 1e6, 'f', 1) + QStringLiteral("M");
    if (abs >= 1e3)  return QString::number(n / 1e3, 'f', 1) + QStringLiteral("k");
    return QString::number(static_cast<qint64>(n));
}

// Buffers: bytes → human readable.
inline QString fmtBytes(quint64 b) {
    constexpr quint64 KB = 1024;
    constexpr quint64 MB = KB * 1024;
    constexpr quint64 GB = MB * 1024;
    if (b >= GB) return QString::number(double(b) / GB, 'f', 2) + QStringLiteral(" GB");
    if (b >= MB) return QString::number(double(b) / MB, 'f', 1) + QStringLiteral(" MB");
    if (b >= KB) return QString::number(double(b) / KB, 'f', 1) + QStringLiteral(" KB");
    return QString::number(b) + QStringLiteral(" B");
}

inline QFont monospaceFont(int pointSize = 0) {
    QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    if (pointSize > 0) f.setPointSize(pointSize);
    return f;
}

}
