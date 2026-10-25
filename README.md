# MAGIC Dock

A dock for the COSMIC desktop on Pop!_OS. It uses the edge, size, autohide, and opacity already stored by COSMIC Settings, pins apps into stacks, and keeps a System Monitor button at the far end.

The program is `magic-dock`. This tree started as a copy of COSMIC Bar.

## Download a build that runs

The [`dist` branch](https://github.com/TheWhiteWitchTM/MAGIC-Dock/tree/dist) is a prebuilt bundle for a 64-bit Pop!_OS COSMIC session. Qt 6.12 is included. A normal COSMIC install already provides Wayland, glibc, and the GPU libraries the dock still loads from the system. You need glibc 2.38 or newer, which Pop!_OS 24.04 has.

```bash
curl -L -o magic-dock.tar.gz https://github.com/TheWhiteWitchTM/MAGIC-Dock/archive/refs/heads/dist.tar.gz
tar -xzf magic-dock.tar.gz
cd MAGIC-Dock-dist
./scripts/replace-cosmic-dock.sh
```

That hides the COSMIC bottom dock, leaves the top panel in place, starts MAGIC Dock in this session, and starts it again at the next login. The original panel list is backed up once to `~/.config/cosmic-bar/backup/`.

To try it without installing:

```bash
./magic-dock
```

Put the Pop dock back from the launcher entry **Restore COSMIC Dock**, or run `~/.local/bin/restore-cosmic-dock`. That restores the panel entries, removes the autostart file, and stops MAGIC Dock.

Download the `.tar.gz`. It keeps the library symlinks in `lib/`. A zip of the same branch can turn those symlinks into plain text files, and the dock will then fail to start.

## What the dock shows

On the near end: Launcher, Workspaces, Applications, and Search. The first three start `cosmic-launcher`, `cosmic-workspaces`, and `cosmic-app-library`. Search opens a field on the dock and launches the app you pick.

In the middle: pinned apps, stacks, and any other app that is running.

On the far end: minimized windows, then System Monitor, then a menu. The menu has Settings, About, and Contribute. Settings opens COSMIC Settings. Contribute opens the PayPal coffee link. About names the dock and includes the same coffee link.

A click on a pin launches it. A click on the running app focuses its window, and a second click minimizes it. Several windows open a preview strip. A minimized window on the dock restores with a click.

Drag one pin onto another to make a stack. Right-click a stack to rename it, open every app in it, or ungroup it. Right-click an app for a new window, pin, unpin, or add to a stack.

While an app is opening and no window has appeared yet, its icon hops inward for up to about eight seconds.

The pointer keeps the cursor from `XCURSOR_THEME` and `XCURSOR_SIZE` while it is over the bar.

A second copy exits if `magic-dock` is already running.

## Settings it follows

COSMIC writes the dock as plain files in `~/.config/cosmic/com.system76.CosmicPanel.Dock/v1/`. MAGIC Dock reads them about once a second. Changing them in Settings, or by editing the files, moves the dock without a restart.

| Pop file | What the dock does |
| --- | --- |
| `anchor` | `Top`, `Bottom`, `Left`, or `Right` |
| `margin`, `anchor_gap` | Gap between the dock and the screen edge |
| `size` | `XS` 24, `S` 32, `M` 40, `L` 48, `XL` 56. `Custom(n)` snaps to a multiple of 4, then uses 70% |
| `padding`, `border_radius`, `opacity` | Inner padding, corner radius, and bar opacity |
| `autohide` | `Never`, `OnOverlap`, or `Always` |
| `autohide_behavior` | Wait, unhide delay, and the size of the handle that stays on screen |
| `exclusive_zone` | Reserves space on the edge when true. Windows can cover the dock when false |
| `expand_to_edges` | The whole edge when true. A pill centered on that edge when false |
| `output` | `All`, `Active`, or `Name(the-connector)` |

Fullscreen windows hide the dock. Set `MAGIC_DOCK_SHOW_IN_FULLSCREEN=1` to keep it on screen.

## Pins

`~/.config/magic-dock/dock.json` stores pinned apps, stacks, and a `launch` map of programs that have been seen running. The first launch copies `~/.config/cosmic-bar/dock.json` when that file is the only one present. With no pin file at all, the dock starts from the COSMIC favorites list.

Icon size, autohide, and which edge come from the COSMIC dock settings above.

## Launching an app

A click tries, in order, `gio launch` on the desktop file, the `Exec` line, `gtk-launch`, then a program remembered from the last time that pin was running.

If the desktop file has no `Path`, the app starts in your home directory. The dock does not pass along its own directory. COSMIC Terminal is started with `--working-directory` set to that folder, so a terminal opened from the dock opens at home.

The environment passed to the app is the session environment with the dock's own Qt paths taken out:

- `QT_QPA_PLATFORM=offscreen` is removed. The dock uses the offscreen plugin so Qt does not open a second window of its own.
- `QT_PLUGIN_PATH`, `QT_QPA_PLATFORM_PLUGIN_PATH`, and `LD_LIBRARY_PATH` entries that point inside this bundle are removed. The bundle only ships the offscreen plugin. Leaving that path set makes a pinned Qt app start with no window. A Qt path elsewhere in the session is kept.

Qt on Wayland reports an app id of reversed organization domain plus the executable name. The dock maps that id to the desktop file. If the desktop file is not installed, it also looks in `~/Projects/<name>/`.

## Web app icons

Chrome, Chromium, Brave, and Edge install a web app icon as a file under `~/.local/share/icons/hicolor/<size>/apps/`, named like `chrome-<id>-Default.png`. The same files are read from Flatpak browser data under `~/.var/app/`. The dock reads those files directly and notices a new applications directory or hicolor directory, so a new web app does not wait on `gtk-update-icon-cache`. A window whose app id is `crx_<id>` is matched to that desktop file.

## Build

Qt 6 Widgets and Wayland client libraries are required, along with `g++`, `make`, and `wayland-scanner`. The published bundle is built with Qt 6.12. `pkg-config` is the fallback when `QT_PREFIX` and `LD_LIBRARY_PATH` do not already point at a Qt tree. The Qt 6.12 `.pc` files still name the prefix of the machine that built them, so the Makefile prefers a tree that actually contains the headers and libraries.

```bash
make
make test
make dist
```

`make test` runs `--self-test` and writes preview PNGs to `build/preview/`. The self-test leaves your pins alone.

`make dist` fills `dist/` with the binary, the Qt libraries it links, and the plugins it needs. `dist/magic-dock` is the launcher. Apps it starts keep the session Qt plugin path.

To replace the COSMIC dock from a source checkout:

```bash
make
./scripts/replace-cosmic-dock.sh
```

The script links `~/.local/bin/magic-dock` at this tree, installs an autostart entry, removes the Dock entry from the COSMIC panel list, stops a running `cosmic-bar` or `magic-dock`, and starts MAGIC Dock in this session. The session wrapper's process name is truncated to `magic-dock-sess` by the kernel. The restore script matches that name and the script path.

## Layout of the source

`src/main.cpp` starts the offscreen Qt process and the Wayland backend. `src/settings.cpp` reads the COSMIC dock files. `src/apps.cpp` indexes desktop files and icons and launches apps. `src/dock.cpp` is the layout, input, and painting. `src/backend.cpp` is the layer shell, toplevels, and cursor. Protocol XML lives in `protocols/`.

## License

MAGIC Dock is MIT. See [LICENSE](LICENSE).

The prebuilt bundle also contains unmodified Qt 6.12 shared libraries (LGPL-3.0) and the ICU 73 libraries shipped with that Qt, plus Qt image-format plugins. Those files live in `lib/` and `plugins/` and can be replaced. License copies and a notice are in the bundle.
