# Gridex — Windows Build Guide

Hướng dẫn build Gridex từ source và tạo installer `Setup.exe` trên Windows, chạy local không phụ thuộc CI.

## Yêu cầu môi trường

| Thành phần | Version | Ghi chú |
|---|---|---|
| Windows | 10/11 x64 | |
| Visual Studio 2022+ | 17.x hoặc Build Tools | Phải có workload **Desktop development with C++** |
| Windows App SDK | 1.8 | Tự kéo qua NuGet `packages.config` |
| .NET SDK | 8.0 hoặc mới hơn | Cần cho `vpk` global tool |
| vcpkg | latest, ở `C:\vcpkg` | Project hardcode path này |
| PowerShell | 5.1 (pwsh cũng OK) | |

### Cài vcpkg deps (1 lần duy nhất)

Project link các lib native qua vcpkg. Cài 1 lần:

**Git Bash / MSYS2**:
```bash
/c/vcpkg/vcpkg.exe install \
  sqlite3 libpq libmariadb openssl hiredis \
  nlohmann-json cpp-httplib \
  --triplet x64-windows
```

**PowerShell**:
```powershell
C:\vcpkg\vcpkg.exe install `
  sqlite3 libpq libmariadb openssl hiredis `
  nlohmann-json cpp-httplib `
  --triplet x64-windows
```

**CMD**:
```cmd
C:\vcpkg\vcpkg.exe install ^
  sqlite3 libpq libmariadb openssl hiredis ^
  nlohmann-json cpp-httplib ^
  --triplet x64-windows
```

> Mỗi shell có cú pháp line-continuation khác nhau: bash dùng `\`, PowerShell dùng backtick `` ` ``, CMD dùng caret `^`. Hoặc bạn có thể gõ 1 dòng duy nhất không xuống dòng cũng được.

## Cấu trúc scripts

Tất cả build scripts nằm ở `windows/scripts/`:

```
windows/scripts/
├── install-vpk-tool.ps1    # Cài vpk CLI (Velopack) 1 lần
├── build-unpackaged.ps1    # Build Gridex.exe + bundle runtime DLLs
├── pack-velopack.ps1       # Pack Gridex.exe → Setup.exe
└── build-and-pack.ps1      # One-shot: install + build + pack
```

## Build Setup.exe

> **Flags bắt buộc cho mọi lệnh `powershell`**:
> - `-NoProfile`: skip load `$PROFILE` cá nhân — tránh aliases / function overrides ảnh hưởng build.
> - `-ExecutionPolicy Bypass`: Windows mặc định chặn script `.ps1` không được ký (policy `Restricted` / `AllSigned`). Flag này chỉ áp dụng cho 1 process duy nhất, không cần `Set-ExecutionPolicy` global.

### Cách nhanh nhất — 1 lệnh duy nhất

**Git Bash / MSYS2**:
```bash
cd /e/dev/be/vura
powershell -NoProfile -ExecutionPolicy Bypass -File ./windows/scripts/build-and-pack.ps1 -Version "0.1.11"
```

**PowerShell (5.1 / 7+)**:
```powershell
cd E:\dev\be\vura
powershell -NoProfile -ExecutionPolicy Bypass -File .\windows\scripts\build-and-pack.ps1 -Version "0.1.11"
```

**CMD**:
```cmd
cd /d E:\dev\be\vura
powershell -NoProfile -ExecutionPolicy Bypass -File .\windows\scripts\build-and-pack.ps1 -Version "0.1.11"
```

Script sẽ:
1. Cài/update `vpk` global tool (skip nếu đã có)
2. Build `Gridex.exe` unpackaged, stamp version `0.1.11` vào About section
3. Pack thành `Setup.exe` qua Velopack với metadata version `0.1.11`

**Output**: `windows/Releases/`
- `Gridex-stable-Setup.exe` — installer user-facing
- `Gridex-0.1.11-stable-full.nupkg` — package Velopack
- `releases.stable.json`, `RELEASES-stable` — feed manifests
- `Gridex-stable-Portable.zip` — bản portable

### Cài thử sau khi build

**Git Bash / MSYS2**:
```bash
./windows/Releases/Gridex-stable-Setup.exe
```

**PowerShell**:
```powershell
.\windows\Releases\Gridex-stable-Setup.exe
```

**CMD**:
```cmd
.\windows\Releases\Gridex-stable-Setup.exe
```

Sau khi cài, mở Settings (`Ctrl+Shift+P`) → cuộn xuống mục **About** → phải thấy đúng số version đã pass vào.

## Build từng bước (khi debug)

Khi cần iterate nhanh hơn hoặc chỉ build exe mà không pack installer:

### Bước 1 — Cài `vpk` (1 lần)

**Git Bash / MSYS2**:
```bash
powershell -NoProfile -ExecutionPolicy Bypass -File ./windows/scripts/install-vpk-tool.ps1
```

**PowerShell**:
```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\windows\scripts\install-vpk-tool.ps1
```

**CMD**:
```cmd
powershell -NoProfile -ExecutionPolicy Bypass -File .\windows\scripts\install-vpk-tool.ps1
```

### Bước 2 — Build `Gridex.exe` (nhiều lần)

**Git Bash / MSYS2**:
```bash
# Dev build — About section hiện "0.0.0-dev"
powershell -NoProfile -ExecutionPolicy Bypass -File ./windows/scripts/build-unpackaged.ps1

# Build với version cụ thể — About section hiện đúng string
powershell -NoProfile -ExecutionPolicy Bypass -File ./windows/scripts/build-unpackaged.ps1 -Version "0.1.11-test"
```

**PowerShell**:
```powershell
# Dev build — About section hiện "0.0.0-dev"
powershell -NoProfile -ExecutionPolicy Bypass -File .\windows\scripts\build-unpackaged.ps1

# Build với version cụ thể — About section hiện đúng string
powershell -NoProfile -ExecutionPolicy Bypass -File .\windows\scripts\build-unpackaged.ps1 -Version "0.1.11-test"
```

**CMD**:
```cmd
REM Dev build — About section hiện "0.0.0-dev"
powershell -NoProfile -ExecutionPolicy Bypass -File .\windows\scripts\build-unpackaged.ps1

REM Build với version cụ thể — About section hiện đúng string
powershell -NoProfile -ExecutionPolicy Bypass -File .\windows\scripts\build-unpackaged.ps1 -Version "0.1.11-test"
```

Chạy trực tiếp exe để test nhanh mà không cần cài installer:

**Git Bash / MSYS2**:
```bash
./windows/Gridex/x64/Release/Gridex/Gridex.exe
```

**PowerShell / CMD**:
```powershell
.\windows\Gridex\x64\Release\Gridex\Gridex.exe
```

### Bước 3 — Pack Setup.exe

**Git Bash / MSYS2**:
```bash
powershell -NoProfile -ExecutionPolicy Bypass -File ./windows/scripts/pack-velopack.ps1 -Version "0.1.11-test"
```

**PowerShell**:
```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\windows\scripts\pack-velopack.ps1 -Version "0.1.11-test"
```

**CMD**:
```cmd
powershell -NoProfile -ExecutionPolicy Bypass -File .\windows\scripts\pack-velopack.ps1 -Version "0.1.11-test"
```

**Lưu ý**: `-Version` truyền vào `pack-velopack.ps1` **phải khớp** với `-Version` truyền vào `build-unpackaged.ps1`, nếu không thì Velopack sẽ in một số, còn About section của app sẽ in số khác. Dùng `build-and-pack.ps1` ở trên để tránh trường hợp này.

## Version stamping — cách hoạt động

- `windows/Gridex/GridexVersion.h` — include unconditional file generated
- `windows/Gridex/GridexVersion.generated.h` — **auto-generated, gitignored**, do `build-unpackaged.ps1` ghi mỗi lần chạy
  - Có `-Version` → `#define GRIDEX_VERSION L"0.1.11"`
  - Không `-Version` → `#define GRIDEX_VERSION L"0.0.0-dev"`
- `windows/Gridex/SettingsPage.xaml.cpp` — đọc `GRIDEX_VERSION` và set vào `VersionText` text block
- CI workflow `.github/workflows/windows-release.yml` tự parse version từ git tag (`v0.1.11` → `0.1.11`) và forward sang script

Build script chỉ rewrite `GridexVersion.generated.h` khi nội dung khác → không force recompile vô ích.

## Channels (optional)

Mặc định Velopack dùng channel `stable`. Nếu muốn phát hành kênh riêng (beta/nightly):

**Git Bash / MSYS2**:
```bash
powershell -NoProfile -ExecutionPolicy Bypass -File ./windows/scripts/build-and-pack.ps1 -Version "0.1.11-beta.1" -Channel "beta"
```

**PowerShell**:
```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\windows\scripts\build-and-pack.ps1 -Version "0.1.11-beta.1" -Channel "beta"
```

**CMD**:
```cmd
powershell -NoProfile -ExecutionPolicy Bypass -File .\windows\scripts\build-and-pack.ps1 -Version "0.1.11-beta.1" -Channel "beta"
```

Output sẽ là `Gridex-beta-Setup.exe` + `releases.beta.json` — auto-updater phân luồng theo channel.

## Troubleshooting

### `MSBuild.exe not found`
Chưa cài VS 2022 với workload C++. Cài từ: <https://visualstudio.microsoft.com/downloads/>

### `dotnet SDK not found on PATH`
Cài .NET 8 SDK (hoặc mới hơn) từ: <https://dot.net/download>

### `running scripts is disabled on this system` (SecurityError / UnauthorizedAccess)
Thiếu flag `-ExecutionPolicy Bypass` trong lệnh `powershell`. Dùng đúng các lệnh trong README (đã có sẵn flag).

### `cd: E:devbevura: No such file or directory` trong Git Bash
Bash parse `\d`, `\b`, `\v` thành escape sequence → path hỏng. Fix bằng 1 trong 3 cách:
- Forward slash: `cd /e/dev/be/vura`
- Single quote: `cd 'E:\dev\be\vura'`
- Escape backslash: `cd E:\\dev\\be\\vura`

### `vpk installed but not on PATH`
Thêm `%USERPROFILE%\.dotnet\tools` vào PATH rồi mở lại terminal.

### `packages\Microsoft.Web.WebView2...\build\native\Microsoft.Web.WebView2.targets not found`
NuGet restore chưa chạy. Script có gọi `MSBuild /t:Restore /p:RestorePackagesConfig=true` rồi — nếu lỗi, check `windows/NuGet.Config` còn không.

### `vcpkg install failed` hoặc link lỗi `cannot open libpq.lib`
Chưa cài đủ vcpkg deps. Xem section "Cài vcpkg deps" ở đầu file.

### Gridex.exe crash ngay khi khởi động với `MSVCP140.dll not found`
Máy thiếu VC++ 2015-2022 Redistributable. Script `build-unpackaged.ps1` tự copy CRT DLLs vào output folder → nếu vẫn lỗi thì check VS install path / logs script.

### Setup.exe cài xong nhưng About hiện `0.0.0-dev` dù pack với `-Version 0.1.11`
Dùng `build-and-pack.ps1` thay vì gọi `pack-velopack.ps1` trực tiếp. Trường hợp chạy `pack-velopack.ps1` với version khác version lúc build `Gridex.exe` → xóa `windows\Gridex\x64\Release\` rồi build lại đúng thứ tự.

### Lỗi `pwsh.exe exited with code 9009` trong vcpkg applocal step
Bỏ qua — PowerShell Core chưa cài, vcpkg tự fallback sang `powershell.exe` ngay sau đó. Không ảnh hưởng build.

### Có ký tự lạ `[200~` hoặc `~` xuất hiện khi paste
Terminal đang bật **bracketed paste mode**. Fix: tắt bằng `printf '\e[?2004l'` (bash) hoặc chuyển sang terminal khác (Windows Terminal thường không bị).

## Cleanup

Xoá artifacts sau khi test:

**Git Bash / MSYS2**:
```bash
rm -rf ./windows/Gridex/x64 ./windows/Releases ./windows/Gridex/GridexVersion.generated.h
```

**PowerShell**:
```powershell
Remove-Item -Recurse -Force .\windows\Gridex\x64, .\windows\Releases, .\windows\Gridex\GridexVersion.generated.h -ErrorAction SilentlyContinue
```

**CMD**:
```cmd
rmdir /s /q .\windows\Gridex\x64
rmdir /s /q .\windows\Releases
del /q .\windows\Gridex\GridexVersion.generated.h
```

## CI Release (tham khảo)

CI tự chạy khi push tag matching `v*.*.*`:

```bash
git tag v0.1.11
git push origin v0.1.11
```

GitHub Actions workflow `windows-release.yml` sẽ tự build + pack + upload assets lên GitHub Release. Xem chi tiết ở `.github/workflows/windows-release.yml`.
