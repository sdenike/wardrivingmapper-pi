# WarDrivingMapper Pi — Design

> Lightweight spec for the Raspberry Pi wardriving build. Companion to the
> root umbrella and the firmware/backend/iOS repos.

## Goal

A Pi-based wardriving device whose on-screen experience is **as close to the
ESP32 device as possible** — same screens, same look, same interaction model —
while taking advantage of the Pi's richer hardware (USB GPS, USB Wi-Fi, real
storage). The *presentation* mirrors the ESP32; the *plumbing* is Linux-native.

## Strategy: reuse the firmware's LVGL UI

The ESP32 firmware renders its UI with LVGL. LVGL has a Linux port, so the
firmware's screen-building code transfers almost verbatim. We copy
`firmware/main/display.c` → `src/ui/screens.c`, delete the ~310 lines of ESP
hardware glue (panel init, flush callback, render task), and provide a small
compatibility shim (`src/platform/wdm_platform.h`) that redefines the handful
of ESP-IDF / FreeRTOS symbols the widget code references as Linux no-ops /
equivalents. The result: the SCAN/NAV/Setup/Upload/Pair screens compile and
render unmodified.

**Invariant:** `screens.c` should stay a faithful port. Visual changes belong
upstream in the firmware's `display.c` and flow down; the struct definitions in
`wdm_platform.h` must stay in sync with `firmware/main/config.h`.

## Architecture

```
                    ┌─────────────────────────────┐
   stub / gpsd /    │  g_gps_data, g_system_state │   (platform_stub.c now;
   iw-scan  ───────▶│  nvs_config_get(), buttons  │    real backends later)
                    └──────────────┬──────────────┘
                                   │ reads
                    ┌──────────────▼──────────────┐
                    │  src/ui/screens.c (LVGL)     │   ← ported from firmware
                    │  create_ui() / ui_refresh()  │     display.c, unmodified
                    └──────────────┬──────────────┘
                                   │ LVGL flush
        ┌──────────────────────────┼──────────────────────────┐
        ▼                          ▼                           ▼
  mem-fb → PNG              SDL window (later)          Elecrow fbdev /dev/fb1
  (main.c, today)          (interactive dev)           + XPT2046 touch (Pi)
```

- **LVGL** 8.4.0 vendored under `lib/lvgl` (pinned to the firmware's version so
  the screen code ports without API churn). `include/lv_conf.h` is the
  firmware config, host-tuned (`LV_COLOR_16_SWAP 0`, host clock tick).
- **Render harness** (`main.c`): registers an in-memory display, builds the UI,
  dumps an RGB565→RGB888 PNG. No display server required; runs on Mac and Pi.

## Open decision: panel geometry

The ESP32 layout is **320 px wide × LCD_H_RES tall** (172 on the 1.47B, 240 on
the 2.8"). The Elecrow 3.5" is **480×320**. Two ways to map it:

- **Portrait 320×480** — the 320-wide layout reuses pixel-perfect immediately;
  panel mounted vertically; extra height to fill.
- **Landscape 480×320** — natural HAT orientation, closest to the ESP32's
  wide-screen feel; needs a layout pass to use the full 480 width (~1.5× the
  current metrics).

First screenshot was rendered at the 2.8" profile (320×240) to validate the
port. **Geometry TBD with the user.**

## Scanning / GPS (v1)

- **GPS:** USB u-blox 7 at `/dev/ttyACM0` via **gpsd** (not yet installed on the
  Pi). screens.c reads `g_gps_data`; a gpsd client fills it.
- **Wi-Fi:** active `iw scan` on the onboard `wlan0` for v1 (the onboard radio
  has no monitor mode). Kismet-style passive capture would need a USB adapter —
  deferred. Single radio means scan + upload can't run simultaneously (same
  constraint the ESP32 has).

## Pi display bring-up (parallel hardware track)

Researched and confirmed against the actual Pi over SSH:

- **Panel:** Elecrow RR035 HAT = **ILI9486** + XPT2046 touch (ADS7846-compatible).
- **Overlay (CONFIRMED working 2026-06-02):** the stock `piscreen` overlay
  matches the HAT pinout (DC=GPIO24, RST=GPIO25, LED=GPIO22, touch
  PENIRQ=GPIO17). Persisted in `/boot/firmware/config.txt` (with
  `dtparam=spi=on`): `dtoverlay=piscreen,rotate=90,speed=24000000` — the
  **non-drm fbtft** variant. `vc4-kms-v3d` stays.
- **Use the non-drm variant, NOT `,drm`:** the `,drm` variant comes up portrait
  **320×480**; the fbtft variant gives the landscape **480×320** the UI renders.
  With no HDMI attached, vc4-kms creates no framebuffer, so fbtft grabs
  **`/dev/fb0`** (16bpp) — the node the app opens (NOT `/dev/fb1`). The app
  auto-detects the ADS7846 touch node by capability, so event-number shuffles
  across reboots don't matter.
- **Needs apt:** `evtest`, `tslib`/`libts-bin` (calibration), plus `cmake`/`git`
  to build on-device. Touch calibration via tslib `ts_calibrate` → `/etc/pointercal`.
- Reboot + verify `/dev/fb1` + `evtest` is a deliberate step (panel could be
  ILI9488 on some board revs — `dmesg | grep -i ili` disambiguates).

## Milestones

1. ✅ Port the ESP32 screens; render SCAN headless to PNG.
2. ⬜ Pick geometry; adapt the layout to the Elecrow panel; render all screens.
3. ⬜ Light the SPI panel on the Pi (`/dev/fb1` + touch); run the same binary via fbdev.
4. ⬜ Wire real data: gpsd GPS + `iw scan`; map device screens to live state.
5. ⬜ Upload to the backend API; trusted-network gating.
6. ⬜ Installer script + systemd service + kiosk-on-boot.
