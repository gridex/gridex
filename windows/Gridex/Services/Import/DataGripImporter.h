#pragma once
//
// DataGripImporter.h
// Gridex
//
// Reads %APPDATA%\JetBrains\DataGrip<VERSION>\options\dataSources.xml
// and project-level .idea/dataSources.xml files. Passwords are kept
// by IntelliJ's encrypted credential store, not in the XML — we
// skip them; user re-enters after import.

#include <vector>
#include "../../Models/Import/ImportedConnection.h"

namespace DBModels { namespace DataGripImporter {

bool isInstalled();
std::vector<ImportedConnection> importConnections();

}} // namespace
