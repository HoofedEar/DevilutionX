#!/usr/bin/env bash
# Native Windows build of DevilutionX using MSYS2 MinGW64.
# Run from the "MSYS2 MINGW64" shell, from the repo root:
#   ./build-mingw64.sh
# Result: build-mingw64/devilutionx.exe with its assets/ folder and
# required DLLs copied next to it, so it can be launched from Explorer.

set -euo pipefail

if [[ "${MSYSTEM:-}" != "MINGW64" ]]; then
    echo "Error: this script must run in the MSYS2 MINGW64 shell (current: ${MSYSTEM:-none})." >&2
    echo "Launch 'MSYS2 MINGW64' from the Start Menu and re-run." >&2
    exit 1
fi

BUILD_DIR=build-mingw64

echo "==> Installing dependencies (pacman)"
pacman -S --needed --noconfirm \
    git \
    mingw-w64-x86_64-gcc \
    mingw-w64-x86_64-cmake \
    mingw-w64-x86_64-ninja \
    mingw-w64-x86_64-pkgconf \
    mingw-w64-x86_64-SDL2 \
    mingw-w64-x86_64-zlib \
    mingw-w64-x86_64-bzip2

# Optional: gettext tools enable building the translations (.gmo files).
pacman -S --needed --noconfirm mingw-w64-x86_64-gettext-tools 2>/dev/null \
    || pacman -S --needed --noconfirm mingw-w64-x86_64-gettext \
    || echo "Note: gettext not installed; skipping translations."

# Keep MSYS2 from rewriting -DFOO=/path arguments into Windows paths.
export MSYS2_ARG_CONV_EXCL=-D

echo "==> Configuring"
# NONET=ON: single-player only build; skips ZeroTier (libzt), TCP
# networking, and libsodium entirely.
cmake -S. -B"${BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF \
    -DDEVILUTIONX_SYSTEM_BZIP2=OFF \
    -DNONET=ON

echo "==> Building"
cmake --build "${BUILD_DIR}" -j "$(nproc)"

echo "==> Copying runtime DLLs next to the exe"
ldd "${BUILD_DIR}/devilutionx.exe" \
    | awk '$3 ~ /\/mingw64\// { print $3 }' \
    | sort -u \
    | xargs -r cp -u -t "${BUILD_DIR}"

echo
echo "Done: ${BUILD_DIR}/devilutionx.exe"
echo "Remember to place DIABDAT.MPQ (or the shareware spawn.mpq) in the game folder"
echo "or in %AppData%\\diasurgical\\devilution if the game can't find the game data."
