# Releasing Gridex Linux

End-to-end checklist for cutting a new Linux release: bump version, build the
AppImage, upload to R2, publish the update feed.

## Prerequisites (one-time)

- `linuxdeploy` + `linuxdeploy-plugin-qt` on `$PATH`
  ```bash
  cd ~/.local/bin
  wget https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
  wget https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage
  chmod +x linuxdeploy-x86_64.AppImage linuxdeploy-plugin-qt-x86_64.AppImage
  ln -sf linuxdeploy-x86_64.AppImage linuxdeploy
  ln -sf linuxdeploy-plugin-qt-x86_64.AppImage linuxdeploy-plugin-qt
  ```
- Qt 6.4+ available — `export CMAKE_PREFIX_PATH=$HOME/Qt/6.7.3/gcc_64`
- System packages: `qt6-base-dev libxcb-cursor0 imagemagick`
  (`convert` is used to resize the icon to 512×512)
- Wrangler CLI (`npm i -g wrangler`) authenticated against the
  `cdn.gridex.app` R2 bucket, OR the Cloudflare R2 web console open.

## 1. Bump version

Edit `linux/CMakeLists.txt`:

```cmake
project(gridex
    VERSION 0.2.0      # bump here
    ...)
```

This macro flows into `GRIDEX_VERSION` at build time and is read by
`UpdateService::currentVersion()`. The semver-ish comparator treats a
suffix as a pre-release (`0.2.0 > 0.2.0-rc1`) — use `0.2.0-rc1` for
beta builds you want to ship under a separate channel.

Commit the bump on its own (`chore(release): bump version to 0.2.0`).

## 2. Build the AppImage

From the repo root:

```bash
export PATH="$HOME/.local/bin:$PATH"
export CMAKE_PREFIX_PATH="$HOME/Qt/6.7.3/gcc_64"
bash linux/packaging/appimage/build-appimage.sh
```

What the script does:
1. Configures CMake (Ninja, Release).
2. Builds **only** the `gridex` target (skips test binaries).
3. Stages the AppDir manually (binary + desktop file + 512×512 icon +
   `AppRun`). Deliberately bypasses `cmake --install` to avoid the
   FetchContent'd mongo-c / mongo-cxx / json install rules dumping
   ~100 MB of headers and static libs into the bundle.
4. Runs `linuxdeploy --plugin qt --output appimage` to bundle Qt and the
   shared libs the binary actually links against.

Output: `linux/dist/Gridex-x86_64.AppImage` (~40 MB).

Smoke-test it:

```bash
./linux/dist/Gridex-x86_64.AppImage          # should launch the GUI
./linux/dist/Gridex-x86_64.AppImage --mcp-stdio < /dev/null   # exits cleanly
```

## 3. Stage the release artifacts

Rename the AppImage to include the version, generate the feed JSON:

```bash
cd linux/dist
VER="0.2.0"                                  # match CMakeLists.txt
mv Gridex-x86_64.AppImage "Gridex-${VER}-x86_64.AppImage"
SHA=$(sha256sum "Gridex-${VER}-x86_64.AppImage" | awk '{print $1}')
SIZE=$(stat -c %s "Gridex-${VER}-x86_64.AppImage")

cat > releases.stable.json <<EOF
{
  "version": "${VER}",
  "published": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "url": "https://cdn.gridex.app/linux/Gridex-${VER}-x86_64.AppImage",
  "sha256": "${SHA}",
  "size": ${SIZE},
  "notes": "Short markdown release notes shown in the in-app dialog."
}
EOF
```

## 4. Upload to R2

Two files — the AppImage (versioned filename for cache-immutability) and
the feed JSON (stable filename, short cache).

### Via wrangler

```bash
wrangler r2 object put "gridex/linux/Gridex-${VER}-x86_64.AppImage" \
  --file "Gridex-${VER}-x86_64.AppImage" \
  --content-type application/octet-stream

wrangler r2 object put "gridex/linux/releases.stable.json" \
  --file "releases.stable.json" \
  --content-type application/json
```

Replace `gridex` with the actual R2 bucket name if different.

### Via web console

Drop both files into `linux/` inside the R2 bucket. Set:

| File | Content-Type | Cache-Control |
|---|---|---|
| `Gridex-X.Y.Z-x86_64.AppImage` | `application/octet-stream` | `public, max-age=31536000, immutable` |
| `releases.stable.json` | `application/json` | `public, max-age=300` |

The AppImage filename is unique per release, so it can be cached aggressively.
The feed JSON has a fixed name and must propagate quickly when a new
release lands — 5 min is a good balance.

## 5. Verify

```bash
curl -fsS https://cdn.gridex.app/linux/releases.stable.json | jq .
curl -fsI https://cdn.gridex.app/linux/Gridex-${VER}-x86_64.AppImage | head -10
```

Then in the app: **Help → Check for Updates…** should show the new
version, offer Install / Later, and after Install, download + verify
SHA256 + atomically swap `$APPIMAGE` + restart.

If the running build is **not** an AppImage (a dev build or a `.deb`),
the swap step is skipped — the verified AppImage is left in `$TMPDIR`
and the user is told to install it manually.

## 6. Tag + commit

```bash
git tag -a "linux-v${VER}" -m "Linux release ${VER}"
git push origin "linux-v${VER}"
```

## Landing page download URL

`releases.stable.json` always points at the latest versioned file. For a
download button on the website that doesn't need re-editing each release,
use one of:

- A Cloudflare Worker on `cdn.gridex.app/linux/latest` that fetches the
  feed and 302-redirects to `feed.url`.
- Upload the same AppImage twice each release: versioned + a stable
  alias `Gridex-latest-x86_64.AppImage`. Set `Cache-Control: max-age=300`
  on the alias.

The Worker option is preferred — single source of truth, no duplicate
40 MB upload per release, no risk of forgetting the alias step.

## Troubleshooting

**AppImage build fails with `Could not find dependency: libxcb-cursor.so.0`**
Install `libxcb-cursor0` (Ubuntu) — Qt 6 needs it at runtime.

**Icon rejected with `invalid x resolution: 841`**
The bundled `gridex.png` must be exactly one of linuxdeploy's accepted
sizes. The build script auto-resizes from `repo/logo.png` to 512×512 via
ImageMagick, but the cached resized icon at
`linux/packaging/appimage/gridex.png` wins — delete it if you replaced
the source logo.

**Update check reports "Gridex X.Y.Z is the latest version" after release**
Two likely causes:
1. The AppImage was built with the new version but the feed still points
   at the old one. Re-upload the feed.
2. R2 / Cloudflare cache. Wait 5 min (`Cache-Control: max-age=300`) or
   purge the feed URL from the dashboard.
