# MAGIC Dock

This folder is a prebuilt dock for the COSMIC desktop on 64-bit Pop!_OS. Qt 6.12 is in `lib/` and `plugins/`. You need glibc 2.38 or newer, which Pop!_OS 24.04 has. A normal COSMIC install supplies Wayland and the graphics libraries.

Run these commands from a terminal on the COSMIC desktop. The replace script uses that session, including `WAYLAND_DISPLAY`.

## Try it

```bash
./magic-dock
```

`magic-dock` is a script. It adds `lib/` and `plugins/` beside it, then runs `bin/magic-dock`. The process stays in that terminal, and the COSMIC dock stays up, so you will see two bars. Ctrl+C stops MAGIC Dock. From another terminal, `pkill -x magic-dock` stops it.

A second copy prints `magic-dock: already running` and exits.

## Use it in place of the COSMIC dock

```bash
./scripts/replace-cosmic-dock.sh
```

The script does all of the following:

1. Links `~/.local/bin/magic-dock` at `./magic-dock` in this folder. Leave the folder where it is. Moving or deleting it breaks that link.
2. Copies `magic-dock-session` and `restore-cosmic-dock` into `~/.local/bin/`. The session program starts the dock again after a crash, and stops after a clean exit or after five crashes.
3. Writes `~/.config/autostart/magic-dock.desktop`, so the next COSMIC login starts it. The top panel is left in place.
4. Adds a launcher entry named **Restore COSMIC Dock**.
5. Backs up the COSMIC panel list once, to `~/.config/cosmic-bar/backup/`. A backup that already exists is left alone.
6. Removes the Dock entry from `~/.config/cosmic/com.system76.CosmicPanel/v1/entries`. Pop dock settings under `CosmicPanel.Dock` stay on disk. MAGIC Dock reads them.
7. Stops a running COSMIC Bar or MAGIC Dock, then starts MAGIC Dock in this session.

Output from the running dock goes to `~/.config/magic-dock/session.log`.

This archive is a tar.gz, so the files in `lib/` stay symlinks. A zip of the same tree stores those symlinks as plain text files, and the dock then fails to start.

## Put the COSMIC dock back

Open **Restore COSMIC Dock** from the launcher, or run:

```bash
~/.local/bin/restore-cosmic-dock
```

That restores the panel list from the backup, removes the autostart file, and stops MAGIC Dock. The top panel is left as it is. Log out and back in if the original dock does not return. Super still opens the launcher.

## Build it yourself

Source, the dock's behavior, and the Qt 6.12 build steps are in the main tree:

<https://github.com/TheWhiteWitchTM/MAGIC-Dock>

The install guide is [INSTALL.md](https://github.com/TheWhiteWitchTM/MAGIC-Dock/blob/main/INSTALL.md). Changes are in [CHANGELOG.md](https://github.com/TheWhiteWitchTM/MAGIC-Dock/blob/main/CHANGELOG.md).

## License

The dock is MIT. See `LICENSE`. Qt, ICU, and the image libraries in the plugins have their own licenses. See `NOTICE`.
