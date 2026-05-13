#pragma once

#include <QObject>
#include <QString>

class QApplication;

namespace gridex {

class ThemeManager : public QObject {
    Q_OBJECT
public:
    enum class Mode { Light, Dark, Auto };

    static ThemeManager& instance();

    void apply(QApplication* app);
    void setMode(Mode mode, QApplication* app);
    Mode mode() const;

    // Effective dark-vs-light decision after Auto resolution. Use for
    // widgets that paint outside the QSS pipeline (e.g. QStyledItemDelegate
    // or paintEvent overrides) and need a colour at draw time.
    bool isDark() const;

signals:
    void themeChanged();

private:
    explicit ThemeManager(QObject* parent = nullptr);
    ThemeManager(const ThemeManager&) = delete;
    ThemeManager& operator=(const ThemeManager&) = delete;

    void applyQss(QApplication* app, const QString& path);
    void applyGxForMode(QApplication* app);
    void applyLegacyForSystem(QApplication* app);

    Mode mode_ = Mode::Auto;
    QApplication* app_ = nullptr;
};

}  // namespace gridex
