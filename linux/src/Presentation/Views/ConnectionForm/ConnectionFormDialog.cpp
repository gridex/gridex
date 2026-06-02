#include "Presentation/Views/ConnectionForm/ConnectionFormDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QString>
#include <QStyle>
#include <QUuid>
#include <QVBoxLayout>
#include <QWidget>

#include "Core/Enums/SSLMode.h"
#include "Presentation/Views/Chrome/GxIcons.h"

namespace gridex {

namespace {

QString qs(const std::string& s) { return QString::fromUtf8(s.c_str(), static_cast<int>(s.size())); }
std::string st(const QString& s) { return s.toStdString(); }

constexpr int kLabelWidth = 110;
constexpr int kRowSpacing = 4;

// One labeled form row: fixed-width label on the left, content on the right.
QWidget* makeRow(const QString& label, QWidget* content, QWidget* parent) {
    auto* row = new QWidget(parent);
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(8);
    auto* lbl = new QLabel(label, row);
    lbl->setMinimumWidth(kLabelWidth);
    lbl->setMaximumWidth(kLabelWidth);
    lbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    h->addWidget(lbl);
    h->addWidget(content, 1);
    return row;
}

}

ConnectionFormDialog::ConnectionFormDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Connection"));
    setMinimumWidth(580);

    // Give the dialog a clearly-separated body: a bordered inner frame sitting on
    // an outer transparent margin. This reads as a distinct "sheet" even when
    // the WM doesn't paint a strong shadow (Wayland + minimal themes).
    setModal(true);
    setWindowModality(Qt::ApplicationModal);
    setAttribute(Qt::WA_TranslucentBackground, false);

    // QFrame#ConnectionFormCard + QLabel#ConnectionFormHeader styling is
    // defined in resources/style-gx{,-light}.qss so the dialog tracks the
    // active theme.

    buildUi();
    applyLayoutForType(DatabaseType::PostgreSQL);
    updateButtonStates();
}

void ConnectionFormDialog::buildUi() {
    // Wrap content in a bordered card with drop shadow so the dialog reads as
    // a distinct sheet even when the window manager (e.g. Wayland + Fusion)
    // doesn't render a strong shadow around the OS window decoration.
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(14, 14, 14, 14);
    outer->setSpacing(0);

    auto* card = new QFrame(this);
    card->setObjectName(QStringLiteral("ConnectionFormCard"));
    auto* shadow = new QGraphicsDropShadowEffect(card);
    shadow->setBlurRadius(28);
    shadow->setOffset(0, 4);
    shadow->setColor(QColor(0, 0, 0, 120));
    card->setGraphicsEffect(shadow);
    outer->addWidget(card);

    auto* root = new QVBoxLayout(card);
    root->setContentsMargins(24, 20, 24, 12);
    root->setSpacing(kRowSpacing);

    // Title bar inside the card for visual anchor (matches macOS sheet header).
    auto* header = new QLabel(tr("Connection"), card);
    header->setObjectName(QStringLiteral("ConnectionFormHeader"));
    root->addWidget(header);
    auto* headerDiv = new QFrame(card);
    headerDiv->setFrameShape(QFrame::HLine);
    headerDiv->setObjectName(QStringLiteral("gxConnDivider"));
    root->addWidget(headerDiv);
    root->addSpacing(4);

    // --- Row: Name ---
    nameEdit_ = new QLineEdit(this);
    nameEdit_->setPlaceholderText(tr("Connection name"));
    connect(nameEdit_, &QLineEdit::textChanged, this, &ConnectionFormDialog::onFieldChanged);
    root->addWidget(makeRow(tr("Name"), nameEdit_, this));

    // --- Row: Type (Linux-only helper — macOS chooses via DatabaseTypePicker before opening the form) ---
    typeCombo_ = new QComboBox(this);
    for (auto t : kAllDatabaseTypes) {
        typeCombo_->addItem(QString::fromUtf8(std::string(displayName(t)).c_str()),
                            QString::fromUtf8(std::string(rawValue(t)).c_str()));
    }
    connect(typeCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ConnectionFormDialog::onDatabaseTypeChanged);
    root->addWidget(makeRow(tr("Type"), typeCombo_, this));

    // --- Row: Status color (6 dots + Tag dropdown) ---
    auto* colorRow = new QWidget(this);
    auto* colorH = new QHBoxLayout(colorRow);
    colorH->setContentsMargins(0, 0, 0, 0);
    colorH->setSpacing(6);
    const std::array<ColorTag, 6> tags = {
        ColorTag::Red, ColorTag::Orange, ColorTag::Green,
        ColorTag::Blue, ColorTag::Purple, ColorTag::Gray,
    };
    for (int i = 0; i < 6; ++i) {
        auto* dot = new QPushButton(colorRow);
        dot->setFixedSize(28, 20);
        dot->setCheckable(true);
        const auto rgb = rgbColor(tags[i]);
        dot->setStyleSheet(QString("QPushButton { background-color: rgb(%1,%2,%3); border-radius: 3px; border: 0px solid white; }"
                                   "QPushButton:checked { border: 2px solid palette(highlight); }")
                               .arg(rgb.r).arg(rgb.g).arg(rgb.b));
        connect(dot, &QPushButton::clicked, this, [this, i] { onColorDotClicked(i); });
        colorDots_[i] = dot;
        colorH->addWidget(dot);
    }
    colorH->addStretch();
    auto* tagLbl = new QLabel(tr("Tag"), colorRow);
    colorH->addWidget(tagLbl);
    colorCombo_ = new QComboBox(colorRow);
    colorCombo_->setMinimumWidth(120);
    for (auto t : kAllColorTags) {
        colorCombo_->addItem(QString::fromUtf8(std::string(environmentHint(t)).c_str()),
                             QString::fromUtf8(std::string(rawValue(t)).c_str()));
    }
    connect(colorCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) {
                const auto raw = st(colorCombo_->itemData(idx).toString());
                colorTag_ = colorTagFromRaw(raw).value_or(ColorTag::Blue);
                refreshColorDotBorders();
            });
    colorH->addWidget(colorCombo_);
    root->addWidget(makeRow(tr("Status color"), colorRow, this));

    // --- SQLite file page ---
    filePage_ = new QWidget(this);
    {
        auto* h = new QHBoxLayout(filePage_);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(8);
        filePathEdit_ = new QLineEdit(filePage_);
        filePathEdit_->setPlaceholderText(tr("path/to/database.db"));
        connect(filePathEdit_, &QLineEdit::textChanged, this, &ConnectionFormDialog::onFieldChanged);
        filePickBtn_ = new QPushButton(tr("Browse..."), filePage_);
        connect(filePickBtn_, &QPushButton::clicked, this, &ConnectionFormDialog::onBrowseFile);
        h->addWidget(filePathEdit_, 1);
        h->addWidget(filePickBtn_);
    }
    root->addWidget(makeRow(tr("File"), filePage_, this));

    // --- Host-based page (contains all PG/MySQL/MSSQL/Mongo/Redis rows) ---
    hostPage_ = new QWidget(this);
    auto* hostV = new QVBoxLayout(hostPage_);
    hostV->setContentsMargins(0, 0, 0, 0);
    hostV->setSpacing(kRowSpacing);

    // Host + Port (same row, aligned to form via makeRow semantics on the child)
    {
        auto* hp = new QWidget(hostPage_);
        auto* h = new QHBoxLayout(hp);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(8);
        hostEdit_ = new QLineEdit(hp);
        hostEdit_->setPlaceholderText(QStringLiteral("127.0.0.1"));
        connect(hostEdit_, &QLineEdit::textChanged, this, &ConnectionFormDialog::onFieldChanged);
        h->addWidget(hostEdit_, 1);
        auto* portLbl = new QLabel(tr("Port"), hp);
        h->addWidget(portLbl);
        portSpin_ = new QSpinBox(hp);
        portSpin_->setRange(0, 65535);
        portSpin_->setFixedWidth(80);
        h->addWidget(portSpin_);
        hostV->addWidget(makeRow(tr("Host/Socket"), hp, hostPage_));
    }

    // User + Other options
    {
        auto* up = new QWidget(hostPage_);
        auto* h = new QHBoxLayout(up);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(12);
        userEdit_ = new QLineEdit(up);
        userEdit_->setPlaceholderText(tr("user name"));
        h->addWidget(userEdit_, 1);
        otherOptsBtn_ = new QPushButton(tr("Other options"), up);
        otherOptsBtn_->setFixedWidth(130);
        otherOptsBtn_->setEnabled(false);  // Placeholder — advanced options in later phase
        h->addWidget(otherOptsBtn_);
        hostV->addWidget(makeRow(tr("User"), up, hostPage_));
    }

    // Password + Store dropdown
    {
        auto* pp = new QWidget(hostPage_);
        auto* h = new QHBoxLayout(pp);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(12);
        passwordEdit_ = new QLineEdit(pp);
        passwordEdit_->setPlaceholderText(tr("password"));
        passwordEdit_->setEchoMode(QLineEdit::Password);
        h->addWidget(passwordEdit_, 1);
        storeCombo_ = new QComboBox(pp);
        storeCombo_->addItem(tr("Store in keychain"), true);
        storeCombo_->addItem(tr("Don't store"), false);
        storeCombo_->setFixedWidth(170);
        h->addWidget(storeCombo_);
        hostV->addWidget(makeRow(tr("Password"), pp, hostPage_));
    }

    // Database + SSL mode
    {
        auto* dp = new QWidget(hostPage_);
        auto* h = new QHBoxLayout(dp);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(12);
        databaseEdit_ = new QLineEdit(dp);
        databaseEdit_->setPlaceholderText(tr("database name"));
        h->addWidget(databaseEdit_, 1);
        auto* sslLbl = new QLabel(tr("SSL mode"), dp);
        h->addWidget(sslLbl);
        sslModeCombo_ = new QComboBox(dp);
        sslModeCombo_->setFixedWidth(140);
        for (auto m : kAllSSLModes) {
            sslModeCombo_->addItem(QString::fromUtf8(std::string(displayName(m)).c_str()),
                                   QString::fromUtf8(std::string(rawValue(m)).c_str()));
        }
        h->addWidget(sslModeCombo_);
        hostV->addWidget(makeRow(tr("Database"), dp, hostPage_));
    }

    // SSL keys
    {
        auto* sp = new QWidget(hostPage_);
        auto* h = new QHBoxLayout(sp);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(6);

        auto makeSslField = [this, sp](QLineEdit*& edit, const QString& title, int slot) {
            edit = new QLineEdit(sp);
            edit->setPlaceholderText(title);
            edit->setReadOnly(true);
            auto* btn = new QPushButton(title, sp);
            btn->setFixedWidth(90);
            connect(btn, &QPushButton::clicked, this, [this, slot] { onBrowseSSLKey(slot); });
            auto* w = new QWidget(sp);
            auto* wh = new QHBoxLayout(w);
            wh->setContentsMargins(0, 0, 0, 0);
            wh->setSpacing(4);
            wh->addWidget(btn);
            wh->addWidget(edit, 1);
            return w;
        };

        h->addWidget(makeSslField(sslKeyPathEdit_,  tr("Key..."),  0), 1);
        h->addWidget(makeSslField(sslCertPathEdit_, tr("Cert..."), 1), 1);
        h->addWidget(makeSslField(sslCAPathEdit_,   tr("CA Cert..."), 2), 1);
        auto* clearBtn = new QPushButton(sp);
        clearBtn->setIcon(GxIcons::glyph("x"));
        clearBtn->setIconSize(QSize(12, 12));
        clearBtn->setFixedSize(28, 24);
        clearBtn->setToolTip(tr("Clear SSL keys"));
        connect(clearBtn, &QPushButton::clicked, this, &ConnectionFormDialog::onClearSSLKeys);
        h->addWidget(clearBtn);
        hostV->addWidget(makeRow(tr("SSL keys"), sp, hostPage_));
    }

    // SSH section (hidden until toggled)
    sshSection_ = new QWidget(hostPage_);
    {
        auto* sv = new QVBoxLayout(sshSection_);
        sv->setContentsMargins(0, 0, 0, 0);
        sv->setSpacing(kRowSpacing);

        auto* div = new QFrame(sshSection_);
        div->setFrameShape(QFrame::HLine);
        div->setObjectName(QStringLiteral("gxConnDivider"));
        sv->addWidget(div);

        // SSH Host + Port
        auto* hp = new QWidget(sshSection_);
        auto* hh = new QHBoxLayout(hp);
        hh->setContentsMargins(0, 0, 0, 0);
        hh->setSpacing(8);
        sshHostEdit_ = new QLineEdit(hp);
        sshHostEdit_->setPlaceholderText(tr("ssh.example.com"));
        hh->addWidget(sshHostEdit_, 1);
        auto* pLbl = new QLabel(tr("Port"), hp);
        hh->addWidget(pLbl);
        sshPortSpin_ = new QSpinBox(hp);
        sshPortSpin_->setRange(1, 65535);
        sshPortSpin_->setValue(22);
        sshPortSpin_->setFixedWidth(80);
        hh->addWidget(sshPortSpin_);
        sv->addWidget(makeRow(tr("SSH Host"), hp, sshSection_));

        // SSH User + Auth method
        auto* up = new QWidget(sshSection_);
        auto* uh = new QHBoxLayout(up);
        uh->setContentsMargins(0, 0, 0, 0);
        uh->setSpacing(12);
        sshUserEdit_ = new QLineEdit(up);
        sshUserEdit_->setPlaceholderText(tr("username"));
        uh->addWidget(sshUserEdit_, 1);
        sshAuthCombo_ = new QComboBox(up);
        sshAuthCombo_->addItem(tr("Password"), int(SSHAuthMethod::Password));
        sshAuthCombo_->addItem(tr("Private Key"), int(SSHAuthMethod::PrivateKey));
        sshAuthCombo_->addItem(tr("Key + Passphrase"), int(SSHAuthMethod::KeyWithPassphrase));
        sshAuthCombo_->setFixedWidth(160);
        connect(sshAuthCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &ConnectionFormDialog::onSSHAuthChanged);
        uh->addWidget(sshAuthCombo_);
        sv->addWidget(makeRow(tr("SSH User"), up, sshSection_));

        // SSH Password row
        sshPasswordEdit_ = new QLineEdit(sshSection_);
        sshPasswordEdit_->setPlaceholderText(tr("password"));
        sshPasswordEdit_->setEchoMode(QLineEdit::Password);
        sshPasswordRow_ = makeRow(tr("SSH Password"), sshPasswordEdit_, sshSection_);
        sv->addWidget(sshPasswordRow_);

        // SSH Key path + Browse
        auto* kp = new QWidget(sshSection_);
        auto* kh = new QHBoxLayout(kp);
        kh->setContentsMargins(0, 0, 0, 0);
        kh->setSpacing(8);
        sshKeyPathEdit_ = new QLineEdit(kp);
        sshKeyPathEdit_->setPlaceholderText(QStringLiteral("~/.ssh/id_rsa"));
        kh->addWidget(sshKeyPathEdit_, 1);
        auto* kbtn = new QPushButton(tr("Browse..."), kp);
        connect(kbtn, &QPushButton::clicked, this, &ConnectionFormDialog::onBrowseSSHKey);
        kh->addWidget(kbtn);
        sshKeyRow_ = makeRow(tr("SSH Key"), kp, sshSection_);
        sv->addWidget(sshKeyRow_);
    }
    sshSection_->setVisible(false);
    hostV->addWidget(sshSection_);

    root->addWidget(hostPage_);

    // --- Test result label ---
    testResult_ = new QLabel(this);
    testResult_->setObjectName(QStringLiteral("gxConnTestResult"));
    testResult_->setVisible(false);
    testResult_->setWordWrap(true);
    root->addWidget(testResult_);

    // --- Divider ---
    auto* divider = new QFrame(this);
    divider->setFrameShape(QFrame::HLine);
    divider->setObjectName(QStringLiteral("gxConnDivider"));
    root->addWidget(divider);

    // --- Bottom bar: [Over SSH]                 [Save] [Test] [Connect] ---
    auto* bottom = new QHBoxLayout();
    bottom->setContentsMargins(0, 0, 0, 0);
    bottom->setSpacing(8);
    overSshBtn_ = new QPushButton(tr("Over SSH"), this);
    connect(overSshBtn_, &QPushButton::clicked, this, &ConnectionFormDialog::onToggleSSH);
    bottom->addWidget(overSshBtn_);
    bottom->addStretch();
    saveBtn_ = new QPushButton(tr("Save"), this);
    connect(saveBtn_, &QPushButton::clicked, this, &ConnectionFormDialog::onSaveClicked);
    bottom->addWidget(saveBtn_);
    testBtn_ = new QPushButton(tr("Test"), this);
    connect(testBtn_, &QPushButton::clicked, this, &ConnectionFormDialog::onTestClicked);
    bottom->addWidget(testBtn_);
    connectBtn_ = new QPushButton(tr("Connect"), this);
    connectBtn_->setProperty("gxKind", QStringLiteral("primary"));
    connectBtn_->setDefault(true);
    connect(connectBtn_, &QPushButton::clicked, this, &ConnectionFormDialog::onConnectClicked);
    bottom->addWidget(connectBtn_);
    root->addLayout(bottom);

    // Initial color selection
    colorDots_[3]->setChecked(true);  // Blue is index 3
}

void ConnectionFormDialog::applyLayoutForType(DatabaseType type) {
    const bool isSqlite = type == DatabaseType::SQLite;
    filePage_->setVisible(isSqlite);
    hostPage_->setVisible(!isSqlite);
    overSshBtn_->setVisible(!isSqlite);
    if (!isSqlite) portSpin_->setValue(defaultPort(type));
}

void ConnectionFormDialog::onDatabaseTypeChanged(int index) {
    const auto raw = st(typeCombo_->itemData(index).toString());
    const auto t = databaseTypeFromRaw(raw);
    if (!t) return;
    applyLayoutForType(*t);
    updateButtonStates();
}

void ConnectionFormDialog::onBrowseFile() {
    const auto file = QFileDialog::getOpenFileName(
        this, tr("Choose SQLite file"), QString{},
        tr("SQLite (*.sqlite *.db *.sqlite3);;All files (*)"));
    if (!file.isEmpty()) filePathEdit_->setText(file);
}

void ConnectionFormDialog::onColorDotClicked(int index) {
    const std::array<ColorTag, 6> tags = {
        ColorTag::Red, ColorTag::Orange, ColorTag::Green,
        ColorTag::Blue, ColorTag::Purple, ColorTag::Gray,
    };
    colorTag_ = tags[index];
    refreshColorDotBorders();
    const auto raw = QString::fromUtf8(std::string(rawValue(colorTag_)).c_str());
    const int comboIdx = colorCombo_->findData(raw);
    if (comboIdx >= 0) {
        QSignalBlocker block(colorCombo_);
        colorCombo_->setCurrentIndex(comboIdx);
    }
}

void ConnectionFormDialog::refreshColorDotBorders() {
    const std::array<ColorTag, 6> tags = {
        ColorTag::Red, ColorTag::Orange, ColorTag::Green,
        ColorTag::Blue, ColorTag::Purple, ColorTag::Gray,
    };
    for (int i = 0; i < 6; ++i) {
        colorDots_[i]->setChecked(tags[i] == colorTag_);
    }
}

void ConnectionFormDialog::onToggleSSH() {
    sshEnabled_ = !sshEnabled_;
    sshSection_->setVisible(sshEnabled_);
    overSshBtn_->setText(sshEnabled_ ? tr("Close SSH") : tr("Over SSH"));
    rebuildSSHRows();
    adjustSize();
}

void ConnectionFormDialog::rebuildSSHRows() {
    if (!sshEnabled_) return;
    const auto auth = static_cast<SSHAuthMethod>(sshAuthCombo_->currentData().toInt());
    const bool needsPassword = auth == SSHAuthMethod::Password || auth == SSHAuthMethod::KeyWithPassphrase;
    const bool needsKey      = auth == SSHAuthMethod::PrivateKey || auth == SSHAuthMethod::KeyWithPassphrase;
    sshPasswordRow_->setVisible(needsPassword);
    sshKeyRow_->setVisible(needsKey);
}

void ConnectionFormDialog::onSSHAuthChanged(int /*index*/) { rebuildSSHRows(); }

void ConnectionFormDialog::onBrowseSSHKey() {
    const auto file = QFileDialog::getOpenFileName(this, tr("Choose SSH private key"));
    if (!file.isEmpty()) sshKeyPathEdit_->setText(file);
}

void ConnectionFormDialog::onBrowseSSLKey(int slot) {
    QLineEdit* target = slot == 0 ? sslKeyPathEdit_ : (slot == 1 ? sslCertPathEdit_ : sslCAPathEdit_);
    const auto file = QFileDialog::getOpenFileName(this, tr("Choose SSL file"));
    if (!file.isEmpty()) target->setText(file);
}

void ConnectionFormDialog::onClearSSLKeys() {
    sslKeyPathEdit_->clear();
    sslCertPathEdit_->clear();
    sslCAPathEdit_->clear();
}

void ConnectionFormDialog::onFieldChanged() {
    updateButtonStates();
}

void ConnectionFormDialog::updateButtonStates() {
    const bool nameOk = !nameEdit_->text().trimmed().isEmpty();
    const auto t = databaseTypeFromRaw(st(typeCombo_->currentData().toString()))
                       .value_or(DatabaseType::PostgreSQL);
    const bool targetOk = (t == DatabaseType::SQLite)
                              ? !filePathEdit_->text().trimmed().isEmpty()
                              : !hostEdit_->text().trimmed().isEmpty();

    saveBtn_->setEnabled(nameOk && targetOk);
    testBtn_->setEnabled(targetOk);
    connectBtn_->setEnabled(nameOk && targetOk);

    saveBtn_->setToolTip(saveBtn_->isEnabled() ? QString{}
                                                : tr("Enter a name and the target (host or file) to save."));
    connectBtn_->setToolTip(connectBtn_->isEnabled() ? QString{}
                                                    : tr("Enter a name and the target (host or file) to connect."));
}

void ConnectionFormDialog::showTestResult(bool success, const QString& detail) {
    testResult_->setVisible(true);
    testResult_->setProperty("gxText", success ? QStringLiteral("success")
                                               : QStringLiteral("danger"));
    testResult_->style()->unpolish(testResult_);
    testResult_->style()->polish(testResult_);
    testResult_->setText((success ? tr("✓ ") : tr("✗ ")) + detail);
}

void ConnectionFormDialog::onSaveClicked() {
    if (!saveBtn_->isEnabled()) return;
    done(Saved);
}

void ConnectionFormDialog::onTestClicked() {
    if (!testBtn_->isEnabled()) return;
    emit testRequested(config(), password());
}

void ConnectionFormDialog::onConnectClicked() {
    if (!connectBtn_->isEnabled()) return;
    done(Connect);
}

void ConnectionFormDialog::setConfig(const ConnectionConfig& config) {
    editingId_ = config.id;
    setWindowTitle(tr("Edit Connection"));
    nameEdit_->setText(qs(config.name));

    const auto raw = std::string(rawValue(config.databaseType));
    const int idx = typeCombo_->findData(QString::fromUtf8(raw.c_str()));
    if (idx >= 0) typeCombo_->setCurrentIndex(idx);
    applyLayoutForType(config.databaseType);

    colorTag_ = config.colorTag.value_or(ColorTag::Blue);
    refreshColorDotBorders();
    const int tagIdx = colorCombo_->findData(
        QString::fromUtf8(std::string(rawValue(colorTag_)).c_str()));
    if (tagIdx >= 0) colorCombo_->setCurrentIndex(tagIdx);

    if (config.databaseType == DatabaseType::SQLite) {
        filePathEdit_->setText(qs(config.filePath.value_or("")));
    } else {
        hostEdit_->setText(qs(config.host.value_or("")));
        portSpin_->setValue(config.port.value_or(defaultPort(config.databaseType)));
        databaseEdit_->setText(qs(config.database.value_or("")));
        userEdit_->setText(qs(config.username.value_or("")));
        const int sslIdx = sslModeCombo_->findData(
            config.sslEnabled ? QStringLiteral("REQUIRED") : QStringLiteral("DISABLED"));
        if (sslIdx >= 0) sslModeCombo_->setCurrentIndex(sslIdx);

        if (config.sshConfig) {
            sshEnabled_ = true;
            sshSection_->setVisible(true);
            overSshBtn_->setText(tr("Close SSH"));
            sshHostEdit_->setText(qs(config.sshConfig->host));
            sshPortSpin_->setValue(config.sshConfig->port);
            sshUserEdit_->setText(qs(config.sshConfig->username));
            const int authIdx = sshAuthCombo_->findData(int(config.sshConfig->authMethod));
            if (authIdx >= 0) sshAuthCombo_->setCurrentIndex(authIdx);
            if (config.sshConfig->keyPath) sshKeyPathEdit_->setText(qs(*config.sshConfig->keyPath));
            rebuildSSHRows();
        }
    }
    updateButtonStates();
}

void ConnectionFormDialog::setPassword(const std::string& password) {
    passwordEdit_->setText(qs(password));
}

bool ConnectionFormDialog::storeInKeychain() const {
    return storeCombo_ && storeCombo_->currentData().toBool();
}

ConnectionConfig ConnectionFormDialog::config() const {
    ConnectionConfig c;
    c.id = editingId_.empty()
               ? QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString()
               : editingId_;
    c.name = st(nameEdit_->text().trimmed());
    const auto raw = st(typeCombo_->currentData().toString());
    c.databaseType = databaseTypeFromRaw(raw).value_or(DatabaseType::PostgreSQL);
    c.colorTag = colorTag_;

    if (c.databaseType == DatabaseType::SQLite) {
        const auto p = st(filePathEdit_->text().trimmed());
        if (!p.empty()) c.filePath = p;
    } else {
        const auto h = st(hostEdit_->text().trimmed());
        if (!h.empty()) c.host = h;
        c.port = portSpin_->value();
        const auto db = st(databaseEdit_->text().trimmed());
        if (!db.empty()) c.database = db;
        const auto u = st(userEdit_->text().trimmed());
        if (!u.empty()) c.username = u;
        const auto sslRaw = st(sslModeCombo_->currentData().toString());
        c.sslEnabled = !(sslRaw == "DISABLED");

        if (sshEnabled_) {
            SSHTunnelConfig s;
            s.host = st(sshHostEdit_->text().trimmed());
            s.port = sshPortSpin_->value();
            s.username = st(sshUserEdit_->text().trimmed());
            s.authMethod = static_cast<SSHAuthMethod>(sshAuthCombo_->currentData().toInt());
            const auto kp = st(sshKeyPathEdit_->text().trimmed());
            if (!kp.empty()) s.keyPath = kp;
            if (!s.host.empty()) c.sshConfig = s;
        }
    }
    return c;
}

std::optional<std::string> ConnectionFormDialog::password() const {
    const auto pw = passwordEdit_->text();
    if (pw.isEmpty()) return std::nullopt;
    return st(pw);
}

std::optional<std::string> ConnectionFormDialog::sshPassword() const {
    if (!sshPasswordEdit_) return std::nullopt;
    const auto pw = sshPasswordEdit_->text();
    if (pw.isEmpty()) return std::nullopt;
    return st(pw);
}

void ConnectionFormDialog::setSshPassword(const std::string& password) {
    if (sshPasswordEdit_) sshPasswordEdit_->setText(qs(password));
}

void ConnectionFormDialog::hideAllSSHRows() {
    sshPasswordRow_->setVisible(false);
    sshKeyRow_->setVisible(false);
}

}
