# Contributing to Gridex

Thanks for your interest in improving Gridex. This document covers the basics; more context lives in [README.md](README.md) and [CLAUDE.md](CLAUDE.md).

## Ways to contribute

- **Bug reports** — open a GitHub issue with reproduction steps, expected vs. actual, and your OS/version.
- **Feature requests** — open an issue first to discuss. Describe the problem before the proposed solution.
- **Pull requests** — for anything beyond a small typo fix, please open an issue first so we can align on scope.
- **Documentation** — improvements to README, guides, or inline comments are always welcome.

## Before you start

### For bug reports

Search for existing issues first. When opening a new one, include:
- Gridex version (from About window)
- OS and version (macOS 14+, Windows 10+, Ubuntu 22.04+, etc.)
- Database type and version
- Steps to reproduce — the more precise, the better
- Expected behavior vs. actual behavior
- Any relevant log output (from Console.app on macOS, Event Viewer on Windows)

### For feature requests

Open an issue with:
- The problem you're trying to solve (not just the feature you want)
- Who benefits and how
- Any constraints or edge cases you know about

This lets us discuss before you invest time in code.

## Development setup

### macOS

Prerequisites:
- macOS 14 (Sonoma) or later
- Xcode 15.3+ / Swift 5.10+

```bash
git clone https://github.com/YOUR_FORK/gridex.git
cd gridex

# Debug build — ad-hoc signed, runs locally
./scripts/build-app.sh
open dist/Gridex.app

# Or run directly (no bundle)
swift build
.build/debug/Gridex
```

### Linux

Prerequisites:
- Ubuntu 22.04+/24.04, Debian 12, Fedora 40, or any distro with Qt 6 ≥ 6.4
- GCC ≥ 11 or Clang ≥ 14, CMake ≥ 3.24, Ninja

```bash
git clone https://github.com/YOUR_FORK/gridex.git
cd gridex

cmake -S linux -B linux/build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build linux/build --parallel
./linux/build/gridex
```

See [linux/README.md](linux/README.md) for full build and packaging instructions including AppImage creation.

### Windows

Prerequisites:
- Windows 10 or later (64-bit)
- Visual Studio 2022+, .NET 8 SDK, vcpkg

See [windows/README.md](windows/README.md) for the full WinUI 3 / vcpkg / Velopack toolchain.

### Optional: Node 20+

Required only if working on the `landing/` folder (marketing site).

## Project structure

```
macos/        macOS app (Swift + AppKit + SwiftUI)
windows/      Windows app (C++ / WinUI 3 / cppwinrt)
linux/        Linux app (C++20 / Qt 6) — adapters, MCP, AppImage
landing/      Marketing site (Next.js)
scripts/      Build, sign, notarize, release scripts
```

See [CLAUDE.md](CLAUDE.md) for architecture conventions (Clean Architecture, 5 layers).

## Running tests

Gridex currently has a smoke-test script for the MCP server component:

```bash
# Requires a debug build first
swift build
./scripts/test-mcp.sh .build/debug/Gridex
```

The script starts the MCP server in stdio mode, sends an `initialize` request, and verifies a correct JSON-RPC response. It cleans up named pipes and the server process on exit.

For the macOS app, run Xcode's test suite or use the Test Navigator. For Linux/Windows, check the respective sub-project test documentation.

## Code style

- **Swift**: follow the surrounding style. Prefer `struct` over `class` unless you need identity. Use `actor` for thread-safe services. Run `swift format` before committing if available.
- **C++ (Windows/Linux)**: match the existing project conventions. Use `const` wherever applicable; avoid raw pointers for owned memory.
- No comments that restate what the code does. Add a comment only when the *why* is non-obvious.
- Small, focused commits. Conventional commits (`feat:`, `fix:`, `chore:`, `refactor:`) encouraged but not required.

## Commit conventions

We use a lightweight conventional-commit style:

```
feat: add MongoDB nested field filter support
fix: prevent out-of-bounds crash in MSSQL schema introspection
chore: update RediStack to 1.6.0
refactor: extract SQL sanitization into MCPSQLSanitizer
docs: clarify SSH tunnel auth options in README
```

Keep each commit doing one thing. Rebase freely before opening a PR — a clean commit history makes review easier but is not required.

## Pull request checklist

Before marking a PR ready for review:

- [ ] Build passes: `swift build` (macOS) or `cmake --build linux/build` (Linux) succeeds with no new warnings
- [ ] Manual smoke test of the affected feature on macOS 14+ or your target platform
- [ ] Updated [CHANGELOG.md](CHANGELOG.md) with a one-liner under the `[Unreleased]` heading (or create it if missing)
- [ ] No personal API keys, paths, or credentials in the diff (automated via `.gitignore`)
- [ ] PR description explains *what* changed and *why*, with a link to the relevant issue

## PR review process

- All PRs require at least one review before merge.
- Small PRs (typos, docs, small bug fixes): one review sufficient.
- Feature PRs: we may request a second review or ask you to demo the changes.
- If a PR goes stale for > 14 days with no activity, we may close it — you can always reopen if the use case is still relevant.

## Labels we use

| Label | Meaning |
|-------|---------|
| `bug` | Confirmed bug, needs a fix |
| `enhancement` | Feature request or improvement |
| `help wanted` | Good first issue, we can mentor |
| `good first issue` | Suitable for new contributors |
| `documentation` | Docs improvement task |
| `question` | Not yet a bug or feature, needs discussion |
| `windows` / `linux` / `macos` | Platform-specific |

## License

By contributing, you agree that your contributions will be licensed under the [Apache License 2.0](LICENSE) — the same license as the project.