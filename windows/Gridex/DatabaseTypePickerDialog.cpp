#include "pch.h"
#include "xaml-includes.h"
#include "DatabaseTypePickerDialog.h"
#if __has_include("DatabaseTypePickerDialog.g.cpp")
#include "DatabaseTypePickerDialog.g.cpp"
#endif

namespace winrt::Gridex::implementation
{
    DatabaseTypePickerDialog::DatabaseTypePickerDialog()
    {
        InitializeComponent();
    }

    void DatabaseTypePickerDialog::TypeButton_Click(
        winrt::Windows::Foundation::IInspectable const& sender,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        // Button.Tag contains the database type string
        if (auto btn = sender.try_as<winrt::Microsoft::UI::Xaml::Controls::Button>())
        {
            auto tag = winrt::unbox_value<winrt::hstring>(btn.Tag());
            DBModels::DatabaseType selectedType = DBModels::DatabaseType::PostgreSQL;
            if (tag == L"MySQL") selectedType = DBModels::DatabaseType::MySQL;
            else if (tag == L"SQLite") selectedType = DBModels::DatabaseType::SQLite;
            else if (tag == L"Redis") selectedType = DBModels::DatabaseType::Redis;
            else if (tag == L"MongoDB") selectedType = DBModels::DatabaseType::MongoDB;
            else if (tag == L"MSSQLServer") selectedType = DBModels::DatabaseType::MSSQLServer;
            else if (tag == L"ClickHouse") selectedType = DBModels::DatabaseType::ClickHouse;
            if (OnTypeSelected) OnTypeSelected(selectedType);
        }
    }
}
