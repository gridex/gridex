//
// TablePlusImporter.cpp
//
// Windows v1 stub. Detects TablePlus via its %APPDATA% folder but
// doesn't parse the store yet — different encrypted format from
// mac, needs reverse engineering.

#include "TablePlusImporter.h"

#include <windows.h>
#include <shlobj.h>

namespace DBModels { namespace TablePlusImporter {

namespace {
    std::wstring tablePlusDataDir()
    {
        wchar_t* p = nullptr;
        if (SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p) != S_OK)
            return {};
        std::wstring out(p);
        CoTaskMemFree(p);
        return out + L"\\com.tinyapp.TablePlus";
    }
}

bool isInstalled()
{
    const auto d = tablePlusDataDir();
    if (d.empty()) return false;
    return GetFileAttributesW(d.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::vector<ImportedConnection> importConnections()
{
    // Parsing the Windows TablePlus store is not implemented yet —
    // see windowsSupportNote() for the UI-facing explanation.
    return {};
}

std::wstring windowsSupportNote()
{
    return L"TablePlus import is macOS-only for now. On Windows, export your connections "
           L"from TablePlus and use the Navicat NCX / DBeaver path instead.";
}

}} // namespace
