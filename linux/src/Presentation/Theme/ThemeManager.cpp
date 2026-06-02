#include "Presentation/Theme/ThemeManager.h"

#include <QApplication>
#include <QFile>
#include <QGuiApplication>
#include <QSettings>
#include <QStyleHints>

namespace gridex {

ThemeManager& ThemeManager::instance() {
    static ThemeManager inst;
    return inst;
}

ThemeManager::ThemeManager(QObject* parent) : QObject(parent) {
    QSettings s;
    const QString saved = s.value(QStringLiteral("ui/theme"), QStringLiteral("Auto")).toString();
    if (saved == QLatin1String("Light"))       mode_ = Mode::Light;
    else if (saved == QLatin1String("Dark"))   mode_ = Mode::Dark;
    else                                        mode_ = Mode::Auto;
}

void ThemeManager::apply(QApplication* app) {
    app_ = app;

    // The legacy Phase-2 palette is still reachable via `--legacy-theme`
    // for A/B comparison during the UI refactor.
    const bool useLegacy = app && app->arguments().contains(QStringLiteral("--legacy-theme"));
    if (useLegacy) {
        if (mode_ == Mode::Auto) {
            applyLegacyForSystem(app);
            connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
                    this, [this](Qt::ColorScheme) {
                        if (mode_ == Mode::Auto && app_) {
                            applyLegacyForSystem(app_);
                            emit themeChanged();
                        }
                    }, Qt::UniqueConnection);
        } else {
            applyQss(app, mode_ == Mode::Light
                         ? QStringLiteral(":/style-light.qss")
                         : QStringLiteral(":/style-dark.qss"));
        }
        return;
    }

    // gx skin — pick dark or light based on mode (Auto follows system).
    applyGxForMode(app);
    if (mode_ == Mode::Auto) {
        connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
                this, [this](Qt::ColorScheme) {
                    if (mode_ == Mode::Auto && app_) {
                        applyGxForMode(app_);
                        emit themeChanged();
                    }
                }, Qt::UniqueConnection);
    }
}

void ThemeManager::applyGxForMode(QApplication* app) {
    bool dark = true;
    if (mode_ == Mode::Light) dark = false;
    else if (mode_ == Mode::Dark) dark = true;
    else {
        const Qt::ColorScheme s = QGuiApplication::styleHints()->colorScheme();
        dark = (s == Qt::ColorScheme::Dark) || (s == Qt::ColorScheme::Unknown);
    }
    applyQss(app, dark ? QStringLiteral(":/style-gx.qss")
                       : QStringLiteral(":/style-gx-light.qss"));
}

void ThemeManager::setMode(Mode mode, QApplication* app) {
    app_ = app;
    mode_ = mode;

    const QString key = (mode == Mode::Light) ? QStringLiteral("Light")
                      : (mode == Mode::Dark)  ? QStringLiteral("Dark")
                                              : QStringLiteral("Auto");
    QSettings s;
    s.setValue(QStringLiteral("ui/theme"), key);

    apply(app);
    emit themeChanged();
}

ThemeManager::Mode ThemeManager::mode() const {
    return mode_;
}

bool ThemeManager::isDark() const {
    if (mode_ == Mode::Light) return false;
    if (mode_ == Mode::Dark)  return true;
    const Qt::ColorScheme s = QGuiApplication::styleHints()->colorScheme();
    return (s == Qt::ColorScheme::Dark) || (s == Qt::ColorScheme::Unknown);
}

void ThemeManager::applyQss(QApplication* app, const QString& path) {
    QFile f(path);
    if (f.open(QFile::ReadOnly | QFile::Text)) {
        app->setStyleSheet(QString::fromUtf8(f.readAll()));
        f.close();
    }
}

void ThemeManager::applyLegacyForSystem(QApplication* app) {
    const Qt::ColorScheme scheme = QGuiApplication::styleHints()->colorScheme();
    const bool dark = (scheme == Qt::ColorScheme::Dark)
                   || (scheme == Qt::ColorScheme::Unknown);
    applyQss(app, dark ? QStringLiteral(":/style-dark.qss")
                       : QStringLiteral(":/style-light.qss"));
}

}  // namespace gridex
