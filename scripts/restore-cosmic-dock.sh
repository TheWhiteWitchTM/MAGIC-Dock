#!/bin/bash
# Puts the original COSMIC dock back and stops MAGIC Dock. The top panel is not touched.
set -u
BACKUP="$HOME/.config/cosmic-bar/backup"
ENTRIES="$HOME/.config/cosmic/com.system76.CosmicPanel/v1/entries"
mkdir -p "$(dirname "$ENTRIES")"
if [ -f "$BACKUP/entries" ]; then
    cp -a "$BACKUP/entries" "$ENTRIES"
    echo "Restored $ENTRIES from backup."
else
    printf '%s\n' '[' '    "Panel",' '    "Dock",' ']' > "$ENTRIES"
    echo "Wrote the default Panel + Dock entries."
fi
if [ -d "$BACKUP/CosmicPanel.Dock" ]; then
    rm -rf "$HOME/.config/cosmic/com.system76.CosmicPanel.Dock"
    cp -a "$BACKUP/CosmicPanel.Dock" "$HOME/.config/cosmic/com.system76.CosmicPanel.Dock"
    echo "Restored the Dock panel settings."
fi
rm -f "$HOME/.config/autostart/magic-dock.desktop" "$HOME/.config/autostart/cosmic-bar.desktop"
# The kernel keeps 15 characters of the process name, so the session scripts
# show up as magic-dock-sess and cosmic-bar-sess. Match the path too.
pkill -x magic-dock-sess 2>/dev/null || true
pkill -x cosmic-bar-sess 2>/dev/null || true
pkill -f "$HOME/.local/bin/magic-dock-session" 2>/dev/null || true
pkill -f "$HOME/.local/bin/cosmic-bar-session" 2>/dev/null || true
stop_one() {
    local file="$1"
    if [ -f "$file" ]; then
        local pid
        pid="$(tr -d '[:space:]' < "$file")"
        if [ -n "$pid" ]; then
            kill "$pid" 2>/dev/null || true
        fi
    fi
}
stop_one "$HOME/.config/magic-dock/magic-dock.pid"
stop_one "$HOME/.config/cosmic-bar/cosmic-bar.pid"
pkill -x magic-dock 2>/dev/null || true
pkill -x cosmic-bar 2>/dev/null || true
echo "MAGIC Dock is stopped. The top panel was left as it was."
echo "If the original dock does not return, log out and back in. Super still opens the launcher."
