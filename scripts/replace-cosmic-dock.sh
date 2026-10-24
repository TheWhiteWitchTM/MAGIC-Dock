#!/bin/bash
# Hides the COSMIC bottom dock, stops the old COSMIC Bar, and starts MAGIC Dock.
# The original entries file is kept at ~/.config/cosmic-bar/backup/entries.
# Pop!_OS dock settings under CosmicPanel.Dock are left in place. MAGIC Dock reads them.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BACKUP="$HOME/.config/cosmic-bar/backup"
ENTRIES="$HOME/.config/cosmic/com.system76.CosmicPanel/v1/entries"
PANEL_DOCK="$HOME/.config/cosmic/com.system76.CosmicPanel.Dock"

if [ ! -x "$ROOT/magic-dock" ]; then
    echo "Build magic-dock first (make) in $ROOT" >&2
    exit 1
fi
mkdir -p "$HOME/.local/bin" "$HOME/.config/autostart" "$HOME/.local/share/applications" "$BACKUP" "$HOME/.config/magic-dock"
ln -sfn "$ROOT/magic-dock" "$HOME/.local/bin/magic-dock"
install -m 0755 "$ROOT/scripts/magic-dock-session.sh" "$HOME/.local/bin/magic-dock-session"
install -m 0755 "$ROOT/scripts/restore-cosmic-dock.sh" "$HOME/.local/bin/restore-cosmic-dock"
rm -f "$HOME/.local/bin/cosmic-bar" "$HOME/.local/bin/cosmic-bar-session"

if [ ! -f "$BACKUP/entries" ]; then
    if [ -f "$ENTRIES" ]; then
        cp -a "$ENTRIES" "$BACKUP/entries"
    fi
    if [ -d "$PANEL_DOCK" ]; then
        cp -a "$PANEL_DOCK" "$BACKUP/CosmicPanel.Dock"
    fi
    echo "Backed up the COSMIC dock to $BACKUP"
else
    echo "Backup already exists at $BACKUP (left untouched)"
fi

cat > "$HOME/.config/autostart/magic-dock.desktop" << EOF
[Desktop Entry]
Type=Application
Name=MAGIC Dock
Comment=Dock that follows the Pop!_OS dock settings
Exec=$HOME/.local/bin/magic-dock-session
X-GNOME-Autostart-enabled=true
OnlyShowIn=COSMIC;
StartupNotify=false
EOF
rm -f "$HOME/.config/autostart/cosmic-bar.desktop"

cat > "$HOME/.local/share/applications/restore-cosmic-dock.desktop" << EOF
[Desktop Entry]
Type=Application
Name=Restore COSMIC Dock
Comment=Stop MAGIC Dock and put the original bottom dock back
Exec=$HOME/.local/bin/restore-cosmic-dock
Terminal=true
Categories=Utility;
StartupNotify=false
EOF

# cosmic-panel reads this file the moment it changes. Write the whole
# document from outside the config directory, then rename it into place.
desired="$(printf '%s\n' '[' '    "Panel",' ']' '')"
if [ "$(cat "$ENTRIES" 2>/dev/null || true)" = "$desired" ]; then
    echo "Dock is already removed from $ENTRIES."
else
    tmp="$(mktemp /tmp/cosmic-panel-entries.XXXXXX)"
    printf '%s' "$desired" > "$tmp"
    mv -f "$tmp" "$ENTRIES"
    echo "Dock removed from $ENTRIES. The top panel entry is still there."
fi

stop_pid() {
    local file="$1"
    if [ -f "$file" ]; then
        local pid
        pid="$(tr -d '[:space:]' < "$file")"
        if [ -n "$pid" ]; then
            kill "$pid" 2>/dev/null || true
        fi
    fi
}
# The kernel keeps 15 characters of the process name, so the session scripts
# show up as magic-dock-sess and cosmic-bar-sess. Match the path too.
pkill -x cosmic-bar-sess 2>/dev/null || true
pkill -x magic-dock-sess 2>/dev/null || true
pkill -f "$HOME/.local/bin/cosmic-bar-session" 2>/dev/null || true
pkill -f "$HOME/.local/bin/magic-dock-session" 2>/dev/null || true
stop_pid "$HOME/.config/cosmic-bar/cosmic-bar.pid"
stop_pid "$HOME/.config/magic-dock/magic-dock.pid"
pkill -x cosmic-bar 2>/dev/null || true
pkill -x magic-dock 2>/dev/null || true
sleep 0.4

# Start in this session as well as at the next login.
setsid "$HOME/.local/bin/magic-dock-session" >> "$HOME/.config/magic-dock/session.log" 2>&1 < /dev/null &
echo "MAGIC Dock is the startup dock. Log: $HOME/.config/magic-dock/session.log"
echo "To undo: Restore COSMIC Dock in the launcher, or $HOME/.local/bin/restore-cosmic-dock"
