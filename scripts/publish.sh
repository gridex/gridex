#!/bin/bash
# publish.sh — Generate Sparkle appcast and upload release artifacts to Cloudflare R2.
#
# Usage:
#   ./scripts/publish.sh                    # Upload only the current version's DMG + appcast
#   UPLOAD_ALL=1 ./scripts/publish.sh       # Re-upload every DMG in dist/ (bucket resync)
#   DRY_RUN=1 ./scripts/publish.sh          # Generate appcast locally, skip upload
#   SKIP_APPCAST=1 ./scripts/publish.sh     # Upload existing artifacts, no appcast regen
#
# Env:
#   R2_BUCKET         Cloudflare R2 bucket name (default: gridex-downloads)
#   R2_PREFIX         Path prefix in bucket (default: macos)
#   FEED_BASE_URL     Public URL where the DMGs are served (default: https://cdn.gridex.app/macos)
#   DRY_RUN           1 = generate appcast locally, skip R2 upload
#   SKIP_APPCAST      1 = skip appcast regeneration (re-upload only)
#   UPLOAD_ALL        1 = upload every dist/*.dmg, not just the current version
#
# Requirements:
#   • generate_appcast from Sparkle (found automatically under .build/artifacts)
#   • wrangler CLI (npm i -g wrangler) with `wrangler login` completed
#   • EdDSA private key previously generated via `generate_keys` (stored in Keychain)
#
# Flow:
#   1. Find generate_appcast in SPM artifacts
#   2. Run generate_appcast against dist/ → signs each DMG with EdDSA, emits appcast.xml
#   3. Upload the current version's DMG (or everything if UPLOAD_ALL=1) + appcast.xml

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
DIST_DIR="$PROJECT_DIR/dist"

R2_BUCKET="${R2_BUCKET:-gridex}"
R2_PREFIX="${R2_PREFIX:-macos}"
FEED_BASE_URL="${FEED_BASE_URL:-https://cdn.gridex.app/macos}"
DRY_RUN="${DRY_RUN:-0}"
SKIP_APPCAST="${SKIP_APPCAST:-0}"
UPLOAD_ALL="${UPLOAD_ALL:-0}"

# Current version drives the default "new DMG only" upload path.
CURRENT_VERSION=$(/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" "$PROJECT_DIR/macos/Resources/Info.plist" 2>/dev/null || true)
if [ -z "$CURRENT_VERSION" ]; then
    echo "✗ Could not read CFBundleShortVersionString from Info.plist"
    exit 1
fi

echo "═══════════════════════════════════════════"
echo "  Publish to Cloudflare R2"
echo "  Bucket:   $R2_BUCKET"
echo "  Prefix:   $R2_PREFIX"
echo "  Feed:     $FEED_BASE_URL"
echo "  Version:  $CURRENT_VERSION"
[ "$DRY_RUN" = "1" ] && echo "  Mode:     DRY RUN (no upload)"
[ "$UPLOAD_ALL" = "1" ] && echo "  Mode:     UPLOAD ALL (re-syncing every DMG)"
echo "═══════════════════════════════════════════"

if [ ! -d "$DIST_DIR" ] || [ -z "$(ls -A "$DIST_DIR"/*.dmg 2>/dev/null)" ]; then
    echo "✗ No .dmg files in $DIST_DIR"
    echo "  Run ./scripts/release.sh or ./scripts/release-all.sh first."
    exit 1
fi

# 1. Locate generate_appcast
if [ "$SKIP_APPCAST" != "1" ]; then
    GENERATE_APPCAST=$(find "$PROJECT_DIR/.build/artifacts" -type f -name "generate_appcast" 2>/dev/null | head -1)
    if [ -z "$GENERATE_APPCAST" ]; then
        echo "✗ generate_appcast not found. Run 'swift build' at least once to fetch Sparkle artifacts."
        exit 1
    fi

    echo "→ Generating appcast.xml via $GENERATE_APPCAST..."
    # --download-url-prefix tells Sparkle where to fetch DMGs from at update time.
    "$GENERATE_APPCAST" \
        --download-url-prefix "$FEED_BASE_URL/" \
        "$DIST_DIR"
    echo "✓ Appcast generated: $DIST_DIR/appcast.xml"
fi

if [ "$DRY_RUN" = "1" ]; then
    echo ""
    echo "Dry run complete. Artifacts ready in $DIST_DIR:"
    ls -lh "$DIST_DIR"/*.dmg "$DIST_DIR"/appcast.xml 2>/dev/null || true
    exit 0
fi

# 2. Check for wrangler
if ! command -v wrangler >/dev/null 2>&1; then
    echo "✗ wrangler not found. Install with: npm i -g wrangler"
    exit 1
fi

# 3. Collect DMGs to upload — by default just the current version; the appcast
#    still references every historical build from $FEED_BASE_URL, but those
#    DMGs were already uploaded in previous publish runs. Pass UPLOAD_ALL=1 to
#    resync the full bucket (e.g. after a bucket wipe or prefix migration).
if [ "$UPLOAD_ALL" = "1" ]; then
    candidate_dmgs=( "$DIST_DIR"/*.dmg )
else
    candidate_dmgs=( "$DIST_DIR/Gridex-${CURRENT_VERSION}-"*.dmg )
fi
dmgs_to_upload=()
for dmg in "${candidate_dmgs[@]}"; do
    [ -f "$dmg" ] && dmgs_to_upload+=("$dmg")
done
if [ ${#dmgs_to_upload[@]} -eq 0 ]; then
    echo "✗ No DMG found for version $CURRENT_VERSION in $DIST_DIR"
    echo "  Expected: $DIST_DIR/Gridex-${CURRENT_VERSION}-*.dmg"
    echo "  Run ./scripts/release.sh or ./scripts/release-all.sh first,"
    echo "  or pass UPLOAD_ALL=1 to upload every existing DMG."
    exit 1
fi

echo "→ Uploading DMGs to R2..."
for dmg in "${dmgs_to_upload[@]}"; do
    name=$(basename "$dmg")
    echo "  ↑ $name"
    wrangler r2 object put "$R2_BUCKET/$R2_PREFIX/$name" \
        --file "$dmg" \
        --content-type "application/x-apple-diskimage" \
        --remote
done

# 4. Upload appcast.xml (cache-control short so updates propagate fast)
if [ -f "$DIST_DIR/appcast.xml" ]; then
    echo "  ↑ appcast.xml"
    wrangler r2 object put "$R2_BUCKET/$R2_PREFIX/appcast.xml" \
        --file "$DIST_DIR/appcast.xml" \
        --content-type "application/xml" \
        --cache-control "public, max-age=300" \
        --remote
fi

echo ""
echo "═══════════════════════════════════════════"
echo "  ✓ Published to R2"
echo "  Appcast: $FEED_BASE_URL/appcast.xml"
echo "═══════════════════════════════════════════"
