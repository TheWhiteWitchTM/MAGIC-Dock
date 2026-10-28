# Changelog

Dates are the days the changes landed on `main`. The prebuilt files live on the [`dist` branch](https://github.com/TheWhiteWitchTM/MAGIC-Dock/tree/dist) and follow the same days.

## 2026-10-28

- Install and build steps moved to [INSTALL.md](INSTALL.md), including the compiler packages, Wayland, and Qt 6.12 from the online installer or from aqtinstall.
- README covers the dock itself and points at that install guide.

## 2026-10-27

- An open stack draws the accent border on its dock icon and a green count of every window inside it, including a nested stack.
- Opening a stack lists those windows as cards. A click focuses, minimizes, or restores that window. The close mark on a card closes the window and leaves the stack up.
- Popups stay inset from the screen edge. A COSMIC margin of zero still leaves two pixels so the bar border is not clipped.
- The `dist` branch was rebuilt with this dock.

## 2026-10-25

- Search on the near end of the dock. The field matches installed apps and launches the one you pick.
- A menu at the far end, with Settings, About, and Contribute. Settings opens COSMIC Settings. About and Contribute use the PayPal coffee link.
- An app whose desktop file has no `Path` starts in the home directory. COSMIC Terminal is started with `--working-directory` set to that folder.
- The dock process itself moves to the home directory at startup, so a checkout path is not handed to apps it launches.

## 2026-10-24

- First source import. The dock reads the Pop!_OS COSMIC dock settings, pins apps into stacks, and can replace the COSMIC bottom dock.
- `scripts/replace-cosmic-dock.sh` hides that dock, installs an autostart entry, and starts MAGIC Dock. `scripts/restore-cosmic-dock.sh` puts the COSMIC dock back.
- `make dist` packs the binary with the Qt libraries and plugins it needs.
- README documents the prebuilt download. The `dist` branch carries that bundle, so a COSMIC session can run the dock without building.
