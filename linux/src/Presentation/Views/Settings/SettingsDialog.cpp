#include "Presentation/Views/Settings/SettingsDialog.h"

#include "Core/Models/AI/ChatGPTTokenBundle.h"
#include "Services/AI/Auth/ChatGPTOAuthService.h"

#include <nlohmann/json.hpp>

#include <QApplication>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>
#include <QComboBox>
#include <QFormLayout>
#include <QFrame>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

#include "Data/Keychain/SecretStore.h"
#include "Presentation/Theme/ThemeManager.h"
#include "Presentation/ViewModels/AIChatViewModel.h"
#include "Services/AI/AIServiceFactory.h"

namespace gridex {

namespace {

constexpr const char* kProviders[] = {"Anthropic", "OpenAI", "Ollama", "Gemini", "ChatGPT"};

QString endpointKey(const QString& provider) {
    return QStringLiteral("ai/endpoint/") + provider;
}

QString endpointPlaceholder(const QString& provider) {
    if (provider == QLatin1String("Ollama"))
        return QStringLiteral("http://localhost:11434");
    if (provider == QLatin1String("OpenAI"))
        return QStringLiteral("https://api.openai.com/v1  (or OpenAI-compatible URL)");
    if (provider == QLatin1String("Anthropic"))
        return QStringLiteral("https://api.anthropic.com");
    if (provider == QLatin1String("Gemini"))
        return QStringLiteral("https://generativelanguage.googleapis.com");
    return QStringLiteral("https://…");
}

}  // namespace

SettingsDialog::~SettingsDialog() = default;

SettingsDialog::SettingsDialog(SecretStore* secretStore, QWidget* parent)
    : QDialog(parent), secretStore_(secretStore) {
    buildUi();
    // Initial load from saved default.
    QSettings s;
    const QString savedProvider = s.value("ai/provider", QStringLiteral("Anthropic")).toString();
    providerCombo_->setCurrentText(savedProvider);
    onProviderChanged(savedProvider);
}

void SettingsDialog::buildUi() {
    setWindowTitle(tr("Preferences"));
    setMinimumSize(720, 440);

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    navList_ = new QListWidget(this);
    navList_->setFixedWidth(180);
    navList_->setFrameShape(QFrame::NoFrame);
    navList_->addItem(tr("AI Providers"));
    navList_->addItem(tr("Appearance"));
    root->addWidget(navList_);

    auto* div = new QFrame(this);
    div->setFrameShape(QFrame::VLine);
    root->addWidget(div);

    pages_ = new QStackedWidget(this);
    root->addWidget(pages_, 1);

    auto* aiPage = new QWidget(pages_);
    buildAiPage(aiPage);
    pages_->addWidget(aiPage);

    auto* appearancePage = new QWidget(pages_);
    buildAppearancePage(appearancePage);
    pages_->addWidget(appearancePage);

    connect(navList_, &QListWidget::currentRowChanged,
            pages_,   &QStackedWidget::setCurrentIndex);
    navList_->setCurrentRow(0);
}

void SettingsDialog::buildAiPage(QWidget* page) {
    auto* root = new QVBoxLayout(page);
    root->setContentsMargins(24, 24, 24, 24);
    root->setSpacing(14);

    auto* title = new QLabel(tr("AI Providers"), page);
    {
        QFont f = title->font();
        f.setPointSize(14);
        f.setWeight(QFont::DemiBold);
        title->setFont(f);
    }
    root->addWidget(title);

    auto* desc = new QLabel(
        tr("Pick a provider and model. API keys are stored encrypted in the "
           "system keychain (libsecret); endpoints override the default base "
           "URL (useful for self-hosted or OpenAI-compatible servers)."),
        page);
    desc->setWordWrap(true);
    desc->setForegroundRole(QPalette::PlaceholderText);
    root->addWidget(desc);

    auto* form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignRight);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(10);
    aiForm_ = form;  // captured for runtime row-visibility toggling

    // Provider
    providerCombo_ = new QComboBox(page);
    for (const char* p : kProviders) providerCombo_->addItem(p);
    connect(providerCombo_, &QComboBox::currentTextChanged,
            this, &SettingsDialog::onProviderChanged);
    form->addRow(tr("Provider:"), providerCombo_);

    // Model + spinner
    auto* modelRow = new QWidget(page);
    auto* mrH = new QHBoxLayout(modelRow);
    mrH->setContentsMargins(0, 0, 0, 0);
    mrH->setSpacing(6);
    modelCombo_ = new QComboBox(modelRow);
    modelCombo_->setMinimumWidth(260);
    connect(modelCombo_, &QComboBox::currentTextChanged,
            this, &SettingsDialog::onModelChanged);
    mrH->addWidget(modelCombo_, 1);
    // Tiny Braille-glyph spinner — replaces the 80×16 QProgressBar bar with
    // a single character that ticks at ~12fps next to the model combo.
    modelSpinner_ = new QLabel(modelRow);
    modelSpinner_->setFixedWidth(14);
    modelSpinner_->setAlignment(Qt::AlignCenter);
    modelSpinner_->setStyleSheet("color: #89b4fa; font-size: 14px; font-weight: 600;");
    modelSpinner_->hide();
    mrH->addWidget(modelSpinner_);
    spinnerTimer_ = new QTimer(this);
    spinnerTimer_->setInterval(80);
    static const QStringList kFrames = {
        QStringLiteral("⠋"), QStringLiteral("⠙"), QStringLiteral("⠹"),
        QStringLiteral("⠸"), QStringLiteral("⠼"), QStringLiteral("⠴"),
        QStringLiteral("⠦"), QStringLiteral("⠧"), QStringLiteral("⠇"),
        QStringLiteral("⠏"),
    };
    connect(spinnerTimer_, &QTimer::timeout, this, [this]() {
        modelSpinner_->setText(kFrames[spinnerFrame_]);
        spinnerFrame_ = (spinnerFrame_ + 1) % kFrames.size();
    });
    modelStatus_ = new QLabel(modelRow);
    modelStatus_->setForegroundRole(QPalette::PlaceholderText);
    mrH->addWidget(modelStatus_);
    form->addRow(tr("Model:"), modelRow);

    // API Key + eye toggle
    auto* keyRow = new QWidget(page);
    auto* keyH = new QHBoxLayout(keyRow);
    keyH->setContentsMargins(0, 0, 0, 0);
    keyH->setSpacing(6);
    apiKeyEdit_ = new QLineEdit(keyRow);
    apiKeyEdit_->setEchoMode(QLineEdit::Password);
    apiKeyEdit_->setClearButtonEnabled(true);
    apiKeyEdit_->setPlaceholderText(tr("Paste API key…"));
    // Debounce: wait 600 ms after the user stops typing, then fetch models.
    keyDebounce_ = new QTimer(this);
    keyDebounce_->setSingleShot(true);
    keyDebounce_->setInterval(600);
    connect(keyDebounce_, &QTimer::timeout, this, &SettingsDialog::onKeyEditTimeout);
    connect(apiKeyEdit_, &QLineEdit::textChanged, this, [this](const QString&) {
        keyDebounce_->start();
    });
    keyH->addWidget(apiKeyEdit_, 1);
    auto* eye = new QToolButton(keyRow);
    eye->setText(QStringLiteral("👁"));
    eye->setToolTip(tr("Show/hide key"));
    eye->setCheckable(true);
    connect(eye, &QToolButton::toggled, this, [this](bool on) {
        apiKeyEdit_->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
    });
    keyH->addWidget(eye);
    form->addRow(tr("API Key:"), keyRow);
    apiKeyRow_ = keyRow;
    if (auto* lbl = qobject_cast<QLabel*>(form->labelForField(keyRow))) {
        // Capture label so toggling visibility hides both halves of the row.
        keyRow->setProperty("formLabel", QVariant::fromValue<QObject*>(lbl));
    }

    // ChatGPT OAuth — replaces API Key when provider == ChatGPT.
    oauthRow_ = new QWidget(page);
    auto* oauthH = new QHBoxLayout(oauthRow_);
    oauthH->setContentsMargins(0, 0, 0, 0);
    oauthH->setSpacing(8);
    oauthStatus_ = new QLabel(oauthRow_);
    oauthStatus_->setForegroundRole(QPalette::PlaceholderText);
    signInBtn_   = new QPushButton(tr("Sign in with ChatGPT"), oauthRow_);
    signInBtn_->setObjectName(QStringLiteral("primaryButton"));
    signOutBtn_  = new QPushButton(tr("Sign out"), oauthRow_);
    signOutBtn_->hide();
    oauthH->addWidget(oauthStatus_, 1);
    oauthH->addWidget(signInBtn_);
    oauthH->addWidget(signOutBtn_);
    form->addRow(tr("ChatGPT account:"), oauthRow_);
    // Hide both label and field by default — onProviderChanged() flips them
    // when the user picks ChatGPT.
    if (auto* lbl = qobject_cast<QLabel*>(form->labelForField(oauthRow_))) lbl->hide();
    oauthRow_->hide();
    connect(signInBtn_,  &QPushButton::clicked, this, &SettingsDialog::onOAuthSignInClicked);
    connect(signOutBtn_, &QPushButton::clicked, this, &SettingsDialog::onOAuthSignOutClicked);

    oauthService_ = std::make_unique<ChatGPTOAuthService>(secretStore_);
    connect(oauthService_.get(), &ChatGPTOAuthService::signInCompleted,
            this, [this](const QString&, const ChatGPTTokenBundle&) {
        refreshOAuthStatus();
        fetchChatGPTModels();
    });
    connect(oauthService_.get(), &ChatGPTOAuthService::signInFailed,
            this, [this](const QString&, const QString& msg) {
        signInBtn_->setEnabled(true);
        signInBtn_->setText(tr("Sign in with ChatGPT"));
        oauthStatus_->setText(QString("<span style='color:#f38ba8;'>%1</span>").arg(msg));
        oauthStatus_->setTextFormat(Qt::RichText);
    });

    // Custom endpoint
    endpointEdit_ = new QLineEdit(page);
    endpointEdit_->setClearButtonEnabled(true);
    form->addRow(tr("Custom endpoint:"), endpointEdit_);
    endpointRow_ = endpointEdit_;

    root->addLayout(form);
    root->addStretch();

    // Save / Close row
    auto* btnRow = new QWidget(page);
    auto* btnH = new QHBoxLayout(btnRow);
    btnH->setContentsMargins(0, 0, 0, 0);
    btnH->addStretch();

    auto* closeBtn = new QPushButton(tr("Close"), btnRow);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    btnH->addWidget(closeBtn);

    saveBtn_ = new QPushButton(tr("Save"), btnRow);
    saveBtn_->setObjectName(QStringLiteral("primaryButton"));
    saveBtn_->setDefault(true);
    connect(saveBtn_, &QPushButton::clicked, this, &SettingsDialog::onSaveClicked);
    btnH->addWidget(saveBtn_);

    root->addWidget(btnRow);
}

void SettingsDialog::loadForProvider(const QString& provider) {
    // API key from keychain.
    QString savedKey;
    if (secretStore_ && secretStore_->isAvailable()) {
        const auto key = secretStore_->loadAPIKey(provider.toStdString());
        if (key) savedKey = QString::fromStdString(*key);
    }
    apiKeyEdit_->blockSignals(true);
    apiKeyEdit_->setText(savedKey);
    apiKeyEdit_->blockSignals(false);

    // Custom endpoint.
    QSettings s;
    endpointEdit_->setPlaceholderText(endpointPlaceholder(provider));
    endpointEdit_->setText(s.value(endpointKey(provider)).toString());

    // Fetch models with the saved key (empty key falls back to hardcoded
    // defaults without hitting the network).
    fetchModels(provider, savedKey);
}

void SettingsDialog::onKeyEditTimeout() {
    fetchModels(providerCombo_->currentText(), apiKeyEdit_->text().trimmed());
}

void SettingsDialog::fetchModels(const QString& provider, const QString& apiKey) {
    // Show spinner + status while fetching.
    modelSpinner_->show(); spinnerFrame_ = 0; spinnerTimer_->start();
    modelStatus_->setText(tr("Loading models…"));
    modelCombo_->setEnabled(false);

    // Snapshot the saved selection so we can try to restore it after refresh.
    QSettings s;
    const QString savedProvider = s.value("ai/provider").toString();
    const QString savedModel    = s.value("ai/model").toString();

    const auto providerStd = provider.toStdString();
    const auto keyStd      = apiKey.toStdString();
    // Pick up a custom endpoint — user may be typing it right now.
    const auto urlStd = endpointEdit_->text().trimmed().toStdString();

    auto* watcher = new QFutureWatcher<std::vector<LLMModel>>(this);
    connect(watcher, &QFutureWatcher<std::vector<LLMModel>>::finished, this,
        [this, watcher, provider, savedProvider, savedModel]() {
            const auto models = watcher->result();
            watcher->deleteLater();

            // Ignore if the user already switched to a different provider.
            if (providerCombo_->currentText() != provider) return;

            modelCombo_->blockSignals(true);
            modelCombo_->clear();
            for (const auto& m : models) {
                modelCombo_->addItem(QString::fromStdString(m.name),
                                     QString::fromStdString(m.id));
            }
            if (!savedModel.isEmpty() && provider == savedProvider) {
                int idx = modelCombo_->findData(savedModel);
                if (idx < 0) idx = modelCombo_->findText(savedModel);
                if (idx >= 0) modelCombo_->setCurrentIndex(idx);
            }
            modelCombo_->blockSignals(false);

            spinnerTimer_->stop(); modelSpinner_->hide();
            modelCombo_->setEnabled(true);
            if (models.empty()) {
                const bool hasKey = !apiKeyEdit_->text().trimmed().isEmpty();
                modelStatus_->setText(hasKey
                    ? tr("No models returned — check API key or endpoint.")
                    : tr("Enter API key to load models."));
            } else {
                modelStatus_->setText(tr("%1 models").arg(models.size()));
            }
        });

    auto future = QtConcurrent::run([providerStd, keyStd, urlStd]() {
        try {
            auto svc = AIServiceFactory::createAIService(providerStd, keyStd, urlStd);
            return svc->availableModels();
        } catch (...) {
            return std::vector<LLMModel>{};
        }
    });
    watcher->setFuture(future);
}

void SettingsDialog::onProviderChanged(const QString& provider) {
    const bool isChatGPT = (provider == QLatin1String("ChatGPT"));

    // Toggle the API-key + custom-endpoint rows out of the form when the
    // user picks the OAuth-based ChatGPT provider; show the OAuth row
    // instead. Both halves of a QFormLayout row (label + field) need to be
    // hidden together, hence the labelForField lookups.
    if (aiForm_) {
        auto setRowVisible = [&](QWidget* row, bool visible) {
            if (!row) return;
            if (auto* lbl = qobject_cast<QLabel*>(aiForm_->labelForField(row))) {
                lbl->setVisible(visible);
            }
            row->setVisible(visible);
        };
        setRowVisible(apiKeyRow_,   !isChatGPT);
        setRowVisible(endpointRow_, !isChatGPT);
        setRowVisible(oauthRow_,    isChatGPT);
    }

    if (isChatGPT) {
        refreshOAuthStatus();
        if (oauthService_ && oauthService_->status().signedIn) {
            fetchChatGPTModels();
        } else {
            modelCombo_->clear();
            modelStatus_->setText(tr("Sign in to load models."));
        }
    } else {
        loadForProvider(provider);
    }
}

void SettingsDialog::fetchChatGPTModels() {
    if (!oauthService_) return;
    auto bundle = oauthService_->currentBundle();
    if (!bundle) return;

    modelSpinner_->show(); spinnerFrame_ = 0; spinnerTimer_->start();
    modelStatus_->setText(tr("Loading models…"));
    modelCombo_->setEnabled(false);
    modelCombo_->clear();

    QUrl url(QStringLiteral("https://chatgpt.com/backend-api/codex/models"));
    QUrlQuery q;
    q.addQueryItem("client_version", "1.0.0");
    url.setQuery(q);

    QNetworkRequest req(url);
    req.setRawHeader("Authorization",
                     QByteArrayLiteral("Bearer ") + QByteArray::fromStdString(bundle->accessToken));
    if (bundle->accountId) {
        req.setRawHeader("ChatGPT-Account-ID",
                         QByteArray::fromStdString(*bundle->accountId));
    }
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);

    auto* nam = new QNetworkAccessManager(this);
    auto* reply = nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, nam, reply]() {
        reply->deleteLater();
        nam->deleteLater();
        spinnerTimer_->stop(); modelSpinner_->hide();
        modelCombo_->setEnabled(true);

        int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 401 || status == 403) {
            modelStatus_->setText(tr("Sign-in expired — sign in again."));
            return;
        }
        if (status < 200 || status >= 300) {
            modelStatus_->setText(tr("HTTP %1 on /models").arg(status));
            return;
        }
        QByteArray data = reply->readAll();
        try {
            auto j = nlohmann::json::parse(data.toStdString());
            if (!j.contains("models") || !j["models"].is_array()) {
                modelStatus_->setText(tr("/models response missing 'models' array"));
                return;
            }
            QSettings s;
            const QString savedModel = s.value("ai/model").toString();
            modelCombo_->blockSignals(true);
            int count = 0;
            for (const auto& m : j["models"]) {
                if (!m.contains("slug") || !m["slug"].is_string()) continue;
                bool supported  = m.value("supported_in_api", true);
                std::string vis = m.value("visibility", std::string("list"));
                if (!supported || vis != "list") continue;
                QString slug = QString::fromStdString(m["slug"].get<std::string>());
                QString name = m.contains("name") && m["name"].is_string()
                    ? QString::fromStdString(m["name"].get<std::string>()) : slug;
                modelCombo_->addItem(name, slug);
                ++count;
            }
            if (!savedModel.isEmpty()) {
                int idx = modelCombo_->findData(savedModel);
                if (idx < 0) idx = modelCombo_->findText(savedModel);
                if (idx >= 0) modelCombo_->setCurrentIndex(idx);
            }
            modelCombo_->blockSignals(false);
            modelStatus_->setText(count == 0
                ? tr("No models returned.")
                : tr("%1 models").arg(count));
        } catch (const std::exception& e) {
            modelStatus_->setText(tr("Could not parse /models response: ") + e.what());
        }
    });
}

void SettingsDialog::refreshOAuthStatus() {
    if (!oauthService_) return;
    auto status = oauthService_->status();
    if (status.signedIn) {
        QString line = status.email.isEmpty() ? tr("Signed in") : status.email;
        if (!status.planType.isEmpty()) {
            line += QString(" <span style='color:#a6e3a1;'>(%1)</span>").arg(status.planType);
        }
        oauthStatus_->setText(QString("<span style='color:#a6e3a1;'>●</span> %1").arg(line));
        oauthStatus_->setTextFormat(Qt::RichText);
        signInBtn_->hide();
        signOutBtn_->show();
    } else {
        oauthStatus_->setText(tr("Not signed in"));
        oauthStatus_->setTextFormat(Qt::PlainText);
        signInBtn_->show();
        signInBtn_->setEnabled(true);
        signInBtn_->setText(tr("Sign in with ChatGPT"));
        signOutBtn_->hide();
    }
}

void SettingsDialog::onOAuthSignInClicked() {
    if (!oauthService_) return;
    signInBtn_->setEnabled(false);
    signInBtn_->setText(tr("Waiting for browser…"));
    oauthStatus_->setText(tr("Browser opened — complete sign-in there."));
    oauthService_->signIn();
}

void SettingsDialog::onOAuthSignOutClicked() {
    if (!oauthService_) return;
    oauthService_->signOut();
    refreshOAuthStatus();
}

void SettingsDialog::onModelChanged(const QString& /*modelName*/) {
    // No-op until Save; avoids writing on every keystroke.
}

void SettingsDialog::buildAppearancePage(QWidget* page) {
    auto* root = new QVBoxLayout(page);
    root->setContentsMargins(24, 24, 24, 24);
    root->setSpacing(14);

    auto* title = new QLabel(tr("Appearance"), page);
    {
        QFont f = title->font();
        f.setPointSize(14);
        f.setWeight(QFont::DemiBold);
        title->setFont(f);
    }
    root->addWidget(title);

    auto* form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignRight);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(10);

    themeCombo_ = new QComboBox(page);
    themeCombo_->addItem(tr("Light"),         QStringLiteral("Light"));
    themeCombo_->addItem(tr("Dark"),          QStringLiteral("Dark"));
    themeCombo_->addItem(tr("Auto (system)"), QStringLiteral("Auto"));

    const auto current = ThemeManager::instance().mode();
    const int idx = (current == ThemeManager::Mode::Light) ? 0
                  : (current == ThemeManager::Mode::Dark)  ? 1
                                                           : 2;
    themeCombo_->setCurrentIndex(idx);

    connect(themeCombo_, &QComboBox::currentIndexChanged, this, [this](int i) {
        const ThemeManager::Mode mode = (i == 0) ? ThemeManager::Mode::Light
                                      : (i == 1) ? ThemeManager::Mode::Dark
                                                 : ThemeManager::Mode::Auto;
        ThemeManager::instance().setMode(mode, qobject_cast<QApplication*>(QApplication::instance()));
    });

    form->addRow(tr("Theme:"), themeCombo_);
    root->addLayout(form);
    root->addStretch();
}

void SettingsDialog::onSaveClicked() {
    const QString provider = providerCombo_->currentText();
    const QString model    = modelCombo_->currentData().toString().isEmpty()
        ? modelCombo_->currentText()
        : modelCombo_->currentData().toString();
    const QString key      = apiKeyEdit_->text().trimmed();
    const QString endpoint = endpointEdit_->text().trimmed();

    // Persist default provider + model (used by AIChatViewModel on start).
    QSettings s;
    s.setValue("ai/provider", provider);
    if (!model.isEmpty()) s.setValue("ai/model", model);

    // Custom endpoint (may be empty to fall back to provider default).
    if (endpoint.isEmpty()) s.remove(endpointKey(provider));
    else                    s.setValue(endpointKey(provider), endpoint);

    // API key → system keychain.
    if (secretStore_) {
        try {
            if (key.isEmpty()) {
                secretStore_->remove(std::string("ai.apikey.") + provider.toStdString());
            } else {
                secretStore_->saveAPIKey(provider.toStdString(), key.toStdString());
            }
        } catch (const std::exception& e) {
            QMessageBox::critical(this, tr("Save failed"), QString::fromUtf8(e.what()));
            return;
        }
    }

    accept();
}

}  // namespace gridex
