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

- **GPS:** USB u-blox 7 via **gpsd**, pinned to the stable
  `/dev/serial/by-id/usb-u-blox…` path (NOT `/dev/ttyACMx`, which renumbers when
  the dongle is moved to another USB port / replugged). `gps_gpsd.c` (libgps)
  fills `g_gps_data`; screens.c reads it.
- **Cold-start aiding (2026-06-08):** this u-blox 7 retains no almanac/time
  across boots (SKY shows `az=0`/no elevation; the receiver clock free-runs at
  the 2019 GPS week-rollover epoch), so first fix is slow even outdoors — and
  "22 sats" is satellites *in view*, not used. `gps-aid.py` (run once per boot
  by `gps-aid.service`, after gpsd) pushes **UBX-AID-INI** — the legacy aiding
  message, correct for this protocol-14 receiver (MGA is proto 15+) — with the
  Pi's NTP time (honest `tAcc`: 2 s when NTP-synced, 30 min when free-running).
  Verified: the receiver clock snaps 2019→present the instant it lands. Sent via
  `ubxtool -c` (reaches the receiver through gpsd; no need to free the device).
  Near-instant fixes would need AssistNow ephemeris (free u-blox token) — TODO.
- **Orbit cache + warm start (2026-06-09):** `gps-cache.py` extends the aiding to
  a software backup-cell. It saves the receiver's almanac / ephemeris / AOP to the
  SD card while it has a fix (30-min timer + on shutdown, real frames only) and
  replays them on boot via `gps-cache.service` (which supersedes `gps-aid.service`
  — it sends AID-INI itself). Restore is one direct-serial write (ubxtool is
  ~2.3 s/call so per-frame would take ~90 s); save stays through gpsd (no GPS
  interruption). AssistNow Autonomous (AOP) is enabled (`CFG-NAVX5`) so the
  receiver predicts ~3-day orbits offline. **Verified: a warm boot fixed in ~25 s**
  (vs minutes / never when cold) once in adequate sky — the cache removes the
  ephemeris-decode delay; the weak dongle antenna still needs ~30 dB-Hz to lock.
  `gps-diag.py` logs boot timing + per-satellite C/N0 for diagnosis. Next:
  **AssistNow Online** — per-user u-blox token set in the Setup web UI (never
  shipped in the repo) for fast first-boot fixes in a brand-new location.
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
  `dtparam=spi=on`): `dtoverlay=piscreen,rotate=270,speed=24000000` — the
  **non-drm fbtft** variant. `vc4-kms-v3d` stays. (`rotate=270`, not `90`: the
  panel mounts 180°-flipped in the case (2026-06-08); both are landscape
  480×320 so only the image flips. The app then runs `--live --invert-x` so
  left/right swipe direction matches — tap/hold are position-independent.)
- **Use the non-drm variant, NOT `,drm`:** the `,drm` variant comes up portrait
  **320×480**; the fbtft variant gives the landscape **480×320** the UI renders.
  With no HDMI attached, vc4-kms creates no framebuffer, so fbtft grabs
  **`/dev/fb0`** (16bpp) — the node the app opens (NOT `/dev/fb1`). The app
  auto-detects the ADS7846 touch node by capability, so event-number shuffles
  across reboots don't matter.
- **Kiosk console hardening (2026-06-08):** the fbtft panel IS a framebuffer
  console, so the kernel text cursor + login getty bleed a blinking cursor /
  log text through the LVGL UI (top-left, or mid-screen where log text lands).
  Fixed by `vt.global_cursor_default=0` in `cmdline.txt`, `disable getty@tty1`,
  and `wdm-console-off.sh` — run as the UI service's `ExecStartPre` (root), it
  unbinds fbcon from the panel fb on every start, late enough to beat the
  early-boot rebind. All baked into `install-service.sh`.
- **Needs apt:** `evtest`, `tslib`/`libts-bin` (calibration), plus `cmake`/`git`
  to build on-device. Touch calibration via tslib `ts_calibrate` → `/etc/pointercal`.
- Reboot + verify `/dev/fb1` + `evtest` is a deliberate step (panel could be
  ILI9488 on some board revs — `dmesg | grep -i ili` disambiguates).

## Milestones

1. ✅ Port the ESP32 screens; render SCAN headless to PNG.
2. ⬜ Pick geometry; adapt the layout to the Elecrow panel; render all screens.
3. ✅ Light the SPI panel on the Pi (`/dev/fb0` + touch); run the same binary via fbdev.
4. ✅ Wire real data: gpsd GPS + `iw scan`; map device screens to live state.
5. ⬜ Upload to the backend API; trusted-network gating.
6. ⬜ Installer script + systemd service + kiosk-on-boot.
