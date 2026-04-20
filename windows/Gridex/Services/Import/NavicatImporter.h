#pragma once
//
// NavicatImporter.h
// Gridex
//
// Imports Navicat connections from a user-picked .ncx export file.
// Mac parses the same XML shape; on Windows we additionally skip
// Blowfish password decryption (CommonCrypto-only) and leave the
// password empty — the user re-enters after import.

#include <vector>
#include <string>
#include "../../Models/Import/ImportedConnection.h"

namespace DBModels { namespace NavicatImporter {

// Always true — the feature is gated by the user picking an .ncx.
bool isInstalled();

// Parse the .ncx file at `ncxPath` and return the embedded
// connections. Passwords are left empty on Windows.
std::vector<ImportedConnection> importFromNCX(const std::wstring& ncxPath);

}} // namespace
