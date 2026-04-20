#pragma once
//
// TablePlusImporter.h
// Gridex
//
// Windows v1 stub. TablePlus on macOS stores connections in a
// plist under com.tinyapp.TablePlus; the Windows build uses a
// different encrypted store (SQLite + DPAPI-wrapped passwords)
// that we haven't reverse-engineered yet. For now we detect
// installation but return an empty list + surface an "UI note"
// string so the dialog can explain why.

#include <vector>
#include <string>
#include "../../Models/Import/ImportedConnection.h"

namespace DBModels { namespace TablePlusImporter {

bool isInstalled();
std::vector<ImportedConnection> importConnections();

// Message for the import dialog when the user clicks TablePlus
// but nothing can be parsed yet. Non-fatal — kept separate from
// the generic ImportError path so the UI can show a friendly hint.
std::wstring windowsSupportNote();

}} // namespace
