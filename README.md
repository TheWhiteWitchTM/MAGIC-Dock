# MAGIC Dock

A dock for the COSMIC desktop on Pop!_OS. It uses the edge, size, autohide, and opacity already stored by COSMIC Settings, pins apps into stacks, and keeps switchable applets at the far end.

The program is `magic-dock`. This tree started as a copy of COSMIC Bar.

Install a prebuilt bundle, or build it, with every step in [INSTALL.md](INSTALL.md). Changes are in [CHANGELOG.md](CHANGELOG.md).

## Run a prebuilt bundle

The [`dist` branch](https://github.com/TheWhiteWitchTM/MAGIC-Dock/tree/dist) is a 64-bit build for a Pop!_OS COSMIC session. Qt 6.12 is included. A normal COSMIC install already provides Wayland, glibc, and the GPU libraries. The dock needs glibc 2.38 or newer. Pop!_OS 24.04 has that.

```bash
curl -L -o magic-dock.tar.gz https://github.com/TheWhiteWitchTM/MAGIC-Dock/archive/refs/heads/dist.tar.gz
tar -xzf magic-dock.tar.gz
cd MAGIC-Dock-dist
./scripts/replace-cosmic-dock.sh
```

Run that from a terminal inside the COSMIC session. The script hides the COSMIC bottom dock, leaves the top panel in place, starts MAGIC Dock now, and starts it again at the next login. The original panel list is backed up once to `~/.config/cosmic-bar/backup/`.

Download the `.tar.gz`. It keeps the library symlinks in `lib/`. A zip of the same branch stores those symlinks as plain text files, and the dock then fails to start.

To try the bundle without replacing the COSMIC dock, run `./magic-dock` from the unpacked folder. Put the Pop dock back from the launcher entry **Restore COSMIC Dock**, or run `~/.local/bin/restore-cosmic-dock`. The full account of what those commands change is in [INSTALL.md](INSTALL.md).

## What the dock shows

On the near end: Launcher, Workspaces, Applications, and Search. The first three start `cosmic-launcher`, `cosmic-workspaces`, and `cosmic-app-library`. Search opens a field on the dock and launches the app you pick.

In the middle: pinned apps, stacks, and any other app that is running.

On the far end: minimized windows, then the applets, then a menu. The applets are Trash, Grok, and System Monitor. Each one can be shown or hidden from the menu under Applets. All three can be off. The menu stays.

### Trash

Trash opens the wastebasket. Right-click it to empty it. Drop files on the trash icon, or on its menu, to throw them away. Drop a pinned app on it to take that pin off the dock. Drop a stack on it and the dock asks before the whole stack leaves. The trash badge is red and shows how many items are in the wastebasket, including other disks.

### Grok

Grok sits just left of System Monitor. The ring is how much of the Grok allowance is still left. Above 70 it is green, down through 11 it is orange, and 10 or less is red. Until the number is known the ring is gray and shows a dash. A blue badge appears while Grok bots are running, and the badge shows how many.

Hovering the ring opens a card with two tabs. Summary shows what is left, the prepaid balance, and usage by product. Bots lists who is on the desk, with a role and the latest note, and whether the writer is up. The card stays up while the pointer moves onto it, so the tabs can be clicked. One click on the ring opens Grogy, the same way one click on the meters opens System Monitor. The dock looks for `grogy` on `PATH` and remembers that program. `MAGIC_DOCK_GROGY` names a program to use instead.

### System Monitor

The meters open System Monitor. The top bar is CPU and the bottom bar is memory, colored from green through orange to red. Hovering them shows a card with the percentages.

### Windows and badges

A running app gets a green badge. One window is a dot, and several windows show a count. A stack folder uses that same green badge for every open window inside it, including windows in a nested stack, and the folder always shows the number.

A click on a pin launches it. A click on the running app focuses its window, and a second click minimizes it. Several windows open a preview strip. A minimized window on the dock restores with a click.

While an app is opening and no window has appeared yet, its icon hops inward for up to about eight seconds.

### Stacks

Drag one pin onto the middle of another to make a stack. Drag a stack onto the middle of a stack to nest it, or drop it into an open stack. Drag past a pin to reorder. A stack is drawn as a yellow folder, so one app in a stack still looks like a stack. A nested stack adds a darker tab behind that folder.

While that stack is open, or one of its windows is picked from a preview strip, the dock icon keeps the accent border. Opening it lists every open window in the stack, including windows from a nested stack, as the same cards a multi-window app uses on the dock.

- A click focuses, minimizes, or restores that window and closes the stack.
- The close mark on a card closes the window and leaves the stack up.
- A nested folder shows the same count. Opening it beside the parent lists only the windows in that folder.
- Right-click a stack for Open All, Rename, or Stack. Stack is the page where you split it back onto the dock or unpin the whole thing. Both ask first.
- With the stack open, the ··· button opens that same page, and double-clicking the title still renames it.
- A click on a nested folder opens that folder beside the one you already have open, on the left or the right, so icons can be dragged from one into the other. Click the same folder again to close the side.
- Drag an icon onto another inside an open stack to reorder them, or onto a nested folder to drop it inside.
- Drag an app or a nested stack out of a popup and onto the dock to lift it out.
- Add to Stack moves an app into the stack you pick, including from a folder up into the stack that holds that folder.
- Right-click an app for a new window, pin, unpin, send to the desktop, or add to a stack. Unpin works on an app inside a stack and takes it off the dock. Remove from Stack leaves that app on the dock as its own pin. Send to Desktop writes a launcher into the desktop folder.

### Menu, cursor, and a second copy

The menu has Settings, About, and Contribute, then the applet switches. Settings opens COSMIC Settings. Contribute opens the [PayPal coffee link](https://www.paypal.com/ncp/payment/4P85EQRES3RGU). About names the dock and includes the same link.

The pointer keeps the cursor from `XCURSOR_THEME` and `XCURSOR_SIZE` while it is over the bar.

A second copy exits if `magic-dock` is already running.

## Settings it follows

COSMIC writes the dock as plain files in `~/.config/cosmic/com.system76.CosmicPanel.Dock/v1/`. MAGIC Dock reads them about once a second. Changing them in Settings, or by editing the files, moves the dock without a restart.

| Pop file | What the dock does |
| --- | --- |
| `anchor` | `Top`, `Bottom`, `Left`, or `Right` |
| `margin`, `anchor_gap` | Gap between the dock and the screen edge. A zero margin still keeps two pixels, so the border is not cut off |
| `size` | `XS` 24, `S` 32, `M` 40, `L` 48, `XL` 56. `Custom(n)` snaps to a multiple of 4, then uses 70% |
| `padding`, `border_radius`, `opacity` | Inner padding, corner radius, and bar opacity |
| Theme `window_hint`, else `accent` | Border color on the bar and every popup. Read from the active light or dark theme. The stroke stays tighter than Active window hint size |
| `autohide` | `Never`, `OnOverlap`, or `Always` |
| `autohide_behavior` | Wait, unhide delay, and the size of the handle that stays on screen |
| `exclusive_zone` | Reserves space on the edge when true. Windows can cover the dock when false |
| `expand_to_edges` | The whole edge when true. A pill centered on that edge when false |
| `output` | `All`, `Active`, or `Name(the-connector)` |

Fullscreen windows hide the dock. Set `MAGIC_DOCK_SHOW_IN_FULLSCREEN=1` to keep it on screen. `COSMIC_BAR_SHOW_IN_FULLSCREEN` does the same.

Popups sit clear of the dock and stay inset from the screen edge, so the last row of a stack, a menu, or a window card is not cut off by the bar.

## Pins

`~/.config/magic-dock/dock.json` stores pinned apps, stacks, which applets are shown, a `launch` map of programs that have been seen running, and the Grogy program under `tools` once it has been found. Version 4 writes each stack as `children`, which can be app ids or nested stacks. An older file that lists `apps` still loads, and the next save writes the new shape. The first launch copies `~/.config/cosmic-bar/dock.json` when that file is the only one present. With no pin file at all, the dock starts from the COSMIC favorites list.

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

## Layout of the source

`src/main.cpp` starts the offscreen Qt process and the Wayland backend. `src/settings.cpp` reads the COSMIC dock files. `src/apps.cpp` indexes desktop files and icons and launches apps. `src/dock.cpp` is the layout, input, and painting. `src/backend.cpp` is the layer shell, toplevels, and cursor. Protocol XML lives in `protocols/`. The build rules are in the `Makefile`. `scripts/bundle-dist.sh` fills `dist/`. `scripts/replace-cosmic-dock.sh` and `scripts/restore-cosmic-dock.sh` swap this dock with the COSMIC one.

## License

MAGIC Dock is MIT. See [LICENSE](LICENSE).

The prebuilt bundle also contains unmodified Qt 6.12 shared libraries (LGPL-3.0) and the ICU 73 libraries shipped with that Qt, plus Qt image-format plugins. Those files live in `lib/` and `plugins/` and can be replaced. License copies and a notice are in the bundle.
