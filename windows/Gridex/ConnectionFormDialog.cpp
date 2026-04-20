#include "pch.h"
#include "xaml-includes.h"
#include <winrt/Windows.UI.h>
#include <winrt/Microsoft.Windows.Storage.Pickers.h>
#include "ConnectionFormDialog.h"
#include "Models/AppSettings.h"
#include "App.xaml.h"
#if __has_include("ConnectionFormDialog.g.cpp")
#include "ConnectionFormDialog.g.cpp"
#endif

namespace winrt::Gridex::implementation
{
    namespace mux = winrt::Microsoft::UI::Xaml;
    namespace muxc = winrt::Microsoft::UI::Xaml::Controls;

    ConnectionFormDialog::ConnectionFormDialog()
    {
        InitializeComponent();
        PopulateGroupCombo();
    }

    void ConnectionFormDialog::PopulateGroupCombo()
    {
        auto combo = GroupCombo();
        combo.Items().Clear();
        // First item: no group
        combo.Items().Append(winrt::box_value(winrt::hstring(L"(None)")));
        // Load groups from settings
        auto s = DBModels::AppSettings::Load();
        for (const auto& g : s.connectionGroups)
            combo.Items().Append(winrt::box_value(winrt::hstring(g)));
        combo.SelectedIndex(0);
    }

    void ConnectionFormDialog::SetDatabaseType(DBModels::DatabaseType type)
    {
        dbType_ = type;
        PortInput().Value(static_cast<double>(DBModels::DatabaseTypeDefaultPort(type)));
        UpdateFieldVisibility();
        BuildColorTagPicker();
    }

    void ConnectionFormDialog::UpdateFieldVisibility()
    {
        bool isSqlite = (dbType_ == DBModels::DatabaseType::SQLite);
        bool isRedis = (dbType_ == DBModels::DatabaseType::Redis);
        bool isMongo = (dbType_ == DBModels::DatabaseType::MongoDB);
        auto show = mux::Visibility::Visible;
        auto hide = mux::Visibility::Collapsed;

        // MongoDB URI + Options section (shown only for MongoDB)
        UriLabel().Visibility(isMongo ? show : hide);
        MongoUriSection().Visibility(isMongo ? show : hide);

        // Network fields hidden only for SQLite
        HostLabel().Visibility(isSqlite ? hide : show);
        HostRow().Visibility(isSqlite ? hide : show);

        // User row hidden for SQLite AND Redis (Redis only uses password)
        UserLabel().Visibility((isSqlite || isRedis) ? hide : show);
        UserRow().Visibility((isSqlite || isRedis) ? hide : show);

        // Password row hidden only for SQLite (Redis can have password)
        PasswordLabel().Visibility(isSqlite ? hide : show);
        PasswordRow().Visibility(isSqlite ? hide : show);

        // SSL keys hidden for SQLite, Redis, and MongoDB (MongoDB TLS via URI options)
        SslKeysLabel().Visibility((isSqlite || isRedis || isMongo) ? hide : show);
        SslKeysRow().Visibility((isSqlite || isRedis || isMongo) ? hide : show);

        // SSH toggle hidden for SQLite AND Redis
        SshToggle().Visibility((isSqlite || isRedis) ? hide : show);

        // Show SQLite file section only for SQLite
        FileLabel().Visibility(isSqlite ? show : hide);
        SqliteFileSection().Visibility(isSqlite ? show : hide);

        // Adjust Database field placeholder per type
        if (isRedis)
            DatabaseInput().PlaceholderText(L"0");
        else if (isMongo)
            DatabaseInput().PlaceholderText(L"mydb (or leave empty for URI)");
        else if (dbType_ == DBModels::DatabaseType::MSSQLServer)
            DatabaseInput().PlaceholderText(L"master");
        else if (!isSqlite)
            DatabaseInput().PlaceholderText(L"mydb");
    }

    void ConnectionFormDialog::BuildColorTagPicker()
    {
        auto panel = ColorTagPicker();
        panel.Children().Clear();

        for (const auto& info : DBModels::GetColorTags())
        {
            auto swatch = muxc::Button();
            swatch.Width(28);
            swatch.Height(20);
            swatch.CornerRadius(mux::CornerRadiusHelper::FromUniformRadius(4));
            swatch.Padding(mux::ThicknessHelper::FromUniformLength(0));
            auto color = winrt::Windows::UI::ColorHelper::FromArgb(255, info.r, info.g, info.b);
            swatch.Background(mux::Media::SolidColorBrush(color));

            auto tag = info.tag;
            swatch.Click([this, tag](auto const&, auto const&) { selectedColor_ = tag; });
            panel.Children().Append(swatch);
        }
    }

    std::wstring ConnectionFormDialog::GetPassword()
    {
        if (auto pwd = PasswordInput())
            return std::wstring(pwd.Password());
        return L"";
    }

    DBModels::ConnectionConfig ConnectionFormDialog::GetConnectionConfig()
    {
        DBModels::ConnectionConfig config;
        config.name = std::wstring(NameInput().Text());
        config.databaseType = dbType_;

        // Read selected group (index 0 = "(None)" = no group)
        int groupIdx = GroupCombo().SelectedIndex();
        if (groupIdx > 0)
        {
            auto item = GroupCombo().Items().GetAt(groupIdx);
            config.group = std::wstring(winrt::unbox_value<winrt::hstring>(item));
        }
        config.host = std::wstring(HostInput().Text());
        config.port = static_cast<uint16_t>(PortInput().Value());
        config.username = std::wstring(UserInput().Text());
        config.database = std::wstring(DatabaseInput().Text());
        config.colorTag = selectedColor_;
        auto sslIdx = SslModeInput().SelectedIndex();
        config.sslMode = static_cast<DBModels::SSLMode>(sslIdx);
        config.sslEnabled = (sslIdx != 1);
        // MCP access mode — 0=Locked / 1=ReadOnly / 2=ReadWrite,
        // matches MCPConnectionMode enum ordering.
        const auto mcpIdx = McpModeInput().SelectedIndex();
        config.mcpMode = static_cast<DBModels::MCPConnectionMode>(
            mcpIdx < 0 ? 0 : mcpIdx);
        if (dbType_ == DBModels::DatabaseType::SQLite)
            config.filePath = std::wstring(FilePathInput().Text());
        if (dbType_ == DBModels::DatabaseType::MongoDB)
        {
            config.connectionUri = std::wstring(UriInput().Text());
            config.mongoOptions = std::wstring(MongoOptionsInput().Text());
        }
        if (SshToggle().IsChecked().Value())
        {
            DBModels::SSHTunnelConfig ssh;
            ssh.host = std::wstring(SshHostInput().Text());
            ssh.port = static_cast<uint16_t>(SshPortInput().Value());
            ssh.username = std::wstring(SshUserInput().Text());
            ssh.password = std::wstring(SshPasswordInput().Password());
            ssh.authMethod = static_cast<DBModels::SSHAuthMethod>(SshAuthInput().SelectedIndex());
            ssh.keyPath = std::wstring(SshKeyInput().Text());
            config.sshConfig = ssh;
        }
        return config;
    }

    void ConnectionFormDialog::SetConnectionConfig(const DBModels::ConnectionConfig& config)
    {
        // Set database type FIRST so it doesn't overwrite user fields with defaults
        SetDatabaseType(config.databaseType);

        NameInput().Text(winrt::hstring(config.name));

        // Select group in combo (or add if not in settings yet)
        if (!config.group.empty())
        {
            auto combo = GroupCombo();
            int foundIdx = -1;
            for (uint32_t i = 0; i < combo.Items().Size(); i++)
            {
                auto item = winrt::unbox_value<winrt::hstring>(combo.Items().GetAt(i));
                if (std::wstring(item) == config.group) { foundIdx = static_cast<int>(i); break; }
            }
            if (foundIdx < 0)
            {
                combo.Items().Append(winrt::box_value(winrt::hstring(config.group)));
                foundIdx = static_cast<int>(combo.Items().Size()) - 1;
            }
            combo.SelectedIndex(foundIdx);
        }

        HostInput().Text(winrt::hstring(config.host));
        PortInput().Value(static_cast<double>(config.port));
        UserInput().Text(winrt::hstring(config.username));
        PasswordInput().Password(winrt::hstring(config.password));
        DatabaseInput().Text(winrt::hstring(config.database));
        SslModeInput().SelectedIndex(static_cast<int>(config.sslMode));
        McpModeInput().SelectedIndex(static_cast<int>(config.mcpMode));
        FilePathInput().Text(winrt::hstring(config.filePath));
        UriInput().Text(winrt::hstring(config.connectionUri));
        MongoOptionsInput().Text(winrt::hstring(config.mongoOptions));
        selectedColor_ = config.colorTag;
        if (config.sshConfig.has_value())
        {
            SshToggle().IsChecked(true);
            SshSection().Visibility(mux::Visibility::Visible);
            SshHostInput().Text(winrt::hstring(config.sshConfig->host));
            SshPortInput().Value(static_cast<double>(config.sshConfig->port));
            SshUserInput().Text(winrt::hstring(config.sshConfig->username));
            SshPasswordInput().Password(winrt::hstring(config.sshConfig->password));
            SshAuthInput().SelectedIndex(static_cast<int>(config.sshConfig->authMethod));
            SshKeyInput().Text(winrt::hstring(config.sshConfig->keyPath));
        }
    }

    void ConnectionFormDialog::SshToggle_Click(
        winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
    {
        bool show = SshToggle().IsChecked().Value();
        SshSection().Visibility(show ? mux::Visibility::Visible : mux::Visibility::Collapsed);
    }

    void ConnectionFormDialog::SshAuth_Changed(
        winrt::Windows::Foundation::IInspectable const&, muxc::SelectionChangedEventArgs const&)
    {
        // Guard: event can fire during InitializeComponent before elements exist
        auto pwd = SshPasswordInput();
        auto key = SshKeySection();
        if (!pwd || !key) return;

        auto idx = SshAuthInput().SelectedIndex();
        pwd.Visibility((idx == 0 || idx == 2) ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        key.Visibility((idx == 1 || idx == 2) ? mux::Visibility::Visible : mux::Visibility::Collapsed);
    }

    winrt::fire_and_forget ConnectionFormDialog::BrowseFile_Click(
        winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
    {
        auto lifetime = get_strong();
        try
        {
            winrt::Microsoft::UI::WindowId windowId{
                reinterpret_cast<uint64_t>(
                    winrt::Gridex::implementation::App::MainHwnd) };
            winrt::Microsoft::Windows::Storage::Pickers::FileOpenPicker picker(windowId);
            picker.FileTypeFilter().Append(L".db");
            picker.FileTypeFilter().Append(L".sqlite");
            picker.FileTypeFilter().Append(L".sqlite3");
            picker.FileTypeFilter().Append(L".db3");

            auto pickedFile = co_await picker.PickSingleFileAsync();
            if (!pickedFile) co_return;
            FilePathInput().Text(pickedFile.Path());
        }
        catch (...) {}
    }

    winrt::fire_and_forget ConnectionFormDialog::BrowseSshKey_Click(
        winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
    {
        auto lifetime = get_strong();
        try
        {
            winrt::Microsoft::UI::WindowId windowId{
                reinterpret_cast<uint64_t>(
                    winrt::Gridex::implementation::App::MainHwnd) };
            winrt::Microsoft::Windows::Storage::Pickers::FileOpenPicker picker(windowId);
            picker.FileTypeFilter().Append(L".pem");
            picker.FileTypeFilter().Append(L".ppk");
            picker.FileTypeFilter().Append(L".key");

            auto pickedFile = co_await picker.PickSingleFileAsync();
            if (!pickedFile) co_return;
            SshKeyInput().Text(pickedFile.Path());
        }
        catch (...) {}
    }

    void ConnectionFormDialog::SaveButton_Click(
        winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
    {
        if (OnSave) OnSave();
    }
    void ConnectionFormDialog::TestButton_Click(
        winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
    {
        if (OnTest) OnTest();
    }
    void ConnectionFormDialog::ConnectButton_Click(
        winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
    {
        if (OnConnect) OnConnect();
    }
}
