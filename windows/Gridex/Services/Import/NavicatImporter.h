#pragma once
//
// NavicatImporter.h
// Gridex
//
// Windows Navicat stores every connection in the registry as
// HKCU\Software\PremiumSoft\Navicat<PRODUCT>\Servers\<name>, one
// subkey per connection. We scan the documented product hives
// (Premium / PG / MARIADB / MSSQL / MONGODB / REDIS / SQLite /
// Oracle / Snowflake / DAMENG) and return every detected entry.
//
// importFromNCX() stays as a fallback for users who keep their
// connections in a .ncx export file instead of letting Navicat
// persist to registry (some team / enterprise setups do this).
//
// Passwords use Blowfish (key "3DC5CA39") which isn't exposed by
// Windows BCrypt and sits behind OpenSSL 3's legacy provider; we
// skip decryption — user re-enters after import.

#include <vector>
#include <string>
#include "../../Models/Import/ImportedConnection.h"

namespace DBModels { namespace NavicatImporter {

// True when any Navicat registry hive is present.
bool isInstalled();

// Walk the known Navicat\*\Servers hives and return one row per
// connection subkey. Passwords stay empty — see header.
std::vector<ImportedConnection> importConnections();

// Parse a user-picked .ncx export and return its <Connection>
// entries. Useful when the user's Navicat isn't on this machine.
std::vector<ImportedConnection> importFromNCX(const std::wstring& ncxPath);

}} // namespace
