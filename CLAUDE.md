# WarDrivingMapper Pi — Claude Code Context

## What this is

A Raspberry Pi wardriving device that mirrors the ESP32 firmware's LVGL UI on
Linux. Fourth sibling alongside `firmware/`, `backend/`, `ios/`. Talks to the
same backend HTTP API; no shared code. AGPL-3.0.

See `docs/design.md` for the full design. Read it before making changes.

## Core principle: screens.c is a faithful port

`src/ui/screens.c` is `firmware/main/display.c` copied with the ESP hardware
glue stripped. **Do not gratuitously edit the widget code** — visual changes
belong upstream in the firmware's `display.c` and flow down here, so the two
devices stay identical. The shim (`src/platform/wdm_platform.h`) re-implements
ESP-IDF/FreeRTOS symbols for Linux; its struct definitions (`gps_data_t`,
`system_state_t`, …) **must stay in sync with `firmware/main/config.h`**.

The port mechanic (if re-porting from a newer firmware display.c):
- copy `display.c` → `screens.c`
- delete: the include block's ESP headers, `lvgl_flush_cb`, `lcd_fill_screen`,
  `display_hw_init`, `lvgl_init`, `display_task`
- inject `#include "wdm_platform.h"` + `#include "screens.h"`; un-`static`
  `create_ui` and `ui_refresh`
- add shims for any new external symbols the widget code references

## Build + run (host)

```bash
cmake -S . -B build
cmake --build build -j
./build/wdm_screenshot docs/screenshots/out.png
```

Override geometry at configure time, e.g. landscape:
`cmake -S . -B build -DCMAKE_C_FLAGS="-DLCD_V_RES=480 -DLCD_H_RES=320"`
(`LCD_V_RES` = width, `LCD_H_RES` = height; see `wdm_platform.h`).

## The Pi

- SSH: reach the Pi over SSH with a key (use your own host/IP). Hostname
  WarDrivingMapper-Pi. Pi 3B, Debian 13 trixie, kernel 6.18, ~905 MB RAM.
- GPS: `/dev/ttyACM0` (USB u-blox 7). **gpsd not installed yet.**
- Display: Elecrow 3.5" 480×320 **ILI9486** SPI + XPT2046 touch. **Not lit yet** —
  SPI is off in `/boot/firmware/config.txt`. Bring-up plan in `docs/design.md`
  (stock `piscreen` overlay). Lighting it needs a reboot — do it deliberately,
  with the user.
- Missing on the Pi: `cmake`, `git`, `evtest`, `tslib` — `apt install` when
  building / calibrating on-device.

## Status

SCAN dashboard renders pixel-faithfully headless (320×240, the 2.8" profile).
Next: pick the Elecrow geometry, adapt the layout, render all screens. Then
light the panel, then wire gpsd + `iw scan`. See `docs/design.md` milestones.
