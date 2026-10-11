#!/usr/bin/env bash
# Build DevilutionX from MSYS2 MinGW64.
# Run from the "MSYS2 MINGW64" shell, from the repo root:
#   ./build-mingw64.sh        Native Windows build.
#                             Result: build-mingw64/devilutionx.exe with its
#                             assets/ folder and required DLLs copied next to
#                             it, so it can be launched from Explorer.
#   ./build-mingw64.sh --web  Web (Emscripten) build.
#                             Result: build-web/index.html + devilutionx.{js,wasm,data}.
#                             Requires an installed and activated Emscripten SDK
#                             (https://emscripten.org/docs/getting_started/downloads.html)
#                             with the EMSDK environment variable pointing at it,
#                             e.g. `emsdk activate latest --permanent`.

set -euo pipefail

usage() {
    echo "Usage: $0 [--web]"
    echo "  (no option)  Native Windows build into build-mingw64/"
    echo "  --web        Web (Emscripten) build into build-web/"
}

TARGET=native
case "${1:-}" in
    "") ;;
    --web | --emscripten | web | emscripten) TARGET=web ;;
    -h | --help) usage; exit 0 ;;
    *) echo "Error: unknown option '$1'." >&2; usage >&2; exit 1 ;;
esac

if [[ "${MSYSTEM:-}" != "MINGW64" ]]; then
    echo "Error: this script must run in the MSYS2 MINGW64 shell (current: ${MSYSTEM:-none})." >&2
    echo "Launch 'MSYS2 MINGW64' from the Start Menu and re-run." >&2
    exit 1
fi

echo "==> Installing dependencies (pacman)"
pacman -S --needed --noconfirm \
    git \
    mingw-w64-x86_64-cmake \
    mingw-w64-x86_64-ninja

if [[ "${TARGET}" == native ]]; then
    pacman -S --needed --noconfirm \
        mingw-w64-x86_64-gcc \
        mingw-w64-x86_64-pkgconf \
        mingw-w64-x86_64-SDL2 \
        mingw-w64-x86_64-zlib \
        mingw-w64-x86_64-bzip2
fi

# Optional: gettext tools enable building the translations (.gmo files).
pacman -S --needed --noconfirm mingw-w64-x86_64-gettext-tools 2>/dev/null \
    || pacman -S --needed --noconfirm mingw-w64-x86_64-gettext \
    || echo "Note: gettext not installed; skipping translations."

# Keep MSYS2 from rewriting -DFOO=/path arguments into Windows paths.
export MSYS2_ARG_CONV_EXCL=-D

if [[ "${TARGET}" == web ]]; then
    BUILD_DIR=build-web

    if [[ -z "${EMSDK:-}" ]]; then
        echo "Error: EMSDK is not set. Install the Emscripten SDK, then run" >&2
        echo "  emsdk install latest && emsdk activate latest --permanent" >&2
        echo "or export EMSDK=C:/path/to/emsdk before running this script." >&2
        exit 1
    fi
    EMSDK="$(cygpath -m "${EMSDK}")"
    EMSCRIPTEN_DIR="${EMSDK}/upstream/emscripten"
    if [[ ! -f "${EMSCRIPTEN_DIR}/emcc.bat" ]]; then
        echo "Error: no Emscripten found in ${EMSCRIPTEN_DIR}." >&2
        echo "Run 'emsdk install latest && emsdk activate latest' first." >&2
        exit 1
    fi

    # emcc.bat runs on EMSDK_PYTHON, falling back to whatever `python` is on
    # PATH. Prefer the Python bundled with emsdk when it isn't set already.
    if [[ -z "${EMSDK_PYTHON:-}" ]]; then
        for py in "${EMSDK}"/python/*/python.exe; do
            [[ -f "${py}" ]] && export EMSDK_PYTHON="$(cygpath -w "${py}")"
        done
    fi
    export PATH="$(cygpath -u "${EMSCRIPTEN_DIR}"):${PATH}"

    echo "==> Configuring (Emscripten $(emcc.bat --version | head -n1 | sed 's/.*) //'))"
    # The Emscripten platform file (CMake/platforms/emscripten.cmake) already
    # disables networking, testing and system libraries that don't apply.
    cmake -S. -B"${BUILD_DIR}" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="${EMSCRIPTEN_DIR}/cmake/Modules/Platform/Emscripten.cmake" \
        -DCMAKE_BUILD_TYPE=Release \
        -DNONET=ON

    echo "==> Building"
    cmake --build "${BUILD_DIR}" -j "$(nproc)"

    echo
    echo "Done: ${BUILD_DIR}/index.html"
    echo "Serve it over HTTP (opening the file directly won't work), e.g.:"
    echo "  cd ${BUILD_DIR} && emrun index.html"
    echo "Then use the in-page file manager to upload DIABDAT.MPQ (or spawn.mpq)."
    exit 0
fi

BUILD_DIR=build-mingw64

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
