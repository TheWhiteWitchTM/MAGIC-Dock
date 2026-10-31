#!/bin/bash
# Build the Linux package for the version in version.txt.
# CMake builds the dock. CPack packs the self-contained folder.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

VER="$(head -n 1 "$ROOT/version.txt" | tr -d '\r' | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')"
SLUG="$(printf '%s' "$VER" | tr '[:upper:]' '[:lower:]' | tr ' _' '-' | tr -cd 'a-z0-9.-')"
TRIPLE="$(printf '%s' "$VER" | grep -oE '^[0-9]+\.[0-9]+\.[0-9]+')"
if [[ -z "$VER" || -z "$SLUG" || -z "$TRIPLE" ]]; then
    echo "magic-dock: version.txt must look like 0.9.1" >&2
    exit 1
fi
if [[ -f "$ROOT/VERSION" ]]; then
    other="$(head -n 1 "$ROOT/VERSION" | tr -d '\r' | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')"
    if [[ "$other" != "$VER" ]]; then
        echo "magic-dock: VERSION is '$other' and version.txt is '$VER'" >&2
        exit 1
    fi
fi

QT_HOST="${QT_HOST:-${QT_PREFIX:-$HOME/Qt/6.12.0/gcc_64}}"
if [[ ! -e "$QT_HOST/lib/libQt6Core.so" ]]; then
    echo "magic-dock: Qt is not at $QT_HOST" >&2
    exit 1
fi
JOBS="${JOBS:-$(nproc)}"
OUT="$ROOT/release"
rm -rf "$OUT"
mkdir -p "$OUT"

echo "magic-dock: linux build $VER"
cmake -S "$ROOT" -B "$ROOT/build-linux" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$QT_HOST"
cmake --build "$ROOT/build-linux" --parallel "$JOBS"
echo "magic-dock: self-test"
"$ROOT/build-linux/magic-dock" --self-test
cp -a "$ROOT/build-linux/magic-dock" "$ROOT/magic-dock"
echo "magic-dock: bundle"
QT_PREFIX="$QT_HOST" "$ROOT/scripts/bundle-dist.sh"
cp -a "$ROOT/LICENSE" "$ROOT/dist/LICENSE"
cat > "$ROOT/dist/README.txt" << EOF
MAGIC Dock ${VER}
Unpack this folder and run ./magic-dock
To replace the COSMIC dock, run ./scripts/replace-cosmic-dock.sh from a terminal in the session.
EOF
mkdir -p "$ROOT/dist/scripts"
cp -a "$ROOT/scripts/replace-cosmic-dock.sh" "$ROOT/scripts/restore-cosmic-dock.sh" \
    "$ROOT/scripts/magic-dock-session.sh" "$ROOT/scripts/magic-dock.desktop" \
    "$ROOT/scripts/restore-cosmic-dock.desktop" "$ROOT/dist/scripts/"

cfg="$OUT/magic-dock-CPackConfig.cmake"
cat > "$cfg" << EOF
set(CPACK_PACKAGE_NAME "magic-dock")
set(CPACK_PACKAGE_VENDOR "Mira (The White Witch)")
set(CPACK_PACKAGE_VERSION "${TRIPLE}")
set(CPACK_PACKAGE_FILE_NAME "magic-dock-${SLUG}-linux-x86_64")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "A dock for the COSMIC desktop. ${VER}.")
set(CPACK_PACKAGE_DESCRIPTION "A dock for the COSMIC desktop. ${VER}.")
set(CPACK_RESOURCE_FILE_LICENSE "${ROOT}/LICENSE")
set(CPACK_GENERATOR "TGZ")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
set(CPACK_INSTALLED_DIRECTORIES "${ROOT}/dist;/")
set(CPACK_PACKAGE_DIRECTORY "${OUT}")
set(CPACK_VERBATIM_VARIABLES ON)
EOF
(cd "$OUT" && cpack --config "$cfg")
rm -f "$cfg"
rm -rf "$OUT/_CPack_Packages"
echo "magic-dock: packages"
ls -lh "$OUT"
