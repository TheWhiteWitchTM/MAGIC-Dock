#!/bin/bash
# Build dist/ so magic-dock runs on a machine that does not have this Qt installed.
# The folder carries the binary, the Qt libraries it links, and the plugins it loads.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

if [[ ! -x "$ROOT/magic-dock" ]]; then
    echo "magic-dock: build the binary first" >&2
    exit 1
fi

if [[ -n "${QT_PREFIX:-}" && -e "$QT_PREFIX/lib/libQt6Core.so" ]]; then
    qt_prefix="$(cd "$QT_PREFIX" && pwd)"
    qt_lib="$qt_prefix/lib"
else
    core="$(ldd "$ROOT/magic-dock" | awk '/libQt6Core\.so/{print $3; exit}')"
    if [[ -z "$core" || ! -e "$core" ]]; then
        echo "magic-dock: could not locate libQt6Core for the binary" >&2
        exit 1
    fi
    qt_lib="$(cd "$(dirname "$core")" && pwd)"
    qt_prefix="$(cd "$qt_lib/.." && pwd)"
fi

rm -rf "$ROOT/dist"
mkdir -p "$ROOT/dist/bin" "$ROOT/dist/lib" "$ROOT/dist/plugins"

cp -a "$ROOT/magic-dock" "$ROOT/dist/bin/magic-dock"
cat > "$ROOT/dist/bin/qt.conf" << 'EOF'
[Paths]
Prefix = ..
Plugins = plugins
Libraries = lib
EOF

plugins=()
for rel in \
    platforms/libqoffscreen.so \
    imageformats/libqsvg.so \
    imageformats/libqico.so \
    imageformats/libqjpeg.so \
    imageformats/libqgif.so \
    imageformats/libqwebp.so \
    iconengines/libqsvgicon.so
do
    src="$qt_prefix/plugins/$rel"
    if [[ -f "$src" ]]; then
        mkdir -p "$ROOT/dist/plugins/$(dirname "$rel")"
        cp -a "$src" "$ROOT/dist/plugins/$rel"
        plugins+=("$ROOT/dist/plugins/$rel")
    fi
done

copy_lib() {
    local src="$1"
    local real base soname
    real="$(readlink -f "$src")"
    base="$(basename "$real")"
    if [[ ! -e "$ROOT/dist/lib/$base" ]]; then
        cp -a "$real" "$ROOT/dist/lib/$base"
    fi
    soname="$(readelf -d "$real" | awk -F'[][]' '/SONAME/{print $2; exit}')"
    if [[ -n "$soname" && ! -e "$ROOT/dist/lib/$soname" ]]; then
        ln -sfn "$base" "$ROOT/dist/lib/$soname"
    fi
}

declare -A seen=()
queue=("$ROOT/dist/bin/magic-dock" "${plugins[@]}")
while [[ ${#queue[@]} -gt 0 ]]; do
    bin="${queue[0]}"
    queue=("${queue[@]:1}")
    [[ -n "${seen[$bin]:-}" ]] && continue
    seen["$bin"]=1
    # Prefer the Qt tree we linked against. ldd can hand back a system copy of the
    # same SONAME when a plugin's $ORIGIN path is not filled in yet.
    while IFS= read -r line; do
        [[ -n "$line" ]] || continue
        soname="${line%% *}"
        resolved=""
        if [[ "$line" == *" => /"* ]]; then
            resolved="$(awk '{print $3}' <<< "$line")"
        fi
        if [[ -e "$qt_lib/$soname" ]]; then
            dep="$qt_lib/$soname"
        elif [[ -n "$resolved" && "$resolved" == "$qt_lib"/* ]]; then
            dep="$resolved"
        else
            continue
        fi
        copy_lib "$dep"
        queue+=("$(readlink -f "$dep")")
    done < <(ldd "$bin" || true)
done

cat > "$ROOT/dist/magic-dock" << 'EOF'
#!/bin/sh
# Launch the bundled dock. Qt libraries and the offscreen plugin live next to this script.
# The dock removes this plugin path from anything it starts, so pinned Qt apps
# still get the session's real platform plugin.
set -e
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export LD_LIBRARY_PATH="$HERE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$HERE/plugins"
export QT_QPA_PLATFORM_PLUGIN_PATH="$HERE/plugins/platforms"
exec "$HERE/bin/magic-dock" "$@"
EOF
chmod 0755 "$ROOT/dist/magic-dock"

if [[ ! -f "$ROOT/dist/plugins/platforms/libqoffscreen.so" ]]; then
    echo "magic-dock: dist is missing the offscreen platform plugin" >&2
    exit 1
fi
if [[ ! -e "$ROOT/dist/lib/libQt6Core.so.6" || ! -e "$ROOT/dist/lib/libQt6Gui.so.6" || ! -e "$ROOT/dist/lib/libQt6Widgets.so.6" ]]; then
    echo "magic-dock: dist is missing Qt libraries" >&2
    exit 1
fi

echo "magic-dock: wrote $ROOT/dist ($(du -sh "$ROOT/dist" | awk '{print $1}'))"
echo "magic-dock: launch with $ROOT/dist/magic-dock"
