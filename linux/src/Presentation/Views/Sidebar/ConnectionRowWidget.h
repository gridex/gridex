#pragma once

#include <QWidget>

#include "Core/Models/Database/ConnectionConfig.h"

class QLabel;

namespace gridex {

// Compact connection row sized for the 280px IDE sidebar. Mirrors the
// design's panels.jsx layout:
//   [3px env color bar] [engine badge 18×14] [name] [conn-status dot 6px]
//
// Host:port / database / DB-type details move into the row's tooltip
// instead of being rendered inline — keeps the row at the same 22-28px
// height as every other tree row in the sidebar.
class ConnectionRowWidget : public QWidget {
    Q_OBJECT

public:
    explicit ConnectionRowWidget(const ConnectionConfig& config, QWidget* parent = nullptr);

    void setSelected(bool selected);
    [[nodiscard]] QString connectionId() const { return connectionId_; }

private:
    void buildUi(const ConnectionConfig& config);
    void applyPalette();

    QString connectionId_;
    bool    selected_ = false;

    QLabel* colorBar_   = nullptr;
    QLabel* engineBadge_ = nullptr;
    QLabel* nameLabel_  = nullptr;
    QLabel* statusDot_  = nullptr;
};

}
