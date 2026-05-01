#include "pch.h"
#include "xaml-includes.h"
#include <winrt/Windows.UI.h>
#include "ConnectionCard.h"
#if __has_include("ConnectionCard.g.cpp")
#include "ConnectionCard.g.cpp"
#endif

namespace winrt::Gridex::implementation
{
    ConnectionCard::ConnectionCard()
    {
        InitializeComponent();
    }

    void ConnectionCard::SetConnection(const DBModels::ConnectionConfig& config)
    {
        connectionId_ = config.id;
        ConnNameText().Text(winrt::hstring(config.name));
        SubtitleText().Text(winrt::hstring(config.subtitle()));
        DbTypeIcon().Glyph(winrt::hstring(DBModels::DatabaseTypeGlyph(config.databaseType)));
        DbTypeLabel().Text(winrt::hstring(DBModels::DatabaseTypeDisplayName(config.databaseType)));

        if (config.colorTag.has_value())
        {
            auto& info = DBModels::GetColorTagInfo(config.colorTag.value());
            auto color = winrt::Windows::UI::ColorHelper::FromArgb(255, info.r, info.g, info.b);
            ColorTagBar().Background(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(color));
            TagBadge().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
            TagBadge().Background(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(color));
            TagBadgeText().Text(winrt::hstring(info.hint));
        }
        else
        {
            auto gray = winrt::Windows::UI::ColorHelper::FromArgb(255, 128, 128, 128);
            ColorTagBar().Background(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(gray));
            TagBadge().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        }

        // Environment tag badge — picks a recognizable colour per
        // well-known tag so prod is visually distinct from dev/staging
        // at a glance. Unknown tags fall back to slate gray.
        if (!config.tag.empty())
        {
            winrt::Windows::UI::Color bg;
            const auto& t = config.tag;
            if      (t == L"Production")  bg = winrt::Windows::UI::ColorHelper::FromArgb(255, 220, 38, 38);   // red
            else if (t == L"Staging")     bg = winrt::Windows::UI::ColorHelper::FromArgb(255, 234, 88, 12);   // orange
            else if (t == L"Development") bg = winrt::Windows::UI::ColorHelper::FromArgb(255, 22, 163, 74);   // green
            else if (t == L"Testing")     bg = winrt::Windows::UI::ColorHelper::FromArgb(255, 124, 58, 237);  // purple
            else if (t == L"Local")       bg = winrt::Windows::UI::ColorHelper::FromArgb(255, 75, 85, 99);    // slate
            else                          bg = winrt::Windows::UI::ColorHelper::FromArgb(255, 100, 100, 100); // gray fallback
            EnvTagBadge().Background(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(bg));
            EnvTagBadgeText().Text(winrt::hstring(config.tag));
            EnvTagBadge().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
        }
        else
        {
            EnvTagBadge().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        }
    }

    void ConnectionCard::ContextConnect_Click(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        // Handled by ListView ItemClick in HomePage
    }

    void ConnectionCard::ContextEdit_Click(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (OnEdit)
            OnEdit(connectionId_);
    }

    void ConnectionCard::ContextDelete_Click(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (OnDelete)
            OnDelete(connectionId_);
    }
}
