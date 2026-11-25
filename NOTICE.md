# Notice: this is a modified version

This repository is an **unofficial, modified build** of
[BatteryHub](https://github.com/krisgfx9/Wireless-Device-Battery-Monitor) by
[krisgfx9](https://github.com/krisgfx9), published from
<https://github.com/lo0kie/Wireless-Device-Battery-Monitor>.

It is not affiliated with, endorsed by or supported by the upstream author. Please report problems with
this build here, not upstream.

## What was changed

Changes made on 2026-11-25, relative to upstream commit `9c632fd` ("Merge PR #2784"):

- The context menu is drawn by this build itself instead of by the Windows menu manager: a layered
  window per level, Direct2D rendering, Windows 11 row metrics, one column for checks and chevrons,
  scrolling, right-to-left layout and per-monitor DPI.
- The menu was reorganised into groups, and settings that belong to one choice were moved into
  submenus; the small headings above groups were removed.
- New *Text style* editor (font, size, weight, icon size and spacing), stored in the registry.
- The small dialogs now share one shell built from stock Win32 controls, the system caption and the
  system message font.
- The embedded Material Icons font is subset to the 36 glyphs the app uses
  (`scripts/subset-icon-font.py`), see *Third-party notices* below.
- `Makefile`/`build.bat` build in parallel with real header dependencies.
- The update check points at this repository instead of upstream.

## Source

The complete corresponding source for every release is the tagged commit of that release in this
repository, e.g. `https://github.com/lo0kie/Wireless-Device-Battery-Monitor/tree/v1.2.0`.

This program is free software under the GNU General Public License v3.0; see [LICENSE](LICENSE).

## Third-party notices

Full notices ship with the installer as `THIRD_PARTY_NOTICES.txt` and are reproduced here.

- **HeadsetControl 4.3.0** (`headsetcontrol.exe`), GPL-3.0, unmodified official release asset. Its
  corresponding source (`headsetcontrol-4.3.0.tar.gz`) is attached to every release.
- **Material Icons Outlined** (`res/MaterialIconsOutlined-Regular.otf`), Copyright Google LLC,
  Apache License 2.0 — see [res/fonts/LICENSE-APACHE.txt](res/fonts/LICENSE-APACHE.txt). **Modified:**
  the font is subset to the glyphs listed in `src/icon_font.cpp`; outlines and metrics are unchanged.
