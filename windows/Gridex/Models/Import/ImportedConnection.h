#pragma once
//
// ImportedConnection.h
// Gridex
//
// Shared data model for connections read from external GUI tools
// (TablePlus, DBeaver, DataGrip, Navicat). Kept separate from
// ConnectionConfig so the pre-import preview list doesn't pollute
// the active store — user ticks rows in the wizard, then we
// convert to ConnectionConfig on Import.
//
// Mirrors macos/Services/Import/ImportedConnection.swift 1:1; same
// field names so cross-platform UI code can read either.

#include <string>
#include <optional>
#include <vector>
#include <cstdint>
#include "../ConnectionConfig.h"

namespace DBModels
{
    enum class ImportSource
    {
        TablePlus,
        DBeaver,
        DataGrip,
        Navicat
    };

    inline std::wstring ImportSourceDisplayName(ImportSource s)
    {
        switch (s)
        {
            case ImportSource::TablePlus: return L"TablePlus";
            case ImportSource::DBeaver:   return L"DBeaver";
            case ImportSource::DataGrip:  return L"DataGrip";
            case ImportSource::Navicat:   return L"Navicat";
        }
        return L"Unknown";
    }

    inline std::wstring ImportSourceBadge(ImportSource s)
    {
        switch (s)
        {
            case ImportSource::TablePlus: return L"T+";
            case ImportSource::DBeaver:   return L"DB";
            case ImportSource::DataGrip:  return L"DG";
            case ImportSource::Navicat:   return L"N";
        }
        return L"?";
    }

    struct ImportedConnection
    {
        std::wstring id;
        ImportSource source = ImportSource::DBeaver;
        std::wstring name;
        DatabaseType databaseType = DatabaseType::PostgreSQL;
        std::wstring host;
        std::optional<uint16_t> port;
        std::wstring database;
        std::wstring username;
        std::wstring password;       // may be empty when tool hides it
        bool sslEnabled = false;
        std::wstring filePath;       // SQLite
        std::wstring sshHost;
        std::optional<uint16_t> sshPort;
        std::wstring sshUser;
        std::wstring group;          // optional folder/group name

        // Convert to ConnectionConfig ready to Save() into
        // ConnectionStore. Caller should generate a fresh id if
        // merging into an already-populated store (mac uses UUID —
        // Windows ConnectionStore keys on wstring id).
        ConnectionConfig toConnectionConfig() const
        {
            ConnectionConfig c;
            c.id = id;
            c.name = name;
            c.databaseType = databaseType;
            c.host = host;
            c.port = port.value_or(DatabaseTypeDefaultPort(databaseType));
            c.database = database;
            c.username = username;
            c.password = password;
            c.sslEnabled = sslEnabled;
            c.filePath = filePath;
            c.group = group;
            if (!sshHost.empty() && !sshUser.empty())
            {
                SSHTunnelConfig ssh;
                ssh.host = sshHost;
                ssh.port = sshPort.value_or(22);
                ssh.username = sshUser;
                ssh.authMethod = SSHAuthMethod::Password;
                c.sshConfig = ssh;
            }
            return c;
        }
    };
}
