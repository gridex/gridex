#pragma once

#include <QDialog>
#include <memory>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;
class QFormLayout;
class QStackedWidget;
class QTimer;
class QWidget;

namespace gridex {

class SecretStore;
class ChatGPTOAuthService;
struct ChatGPTTokenBundle;

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(SecretStore* secretStore, QWidget* parent = nullptr);
    ~SettingsDialog() override;

private slots:
    void onProviderChanged(const QString& provider);
    void onModelChanged(const QString& modelName);
    void onSaveClicked();
    void onKeyEditTimeout();

private:
    void buildUi();
    void buildAiPage(QWidget* page);
    void buildAppearancePage(QWidget* page);
    void loadForProvider(const QString& provider);
    void fetchModels(const QString& provider, const QString& apiKey);

    SecretStore* secretStore_ = nullptr;

    QListWidget*    navList_  = nullptr;
    QStackedWidget* pages_    = nullptr;

    // AI page controls.
    QComboBox*    providerCombo_ = nullptr;
    QComboBox*    modelCombo_    = nullptr;
    QLineEdit*    apiKeyEdit_    = nullptr;
    QLineEdit*    endpointEdit_  = nullptr;
    QLabel*       modelSpinner_  = nullptr;  // small Braille-glyph spinner
    QLabel*       modelStatus_   = nullptr;
    QPushButton*  saveBtn_       = nullptr;

    QTimer* keyDebounce_  = nullptr;
    QTimer* spinnerTimer_ = nullptr;
    int     spinnerFrame_ = 0;

    // ChatGPT OAuth — replaces the API-key row when "ChatGPT" is selected.
    QWidget*     oauthRow_      = nullptr;
    QLabel*      oauthStatus_   = nullptr;
    QPushButton* signInBtn_     = nullptr;
    QPushButton* signOutBtn_    = nullptr;
    QFormLayout* aiForm_       = nullptr;  // owning form so we can hide rows
    QWidget*     apiKeyRow_    = nullptr;  // captured to toggle visibility
    QWidget*     endpointRow_  = nullptr;
    std::unique_ptr<ChatGPTOAuthService> oauthService_;

    void refreshOAuthStatus();
    void onOAuthSignInClicked();
    void onOAuthSignOutClicked();
    void fetchChatGPTModels();

    // Appearance page controls.
    QComboBox* themeCombo_ = nullptr;
};

}  // namespace gridex
