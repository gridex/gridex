# Gridex

A native database IDE for macOS and Windows with built-in AI chat. Connect to PostgreSQL, MySQL, SQLite, Redis, MongoDB, and SQL Server from a single app.

![macOS 14+](https://img.shields.io/badge/macOS-14%2B-blue)
![Windows 10+](https://img.shields.io/badge/Windows-10%2B-0078D4)
![Swift 5.10](https://img.shields.io/badge/Swift-5.10-orange)
![License](https://img.shields.io/badge/license-Apache%202.0-blue)

<p align="center">
  <img src="assets/screenshot.jpg" alt="Gridex Screenshot" width="100%">
</p>

## Features

### Database Support

| Database | Driver | Highlights |
|----------|--------|------------|
| **PostgreSQL** | [PostgresNIO](https://github.com/vapor/postgres-nio) | Parameterized queries, SSL/TLS, sequences, full schema inspection |
| **MySQL** | [MySQLNIO](https://github.com/vapor/mysql-nio) | Character set handling, parameterized queries, SSL |
| **SQLite** | System `libsqlite3` | File-based, WAL mode, zero config |
| **Redis** | [RediStack](https://github.com/swift-server/RediStack) | Key browser, SCAN filter, Server INFO dashboard, Slow Log viewer |
| **MongoDB** | [MongoKitten](https://github.com/orlandos-nl/MongoKitten) | Document editor, NDJSON export/restore, aggregation |
| **SQL Server** | [CosmoSQLClient](https://github.com/vkuttyp/CosmoSQLClient-Swift) | TDS 7.4 protocol, native BACKUP DATABASE, stored procedures |

### AI Chat

Built-in AI assistant that understands your schema and writes SQL for you.

- **Anthropic Claude** — streaming responses, direct API
- **OpenAI GPT** — full API support
- **Google Gemini** — Flash model support
- **Ollama** — local LLM, no API key needed

All requests go directly from your machine to the provider. Gridex never proxies prompts. API keys are stored in the macOS Keychain.

### Data Grid

- Inline cell editing with type-aware parsing
- Sort, filter, and paginate large datasets
- Add/delete rows with pending change tracking and commit workflow
- Column resize, multi-column sort
- Copy rows, export to CSV/JSON/SQL

### Query Editor

- Multi-tab with connection-grouped Chrome-style tab bar
- Syntax highlighting (keywords, strings, numbers, comments, functions)
- Full query history persisted via SwiftData
- Execute selection, execute all
- Redis CLI mode

### Schema Tools

- Table structure viewer (columns, indexes, foreign keys, constraints)
- ER Diagram with auto-layout, zoom, pan, and FK relationship lines
- Function inspector with source code and parameter signatures
- Stored procedure support (MSSQL)

### SSH Tunnel

- Password and private-key authentication
- Local port forwarding through bastion hosts
- Managed by `SSHTunnelService` actor (thread-safe, async/await)

### Import & Export

- **Export**: CSV, JSON, SQL (INSERT statements), SQL (full DDL with sequences/indices), schema DDL
- **Import**: CSV (with column mapping), SQL dumps

### Backup & Restore

| Database | Backup Method |
|----------|---------------|
| PostgreSQL | pg_dump (custom, SQL, tar) / pg_restore |
| MySQL | mysqldump / mysql CLI |
| SQLite | File copy |
| MongoDB | NDJSON (one document per line) |
| Redis | JSON snapshot via SCAN |
| SQL Server | Native `BACKUP DATABASE` |

Supports selective table backup, compression options, and progress callbacks.

### Redis Management

- Virtual "Keys" table with SCAN-based browsing
- Key Detail View — edit hash fields, list items, set/zset members
- Pattern-based filter bar (glob syntax: `user:*`, `cache:?`)
- Server INFO dashboard with auto-refresh
- Slow Log viewer, Flush DB, key rename/duplicate, TTL management

### MongoDB Document Editor

- NDJSON document editing with syntax-aware editor
- Document insert/update/delete
- Aggregation pipeline support

### Other

- Multi-window support (Cmd+N)
- macOS Keychain for all credential storage
- Dark mode native
- Connection save/load with SSH tunnel config and SSL/TLS options

## Requirements

### macOS

- macOS 14.0 (Sonoma) or later
- Swift 5.10+ / Xcode 15+

### Windows

- Windows 10 or later (64-bit)
- Visual Studio 2022+, .NET 8 SDK, vcpkg

## Build & Run

### macOS

```bash
git clone https://github.com/gridex/gridex.git
cd gridex

# Debug (ad-hoc signed, fast local testing)
swift build
.build/debug/Gridex

# Or build .app bundle
./scripts/build-app.sh
open dist/Gridex.app
```

### Release

```bash
# Apple Silicon
./scripts/release.sh
# → dist/Gridex-<version>-arm64.dmg

# Intel
ARCH=x86_64 ./scripts/release.sh
# → dist/Gridex-<version>-x86_64.dmg

# Both architectures
./scripts/release-all.sh
```

Release pipeline: `swift build` → `.app` bundle → code sign → notarize → staple → DMG → sign DMG → notarize DMG.

**Environment variables for release:**

| Variable | Description |
|----------|-------------|
| `ARCH` | `arm64` or `x86_64` (default: host) |
| `SIGN_IDENTITY` | Developer ID certificate SHA-1 (or set in `.env`) |
| `NOTARY_PROFILE` | `notarytool` keychain profile (default: `gridex-notarize`) |
| `NOTARIZE` | Set to `0` to skip notarization |

## Architecture

Clean Architecture with 5 layers. Dependencies point inward — Presentation → Services → Data → Domain → Core.

```
gridex/
├── macos/                    macOS app (Swift, AppKit)
│   ├── App/                  Lifecycle, DI container, AppState, constants
│   ├── Core/                 Protocols, models, enums, errors — zero deps
│   │   ├── Protocols/        DatabaseAdapter, LLMService, SchemaInspectable
│   │   ├── Models/           RowValue, ConnectionConfig, QueryResult, etc.
│   │   └── Enums/            DatabaseType, GridexError
│   ├── Domain/               Use cases, repository protocols
│   ├── Data/                 Adapters, SwiftData persistence, Keychain
│   │   ├── Adapters/         SQLite, PostgreSQL, MySQL, MongoDB, Redis, MSSQL
│   │   ├── Persistence/      SwiftData models (connections, query history)
│   │   └── Keychain/         macOS Keychain service
│   ├── Services/             Cross-cutting concerns
│   │   ├── QueryEngine/      QueryEngine actor, ConnectionManager, QueryBuilder
│   │   ├── AI/               Anthropic, OpenAI, Ollama, Gemini providers
│   │   ├── SSH/              SSHTunnelService (NIOSSH port forwarding)
│   │   └── Export/           ExportService, BackupService
│   └── Presentation/         AppKit views, SwiftUI settings, ViewModels
│       ├── Views/            18 view groups (DataGrid, QueryEditor, AIChat, etc.)
│       └── Windows/          macOS window management
├── windows/                  Windows app (C++, WinUI 3)
├── scripts/                  Build and release automation
└── Package.swift             SPM manifest
```

### Key Protocols

- **`DatabaseAdapter`** — ~50 methods covering connection, queries, schema, CRUD, transactions, pagination. All 6 database adapters conform to this.
- **`LLMService`** — Streaming AI responses via `AsyncThrowingStream`. All 4 AI providers conform.
- **`SchemaInspectable`** — Full schema snapshot (tables, views, indices, constraints).

### Concurrency

- `actor` for thread-safe services: `QueryEngine`, `ConnectionManager`, `SSHTunnelService`, `BackupService`, `SchemaInspectorService`
- `async/await` throughout — no completion handlers
- `Sendable` constraints on all data models

### Dependency Injection

`DependencyContainer` (singleton) manages service creation. SwiftData `ModelContainer` shared across windows. Services injected via SwiftUI environment.

## Dependencies

| Package | Version | Purpose |
|---------|---------|---------|
| [postgres-nio](https://github.com/vapor/postgres-nio) | 1.21.0+ | PostgreSQL driver |
| [mysql-nio](https://github.com/vapor/mysql-nio) | 1.7.0+ | MySQL driver |
| [swift-nio-ssl](https://github.com/apple/swift-nio-ssl) | 2.27.0+ | TLS for NIO connections |
| [swift-nio-ssh](https://github.com/apple/swift-nio-ssh) | 0.8.0+ | SSH tunnel support |
| [RediStack](https://github.com/swift-server/RediStack) | 1.6.0+ | Redis driver |
| [MongoKitten](https://github.com/orlandos-nl/MongoKitten) | 7.9.0+ | MongoDB driver (pure Swift) |
| [CosmoSQLClient-Swift](https://github.com/vkuttyp/CosmoSQLClient-Swift) | main | MSSQL via TDS 7.4 (no FreeTDS) |

System library: `libsqlite3` (linked at build time).

## Contributing

Contributions are welcome. Please open an issue first to discuss what you'd like to change.

See [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines and [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md) for community standards.

## License

Licensed under the [Apache License, Version 2.0](LICENSE).

Copyright © 2026 Thinh Nguyen.

You may use, modify, and distribute this software — including in commercial or closed-source products — provided you preserve the copyright notice and NOTICE file. See the LICENSE for full terms.
