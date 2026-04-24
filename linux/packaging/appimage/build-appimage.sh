#!/usr/bin/env bash
# Build AppImage for Gridex Linux.
#
# Skips `cmake --install` deliberately — the FetchContent'd mongo-c-driver /
# mongo-cxx-driver / nlohmann-json projects register install rules that dump
# ~100MB of headers + static libs into the AppDir and contribute nothing to
# the runtime. We copy the `gridex` binary directly; linuxdeploy then bundles
# Qt + the shared libs it actually links against.
#
# Requires: linuxdeploy + linuxdeploy-plugin-qt in PATH.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LINUX_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${LINUX_ROOT}/build}"
APPDIR="${APPDIR:-${LINUX_ROOT}/AppDir}"
OUT_DIR="${OUT_DIR:-${LINUX_ROOT}/dist}"

command -v linuxdeploy >/dev/null || { echo "linuxdeploy not found"; exit 1; }
command -v linuxdeploy-plugin-qt >/dev/null || { echo "linuxdeploy-plugin-qt not found"; exit 1; }

mkdir -p "${BUILD_DIR}" "${OUT_DIR}"
rm -rf "${APPDIR}"

echo "==> Configuring"
cmake -S "${LINUX_ROOT}" -B "${BUILD_DIR}" -G Ninja -DCMAKE_BUILD_TYPE=Release

echo "==> Building gridex"
cmake --build "${BUILD_DIR}" --parallel --target gridex

echo "==> Staging AppDir"
install -Dm755 "${BUILD_DIR}/gridex"            "${APPDIR}/usr/bin/gridex"
install -Dm644 "${SCRIPT_DIR}/gridex.desktop"   "${APPDIR}/usr/share/applications/gridex.desktop"
install -Dm755 "${SCRIPT_DIR}/AppRun"           "${APPDIR}/AppRun"

# Icon — fall back to repo root logo if packaging/ doesn't ship a dedicated one.
ICON_SRC="${SCRIPT_DIR}/gridex.png"
[ -f "${ICON_SRC}" ] || ICON_SRC="${LINUX_ROOT}/../logo.png"
install -Dm644 "${ICON_SRC}" "${APPDIR}/usr/share/icons/hicolor/512x512/apps/gridex.png"

echo "==> Bundling Qt + dependencies"
# linuxdeploy wants to copy resolved .so files next to the binary. Make sure
# Qt can be found via QMAKE so the qt plugin picks the right version.
export QMAKE="${QMAKE:-${CMAKE_PREFIX_PATH:+${CMAKE_PREFIX_PATH}/bin/qmake}}"
export QMAKE="${QMAKE:-/home/$USER/Qt/6.7.3/gcc_64/bin/qmake}"

cd "${OUT_DIR}"
linuxdeploy --appdir "${APPDIR}" --plugin qt --output appimage \
    --desktop-file "${APPDIR}/usr/share/applications/gridex.desktop" \
    --icon-file "${APPDIR}/usr/share/icons/hicolor/512x512/apps/gridex.png"

echo
echo "==> Done"
ls -lh "${OUT_DIR}"/Gridex*.AppImage 2>/dev/null || {
    echo "linuxdeploy finished but no AppImage produced in ${OUT_DIR}" >&2
    exit 1
}
