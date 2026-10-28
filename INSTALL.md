# Install MAGIC Dock

MAGIC Dock runs in a COSMIC Wayland session on 64-bit Pop!_OS. Pick one path.

- [Use the prebuilt bundle](#use-the-prebuilt-bundle). Qt 6.12 is already in the download.
- [Build from source](#build-from-source). You install a compiler, the Wayland client libraries, and Qt 6.12, then run `make`.

Pop!_OS 24.04 is the session this tree is built and run on. The dock needs glibc 2.38 or newer. Pop!_OS 24.04 ships 2.39.

## Use the prebuilt bundle

The bundle is the [`dist` branch](https://github.com/TheWhiteWitchTM/MAGIC-Dock/tree/dist). It is a 64-bit build. Qt 6.12, ICU 73, and the Qt plugins the dock loads are inside the archive. Wayland, glibc, and the GPU libraries still come from the COSMIC install.

Run the commands below from a terminal on the COSMIC desktop. The replace script inherits that session, including `WAYLAND_DISPLAY`. A shell that is not inside the desktop session cannot start the dock.

### 1. Download the tar.gz

```bash
curl -L -o magic-dock.tar.gz https://github.com/TheWhiteWitchTM/MAGIC-Dock/archive/refs/heads/dist.tar.gz
tar -xzf magic-dock.tar.gz
cd MAGIC-Dock-dist
```

`curl -L` follows the redirect GitHub sends for that URL and writes `magic-dock.tar.gz`. `tar` unpacks it to a folder named `MAGIC-Dock-dist`. `cd` moves into that folder. The launcher is `./magic-dock`. The Qt libraries are in `lib/`, and the plugins are in `plugins/`.

Download the `.tar.gz`. It keeps the library symlinks in `lib/`. A zip of the same branch stores those symlinks as plain text files, and the dock then fails to start.

Leave this folder where it is after you install. The install step links `~/.local/bin/magic-dock` at `./magic-dock` inside this folder. Moving or deleting the folder breaks that link. Unpack it again and run the replace script if you move it.

### 2. Try it without replacing the COSMIC dock

```bash
./magic-dock
```

That script adds this folder's `lib/` and `plugins/` to the environment and runs `bin/magic-dock`. The process stays in that terminal. The COSMIC dock stays where it is, so you will see two bars until you stop this one. Ctrl+C stops it. From another terminal, `pkill -x magic-dock` stops it. A second copy prints `magic-dock: already running` and exits.

### 3. Use it in place of the COSMIC dock

```bash
./scripts/replace-cosmic-dock.sh
```

The script does all of the following:

1. Checks that `./magic-dock` is executable.
2. Links `~/.local/bin/magic-dock` at this folder's `./magic-dock`.
3. Copies `magic-dock-session` and `restore-cosmic-dock` into `~/.local/bin/`. The session program starts the dock again if it crashes, and stops after a clean exit or after five crashes.
4. Writes `~/.config/autostart/magic-dock.desktop`, so COSMIC starts the session program at the next login. The entry is limited to COSMIC.
5. Writes a launcher entry named **Restore COSMIC Dock**.
6. Backs up the COSMIC panel list once, to `~/.config/cosmic-bar/backup/`. If COSMIC has a Dock panel directory, that is copied too. A backup that already exists is left alone.
7. Rewrites `~/.config/cosmic/com.system76.CosmicPanel/v1/entries` so the list is only `Panel`. The top panel stays. The Dock entry is what gets removed, which hides the COSMIC bottom dock. Pop dock settings under `CosmicPanel.Dock` stay on disk. MAGIC Dock reads them.
8. Stops a running `cosmic-bar` or `magic-dock`, including the session wrapper. The kernel keeps 15 characters of a process name, so those wrappers show up as `magic-dock-sess` and `cosmic-bar-sess`.
9. Starts MAGIC Dock in this session. Output goes to `~/.config/magic-dock/session.log`.

### 4. Put the COSMIC dock back

Open **Restore COSMIC Dock** from the launcher, or run:

```bash
~/.local/bin/restore-cosmic-dock
```

That copies the backup back over the panel list, restores the Dock panel settings when they were saved, removes the autostart file, and stops MAGIC Dock. The top panel is left as it is. Log out and back in if the original dock does not return on its own. Super still opens the launcher.

## Build from source

A source build needs three things: a C++ compiler and `make`, Wayland client headers, and Qt 6.12 (6.9 or newer also compiles). The published bundle is built with Qt 6.12.0 from the Qt online installer, installed at `$HOME/Qt/6.12.0/gcc_64`.

Pop!_OS 24.04's `qt6-base-dev` package is Qt 6.4.2. That package is too old. The dock calls `QImage::flipped`, which arrived in Qt 6.9, and a build against the distro package stops there.

### 1. Install the compiler and Wayland

```bash
sudo apt update
sudo apt install build-essential pkg-config libwayland-dev
```

| Package | Why it is there |
| --- | --- |
| `build-essential` | `g++` and `make`. The dock is C++17. |
| `pkg-config` | Used only when `QT_PREFIX` is unset and `LD_LIBRARY_PATH` does not already point at a Qt tree. |
| `libwayland-dev` | Headers and libraries for `wayland-client` and `wayland-cursor`. It also pulls in `libwayland-bin`, which provides `wayland-scanner`. The `Makefile` runs `wayland-scanner` on the XML files in `protocols/`. |

Confirm the tools:

```bash
g++ --version
make --version
wayland-scanner --version
pkg-config --exists wayland-client && echo "wayland-client ok"
```

Each command should print a version, and the last line should print `wayland-client ok`.

### 2. Install Qt 6.12

Qt is the toolkit the dock uses to load icons, measure text, and read JSON. The dock draws its own Wayland surface. It links `Qt6Widgets`, `Qt6Gui`, and `Qt6Core`. It does not use Qt's Wayland platform plugin. At run time it loads Qt's offscreen platform plugin, plus SVG and image-format plugins so Cosmic icons and web-app icons paint.

You want this directory when the installer finishes:

```text
$HOME/Qt/6.12.0/gcc_64
```

That folder contains `include/`, `lib/`, and `plugins/`. The `Makefile` calls that path `QT_PREFIX`.

#### Graphical installer

1. Create a free Qt account at <https://login.qt.io/register> if you do not already have one. The online installer asks for it.
2. Download the Linux installer from <https://www.qt.io/download-open-source>, or with:

   ```bash
   curl -OL https://download.qt.io/official_releases/online_installers/qt-online-installer-linux-x64-online.run
   chmod +x qt-online-installer-linux-x64-online.run
   ./qt-online-installer-linux-x64-online.run
   ```

3. Sign in. Choose the open-source license and accept the open-source obligations.
4. Set the installation folder to `$HOME/Qt`. That is the installer's usual default.
5. Choose a custom installation. Under **Qt 6.12.0** select **Desktop gcc 64-bit**. Under **Additional Libraries** select **Qt SVG** and **Qt Image Formats**.
   - Desktop gcc 64-bit is the kit the `Makefile` compiles against. It provides Widgets, Gui, and Core.
   - Qt SVG provides `plugins/iconengines/libqsvgicon.so` and the SVG image plugin. The Cosmic icon theme is mostly SVG. A dock built without that plugin shows missing icons.
   - Qt Image Formats provides the extra image plugins. `make dist` copies `libqgif`, `libqico`, `libqjpeg`, `libqsvg`, and `libqwebp` from `plugins/imageformats` when those files are present.
6. Qt Creator is optional. The dock builds with `make`.
7. Finish the installer. Later changes go through `$HOME/Qt/MaintenanceTool`.

#### Installer from the command line

The same installer can run without the GUI. Replace the email and password with your Qt account. `--root` is the folder that will contain `6.12.0/gcc_64`.

```bash
curl -OL https://download.qt.io/official_releases/online_installers/qt-online-installer-linux-x64-online.run
chmod +x qt-online-installer-linux-x64-online.run
./qt-online-installer-linux-x64-online.run \
  --root "$HOME/Qt" \
  --accept-licenses \
  --accept-obligations \
  --default-answer \
  --confirm-command \
  --email "you@example.com" \
  --pw "your-password" \
  install qt.qt6.6120.linux_gcc_64
```

`qt.qt6.6120.linux_gcc_64` is the Desktop gcc 64-bit component documented for Qt 6.12.0. SVG and Image Formats are separate components. List their package names, then install the ones whose names contain both `6120` and `qtsvg` or `qtimageformats`:

```bash
./qt-online-installer-linux-x64-online.run search qtsvg
./qt-online-installer-linux-x64-online.run search qtimageformats
```

Pass those names to `install` with the same `--root`, `--accept-licenses`, `--accept-obligations`, `--default-answer`, and `--confirm-command` flags. Qt's command-line installer is documented at <https://doc.qt.io/qt-6.12/get-and-install-qt-cli.html>.

#### aqtinstall

[aqtinstall](https://aqtinstall.readthedocs.io/) downloads the same official Qt archives without a Qt account. It needs Python's `pip` and `7z` (`p7zip-full` on Pop!_OS).

```bash
sudo apt install p7zip-full python3-pip python3-venv
python3 -m pip install --user aqtinstall
export PATH="$HOME/.local/bin:$PATH"
aqt install-qt linux desktop 6.12.0 linux_gcc_64 --outputdir "$HOME/Qt"
```

That installs the desktop Qt 6.12.0 archives into `$HOME/Qt/6.12.0/gcc_64`, including Qt SVG and Qt Image Formats. To download a smaller set, add `--archives qtbase qtsvg qtimageformats`. If that list is rejected, run the command again without `--archives`.

#### Check the tree, then point the build at it

```bash
export QT_PREFIX="$HOME/Qt/6.12.0/gcc_64"
test -f "$QT_PREFIX/include/QtCore/qglobal.h" && echo "headers ok"
test -e "$QT_PREFIX/lib/libQt6Core.so" && echo "libQt6Core ok"
test -f "$QT_PREFIX/plugins/platforms/libqoffscreen.so" && echo "offscreen plugin ok"
test -f "$QT_PREFIX/plugins/iconengines/libqsvgicon.so" && echo "svg icon engine ok"
"$QT_PREFIX/bin/qmake" -v
```

`qmake -v` should report Qt 6.12.0 and that same prefix. `headers ok` and `libQt6Core ok` are required before `make`. `offscreen plugin ok` is required before the dock can start. `svg icon engine ok` is required for Cosmic icons.

The `Makefile` looks for Qt in this order:

1. `$QT_PREFIX`, when that directory contains `include/QtCore/qglobal.h` and `lib/libQt6Core.so`.
2. The first `LD_LIBRARY_PATH` entry that contains `libQt6Core.so` and whose parent directory contains those headers. A shell that already exported `$HOME/Qt/6.12.0/gcc_64/lib` selects that tree even when `QT_PREFIX` is empty.
3. `pkg-config --cflags/--libs Qt6Widgets Qt6Gui`.

The Qt 6.12 `.pc` files still name the prefix of the machine that built them. The `Makefile` does not trust `pkg-config` for that tree. Set `QT_PREFIX`.

Put the export in the shell you build from. To keep it for later terminals, add this line to `~/.bashrc`:

```bash
export QT_PREFIX="$HOME/Qt/6.12.0/gcc_64"
```

Open a new terminal, or run `source ~/.bashrc`, before `make`.

### 3. Clone and compile

```bash
git clone https://github.com/TheWhiteWitchTM/MAGIC-Dock.git
cd MAGIC-Dock
export QT_PREFIX="$HOME/Qt/6.12.0/gcc_64"
make -j"$(nproc)"
```

`make` generates Wayland client code from `protocols/` into `build/gen/`, compiles `src/`, and links `./magic-dock`. The binary records an rpath of `$ORIGIN/../lib` and of `$QT_PREFIX/lib`, so a copy under `dist/bin/` finds the bundled Qt, and `./magic-dock` in the source tree finds the Qt you built against.

If `make` stops with `wayland-scanner: command not found`, install `libwayland-dev` again. If it stops with `QtWidgets/QApplication: No such file`, `QT_PREFIX` is empty or points at the wrong folder. If it stops with `QImage has no member named flipped`, the compiler found Qt older than 6.9. Pop's Qt 6.4.2 does that. `make clean`, export the 6.12 prefix, and run `make` again.

### 4. Test the build

```bash
make test
```

`make test` runs `./magic-dock --self-test` and then `./magic-dock --preview build/preview`. The self-test checks settings parsing, app launch, and dock layout. It leaves `~/.config/magic-dock/dock.json` alone. The preview command writes PNGs to `build/preview/`. A failing check prints a line and returns a non-zero status. `make` then stops.

### 5. Run it, or replace the COSMIC dock

From the source tree, in a COSMIC session:

```bash
./magic-dock
```

To hide the COSMIC dock and start this build now and at the next login:

```bash
./scripts/replace-cosmic-dock.sh
```

The script is the same one the prebuilt bundle uses. It links `~/.local/bin/magic-dock` at this tree's `./magic-dock`, so the program keeps running from the checkout. Rebuild with `make` and the next start picks up the new binary. Restore with `~/.local/bin/restore-cosmic-dock`.

The session log is `~/.config/magic-dock/session.log`.

### 6. Build a bundle you can copy to another machine

```bash
export QT_PREFIX="$HOME/Qt/6.12.0/gcc_64"
make dist
```

`make dist` runs `scripts/bundle-dist.sh`. The script creates `dist/` with:

| Path | What it is |
| --- | --- |
| `dist/magic-dock` | Launcher script. Sets `LD_LIBRARY_PATH`, `QT_PLUGIN_PATH`, and `QT_QPA_PLATFORM_PLUGIN_PATH`, then runs the binary. |
| `dist/bin/magic-dock` | The dock binary. |
| `dist/bin/qt.conf` | Tells Qt that libraries live in `../lib` and plugins live in `../plugins`. |
| `dist/lib/` | Qt and ICU shared libraries the binary and plugins link, including the SONAME symlinks. |
| `dist/plugins/` | Offscreen platform, SVG icon engine, and image-format plugins. |

Launch that bundle with `./dist/magic-dock`. Apps the dock starts keep the session Qt plugin path. The script strips this bundle's Qt paths out of their environment, because the bundle only ships the offscreen plugin and a pinned Qt app would otherwise open no window.

`make dist` copies plugins from `$QT_PREFIX/plugins`. The official Qt layout has that directory. Pop's packages put plugins under `/usr/lib/x86_64-linux-gnu/qt6/plugins`, and they are too old to compile this tree anyway.

Copy `dist/` as a directory, or pack it with `tar`. Keep the symlinks in `dist/lib/`. The script checks that `libQt6Core`, `libQt6Gui`, `libQt6Widgets`, and the offscreen plugin are present before it finishes.

### 7. Switch Qt versions

Object files keep the include paths from the Qt they were compiled against. After changing `QT_PREFIX`, clean and build again:

```bash
make clean
export QT_PREFIX="$HOME/Qt/6.12.0/gcc_64"
make -j"$(nproc)"
```

`make clean` deletes `build/`, `./magic-dock`, `./cosmic-bar`, and `dist/`.

## Files an install writes

| Path | Written by | Purpose |
| --- | --- | --- |
| `~/.local/bin/magic-dock` | replace script | Symlink to the bundle launcher or the built binary |
| `~/.local/bin/magic-dock-session` | replace script | Restarts the dock after a crash |
| `~/.local/bin/restore-cosmic-dock` | replace script | Puts the COSMIC dock back |
| `~/.config/autostart/magic-dock.desktop` | replace script | Starts the dock at login, only in COSMIC |
| `~/.local/share/applications/restore-cosmic-dock.desktop` | replace script | Launcher entry **Restore COSMIC Dock** |
| `~/.config/cosmic-bar/backup/` | replace script, once | Copy of the panel list and Dock settings |
| `~/.config/cosmic/com.system76.CosmicPanel/v1/entries` | replace script | Panel list with the Dock entry removed |
| `~/.config/magic-dock/session.log` | session program | Standard output and errors from the running dock |
| `~/.config/magic-dock/magic-dock.pid` | the dock | Pid of the running copy |
| `~/.config/magic-dock/dock.json` | the dock | Pins, stacks, applet switches, remembered programs |

The restore script removes the autostart file, restores the backup, and stops the dock. It does not delete `dock.json`.

## If it does not start

- Read `~/.config/magic-dock/session.log`.
- Run `./magic-dock` from the unpacked folder or the source tree and read the terminal. A line that says the program is already running means an older copy still holds the pid file. `pkill -x magic-dock` stops it. The pid file is `~/.config/magic-dock/magic-dock.pid`.
- `ldd ./magic-dock` on a source build should show `libQt6Core.so.6`, `libQt6Gui.so.6`, `libQt6Widgets.so.6`, `libwayland-client.so.0`, and `libwayland-cursor.so.0` with paths, and no `not found`.
- `ldd bin/magic-dock` inside a bundle is expected to find Qt through the launcher's `LD_LIBRARY_PATH`. Run `./magic-dock` (the script), or `LD_LIBRARY_PATH="$PWD/lib" ldd bin/magic-dock`.
- A bundle whose `libQt6Core.so.6` entry is a short text file was unpacked from zip. Download the tar.gz again.
- The dock draws on Wayland. `echo "$WAYLAND_DISPLAY"` in the same terminal should print a name such as `wayland-1`. An empty value means this shell is outside the COSMIC session.
- After five crashes the session program stops and tells you to run the restore script. The reason is in `session.log`.
- Fullscreen windows hide the dock. `MAGIC_DOCK_SHOW_IN_FULLSCREEN=1` keeps it visible. Set that in the environment of the session, or start `./magic-dock` with the variable set, when you are checking a fullscreen app.
