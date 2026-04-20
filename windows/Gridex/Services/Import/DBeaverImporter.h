#pragma once
//
// DBeaverImporter.h
// Gridex
//
// Reads %APPDATA%\DBeaverData\workspace6\General\.dbeaver\
// data-sources.json and credentials-config.json, returns the
// connections as ImportedConnection. Mirrors mac DBeaverImporter.
// The credentials file uses a fixed 8-byte XOR key — same key
// across mac + Windows installs since DBeaver is Java.

#include <vector>
#include "../../Models/Import/ImportedConnection.h"

namespace DBModels { namespace DBeaverImporter {

bool isInstalled();
std::vector<ImportedConnection> importConnections();

}} // namespace
