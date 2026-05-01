#include "pch.h"
#include "xaml-includes.h"
#include <winrt/Microsoft.UI.Xaml.Interop.h>
#include <shellapi.h>
#include "SettingsPage.h"
#include "Models/AppSettings.h"
#include "Models/AiService.h"
#include "Services/ChatGPTOAuth/ChatGPTOAuthService.h"
#include "Models/ShortcutsCatalog.h"
#include "GridexVersion.h"
#include "UpdateService.h"
#include <thread>
#if __has_include("SettingsPage.g.cpp")
#include "SettingsPage.g.cpp"
#endif

namespace winrt::Gridex::implementation
{
    namespace mux  = winrt::Microsoft::UI::Xaml;
    namespace muxc = winrt::Microsoft::UI::Xaml::Controls;

    SettingsPage::SettingsPage()
    {
        InitializeComponent();

        this->Loaded([this](winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
        {
            // Stamp the build version into the About section. GRIDEX_VERSION
            // comes from GridexVersion.h which pulls the CI-generated header
            // when present (release builds) or falls back to "0.0.0-dev".
            VersionText().Text(winrt::hstring(L"Version " GRIDEX_VERSION));

            // Populate the Keyboard Shortcuts section from the shared
            // catalog — same builder feeds the Ctrl+/ dialog, so edits
            // to ShortcutsCatalog.h propagate to both surfaces.
            ShortcutsHost().Content(DBModels::BuildShortcutsView());

            // Load persisted settings into UI
            auto s = DBModels::AppSettings::Load();
            ThemeCombo().SelectedIndex(s.themeIndex);
            AiProviderCombo().SelectedIndex(s.aiProviderIndex);
            ApiKeyBox().Password(winrt::hstring(s.aiApiKey));
            // ModelBox is an editable ComboBox. Set .Text so any
            // previously-saved model (including free-typed custom
            // names) round-trips, even before Refresh populates the
            // dropdown from the provider API.
            ModelBox().Text(winrt::hstring(s.aiModel));
            OllamaEndpointBox().Text(winrt::hstring(s.ollamaEndpoint));
            EditorFontSizeBox().Value(static_cast<double>(s.editorFontSize));
            RowLimitBox().Value(static_cast<double>(s.rowLimit));

            // Helper: show/hide provider-specific panels (API key /
            // OAuth sign-in / Ollama endpoint) based on selection.
            // Indices: 0 Anthropic, 1 OpenAI, 2 Ollama, 3 Gemini,
            // 4 OpenRouter, 5 ChatGPT.
            auto updateProviderUI = [this]()
            {
                int idx = AiProviderCombo().SelectedIndex();
                bool isChatGPT = (idx == 5);
                bool isOllama  = (idx == 2);

                ApiKeyPanel().Visibility(isChatGPT
                    ? mux::Visibility::Collapsed
                    : mux::Visibility::Visible);
                SignInPanel().Visibility(isChatGPT
                    ? mux::Visibility::Visible
                    : mux::Visibility::Collapsed);
                // Ollama endpoint is only meaningful for the Ollama
                // provider — hide it for everyone else so the form
                // doesn't carry stale state across provider switches.
                OllamaEndpointPanel().Visibility(isOllama
                    ? mux::Visibility::Visible
                    : mux::Visibility::Collapsed);

                if (isChatGPT)
                {
                    auto& svc = ChatGPT::OAuthService::Instance();
                    if (svc.IsSignedIn())
                    {
                        auto email = svc.CurrentEmail();
                        std::wstring statusText = email.empty()
                            ? L"Signed in"
                            : L"Signed in as " + email;
                        ChatGPTStatusText().Text(winrt::hstring(statusText));
                        ChatGPTSignInBtn().Visibility(mux::Visibility::Collapsed);
                        ChatGPTSignOutBtn().Visibility(mux::Visibility::Visible);
                    }
                    else
                    {
                        ChatGPTStatusText().Text(L"Not signed in");
                        ChatGPTSignInBtn().Visibility(mux::Visibility::Visible);
                        ChatGPTSignOutBtn().Visibility(mux::Visibility::Collapsed);
                    }
                }
            };

            // Apply on load
            updateProviderUI();

            // Re-apply when user changes the provider ComboBox
            AiProviderCombo().SelectionChanged(
                [this, updateProviderUI](winrt::Windows::Foundation::IInspectable const&,
                                         muxc::SelectionChangedEventArgs const&)
                {
                    updateProviderUI();
                });

            // Sign in button
            ChatGPTSignInBtn().Click(
                [this, updateProviderUI](winrt::Windows::Foundation::IInspectable const&,
                                         mux::RoutedEventArgs const&)
                {
                    ChatGPTSignInBtn().IsEnabled(false);
                    ChatGPTStatusText().Text(L"Opening browser…");

                    auto dispatcher = this->DispatcherQueue();
                    ChatGPT::OAuthService::Instance().SignIn(
                        [this, dispatcher, updateProviderUI](bool success, std::wstring emailOrError)
                        {
                            dispatcher.TryEnqueue([this, success, emailOrError, updateProviderUI, dispatcher]()
                            {
                                ChatGPTSignInBtn().IsEnabled(true);
                                if (!success)
                                {
                                    ChatGPTStatusText().Text(
                                        winrt::hstring(L"Sign-in failed: " + emailOrError));
                                    return;
                                }
                                updateProviderUI();

                                // Stale model from a previous provider
                                // doesn't apply to ChatGPT — wipe the
                                // field and fetch the live model list
                                // from /backend-api/codex/models for
                                // this account.
                                ModelBox().Items().Clear();
                                ModelBox().Text(L"");
                                ModelFetchStatusText().Visibility(mux::Visibility::Visible);
                                ModelFetchStatusText().Text(L"Fetching models…");

                                DBModels::AiConfig cfg;
                                cfg.provider = DBModels::AiProvider::ChatGPT;
                                std::thread([cfg, dispatcher, weak = get_weak()]()
                                {
                                    auto res = DBModels::AiService::FetchModels(cfg);
                                    dispatcher.TryEnqueue([weak, res]()
                                    {
                                        auto self = weak.get();
                                        if (!self) return;
                                        if (!res.success)
                                        {
                                            self->ModelFetchStatusText().Text(
                                                winrt::hstring(L"Fetch failed: " + res.errorMessage));
                                            return;
                                        }
                                        for (const auto& m : res.models)
                                            self->ModelBox().Items().Append(
                                                winrt::box_value(winrt::hstring(m)));
                                        if (!res.models.empty())
                                            self->ModelBox().Text(winrt::hstring(res.models.front()));
                                        self->ModelFetchStatusText().Text(
                                            winrt::hstring(L"Loaded "
                                                + std::to_wstring(res.models.size())
                                                + L" model(s)"));
                                    });
                                }).detach();
                            });
                        });
                });

            // Sign out button
            ChatGPTSignOutBtn().Click(
                [this, updateProviderUI](winrt::Windows::Foundation::IInspectable const&,
                                         mux::RoutedEventArgs const&)
                {
                    ChatGPT::OAuthService::Instance().SignOut();
                    updateProviderUI();
                });

            // Refresh models: hit the provider's list endpoint on a
            // worker thread, populate ModelBox items on success. Keep
            // the user's current text selection after repopulating so
            // free-typed custom names (for OpenAI-compatible proxies
            // like DeepSeek, Groq, etc.) survive the round-trip.
            RefreshModelsBtn().Click(
                [this](winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
                {
                    DBModels::AiConfig cfg;
                    cfg.provider = static_cast<DBModels::AiProvider>(AiProviderCombo().SelectedIndex());
                    cfg.apiKey   = std::wstring(ApiKeyBox().Password());
                    cfg.model    = std::wstring(ModelBox().Text());
                    cfg.ollamaEndpoint = std::wstring(OllamaEndpointBox().Text());

                    RefreshModelsBtn().IsEnabled(false);
                    ModelFetchStatusText().Visibility(mux::Visibility::Visible);
                    ModelFetchStatusText().Text(L"Fetching models…");

                    auto dispatcher = this->DispatcherQueue();
                    std::thread([cfg, dispatcher,
                                 weak = get_weak()]()
                    {
                        auto res = DBModels::AiService::FetchModels(cfg);
                        dispatcher.TryEnqueue([weak, res]()
                        {
                            auto self = weak.get();
                            if (!self) return;

                            self->RefreshModelsBtn().IsEnabled(true);

                            if (!res.success)
                            {
                                self->ModelFetchStatusText().Text(
                                    winrt::hstring(L"Fetch failed: " + res.errorMessage));
                                return;
                            }

                            // Preserve whatever the user typed / had
                            // selected before; reassign after populating
                            // so the TextBox portion of the editable
                            // ComboBox keeps its value.
                            auto keep = std::wstring(self->ModelBox().Text());

                            self->ModelBox().Items().Clear();
                            for (const auto& m : res.models)
                                self->ModelBox().Items().Append(
                                    winrt::box_value(winrt::hstring(m)));

                            // If the kept value matches one of the
                            // freshly-fetched models, select it so the
                            // ComboBox shows the item highlight; else
                            // just leave the custom text in place.
                            bool matched = false;
                            for (uint32_t i = 0; i < self->ModelBox().Items().Size(); ++i)
                            {
                                auto item = winrt::unbox_value<winrt::hstring>(
                                    self->ModelBox().Items().GetAt(i));
                                if (std::wstring(item) == keep)
                                {
                                    self->ModelBox().SelectedIndex(static_cast<int32_t>(i));
                                    matched = true;
                                    break;
                                }
                            }
                            if (!matched) self->ModelBox().Text(winrt::hstring(keep));

                            self->ModelFetchStatusText().Text(
                                winrt::hstring(L"Loaded " +
                                    std::to_wstring(res.models.size()) + L" models."));
                        });
                    }).detach();
                });

            ThemeCombo().SelectionChanged(
                [this](winrt::Windows::Foundation::IInspectable const&, muxc::SelectionChangedEventArgs const&)
                {
                    // Inline: SelectionChangedEventArgs cannot be default-constructed
                    int index = ThemeCombo().SelectedIndex();
                    auto root = this->XamlRoot();
                    if (!root) return;
                    auto fe = root.Content().try_as<mux::FrameworkElement>();
                    if (!fe) return;
                    switch (index)
                    {
                    case 0: fe.RequestedTheme(mux::ElementTheme::Default); break;
                    case 1: fe.RequestedTheme(mux::ElementTheme::Light); break;
                    case 2: fe.RequestedTheme(mux::ElementTheme::Dark); break;
                    }
                });
            SaveAiButton().Click(
                [this](winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
                {
                    SaveAiSettings_Click({}, {});
                });

            // View Open Source Licenses — open THIRD-PARTY-NOTICES.txt
            // bundled in Assets/ via the system default text editor.
            ViewLicensesButton().Click(
                [](winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
                {
                    wchar_t exePath[MAX_PATH] = {};
                    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
                    std::wstring exeDir(exePath);
                    auto slash = exeDir.find_last_of(L"\\/");
                    if (slash != std::wstring::npos) exeDir = exeDir.substr(0, slash);
                    std::wstring noticesPath = exeDir + L"\\Assets\\THIRD-PARTY-NOTICES.txt";
                    ShellExecuteW(nullptr, L"open", noticesPath.c_str(),
                                  nullptr, nullptr, SW_SHOWNORMAL);
                });

            // Check for Updates — hit the R2 feed via Velopack SDK. Runs
            // on a background thread; results are marshalled back to the
            // UI via the page's DispatcherQueue.
            CheckUpdateButton().Click(
                [this](winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
                {
                    CheckUpdateButton().IsEnabled(false);
                    UpdateStatusText().Text(L"Checking...");

                    auto dispatcher = this->DispatcherQueue();
                    auto xamlRoot   = this->XamlRoot();

                    ::Gridex::CheckForUpdateAsync(
                        [this, dispatcher, xamlRoot](::Gridex::UpdateCheckResult r)
                        {
                            dispatcher.TryEnqueue([this, r, xamlRoot, dispatcher]()
                            {
                                CheckUpdateButton().IsEnabled(true);

                                if (!r.errorMessage.empty())
                                {
                                    UpdateStatusText().Text(winrt::hstring(L"Error: " + r.errorMessage));
                                    return;
                                }
                                if (!r.hasUpdate)
                                {
                                    UpdateStatusText().Text(
                                        winrt::hstring(L"You're on the latest version (" + r.currentVersion + L")."));
                                    return;
                                }

                                // Confirm with user before downloading.
                                std::wstring msg = L"Current: " + r.currentVersion +
                                                   L"\nNew:     " + r.newVersion +
                                                   L"\n\nDownload and install now? The app will restart.";
                                UpdateStatusText().Text(
                                    winrt::hstring(L"Update available: " + r.newVersion));

                                muxc::ContentDialog dlg;
                                dlg.Title(winrt::box_value(winrt::hstring(L"Update Available")));
                                dlg.Content(winrt::box_value(winrt::hstring(msg)));
                                dlg.PrimaryButtonText(L"Install");
                                dlg.CloseButtonText(L"Later");
                                dlg.DefaultButton(muxc::ContentDialogButton::Primary);
                                dlg.XamlRoot(xamlRoot);

                                auto op = dlg.ShowAsync();
                                op.Completed([this, dispatcher](
                                    winrt::Windows::Foundation::IAsyncOperation<muxc::ContentDialogResult> const& asyncOp,
                                    winrt::Windows::Foundation::AsyncStatus)
                                {
                                    if (asyncOp.GetResults() != muxc::ContentDialogResult::Primary) return;

                                    dispatcher.TryEnqueue([this]()
                                    {
                                        CheckUpdateButton().IsEnabled(false);
                                        UpdateStatusText().Text(L"Downloading update...");
                                    });

                                    ::Gridex::DownloadAndApplyAsync(
                                        [this, dispatcher](std::wstring status)
                                        {
                                            dispatcher.TryEnqueue([this, status]()
                                            {
                                                UpdateStatusText().Text(winrt::hstring(status));
                                            });
                                        },
                                        [this, dispatcher](std::wstring err)
                                        {
                                            dispatcher.TryEnqueue([this, err]()
                                            {
                                                CheckUpdateButton().IsEnabled(true);
                                                UpdateStatusText().Text(winrt::hstring(L"Update failed: " + err));
                                            });
                                        });
                                });
                            });
                        });
                });

            // Back button — navigate back to the page user came from
            BackBtn().Click(
                [this](winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
                {
                    muxc::Frame frame{ nullptr };
                    auto fe = this->try_as<mux::FrameworkElement>();
                    while (fe)
                    {
                        if (auto f = fe.try_as<muxc::Frame>()) { frame = f; break; }
                        fe = fe.Parent().try_as<mux::FrameworkElement>();
                    }
                    if (!frame) frame = this->Frame();
                    if (!frame) return;

                    // Read stored previous page (defaults to HomePage)
                    auto saved = DBModels::AppSettings::Load();
                    std::wstring targetPage = saved.lastPageBeforeSettings;
                    if (targetPage.empty()) targetPage = L"Gridex.HomePage";

                    winrt::Windows::UI::Xaml::Interop::TypeName pageType;
                    pageType.Name = winrt::hstring(targetPage);
                    pageType.Kind = winrt::Windows::UI::Xaml::Interop::TypeKind::Metadata;
                    frame.Navigate(pageType);
                });
        });
    }

    void SettingsPage::SaveAiSettings_Click(
        winrt::Windows::Foundation::IInspectable const&,
        mux::RoutedEventArgs const&)
    {
        // Load current (preserve fields not touched here), then overwrite from UI
        auto s = DBModels::AppSettings::Load();
        s.themeIndex         = ThemeCombo().SelectedIndex();
        s.aiProviderIndex    = AiProviderCombo().SelectedIndex();
        s.aiApiKey           = std::wstring(ApiKeyBox().Password());
        // ModelBox is an editable ComboBox; .Text returns whatever the
        // user has in the edit field — a picked list item or a freely
        // typed custom model name, both save the same way.
        s.aiModel            = std::wstring(ModelBox().Text());
        s.ollamaEndpoint     = std::wstring(OllamaEndpointBox().Text());
        s.editorFontSize     = static_cast<int>(EditorFontSizeBox().Value());
        s.rowLimit           = static_cast<int>(RowLimitBox().Value());

        bool ok = s.Save();

        muxc::ContentDialog dialog;
        dialog.Title(winrt::box_value(winrt::hstring(
            ok ? L"Settings Saved" : L"Save Failed")));
        dialog.Content(winrt::box_value(winrt::hstring(
            ok ? L"Settings have been saved."
               : L"Could not write to settings file.")));
        dialog.CloseButtonText(L"OK");
        dialog.XamlRoot(this->XamlRoot());
        dialog.ShowAsync();
    }
}
