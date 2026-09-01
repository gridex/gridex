# Changelog

All notable changes to Gridex are documented in this file. Gridex ships native apps for **macOS** (Swift/AppKit) and **Windows** (C++/WinUI 3). Unless noted otherwise, changes apply to both platforms.

## [Unreleased]

### Added
- **Redis namespace key tree (macOS)** — Redis keys can be browsed as a collapsible sidebar tree using a local configurable delimiter (default `:`). Leaf keys open the existing detail view; the flat SCAN-filtered key list remains available.

## [0.23.0] - 2026-04-12

### Added
- **MongoDB filter & sort support** — data grid filter panel now generates real BSON queries via `buildMongoFilter`:
  - All operators supported: `=`, `≠`, `>`, `<`, `≥`, `≤`, `LIKE`, `NOT LIKE`, `IS NULL`, `IS NOT NULL`, `IN`.
  - `LIKE` patterns translated to MongoDB `$regex` (case-insensitive, `%` → `.*`, `_` → `.`).
  - `_id` field auto-converts 24-char hex strings to `ObjectId` in filter values.
  - Multi-condition filters combined with `$and` / `$or` matching the combinator.
  - Sort applied via `.sort(Document)` on the cursor.
- **MongoDB nested field schema introspection** — `collectFieldPaths` recursively flattens embedded documents into dot-notation paths (e.g. `address.city`). Depth capped at 10 to avoid stack overflow on pathological docs. Array fields are recorded at the parent level (not expanded to `tags.0`) for more useful filter paths.
- **MongoDB array type detection** — `inferType` now returns `"array"` for BSON array documents instead of `"document"`.
- **Backup/Restore: MongoDB NDJSON** — pure Swift backup and restore without `mongodump`/`mongorestore`:
  - Backup streams all collections via MongoKitten cursor, writes one JSON line per document: `{"_collection":"name","_doc":{...}}`.
  - Restore parses NDJSON, groups by collection, batch-inserts via adapter.
  - Format extension: `.ndjson`.
- **Backup/Restore: Redis JSON snapshot** — pure Swift backup and restore without `redis-cli`:
  - Backup uses `SCAN` + `TYPE`/`GET`/`LRANGE`/`SMEMBERS`/`ZRANGE WITHSCORES`/`HGETALL` to capture all key types (string, list, set, zset, hash) + TTL. Written as NDJSON via `RedisJSONSerializer`.
  - Restore replays entries using `SET`/`RPUSH`/`SADD`/`ZADD`/`HSET` + `EXPIRE`.
  - Format extension: `.json`.
- **Backup/Restore: SQL Server native `.bak`** — uses SQL Server's built-in `BACKUP DATABASE … TO DISK` and `RESTORE DATABASE … FROM DISK` (T-SQL). Path is server-side; note for Docker users to volume-mount the path. RESTORE uses `SINGLE_USER WITH ROLLBACK IMMEDIATE` + `WITH REPLACE` pattern.
- **`RedisJSONSerializer`** — new helper that serializes/deserializes `RedisAdapter.RedisKeyEntry` to/from JSON strings. Handles all five Redis types with their native structures.

### Changed
- **AI Chat welcome screen** — empty chat now shows a welcome screen instead of a blank message list. Dividers removed for a cleaner layout; window background color applied.

### Fixed
- **MongoDB `documentToJSONString`** — BSON arrays now serialize as `[…]` instead of `{…}` by checking `doc.isArray`. Iteration switched from `for pair in doc` to `for key in doc.keys` + subscript to avoid iterator quirks with unusual BSON encodings.
- **MongoDB query guard** — added `guard parts.count >= 2` before split on `collectionName.find()` pattern to prevent out-of-bounds crash.
- **MongoDB fetchRows field paths** — `_id` is now always listed first; remaining fields collected with `collectFieldPaths` (dot-notation aware) instead of flat per-document iteration.

## [0.22.0] - 2026-04-11

### Added
- **Microsoft SQL Server support** — full integration as a 6th database type via [CosmoMSSQL](https://github.com/vkuttyp/CosmoSQLClient-Swift) (pure Swift NIO, TDS 7.4, no FreeTDS dependency). Compatible with SQL Server 2014+, Azure SQL Edge, Azure SQL Database.
  - **Connection pool** — `MSSQLConnectionPool` (max 5 conns) avoids concurrent stream conflicts.
  - **Schema introspection via `INFORMATION_SCHEMA`** — portable across all SQL Server variants. Lists databases, schemas (default `dbo`), tables, views, functions, procedures, columns, foreign keys.
  - **CRUD with parameter inlining** — works around CosmoMSSQL bug where RPC `sp_executesql` hardcodes transaction descriptor to 0. Inlines `@pN` placeholders into SQL with proper escaping (`N''` for unicode strings, `0xHEX` for binary, etc.) so transactions work correctly.
  - **Multi-batch script execution** — Query Editor splits scripts on `GO` separator. Uses dedicated script connection so `USE` statements persist across batches (e.g. `CREATE DATABASE → USE → CREATE TABLE` flows work).
  - **Auto-refresh after `USE`/`CREATE DATABASE`/`DROP DATABASE`** — sidebar and database picker sync to new state.
  - **MSSQL-specific dialect** — `[brackets]` identifier quoting, `@pN` placeholders, `OFFSET N ROWS FETCH NEXT M ROWS ONLY` pagination.
  - **MSSQL-aware ALTER TABLE** — uses `ADD col type` (no `COLUMN IF NOT EXISTS`), `EXEC sp_rename` for column rename, `ALTER COLUMN col type` for type change.
  - **Default schema `dbo`** — New Table tab defaults to `dbo` instead of `public` when adapter is MSSQL.
  - **37 native data types** — full list from [Microsoft Learn docs](https://learn.microsoft.com/sql/t-sql/data-types/data-types-transact-sql) including modern `VARCHAR(MAX)`, `NVARCHAR(MAX)`, `JSON`, `HIERARCHYID`, `GEOGRAPHY`, etc.
- **Stored procedures support (MSSQL)**:
  - Sidebar shows separate **Procedures** group with ▶ icon (green).
  - Procedure detail view with **parameter signature bar** — chips show `@id INT`, `@name NVARCHAR(100)`, `@result INT OUTPUT` parsed from `INFORMATION_SCHEMA.PARAMETERS`.
  - **"Execute" button** — auto-generates `EXEC [dbo].[procname] @p1 = NULL, @p2 = NULL OUTPUT;` template and opens it in a new query tab ready to fill.
- **SQL Editor smart script execution** — detect multi-statement scripts and run sequentially. SQL Server uses `GO` batch separator; other databases split on `;` (respecting quoted strings and `--` comments). Shows success status with statement count + rows affected for DDL/DML scripts.
- **Status banner separation** — success info no longer shown as red error; uses green ✓ status banner with `N statement(s) executed · M row(s) affected · 0.50s`.

### Fixed
- **MSSQL connection error handling** — wraps `SQLError` enum in `GridexError.queryExecutionFailed` with `String(describing:)` to preserve rich descriptions ("Server error 911: Database X does not exist") instead of generic "error 0".
- **MSSQL transaction commit** — UPDATE/INSERT/DELETE inside transactions now work correctly via parameter inlining (CosmoMSSQL RPC bug workaround).

## [0.21.0] - 2026-04-10

### Added
- **MongoDB support** — full integration as a 5th database type alongside PostgreSQL/MySQL/SQLite/Redis:
  - **MongoKitten driver** — pure Swift NIO-based, no C dependencies. Port 27017 default.
  - **Connection string field** — paste full `mongodb://` or `mongodb+srv://` URI with **Parse** button to auto-fill host/port/user/password/database/SSL. SRV URIs (Atlas, DigitalOcean) preserved verbatim for proper DNS lookup.
  - **Schema introspection** — collections appear as tables in sidebar; fields inferred from sampled documents (string/integer/double/boolean/date/objectId/document).
  - **CRUD via data grid** — Add row, edit cells, delete rows all route through MongoDB adapter using `insertOne`/`updateOne`/`deleteOne` (no SQL generation).
  - **Insert Document sheet** — JSON editor with auto-template from detected fields. Supports MongoDB Extended JSON shortcuts (`$oid`, `$date`). Allows inserting documents with any structure including new fields not in existing schema.
  - **Database management** — list/create/drop databases via shared connection pool. Database picker (⌘K) reconnects on switch.
  - **Schemaless structure view** — Structure tab shows detected fields as read-only info instead of editable column form (no ALTER TABLE in MongoDB).
  - **Create Collection** — simplified form (just collection name, no columns) for `New Table` tab when adapter is MongoDB.
  - **Auto ObjectId detection** — 24-char hex strings auto-converted to BSON ObjectId.

## [0.20.0] - 2026-04-09

### Added
- **SSH Tunnel support** — NIOSSH-based local port forwarding. ConnectionManager auto-detects SSH config, establishes tunnel, and redirects database connection through `127.0.0.1:localPort`. Password auth implemented, private key skeleton.
- **Delete Database** — trash button in Database Switcher (⌘K) with confirmation dialog. Disabled for currently active database. Uses `DROP DATABASE` for PostgreSQL/MySQL.
- **General Settings** — default page size (100/300/500/1000), confirm before delete toggle, auto-refresh sidebar with configurable interval, show query log default.
- **Editor Settings** — font size (8-24 with stepper), tab size (2/4/8), spaces vs tabs, word wrap, show line numbers, highlight current line.

### Removed
- **Dead import code** — removed unused `ImportService.swift` and `ImportDataUseCase.swift`. Import CSV/SQL already works directly via adapter in the UI views.

## [0.19.3] - 2026-04-09

### Added
- **Backup/Restore progress tracking** — no more endless spinner:
  - **Restore** (SQL): file piped through stdin in 64KB chunks, progress bar shows percentage based on bytes sent vs file size.
  - **Backup** (pg_dump/mysqldump): polls output file size every 0.5s, shows written size + elapsed time (e.g. `12.5 MB · 8s`).
  - Applies to PostgreSQL (psql, pg_dump) and MySQL (mysql, mysqldump).

## [0.19.2] - 2026-04-08

### Added
- **Build script** (`scripts/build-app.sh`) — builds `.app` bundle from SPM with compiled asset catalog, ad-hoc signing, and entitlements. Supports debug/release modes.

### Fixed
- **Edit cell background** — modified cell background (orange) only appears when value actually changes, not on every edit click. Fixed `justFinishedEditing` path not rebuilding pending change caches.
- **Edit cell blanks other columns** — after editing a cell, other columns in the same row appeared blank. Fixed by force-redrawing all visible cells after edit ends.
- **Cell background rendering** — `dirtyRect.fill()` replaced with `bounds.fill()` to ensure full cell background coverage.
- **Display cache sync** — `displayCache` now updates on cell edit, date edit, new row insert, and discard changes.
- **App logo missing in built .app** — `Bundle.module` (SPM) could not find `Assets.car` in `.app` bundle. Added multi-path logo loader that tries `.module`, main bundle, and explicit resource path.

## [0.19.1] - 2026-04-08

### Fixed
- **Data grid scroll performance** — eliminated per-frame bottlenecks:
  - Pre-computed display string cache (`displayCache`) avoids repeated `displayString` calls during scroll (600 array lookups vs 600 enum switches per frame).
  - Cached `NSAttributedString` in cells — reused across `draw()` calls instead of recreating dictionary + layout every frame.
  - Pre-truncate text at 300 chars before Core Text layout; blob display as `"(BLOB N bytes)"` with zero encoding cost.
  - Debounce increased 16ms → 100ms; column resize no longer triggers full visible-cell refresh.
  - Dirty checking in `configureCell` — only mark `needsDisplay` when cell content actually changed.
  - Removed `@Published` from high-frequency properties (`selectedRows`, `columnWidths`, `editingCell`, `insertedRowIndices`) to prevent SwiftUI body re-evaluation on every click/resize.
  - `DateFormatter` shared singleton instead of allocating per cell.
  - Tooltip computed only on text change, not every `viewFor` call.
- **Memory leak on tab close** — RAM did not decrease when closing data tabs:
  - Fixed Combine retain cycle: subscription held strong reference to `viewModel.objectWillChange`, keeping `DataGridViewState` alive indefinitely.
  - Coordinator `rows` snapshot now cleared via `releaseData()`.
  - `DataGridViewState.rows`, `displayCache`, `columns` explicitly cleared before removal from cache.
  - `queryEditorText` cleaned up on `closeAllTabs`.

## [0.19.0] - 2026-04-07

### Added
- **Google Gemini AI provider** — new LLM provider using OpenAI-compatible endpoint (`/v1beta/openai`). Streaming chat, model fetching from API, API key validation.
- **AI Settings UI** — configure provider (Gemini/Anthropic/OpenAI/Ollama), API key (Keychain), base URL, model picker with live fetch from API.
- **AI Chat wired to real LLM** — streaming responses with schema-aware system prompt. Replaces placeholder stub.
- **Attach tables to chat** — `+` button in chat input to select tables/views. Schema is sent inline in user message for precise, per-question context.
- **Chat history in-memory** — messages persist across tab switches (Details/Assistant), scoped per connection, cleared on disconnect.
- **SettingsLink integration** — gear icon and setup prompt in Assistant panel open macOS Settings window directly.

### Changed
- **Assistant tab always visible** — no longer requires `aiPanelVisible` toggle; shows setup prompt when no API key configured.

## [0.18.3] - 2026-04-07

### Added
- **SQL Editor button in toolbar** — `</>` icon in the header toolbar for quick access to open a new query editor tab.

### Fixed
- **Keyboard shortcuts not working (⌘K, ⌘⇧N, etc.)** — menu commands were disabled even after connecting to a database. Root cause: `AppState.active` (static weak var) is not reactive in SwiftUI, so `.disabled()` conditions were never re-evaluated. Fixed by using `@FocusedObject` with fallback to `AppState.active`.

## [0.18.2] - 2026-04-07

### Fixed
- **Truncate table** — now uses pending workflow (orange highlight → commit) instead of executing immediately. SQLite uses `DELETE FROM` since `TRUNCATE TABLE` is unsupported. Data grid refreshes automatically after commit.
- **Delete table cascade default** — cascade option is now enabled by default in the delete confirmation sheet.
- **Delete table error handling** — failed DROP no longer silently removes the table from pending; it stays marked (red) and shows an error alert so users can retry or adjust options.

## [0.18.1] - 2026-04-07

### Added
- **App version in status bar** — bottom-right corner shows current version (e.g. `v0.18.1`), read from Info.plist.

### Fixed
- **Connection with empty password** — previously silently aborted when no keychain entry existed. Now connects normally (needed for Redis without AUTH, passwordless databases).
- **Clearing saved password** — when user empties the password field and saves, the old keychain entry is now deleted. Previously the stale password persisted and was sent on reconnect, causing AUTH errors on passwordless Redis servers.

## [0.18.0] - 2026-04-07

### Changed
- **Rebrand: DataBridge → Gridex** — app name, bundle ID (`com.gridex.app`), all source code references, error types (`GridexError`), entitlements, and file names updated.
- **Project restructured:** `DataBridge/` → `macos/`, new empty `window/` folder for future Windows port.
- **Tests removed** — `DataBridgeTests/` deleted per project decision.
- Package name and executable renamed to `Gridex`.

## [0.17.0] - 2026-04-07

### Added
- **Redis pattern-based filter bar** — single search field with glob patterns (`user:*`, `*session*`), quick-filter presets menu, replaces SQL column filter for Redis connections.
- **ER Diagram toolbar button** — quick access from header toolbar (SQL databases only).

### Fixed
- **Critical crash fixes:**
  - `AppState.connect()`: force unwrap `group.next()!` replaced with safe `guard let` (connection timeout crash).
  - Redis `HGETALL`/`ZRANGE WITHSCORES` parsing: off-by-one in `stride` dropped the last field/member.
  - Redis `insertRow`/`updateRow`/`deleteRow`: keys with spaces broke command parsing — now uses direct `sendCommand()` API.
  - PostgreSQL `updateRow`/`deleteRow`: force unwrap `values[$1]!` replaced with safe `.compactMap`.
  - `ImportCSVSheet`: array index access on empty CSV replaced with safe `.first`.
  - Redis TLS: `try!` NIOSSLClientHandler replaced with proper `do/catch`.
- **Redis safety:**
  - Collection fetches capped at 10,000 items (LRANGE/SSCAN/ZRANGE) to prevent OOM.
  - SCAN loop checks connection state and caps at 100,000 keys.
- **UI error handling:** `TableStructureView`, `TableListView`, `ERDiagramViewModel`, `ConnectionFormPanel` now show errors to user with retry buttons instead of silent `print()`.
- **ImportSQLWizard:** hardcoded dark background color replaced with `NSColor.textBackgroundColor` (light mode fix).

### Changed
- **Toolbar:** all action buttons unified in single `primaryAction` group; commit icon changed to `text.insert`; AI brain icon removed; console log placeholder uses `terminal` icon.
- **Structure tab** hidden for Redis connections (key-value store has no table schema).

## [0.16.1] - 2026-04-06

### Fixed
- **Redis Server INFO** — fixed RESP `\r\n` line ending parsing that caused Server Info dashboard to show blank. Added error display for blocked commands.
- **Redis pipelined key loading** — `fetchRows()` now batches TYPE/TTL/value commands via NIO futures (~50x faster on cloud Redis).
- **Redis `listDatabases()`** — graceful fallback when `CONFIG GET` is blocked (Upstash, ElastiCache).

## [0.16.0] - 2026-04-06

### Added

#### Comprehensive Redis management
- **Add Key with type selection** — create string, hash, list, set, or sorted set keys from a dedicated sheet with field editors and optional TTL.
- **Key Detail View** — double-click any key to open a dedicated tab showing:
  - Hash: field/value table with inline add/delete
  - List: indexed items
  - Set: member list with add/remove
  - Sorted Set: member/score table with add/remove
  - String: full text view
  - TTL management (set/remove) and key rename from the header bar
  - Memory usage display (via `MEMORY USAGE`)
- **Server INFO Dashboard** — new tab showing all Redis server sections (Server, Memory, Clients, Stats, Keyspace) with key metrics cards (memory, clients, uptime, ops/sec, hit rate). Auto-refresh toggle (5s interval).
- **Slow Log Viewer** — new tab displaying `SLOWLOG GET` entries with ID, timestamp, duration, command, client. Reset button with confirmation.
- **DBSIZE in status bar** — total key count shown when connected to Redis.
- **Flush Database** — toolbar action + sidebar context menu with destructive confirmation dialog.
- **Key rename** — from the Key Detail View header.
- **Key duplicate** — uses `COPY` (Redis 6.2+) with type-aware fallback.
- **Redis CLI mode** — Query Editor shows "Redis CLI" label, hides Format/Minify/Explain buttons (SQL-only).
- **Redis sidebar context menu** — Browse Keys, Add Key, Server Info, Slow Log, Flush Database.
- **+ Key button** in DataGrid toolbar replaces "+ Row" for Redis connections.
- Toolbar buttons for Server Info and Slow Log (Redis-only, hidden for SQL databases).

## [0.15.1] - 2026-04-06

### Fixed
- **Redis TLS (rediss://)** — connections to cloud Redis providers (Upstash, AWS ElastiCache, etc.) now work via NIOSSL. A "TLS (rediss://)" checkbox is shown in the Redis connection form.
- **Password character count** — secure password fields now display the number of characters (e.g. "36") so users can verify pasted passwords.
- **Redis connection form cleanup** — SSL keys row hidden for Redis; TLS toggle replaces SSL mode picker.

## [0.15.0] - 2026-04-06

### Added

#### Redis connection support
- New **Redis** database type alongside PostgreSQL, MySQL, and SQLite.
- **RedisAdapter** maps the key-value store into DataBridge's relational model:
  - Virtual **"Keys"** table with columns: `key`, `type`, `value`, `ttl`.
  - SCAN-based pagination for browsing keys with pattern-matching filters.
  - Supports all Redis data types: string, list, set, sorted set, hash, stream.
  - Raw Redis command execution via the query editor (e.g. `SET foo bar`, `HGETALL myhash`).
  - RESP responses rendered as structured tables (arrays, key-value pairs, scalars).
  - MULTI/EXEC/DISCARD transaction support.
  - Database switching via `SELECT <db_number>` (db0–db15).
- **Connection form** adapted for Redis: host, port, optional password, database number (0–15).
- Red gradient badge ("Rd") in Home screen and sidebar.
- RediStack (swift-server/RediStack) added as dependency.

## [0.14.0] - 2026-04-06

### Added

#### Multi-window support — open multiple databases simultaneously
- **Cmd+N** now opens a new window with the Home connection picker, allowing you to connect to a different database in a separate window.
- Each window has its own independent `AppState`: separate connection, sidebar, tabs, and query history.
- Windows operate fully in parallel — edit data in PostgreSQL in one window while querying MySQL in another.

### Changed
- **New Query** shortcut moved from Cmd+N to **Cmd+Shift+N** (Cmd+N is now "New Window").
- `WindowCloseInterceptor` updated for multi-window: closing a window when others are open closes it directly; closing the last window disconnects and shows Home (preserves single-window UX).
- `AppState` ownership moved from the app-level `DataBridgeApp` scene to per-window `MainView` via `@StateObject`, with `@FocusedObject` routing menu commands to the active window.

## [0.13.0] - 2026-04-06

### Changed

#### Backup & Restore — TablePlus-style 3-column wizard
- Completely redesigned `BackupRestorePanel` to mirror the TablePlus backup dialog:
  - **Left column**: saved connection list (grouped, searchable, with DB type badges)
  - **Center column**: database list (auto-loaded via `listDatabases()` when a connection is selected; works even when not currently connected — creates a temporary adapter)
  - **Right column**: format picker (`--format=custom`, `--format=sql`, `--format=tar`), content selector (full / schema-only / data-only), compress toggle, or file browser for restore
  - **Footer**: "Start backup…" / "Start restore…" button with inline result (success/fail, file size, duration)
- File name header shows the selected database name as default backup filename.
- Auto-selects the active connection and database when opened from the sidebar while connected.

#### Home screen — Backup & Restore buttons
- Added "Backup database…" and "Restore database…" action buttons to the left branding panel on the Home screen, above "New Connection" / "New Group" (matches TablePlus layout).
- Opens the full 3-column wizard panel — user can pick any saved connection + database even when not connected.

### Fixed
- **Picker tag mismatch warning** — `selectedFormat` was initialized as `.custom` but SQLite/MySQL only support `.sql`, causing SwiftUI `Picker: the selection "custom" is invalid` console errors. Format is now initialized to `.sql` (universal) and reset to the first available format whenever the selected connection changes.

## [0.12.0] - 2026-04-06

### Added

#### ER Diagram — interactive schema visualization
- New **ER Diagram** tab type renders all tables and foreign-key relationships as an interactive graph using CoreGraphics/AppKit custom `NSView`.
- **Table cards** display column names, abbreviated data types, PK/FK badges, and NOT NULL indicators.
- **Relationship lines** drawn as bezier curves with arrowheads and cardinality markers (1 / N).
- **Drag** any table card to rearrange the layout.
- **Zoom**: Cmd+scroll, trackpad pinch, or toolbar +/− buttons.
- **Pan**: hold Space + drag, or drag on empty canvas area.
- **Auto layout**: Sugiyama-lite BFS layering based on FK relationships.
- **Fit to view**: scales and centers the diagram to fill the viewport.
- **Double-click** a table card to open its data grid tab.
- Hover highlight on table cards; selected card gets accent border.
- Dot-grid background pattern adapts to light/dark mode.
- Open via right-click **Tables** group → "ER Diagram" in the sidebar.

#### Database Backup & Restore
- New `BackupService` (`Services/Export/BackupService.swift`) — actor-based service that wraps native CLI tools:
  - **PostgreSQL**: `pg_dump` / `pg_restore` / `psql` — supports Custom (compressed), Plain SQL, and Tar formats.
  - **MySQL**: `mysqldump` / `mysql` — SQL format.
  - **SQLite**: direct file copy for backup, file replace for restore.
- Automatic tool discovery across Homebrew (Intel + ARM), Postgres.app, and system paths; falls back to `which`.
- New `BackupRestorePanel` — native `NSPanel` centered over the main window:
  - **Backup**: format picker, compress toggle, content selector (full / schema-only / data-only), `NSSavePanel` for output path, inline result (file size, duration, errors).
  - **Restore**: file browser with auto-format detection by extension, danger warning before overwrite, inline result with sidebar refresh on success.
  - Connection header shows name, host, database, and DB type badge.
- Sidebar bottom bar now has two new icon buttons: ↑ **Backup** and ↓ **Restore**.

## [0.11.1] - 2026-04-06

### Changed

- **Warning cleanup** — Cleared all compiler warnings flagged by Xcode (except the deprecated `SecKeychain*` APIs in `KeychainService`, which need a proper migration to the data-protection keychain):
  - `SQLiteAdapter.bind` — discard the result of `withUnsafeBytes` with `_ =`
  - `AppState.openTableStructure` — removed dangling `existing.id` expression that did nothing
  - `AppState.switchDatabase` — discard the result of the MySQL `USE` `executeRaw` call with `_ =`
  - `SQLContextParser.tableBeforeParen` — removed unused `schemaPos` local
  - `SQLFormatter.format` — `var clauseIndent` → `let`; removed the unused `inSelectList`, `inGroupOrderBy`, and JOIN-loop `u` locals that were written but never read
  - `ExportTableSheet.performExport` — dropped `await` in front of `showSavePanel` / `showDirectoryPanel` (both are synchronous `NSSavePanel`/`NSOpenPanel` wrappers)

## [0.11.0] - 2026-04-06

### Added

#### Import SQL Wizard — preview before execute
- New `ImportSQLWizard.swift` — opens after picking a `.sql` file via **Import → From SQL Dump…** in the Tables context menu.
- Shows file metadata (name, size, encoding picker) and a scrollable preview of the first 100,000 characters before hitting the database.
- Import result is rendered inline in the wizard (success/total count + first error) instead of via a separate alert.
- Sidebar is refreshed when the wizard closes (not during execution) so newly-imported tables appear without re-rendering the parent mid-import.

#### 10-second connection timeout
- `AppState.connect(config:password:)` now races the connection attempt against a 10-second timer via `withThrowingTaskGroup`. If the driver blocks (unreachable host, dropped packets, firewall) the UI no longer stays stuck on "Connecting…" forever — the user gets an error and can retry.

### Changed

#### Sidebar group header redesign
- Connection group headers are now rendered as minimal uppercase section labels (similar to Finder sidebar / Xcode navigator) instead of the previous rounded "folder" row.
- Inline `+` button on each group header opens the New Connection form for that group.
- Group count is displayed as a quiet monospaced-digit number next to the label.
- Context menu (New / Rename / Sort / Delete) and drag-and-drop drop target behavior are preserved.

#### Faster connect flow on Home view
- Clicking a saved connection on Home now loads the keychain password on a detached task so the main thread isn't blocked by a keychain prompt.
- If the user denies/cancels the keychain prompt (or there's no stored password), the connect flow silently aborts instead of attempting to connect with an empty password.
- SQLite connections skip the keychain lookup entirely (SQLite has no password).

### Fixed

- **App quit when closing the Connection form panel** — The global `WindowCloseInterceptor` (installed on every window that became key) was also being attached to `ConnectionFormPanel`, an `NSPanel`. Clicking its red ✕ posted `.windowCloseRequested`, which `MainView` interpreted as "Home → quit" and called `NSApplication.shared.terminate(nil)`. The interceptor is now only attached to the main content window — `NSPanel` instances and any window where `canBecomeMain == false` are skipped. Closing the connection form now just dismisses the panel.
- **Connection form panel appeared in the top-right corner of the screen** — `panel.center()` was called before the hosting controller had laid out, so the panel was positioned using the initial 580×520 contentRect and then drifted when its real size was applied. The panel is now centered over `NSApp.mainWindow` after calling `layoutIfNeeded()`, so it appears exactly in the middle of the app window.

## [0.10.0] - 2026-04-05

### Added

#### Table import (sidebar context menu)
- **Import → From CSV…** — Opens `ImportCSVSheet` with:
  - File picker for `.csv` files
  - Delimiter picker (`,` `;` `\t` `|`)
  - First-row-as-header toggle
  - **Auto column mapping** — CSV header columns are matched to table columns by name; user can override or skip via per-column Picker
  - Live preview of first 5 rows
  - Generates `INSERT INTO … VALUES …` for each row with proper quoting and escaping
  - Shows imported row count / errors
- **Import → From SQL Dump…** — Reads an `.sql` file, splits into statements, and executes each one against the active connection. Shows a result alert with success/fail counts and the first error message (if any).

#### Full-fidelity SQL export (matches TablePlus format)
New `ExportService.exportTableSQL(description:rows:databaseType:databaseName:to:)` emits:
- Header comment block with database name, table name, generation timestamp
- `DROP TABLE IF EXISTS "schema"."table"`
- `CREATE SEQUENCE IF NOT EXISTS …` — auto-detected from columns whose default contains `nextval(…)`
- `CREATE TABLE` with full column definitions (types, `NOT NULL`, `DEFAULT`), primary key constraint
- **Multi-row `INSERT`** — single statement for all rows, much smaller and faster than per-row statements
- **Type-aware value formatting**: numerics unquoted, booleans as `true`/`false`, strings/dates quoted & escaped
- `CREATE INDEX` for all non-primary-key indices, including `USING method`, `INCLUDE (…)`, and partial `WHERE` conditions (PostgreSQL)

The old `exportSQL(data:table:to:)` is kept for backward compatibility but `ExportTableSheet` now always routes SQL exports through the new method (calls `describeTable` first to fetch the structure).

#### Smart SQL statement splitter
- `SidebarItemRow.splitSQLStatements(_:)` — Full state-machine tokenizer that splits a SQL dump correctly:
  - Skips `;` inside single-quoted strings (handles `''` escape)
  - Skips `;` inside double-quoted identifiers
  - Skips `;` inside `--` line comments and `/* */` block comments
- Replaces the previous naive `components(separatedBy: ";")` approach that would mis-split dumps containing strings with semicolons or timestamps

### Fixed
- **SQL export was not re-importable** — Previously `exportSQL` emitted only `INSERT` statements without any schema. Importing the file into an empty database failed with `relation "table" does not exist`. The new export includes DROP/CREATE/sequences/indices so the file can recreate the table exactly.

### Changed
- Removed noisy `[Structure]` debug `print` statements from `PostgreSQLAdapter.describeTable`

## [0.9.0] - 2026-04-05

### Added

#### CreateTableView — synced with Structure editor
- **Columns table** now has the same 8 columns as the Structure editor: #, column_name, data_type, is_nullable, check, foreign_key, column_default, comment. The standalone `PK` checkbox column was removed; toggle primary key via right-click context menu ("Set as Primary Key" / "Unset Primary Key").
- **Foreign key editor popover** — click the `foreign_key` cell to open a popover with Referenced Table picker (loads from current schema) and Referenced Column field. Generates `FOREIGN KEY (col) REFERENCES table(col)` in the CREATE TABLE.
- **Default value editor popover** — click the `column_default` cell to open a tabbed popover (String / Expression) with quick-pick buttons for CURRENT_TIMESTAMP, NOW(), gen_random_uuid(), true, false.
- **CHECK constraint** editable inline in the check column, emitted as `CHECK (expr)` in CREATE TABLE.
- **Indexes table** now has the same 7 columns as Structure: index_name, index_algorithm, is_unique, column_name, condition, include, comment. Supports partial indexes (`WHERE`) and INCLUDE columns (PostgreSQL).
- **Inline "+ New column" / "+ New index" buttons** below each table (matches Structure editor style). The toolbar no longer has these buttons; toolbar now only shows table name input, schema label, and Create button.
- **Default table name** is pre-filled as `new_table` so the Create button is enabled immediately.
- **First column auto-selected** on view appear so it's immediately editable.
- **Auto-close Create Table tab** after successful creation.

#### Delete table — mark-for-deletion workflow
- **Delete table popup** with two options (matches TablePlus):
  - ☐ **Ignore foreign key checks** — MySQL `SET FOREIGN_KEY_CHECKS = 0` around the DROP.
  - ☐ **Cascade** — PostgreSQL `DROP TABLE … CASCADE`.
- **Tables are not dropped immediately** — clicking OK marks the table for deletion. The sidebar row shows a red background and strikethrough text so the user can see what's pending.
- **Undo Delete** — right-click a pending table to clear the mark.
- **Commit via toolbar ✓ button** — the existing Commit Changes toolbar button (`checkmark.circle`) now also commits pending table deletions by observing `.commitChanges` in the sidebar. Runs `DROP TABLE IF EXISTS` for each marked table, applies cascade/ignore-FK flags per-table, closes any open tabs referencing dropped tables, and refreshes the sidebar.
- `AppState.pendingTableDeletions` `[String: PendingTableDeletion]` holds the marked tables with their per-table options.
- `DeleteTableSheet.swift` — new SwiftUI sheet for the delete confirmation.

## [0.8.1] - 2026-04-05

### Added
- **Inline "+" button on Tables group** in the sidebar — click to open a new Create Table tab directly (tooltip: "New Table"). Makes the action discoverable without needing to right-click.
- **"New Table…" in table context menus** — Right-clicking a table now shows "New Table…" between "Open structure" and "Copy name". Right-clicking the "Tables" group row also shows "New Table…" and "Open Table List".

### Removed
- **Sidebar bottom bar `+` and trash buttons** — Replaced by the inline "+" button on the Tables group and the context menu actions. Bottom bar now only shows the schema picker.

## [0.8.0] - 2026-04-05

### Added
- **Window auto-resize on launch / connect**:
  - On app launch the main window shrinks to its compact minimum (900×500, centered) so the connection picker feels like a focused starting screen
  - When `activeConnectionId` becomes non-nil (user connects to a database), the window automatically zooms to fill the available screen via `NSWindow.zoom(nil)` so the user gets full workspace immediately
- **Persistent details panel width** — Width is now stored in `AppState.detailsPanelWidth` (default 320pt) and survives toggling the panel off and on. Previously every toggle reset the width to the SwiftUI ideal value.
- **Custom `ResizeHandle`** (NSViewRepresentable) — Draggable vertical divider for the details panel, backed by an `NSView` with native `mouseDown`/`mouseDragged` tracking. Avoids the SwiftUI `DragGesture` re-layout storm that caused jitter during drag. Cursor hit area is 7pt wide for easier grabbing; the visible divider is 1pt.

### Changed
- **Details panel max width** — Capped at 50% of the main screen (was a fixed 420pt). Minimum remains 220pt.
- **No animations on panel toggles** — Removed `withAnimation { ... }` wrappers and the `.animation(.easeInOut, value:)` modifier from sidebar/details/AI panel toggles. Panels now open and close instantly with a `.transaction { $0.disablesAnimations = true }` on the workspace root. Applies to: toolbar toggle buttons, Cmd+\\ (Toggle Sidebar), Cmd+Shift+A (Toggle AI Panel), and the details panel toggle.
- **Replaced `HSplitView` with fixed-width layout** for the details panel. `HSplitView` tracks its own width and resets it whenever the view is recreated; a plain `HStack` + `ResizeHandle` + explicit `.frame(width: detailsPanelWidth)` gives us full control over persistence.
- **Default window size** — Changed `.defaultSize(width: 1200, height: 700)` to `.defaultSize(width: 900, height: 500)` to match the new compact launch experience.

## [0.7.0] - 2026-04-05

### Added
- **Sidebar Query History tab** — Persistent SQL history via SwiftData:
  - Records ONLY queries run from the SQL editor (not data grid loads, structure inspections, or internal DML)
  - Grouped by day with sticky headers (Today, Yesterday, weekday name, date)
  - Row layout: status dot (green/red) · timestamp · duration (ms/s) · row count · favorite star
  - 3-line SQL preview with keyword syntax highlighting (SELECT, FROM, WHERE, JOIN… rendered bold purple)
  - Error message rendered in red beneath the SQL for failed queries
  - Hover actions: inline Copy / Paste buttons appear on mouse-over
  - Double-click a row to paste the SQL into the active query editor tab
  - Context menu: Paste to Editor, Copy SQL, Favorite/Unfavorite, Delete
  - Search bar filters by SQL text (server-side via SwiftData predicate)
  - Footer: entry count + Clear button (scoped to active connection)
  - Empty state with helpful hint
- **`AppState.recordQueryHistory`** — Dedicated method that persists a query to the SwiftData history store, separate from the in-memory `logQuery` used by the bottom SQL log panel
- **`AppState.queryHistoryVersion`** — `@Published` counter that increments when a history entry is saved, observed by `QueryHistoryTab` to trigger reload

### Fixed
- **Query history was never saved** — `SwiftDataQueryHistoryRepository` existed and was registered in DI but was only called from `QueryEngine`, which `QueryEditorView` and `DataGridView` bypass. `QueryEditorView.executeQuery` and `explainQuery` now call `recordQueryHistory` on both success and failure paths.

## [0.6.0] - 2026-04-05

### Added
- **SQL Formatter** — `Format` and `Minify` buttons next to `Run` in the query editor:
  - **Format** (Ctrl+Shift+F): beautifies SQL with clause keywords on their own line, arguments indented by one tab. SELECT columns split onto separate lines, JOINs aligned at base indent with target + ON inline, nested function calls and string literals preserved.
  - **Minify**: collapses SQL onto a single line with single-space separators (strings preserved).
  - `SQLFormatter.swift` with custom tokenizer aware of strings, comments, and multi-char operators.
- **Query editor text persistence** — Switching tabs no longer loses the query being written. `AppState.queryEditorText[tabId]` caches text per-tab; restored on `onAppear`, saved on `onChange`.
- **Run statement at cursor** — Clicking Run (or ⌘R) now executes the single SQL statement that contains the cursor, not the entire editor contents. Splits on `;` while ignoring semicolons inside strings and comments. `SQLEditorView.Coordinator` forwards cursor position via `textViewDidChangeSelection`.
- **AppKitDataGrid for query results** — Query editor results now render in the same `AppKitDataGrid` (NSTableView-backed) used for table data, with column resizing, row selection, copy row/cell context menu, and alternating row backgrounds.

### Fixed
- **PostgreSQL error messages** — Queries that fail with `PSQLError` previously surfaced as the opaque "`PostgresNIO.PSQLError error 1.`". The adapter now extracts `message`, `detail`, `hint`, `position`, and `sqlState` from `PSQLError.serverInfo` and re-throws as `DataBridgeError.queryExecutionFailed` with a human-readable message.

## [0.5.0] - 2026-04-05

### Added
- **SQL Autocomplete Engine** — Professional inline completion popup for the query editor:
  - **Fuzzy matching** with scoring: exact match (1000) > exact prefix (500) > word-boundary prefix (200) > fuzzy subsequence (1-50)
  - **Ranking**: primary SQL keywords boosted, recently-used items tracked, type priority (keyword > table > column > function > join)
  - **Search across all schema** — keywords, tables, columns (from every table), 40+ SQL functions with signature hints
  - **Fuzzy match highlighting** — matched characters rendered in bold + accent color
  - **Match types with icons**: `k` keyword, `T` table, `C` column, `f` function, `J` join
  - **Debounced typing** (80ms) with instant dismiss on space/semicolon/newline
  - **Dot-trigger** — typing `alias.` still works for column completion
  - **Manual trigger** via Ctrl+Space
- **`CompletionWindow`** — Custom NSPanel popup with NSStackView-based row layout:
  - Fixed window size (420pt wide, max 10 items)
  - Pure NSView frame layout (no NSScrollView/NSTableView) — eliminates horizontal scroll/overflow issues
  - Visual effect background with rounded corners
  - Arrow key navigation, Enter/Tab to insert, Escape to dismiss
  - Screen-edge clamping (never overflows visible screen)
- **`SQLContextParser`** — Lightweight SQL tokenizer for cursor context detection (not used in current "search-all" mode but kept for future context-aware suggestions)
- **App Icon (`.app` bundle)** — `dev.sh` now wraps the SPM executable into a proper `.app` bundle with Info.plist + AppIcon.icns so Finder/Dock show the DataBridge logo instead of the generic "exec" icon
- **`CFBundleIconFile=AppIcon`** added to Info.plist

### Fixed
- **Bundle the app** — Running the raw SPM executable showed the generic exec icon; `dev.sh` now builds `.app` bundle with icon + resources
- **Completion popup window level** — Changed from `.popUpMenu` to `.floating` with `hidesOnDeactivate=true` so the popup no longer overlays other apps when DataBridge loses focus
- **Fatal error: "Can't take a prefix of negative length"** — Clamped `cursorOffset` and `prefixLength` in completion coordinator to prevent crash when cursor is at position 0

### Changed
- Replaced `AppIcon.icns` and all Assets icon sizes with new logo
- `logo.png` updated

## [0.4.0] - 2026-04-04

### Added
- **Table Context Menu** — Right-click tables in sidebar with full action menu:
  - Open in new tab / Open structure
  - Copy name
  - Export... (CSV/JSON/SQL with multi-table selection, configurable options)
  - Copy Script As (CREATE TABLE, SELECT, INSERT, UPDATE, DELETE)
  - Truncate... / Delete... with confirmation dialogs
- **Export Dialog** — Split-panel export UI:
  - Left panel: table list with checkboxes (multi-select, default = right-clicked table)
  - Right panel: CSV/JSON/SQL format tabs with options (delimiter, quoting, line break, decimal, NULL handling, field names header)
  - Single table export via NSSavePanel, multi-table via folder picker
- **New Database Dialog** — Create new database from Cmd+K switcher (Name, Encoding, Collation)
- **Open Structure** — Context menu + `AppState.openTableStructure()` opens table directly in Structure mode
- **Pointer Cursor** — `ClickableModifier.swift` with `.pointerCursor()` and `.clickable()` modifiers applied across all interactive elements (sidebar items, tab bar, bottom bar buttons, structure editor, filter bar)
- **Structure Editor — Auto-track on deselect** — Editing a column/index and clicking away now auto-detects changes (name, type, nullable, default, comment) without requiring Enter
- **Structure Editor — Inline add buttons** — "+ New column" / "+ New index" buttons below each table for quick creation
- **Structure Editor — Bottom bar buttons** — `+ Column` and `+ Index` moved from toolbar to bottom bar next to Structure tab

### Changed
- `ContentTab` now includes `initialViewMode` field for opening tabs in specific mode
- `BottomTabBar` accepts `showAddIndex` binding and `onAddColumn` callback
- `+ Index` form changed from popover to sheet for consistent positioning
- Export menu item added to Database menu in menu bar

### Fixed
- "Open in new tab" no longer creates duplicate tabs (reuses existing via `openTable()`)
- Structure editor column cell views extracted into `@ViewBuilder` functions to prevent horizontal scroll on row select

## [0.3.0] - 2026-04-04

### Added
- **Create Table view** — Full table creation UI with columns (name, type, PK, nullable, default, comment) and indexes (name, columns, unique, algorithm)
- **SQL Log toggle** — Bottom bar button to show/hide query log panel
- **Structure editor UX** — Prevent horizontal scroll when selecting rows by extracting cell views and controlling `@FocusState`

### Changed
- Data type arrays changed from private to internal for reuse in CreateTableView

## [0.2.0] - 2026-04-04

### Added
- **Row color coding** — New rows (green), deleted rows (red), edited rows (orange) with full-row NSTableRowView backgrounds
- **Column default editor** — Tabbed popover with String/Expression/Sequence tabs
- **Mark-for-deletion** — Dropped columns/indexes stay visible with red background and strikethrough, context menu shows "Undo Drop"
- **RowBackgroundSetter** — NSViewRepresentable that walks NSView hierarchy to set NSTableRowView.backgroundColor

### Fixed
- NOT NULL column creation auto-adds DEFAULT based on data type (int→0, bool→false, timestamp→CURRENT_TIMESTAMP, etc.)
- Duplicate column prevention with `IF NOT EXISTS`
- TextField height increased with `.squareBorder` style
- Background colors clear after Apply via table reload

### Changed
- `TableDescription`, `IndexInfo`, `ForeignKeyInfo`, `ConstraintInfo` now conform to `Equatable`
- `listForeignKeys`/`listAllConstraints` return CHECK constraint definitions (all adapters)

## [0.1.0] - 2026-04-03

### Added
- **Editable Structure view** — Inline editing of columns (rename, type, nullable, default, comment) and indexes
- **Foreign Key editor** — Popover for creating/editing FK constraints with referenced table/column pickers
- **Index management** — Add/modify/drop indexes with algorithm, unique, condition, include, comment support
- **Structure change tracking** — `StructureChange` enum with SQL generation for ALTER TABLE operations
- **Commit preview** — Review generated SQL before applying structure changes

## [0.0.2] - 2026-04-02

### Added
- **DataGrid performance** — NSTableView-backed grid with virtualized rendering
- **FK/Enum/Boolean UI** — Clickable foreign key links, enum dropdowns, boolean toggles in data cells
- **Function editor** — View and edit database functions
- **Dark mode** — Full dark mode support

## [0.0.1] - 2026-04-01

### Added
- Initial release — macOS (Swift/AppKit) and Windows (C++/WinUI 3)
- Connection management (PostgreSQL, MySQL, SQLite) with SSH tunnel support
- Database browser with sidebar tree (schemas, tables, views, functions)
- Data grid with pagination, sorting, filtering
- Query editor with syntax highlighting
- AI chat integration (Anthropic/OpenAI/Ollama)
