#!/bin/bash
# Restarts the dock if it crashes. A clean exit stops the loop.
set -u
BIN="${MAGIC_DOCK_BIN:-$HOME/.local/bin/magic-dock}"
fails=0
while true; do
    "$BIN"
    code=$?
    if [ "$code" -eq 0 ]; then
        exit 0
    fi
    fails=$((fails + 1))
    echo "magic-dock: exited $code ($fails/5)" >&2
    if [ "$fails" -ge 5 ]; then
        echo "magic-dock: stopped after 5 crashes. Run scripts/restore-cosmic-dock.sh to put the Pop!_OS dock back." >&2
        exit 1
    fi
    sleep 1
done
