#pragma once
#include <string>
#include <vector>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Text.h>

namespace DBModels
{
    // Single entry in the Keyboard Shortcuts reference. Keep this struct
    // flat and Sendable — BuildShortcutsView() reads the list to render
    // both the Ctrl+/ dialog and the Settings section (single source of
    // truth, no risk of drift between the two surfaces).
    struct ShortcutEntry
    {
        std::wstring category;   // "Global", "Data Grid", "Query Editor"
        std::wstring action;     // "Commit changes"
        std::wstring keys;       // "Ctrl+S"
    };

    // Full list. When adding a new hotkey in the app, also add it here so
    // it shows up in the reference. Ordering within a category is the
    // display order shown to the user.
    inline std::vector<ShortcutEntry> GetAllShortcuts()
    {
        return {
            // Global
            { L"Global", L"Show this dialog",   L"Ctrl+/"       },
            { L"Global", L"Settings",           L"Ctrl+Shift+P" },
            { L"Global", L"Home",               L"Ctrl+H"       },

            // Data Grid
            { L"Data Grid", L"Commit changes",       L"Ctrl+S"     },
            { L"Data Grid", L"Delete selected row",  L"Delete"     },
            { L"Data Grid", L"Edit cell",            L"Enter"      },
            { L"Data Grid", L"Cancel edit",          L"Esc"        },
            { L"Data Grid", L"Horizontal scroll",    L"Shift+Wheel"},

            // Query Editor
            { L"Query Editor", L"Run query",     L"Ctrl+Enter" },
            { L"Query Editor", L"Autocomplete",  L"Ctrl+Space" },
            { L"Query Editor", L"Close popup",   L"Esc"        },
        };
    }

    // Build a ready-to-embed view showing all shortcuts grouped by
    // category. Used by both the Ctrl+/ ContentDialog and the Settings
    // page so both stay visually identical without duplicated markup.
    inline winrt::Microsoft::UI::Xaml::Controls::StackPanel BuildShortcutsView()
    {
        namespace mux  = winrt::Microsoft::UI::Xaml;
        namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
        namespace muxm = winrt::Microsoft::UI::Xaml::Media;

        muxc::StackPanel root;
        root.Spacing(16.0);

        const auto shortcuts = GetAllShortcuts();

        // Walk the list in order, starting a new section every time the
        // category changes. Relies on GetAllShortcuts() returning entries
        // already grouped by category — simpler than a map and preserves
        // the author-specified ordering within each group.
        std::wstring lastCategory;
        muxc::StackPanel currentSection{ nullptr };

        for (const auto& s : shortcuts)
        {
            if (s.category != lastCategory)
            {
                lastCategory = s.category;

                muxc::TextBlock header;
                header.Text(winrt::hstring(s.category));
                header.FontSize(11.0);
                header.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
                header.Opacity(0.6);
                header.Margin(mux::Thickness{ 0, 0, 0, 4 });
                root.Children().Append(header);

                currentSection = muxc::StackPanel{};
                currentSection.Spacing(6.0);
                root.Children().Append(currentSection);
            }

            // Row: action label (left, stretches) + key cap (right).
            muxc::Grid row;
            muxc::ColumnDefinition labelCol, keyCol;
            labelCol.Width(mux::GridLength{ 1.0, mux::GridUnitType::Star });
            keyCol.Width(mux::GridLength{ 0.0, mux::GridUnitType::Auto });
            row.ColumnDefinitions().Append(labelCol);
            row.ColumnDefinitions().Append(keyCol);

            muxc::TextBlock action;
            action.Text(winrt::hstring(s.action));
            action.FontSize(13.0);
            action.VerticalAlignment(mux::VerticalAlignment::Center);
            muxc::Grid::SetColumn(action, 0);
            row.Children().Append(action);

            // Key cap: rounded rect with monospace text so "Ctrl+S" reads
            // like a keyboard key and not a word in a sentence.
            muxc::Border keyCap;
            keyCap.Padding(mux::Thickness{ 8, 2, 8, 2 });
            keyCap.CornerRadius(mux::CornerRadius{ 4, 4, 4, 4 });
            keyCap.Background(muxm::SolidColorBrush(
                winrt::Windows::UI::ColorHelper::FromArgb(40, 128, 128, 128)));
            keyCap.BorderBrush(muxm::SolidColorBrush(
                winrt::Windows::UI::ColorHelper::FromArgb(60, 128, 128, 128)));
            keyCap.BorderThickness(mux::Thickness{ 1, 1, 1, 1 });

            muxc::TextBlock keysText;
            keysText.Text(winrt::hstring(s.keys));
            keysText.FontSize(11.0);
            keysText.FontFamily(muxm::FontFamily(L"Cascadia Code,Consolas,monospace"));
            keysText.VerticalAlignment(mux::VerticalAlignment::Center);
            keyCap.Child(keysText);

            muxc::Grid::SetColumn(keyCap, 1);
            row.Children().Append(keyCap);

            if (currentSection) currentSection.Children().Append(row);
        }

        return root;
    }
}
