#include "Presentation/Views/Home/HomeBrandingPanel.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QVBoxLayout>

#include "Presentation/Views/Chrome/GxIcons.h"

namespace gridex {

namespace {

// Bottom-panel action row: 14px SVG icon + label, left-aligned, flat with
// hover bg from the gx palette. Replaces emoji-glyph buttons that rendered
// as missing-char boxes on dark theme.
QPushButton* makeActionButton(const QString& iconName, const QString& title, QWidget* parent) {
    auto* btn = new QPushButton(parent);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setFlat(true);
    btn->setMinimumHeight(34);
    btn->setIcon(GxIcons::glyph(iconName));
    btn->setIconSize({14, 14});
    btn->setText(title);
    btn->setProperty("gxRole", QStringLiteral("home-action"));
    return btn;
}

}

HomeBrandingPanel::HomeBrandingPanel(QWidget* parent) : QWidget(parent) {
    buildUi();
}

void HomeBrandingPanel::buildUi() {
    setFixedWidth(260);
    setAutoFillBackground(true);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 28);
    root->setSpacing(0);

    root->addStretch();

    // Logo + title block (centered)
    auto* titleStack = new QVBoxLayout();
    titleStack->setAlignment(Qt::AlignHCenter);
    titleStack->setSpacing(16);

    auto* logo = new QLabel(this);
    QPixmap src(QStringLiteral(":/logo.png"));
    if (src.isNull()) {
        logo->setText(QStringLiteral("G"));
        logo->setObjectName(QStringLiteral("gxHomeLogoFallback"));
        logo->setAlignment(Qt::AlignCenter);
        logo->setFixedSize(100, 100);
    } else {
        logo->setFixedSize(100, 100);
        logo->setPixmap(src.scaled(100, 100, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        logo->setAlignment(Qt::AlignCenter);
    }
    auto* logoHolder = new QHBoxLayout();
    logoHolder->addStretch();
    logoHolder->addWidget(logo);
    logoHolder->addStretch();
    titleStack->addLayout(logoHolder);

    auto* titleBlock = new QVBoxLayout();
    titleBlock->setSpacing(4);
    titleBlock->setAlignment(Qt::AlignHCenter);
    auto* title = new QLabel(QStringLiteral("Gridex"), this);
    title->setAlignment(Qt::AlignHCenter);
    titleBlock->addWidget(title);
    auto* sub = new QLabel(tr("AI-Native Database IDE"), this);
    sub->setAlignment(Qt::AlignHCenter);
    titleBlock->addWidget(sub);
    titleStack->addLayout(titleBlock);

    root->addLayout(titleStack);
    root->addStretch();

    // Bottom actions block
    auto* actions = new QVBoxLayout();
    actions->setContentsMargins(20, 0, 20, 0);
    actions->setSpacing(2);

    auto* backup  = makeActionButton(QStringLiteral("export"), tr("Backup database…"), this);
    auto* restore = makeActionButton(QStringLiteral("save"),   tr("Restore database…"), this);
    connect(backup,  &QPushButton::clicked, this, &HomeBrandingPanel::backupRequested);
    connect(restore, &QPushButton::clicked, this, &HomeBrandingPanel::restoreRequested);
    actions->addWidget(backup);
    actions->addWidget(restore);

    auto* div = new QFrame(this);
    div->setFrameShape(QFrame::HLine);
    actions->addWidget(div);

    auto* newConn  = makeActionButton(QStringLiteral("plug"),   tr("New Connection"), this);
    auto* newGroup = makeActionButton(QStringLiteral("folder"), tr("New Group"), this);
    connect(newConn,  &QPushButton::clicked, this, &HomeBrandingPanel::newConnectionRequested);
    connect(newGroup, &QPushButton::clicked, this, &HomeBrandingPanel::newGroupRequested);
    actions->addWidget(newConn);
    actions->addWidget(newGroup);

    root->addLayout(actions);
}

}
