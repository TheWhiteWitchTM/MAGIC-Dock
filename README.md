# MAGIC Dock

A bottom, top, left, or right dock for the COSMIC desktop on Pop!_OS. It follows the dock settings Pop already stores, pins apps into stacks, and keeps a System Monitor button at the far end.

This tree started as a copy of COSMIC Bar. The program name is `magic-dock`. Pins are read from `~/.config/magic-dock/dock.json`, and the first launch copies `~/.config/cosmic-bar/dock.json` when that file is the only one present.

## What it follows

Pop writes the dock as plain files in `~/.config/cosmic/com.system76.CosmicPanel.Dock/v1/`. MAGIC Dock reads them about once a second.

| Pop file | What the dock does |
| --- | --- |
| `anchor` | `Top`, `Bottom`, `Left`, or `Right` |
| `margin`, `anchor_gap` | Gap between the dock and the screen edge |
| `size` | `XS` 24, `S` 32, `M` 40, `L` 48, `XL` 56. `Custom(n)` snaps to a multiple of 4, then uses 70% |
| `padding`, `border_radius`, `opacity` | Inner padding, corner radius, and bar opacity |
| `autohide` | `Never`, `OnOverlap`, or `Always` |
| `autohide_behavior` | Wait, unhide delay, and the size of the handle that stays on screen |
| `exclusive_zone` | Reserves space on the edge when true. Leaves windows free to cover the dock when false |
| `expand_to_edges` | Full edge, or a pill centered on that edge |
| `output` | `All`, `Active`, or `Name(the-connector)` |

Changing those files in Settings, or by editing them, moves the dock without a restart. Fullscreen windows still hide it. Set `MAGIC_DOCK_SHOW_IN_FULLSCREEN=1` to keep it up.

The wings are the launcher, workspaces, and application library on the near end. Pinned apps and stacks sit in the middle. Minimized windows sit just before System Monitor, which is always the far-end button.

## Launching pinned apps

A click launches the desktop file with `gio`, then the `Exec` line, then a program remembered from the last time that pin was actually running. The environment passed to the app is the session environment with two edits:

- `QT_QPA_PLATFORM=offscreen` is removed. The dock itself uses the offscreen plugin so it does not open a second window.
- `QT_PLUGIN_PATH`, `QT_QPA_PLATFORM_PLUGIN_PATH`, and `LD_LIBRARY_PATH` entries that point inside this dock's own bundle are removed. A bundled dock only ships the offscreen plugin. Leaving that path in place is what made a pinned Qt app such as Vesper or Cinder start with no window. The session Qt path, for example `~/Qt/6.12.0/gcc_64`, is kept.

Qt on Wayland reports an app id of reversed organization domain plus the executable name. Cinder's window id is `local.cinder.cinder` while its desktop file is `cinder`. The dock maps that id, and if the desktop file is not installed it adopts `~/Projects/<name>/share/<name>.desktop`.

While an app is opening and no window has appeared yet, its icon hops inward for a short bounce, then a smaller hop, for up to about eight seconds.

## Web app icons

Chrome, Chromium, Brave, and Edge install a web app icon as a file under `~/.local/share/icons/hicolor/<size>/apps/` named like `chrome-<id>-Default.png`. COSMIC looks that name up through the GTK icon cache, so a new web app keeps the wrong icon until someone runs `gtk-update-icon-cache` by hand. This dock reads the icon files directly, including Flatpak browser data under `~/.var/app/`, and notices a new applications directory or hicolor directory without a cache update. A window whose app id is `crx_<id>` is matched to that desktop file.

## Pointer

The dock sets a cursor on its surface so the pointer does not disappear over the bar. The theme and size come from `XCURSOR_THEME` and `XCURSOR_SIZE` (this session uses 48). It does not force a 24px cursor.

## Build

Qt 6.12 (Widgets) and Wayland client libraries are required. `pkg-config` is the fallback when `QT_PREFIX` and `LD_LIBRARY_PATH` do not already point at a Qt tree.

```bash
make
make test
```

`make test` runs `--self-test` and writes preview PNGs to `build/preview/`. The self-test does not rewrite your pins. The preview loads them, draws the Social stack, and checks that ink lands on the left, center, and right of a bottom dock.

`make dist` fills `dist/` with the binary, the Qt libraries it links, and the plugins it needs. `dist/magic-dock` is the launcher. Children it starts do not inherit that plugin path.

## Use it as the dock

This replaces the COSMIC bottom dock and the old COSMIC Bar. The top panel stays. A backup of the panel entries is written once to `~/.config/cosmic-bar/backup/`.

```bash
make
./scripts/replace-cosmic-dock.sh
```

The script links `~/.local/bin/magic-dock`, installs an autostart entry, removes the Dock entry from the COSMIC panel list, stops a running `cosmic-bar`, and starts MAGIC Dock in this session. The next login starts it the same way. The session wrapper's process name is truncated to `magic-dock-sess` by the kernel. The restore script matches that name and the script path.

Put the Pop dock back with `~/.local/bin/restore-cosmic-dock`, or with Restore COSMIC Dock in the launcher. That restores the panel entries, removes the autostart file, and stops both `magic-dock` and `cosmic-bar`.

## Pins

`~/.config/magic-dock/dock.json` stores the pinned apps, stacks, and a `launch` map of absolute programs that have successfully been seen running. Icon size, autohide, and anchor are not stored there. Pop's dock settings win.

Drag one pin onto another to make a stack. Right-click a stack to rename it. A pin with several windows opens a preview strip; one window focuses or minimizes.

## Layout of the source

`src/main.cpp` starts the offscreen Qt process and the Wayland backend. `src/settings.cpp` reads the Pop dock files. `src/apps.cpp` indexes desktop files and icons and launches apps. `src/dock.cpp` is the layout, input, and painting. `src/backend.cpp` is the layer shell, toplevels, and cursor. Protocol XML lives in `protocols/`.
