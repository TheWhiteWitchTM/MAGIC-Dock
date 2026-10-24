# MAGIC Dock

This is a prebuilt dock for the COSMIC desktop on 64-bit Pop!_OS. Qt 6.12 is in `lib/` and `plugins/`. You need glibc 2.38 or newer, which Pop!_OS 24.04 has, and a normal COSMIC session for Wayland and the system graphics libraries.

Run it once:

```bash
./magic-dock
```

Use it in place of the COSMIC dock, including at the next login:

```bash
./scripts/replace-cosmic-dock.sh
```

The top panel stays. A backup of the panel list is written once to `~/.config/cosmic-bar/backup/`.

Put the original dock back from **Restore COSMIC Dock** in the launcher, or:

```bash
~/.local/bin/restore-cosmic-dock
```

Source and the full description: https://github.com/TheWhiteWitchTM/MAGIC-Dock

The dock is MIT. See `LICENSE`. Qt, ICU, and the image libraries in the plugins have their own licenses. See `NOTICE`.
