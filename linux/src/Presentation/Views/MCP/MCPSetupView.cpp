#include "Presentation/Views/MCP/MCPSetupView.h"

#include <QApplication>
#include <QComboBox>
#include <QFrame>
#include <QScrollArea>
#include <QClipboard>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <nlohmann/json.hpp>

namespace gridex {

namespace {

struct ClientSpec {
    const char* id;
    const char* displayName;
    const char* pathTemplate;   // ~/ is expanded
};

// Linux-only clients. Claude Desktop has no Linux build, so it's omitted.
static const ClientSpec kClients[] = {
    {"claude_code", "Claude Code",  "~/.claude.json"},
    {"cursor",      "Cursor",       "~/.cursor/mcp.json"},
    {"windsurf",    "Windsurf",     "~/.codeium/windsurf/mcp_config.json"},
    {"gemini_cli",  "Gemini CLI",   "~/.gemini/settings.json"},
};

QString expand(const QString& p) {
    if (p.startsWith("~")) return QDir::homePath() + p.mid(1);
    return p;
}

}  // namespace

MCPSetupView::MCPSetupView(QWidget* parent) : QWidget(parent) {
    buildUi();
    onClientChanged();
}

void MCPSetupView::buildUi() {
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("mcpScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    rootLayout->addWidget(scroll);

    auto* host = new QWidget(scroll);
    auto* hostH = new QHBoxLayout(host);
    hostH->setContentsMargins(24, 20, 24, 24);
    hostH->setSpacing(0);
    hostH->addStretch();
    auto* content = new QWidget(host);
    content->setMaximumWidth(900);
    auto* outer = new QVBoxLayout(content);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(16);

    // Info banner
    auto* banner = new QFrame(this);
    banner->setObjectName(QStringLiteral("mcpBanner"));
    auto* bannerH = new QHBoxLayout(banner);
    bannerH->setContentsMargins(14, 12, 14, 12);
    bannerH->setSpacing(10);
    auto* bannerIcon = new QLabel(QStringLiteral("ℹ"), banner);
    bannerIcon->setStyleSheet("color: #378add; font-size: 16px; font-weight: bold;");
    auto* bannerTextBox = new QVBoxLayout();
    bannerTextBox->setSpacing(2);
    auto* bannerTitle = new QLabel(tr("This is a config file, not a terminal command"), banner);
    QFont btf = bannerTitle->font(); btf.setBold(true); bannerTitle->setFont(btf);
    auto* bannerSub = new QLabel(
        tr("Use \"Install\" below to add Gridex to your client automatically, or copy and paste into "
           "the config file manually."), banner);
    bannerSub->setProperty("role", "muted");
    bannerSub->setWordWrap(true);
    bannerTextBox->addWidget(bannerTitle);
    bannerTextBox->addWidget(bannerSub);
    bannerH->addWidget(bannerIcon, 0, Qt::AlignTop);
    bannerH->addLayout(bannerTextBox, 1);
    outer->addWidget(banner);

    // Client
    auto* clientBox = new QGroupBox(tr("Client"), this);
    auto* form = new QFormLayout(clientBox);
    clientCombo_ = new QComboBox(clientBox);
    for (const auto& c : kClients) clientCombo_->addItem(c.displayName, c.id);
    connect(clientCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MCPSetupView::onClientChanged);
    form->addRow(tr("AI Client"), clientCombo_);

    auto* pathRow = new QHBoxLayout();
    configPathLabel_ = new QLabel(clientBox);
    configPathLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFont f = configPathLabel_->font(); f.setFamily("monospace"); configPathLabel_->setFont(f);
    configPathLabel_->setStyleSheet("color: palette(mid);");
    auto* openBtn = new QPushButton(QStringLiteral("⎋"), clientBox);
    openBtn->setToolTip(tr("Open in file manager"));
    openBtn->setFlat(true);
    connect(openBtn, &QPushButton::clicked, this, &MCPSetupView::onOpenPathClicked);
    pathRow->addWidget(configPathLabel_, 1);
    pathRow->addWidget(openBtn);
    form->addRow(tr("Config file"), pathRow);
    outer->addWidget(clientBox);

    // Quick install
    auto* installBox = new QGroupBox(tr("Quick Install"), this);
    auto* installV = new QVBoxLayout(installBox);
    auto* buttons = new QHBoxLayout();
    installBtn_ = new QPushButton(tr("Install for Claude Desktop"), installBox);
    installBtn_->setDefault(true);
    installBtn_->setProperty("accent", true);
    installBtn_->setCursor(Qt::PointingHandCursor);
    installBtn_->setMinimumHeight(34);
    connect(installBtn_, &QPushButton::clicked, this, &MCPSetupView::onInstallClicked);
    copyBtn_ = new QPushButton(tr("Copy"), installBox);
    connect(copyBtn_, &QPushButton::clicked, this, &MCPSetupView::onCopyClicked);
    buttons->addWidget(installBtn_, 2);
    buttons->addWidget(copyBtn_, 1);
    installV->addLayout(buttons);
    auto* footer = new QLabel(
        tr("Install merges Gridex into your existing config automatically. A backup of the original "
           "file is kept next to it."), installBox);
    footer->setStyleSheet("color: palette(mid); font-size: small;");
    footer->setWordWrap(true);
    installV->addWidget(footer);
    outer->addWidget(installBox);

    // JSON preview
    auto* previewBox = new QGroupBox(tr("Configuration Preview"), this);
    auto* previewV = new QVBoxLayout(previewBox);
    jsonPreview_ = new QPlainTextEdit(previewBox);
    jsonPreview_->setReadOnly(true);
    QFont mf = jsonPreview_->font(); mf.setFamily("monospace"); jsonPreview_->setFont(mf);
    jsonPreview_->setMaximumHeight(180);
    previewV->addWidget(jsonPreview_);
    auto* previewFooter = new QLabel(
        tr("This is the JSON that will be merged into the mcpServers section of your config file."),
        previewBox);
    previewFooter->setStyleSheet("color: palette(mid); font-size: small;");
    previewFooter->setWordWrap(true);
    previewV->addWidget(previewFooter);
    outer->addWidget(previewBox);

    // Manual steps
    auto* stepsBox = new QGroupBox(tr("Manual Steps"), this);
    auto* stepsV = new QVBoxLayout(stepsBox);
    const QStringList steps = {
        tr("Click \"Install\" above, or copy the JSON manually"),
        tr("If manual: open the config file"),
        tr("Paste the JSON inside \"mcpServers\": { ... }"),
        tr("Save the file and restart the client"),
        tr("Ask: \"List my database connections\""),
    };
    for (int i = 0; i < steps.size(); ++i) {
        auto* row = new QLabel(stepsBox);
        row->setTextFormat(Qt::RichText);
        row->setWordWrap(true);
        row->setText(QString(
            "<b><span style='color:#378add;'>%1.</span></b> %2").arg(i + 1).arg(steps[i]));
        stepsV->addWidget(row);
    }
    outer->addWidget(stepsBox);

    outer->addStretch();
    hostH->addWidget(content, 0, Qt::AlignTop);
    hostH->addStretch();
    scroll->setWidget(host);
}

void MCPSetupView::onClientChanged() {
    configPathLabel_->setText(configPathForSelected());
    jsonPreview_->setPlainText(configJsonForSelected());
    installBtn_->setText(tr("Install for %1").arg(clientCombo_->currentText()));
}

QString MCPSetupView::configPathForSelected() const {
    QString id = clientCombo_->currentData().toString();
    for (const auto& c : kClients) if (id == c.id) return c.pathTemplate;
    return {};
}

QString MCPSetupView::configJsonForSelected() const {
    const QString exe = QCoreApplication::applicationFilePath();
    nlohmann::json j;
    j["mcpServers"]["gridex"] = {
        {"command", exe.toStdString()},
        {"args", nlohmann::json::array({"--mcp-stdio"})},
    };
    return QString::fromStdString(j.dump(2));
}

void MCPSetupView::onCopyClicked() {
    QApplication::clipboard()->setText(jsonPreview_->toPlainText());
    copyBtn_->setText(tr("Copied"));
    QTimer::singleShot(2000, this, [this] { copyBtn_->setText(tr("Copy")); });
}

void MCPSetupView::onOpenPathClicked() {
    QFileInfo fi(expand(configPathForSelected()));
    QDir dir = fi.dir();
    if (!dir.exists()) dir.mkpath(".");
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir.absolutePath()));
}

void MCPSetupView::onInstallClicked() {
    const QString path = expand(configPathForSelected());
    const QString exe  = QCoreApplication::applicationFilePath();

    QFileInfo fi(path);
    QDir dir = fi.dir();
    if (!dir.exists() && !dir.mkpath(".")) {
        QMessageBox::warning(this, tr("Install failed"),
            tr("Could not create config directory: %1").arg(dir.absolutePath()));
        return;
    }

    nlohmann::json existing = nlohmann::json::object();
    QFile file(path);
    if (file.exists()) {
        if (file.open(QIODevice::ReadOnly)) {
            QByteArray data = file.readAll();
            file.close();
            if (!data.isEmpty()) {
                try {
                    existing = nlohmann::json::parse(data.toStdString());
                } catch (const std::exception&) {
                    QMessageBox::warning(this, tr("Install failed"),
                        tr("Existing config file is not valid JSON. Please fix or delete it, then try again."));
                    return;
                }
            }
            // Backup
            QFile::copy(path, path + ".bak");
        }
    }

    nlohmann::json entry = {
        {"command", exe.toStdString()},
        {"args", nlohmann::json::array({"--mcp-stdio"})},
    };

    if (!existing.contains("mcpServers") || !existing["mcpServers"].is_object()) {
        existing["mcpServers"] = nlohmann::json::object();
    }
    existing["mcpServers"]["gridex"] = entry;

    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, tr("Install failed"),
            tr("Could not write config file: %1").arg(path));
        return;
    }
    file.write(QByteArray::fromStdString(existing.dump(2)));
    file.close();

    QMessageBox::information(this, tr("Installed"),
        tr("Gridex has been added to %1's config. Restart %1 to apply.").arg(clientCombo_->currentText()));
}

}  // namespace gridex
