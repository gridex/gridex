#pragma once
//
// MCPToolHelpers.h
// Gridex
//
// Inline helpers shared by every MCP tool implementation. Kept
// header-only to avoid a separate TU for string conversion.

#include <string>
#include <windows.h>

namespace DBModels { namespace MCPToolHelpers {

inline std::string toUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int sz = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                 nullptr, 0, nullptr, nullptr);
    std::string out(sz, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], sz, nullptr, nullptr);
    return out;
}

inline std::wstring fromUtf8(const std::string& s)
{
    if (s.empty()) return {};
    int sz = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out(sz, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], sz);
    return out;
}

}} // namespace
