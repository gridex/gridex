#!/usr/bin/env bash
# release.sh — Cut a Linux AppImage release.
#
# Usage:
#   ./linux/scripts/release.sh                  # build at CMakeLists' current version
#   ./linux/scripts/release.sh 0.3.0            # bump to 0.3.0 + build
#   UPLOAD=1   ./linux/scripts/release.sh 0.3.0 # also wrangler-upload to R2
#   GH_RELEASE=1 ./linux/scripts/release.sh 0.3.0 # also create GitHub release
#
# Env:
#   R2_BUCKET       Cloudflare R2 bucket name           (default: gridex)
#   R2_PREFIX       Path prefix in the bucket           (default: linux)
#   R2_ACCOUNT_ID   Team Cloudflare account id          (default: 04285282d537ae420d72fa21d0de38af — Gridex team)
#   FEED_BASE_URL   Public URL where AppImages live     (default: https://cdn.gridex.app/linux)
#   FEED_FILE       Stable-channel feed JSON name       (default: releases.stable.json)
#   UPLOAD          1 = run wrangler r2 put for both files
#   GH_RELEASE      1 = create GitHub release v<version>-linux with the AppImage attached
#
# Important: wrangler 3.x defaults `r2 object put` to a LOCAL emulator.
# This script always passes --remote so writes hit real R2, and accepts
# R2_ACCOUNT_ID so multi-account users don't silently push to the wrong
# Cloudflare account.
#   NOTES           Release notes string (default: short auto blurb)
#
# Steps (all stop on first error):
#   1. Bump linux/CMakeLists.txt VERSION if a version arg is given.
#   2. Build the AppImage via packaging/appimage/build-appimage.sh.
#   3. Regenerate linux/dist/releases.stable.json (sha256, size, url, ISO timestamp).
#   4. Print the wrangler/gh commands (or run them when UPLOAD/GH_RELEASE=1).
#
# Requirements:
#   • linuxdeploy + linuxdeploy-plugin-qt in PATH (consumed by build-appimage.sh)
#   • jq (for safe JSON emission)
#   • wrangler CLI logged in (`wrangler login`) when UPLOAD=1
#   • gh CLI authenticated (`gh auth login`)        when GH_RELEASE=1

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LINUX_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
DIST_DIR="${LINUX_ROOT}/dist"
CMAKE_FILE="${LINUX_ROOT}/CMakeLists.txt"

R2_BUCKET="${R2_BUCKET:-gridex}"
R2_PREFIX="${R2_PREFIX:-linux}"
R2_ACCOUNT_ID="${R2_ACCOUNT_ID:-04285282d537ae420d72fa21d0de38af}"   # Gridex team account.
FEED_BASE_URL="${FEED_BASE_URL:-https://cdn.gridex.app/linux}"
FEED_FILE="${FEED_FILE:-releases.stable.json}"
UPLOAD="${UPLOAD:-0}"
GH_RELEASE="${GH_RELEASE:-0}"

# wrangler 4.x defaults `r2 object put` to LOCAL mode (Miniflare emulator).
# Always pass --remote so the put hits the real R2 bucket. Account
# targeting goes through the CLOUDFLARE_ACCOUNT_ID env var — wrangler 4
# dropped the --account-id flag for `r2 object put`.
WRANGLER_FLAGS="--remote"
if [ -n "${R2_ACCOUNT_ID}" ]; then
    export CLOUDFLARE_ACCOUNT_ID="${R2_ACCOUNT_ID}"
fi

# ── 1. Resolve version ─────────────────────────────────────────
NEW_VERSION="${1:-}"
CURRENT_VERSION=$(grep -E "^\s*VERSION\s+[0-9]+\.[0-9]+\.[0-9]+" "${CMAKE_FILE}" \
                   | head -1 | awk '{print $2}')

if [ -z "${CURRENT_VERSION}" ]; then
    echo "✗ Could not read VERSION from ${CMAKE_FILE}"
    exit 1
fi

if [ -n "${NEW_VERSION}" ] && [ "${NEW_VERSION}" != "${CURRENT_VERSION}" ]; then
    echo "→ Bumping ${CURRENT_VERSION} → ${NEW_VERSION} in CMakeLists.txt"
    sed -i.bak -E "s/^(\s*VERSION\s+)[0-9]+\.[0-9]+\.[0-9]+/\1${NEW_VERSION}/" "${CMAKE_FILE}"
    rm -f "${CMAKE_FILE}.bak"
    CURRENT_VERSION="${NEW_VERSION}"
fi

ARCH="x86_64"
APPIMAGE_NAME="Gridex-${CURRENT_VERSION}-${ARCH}.AppImage"
APPIMAGE_PATH="${DIST_DIR}/${APPIMAGE_NAME}"

echo "═══════════════════════════════════════════"
echo "  Gridex Linux release"
echo "  Version:  ${CURRENT_VERSION}"
echo "  AppImage: ${APPIMAGE_NAME}"
echo "  Bucket:   r2://${R2_BUCKET}/${R2_PREFIX}/"
echo "  Feed:     ${FEED_BASE_URL}/${FEED_FILE}"
[ "${UPLOAD}" = "1" ] && echo "  Mode:     will upload to R2 via wrangler"
[ "${GH_RELEASE}" = "1" ] && echo "  Mode:     will cut GitHub release"
echo "═══════════════════════════════════════════"

# ── 2. Build the AppImage ──────────────────────────────────────
echo "→ Building AppImage"
"${LINUX_ROOT}/packaging/appimage/build-appimage.sh"

# linuxdeploy emits a file like "Gridex-x86_64.AppImage" by default — rename
# to include the version so historical releases coexist in dist/ and R2.
if [ ! -f "${APPIMAGE_PATH}" ]; then
    fallback=$(find "${DIST_DIR}" -maxdepth 1 -type f -name "Gridex*.AppImage" \
                   -newer "${CMAKE_FILE}" 2>/dev/null | head -1 || true)
    if [ -n "${fallback}" ] && [ "${fallback}" != "${APPIMAGE_PATH}" ]; then
        mv "${fallback}" "${APPIMAGE_PATH}"
    fi
fi

if [ ! -f "${APPIMAGE_PATH}" ]; then
    echo "✗ AppImage not found at ${APPIMAGE_PATH}"
    echo "  Check linuxdeploy output above."
    exit 1
fi

SIZE=$(stat -c %s "${APPIMAGE_PATH}")
SHA256=$(sha256sum "${APPIMAGE_PATH}" | awk '{print $1}')
PUBLISHED=$(date -u +"%Y-%m-%dT%H:%M:%SZ")
NOTES="${NOTES:-Linux ${CURRENT_VERSION}}"

echo "→ sha256 ${SHA256}"
echo "→ size   ${SIZE} bytes"

# ── 3. Regenerate the stable-channel feed ──────────────────────
FEED_PATH="${DIST_DIR}/${FEED_FILE}"
command -v jq >/dev/null || { echo "✗ jq is required (sudo apt install jq)"; exit 1; }

jq -n \
  --arg v        "${CURRENT_VERSION}" \
  --arg p        "${PUBLISHED}" \
  --arg url      "${FEED_BASE_URL}/${APPIMAGE_NAME}" \
  --arg sha      "${SHA256}" \
  --argjson size "${SIZE}" \
  --arg notes    "${NOTES}" \
  '{version:$v, published:$p, url:$url, sha256:$sha, size:$size, notes:$notes}' \
  > "${FEED_PATH}"

echo "→ Wrote ${FEED_PATH}"
cat "${FEED_PATH}"
echo

# ── 4. Upload (optional) ───────────────────────────────────────
WRANGLER_APPIMAGE="wrangler r2 object put ${R2_BUCKET}/${R2_PREFIX}/${APPIMAGE_NAME} --file ${APPIMAGE_PATH} --content-type application/octet-stream ${WRANGLER_FLAGS}"
WRANGLER_FEED="wrangler r2 object put ${R2_BUCKET}/${R2_PREFIX}/${FEED_FILE} --file ${FEED_PATH} --content-type application/json ${WRANGLER_FLAGS}"

if [ "${UPLOAD}" = "1" ]; then
    command -v wrangler >/dev/null || { echo "✗ wrangler not in PATH (npm i -g wrangler)"; exit 1; }
    echo "→ Uploading AppImage to R2"
    eval "${WRANGLER_APPIMAGE}"
    echo "→ Uploading feed to R2"
    eval "${WRANGLER_FEED}"
else
    echo "Skipping upload. Run these manually (or rerun with UPLOAD=1):"
    echo "  ${WRANGLER_APPIMAGE}"
    echo "  ${WRANGLER_FEED}"
fi

# ── 5. GitHub release (optional) ───────────────────────────────
if [ "${GH_RELEASE}" = "1" ]; then
    command -v gh >/dev/null || { echo "✗ gh CLI not in PATH"; exit 1; }
    TAG="v${CURRENT_VERSION}-linux"
    echo "→ Creating GitHub release ${TAG}"
    gh release create "${TAG}" "${APPIMAGE_PATH}" \
        --title "Linux ${CURRENT_VERSION}" \
        --notes "${NOTES}" || \
    gh release upload "${TAG}" "${APPIMAGE_PATH}" --clobber
fi

echo
echo "✓ Done. Output: ${APPIMAGE_PATH}"
