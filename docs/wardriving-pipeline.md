# Pi wardriving pipeline — design (approved 2026-06-02)

Turn the Pi app from "UI on stub data" into a real wardriver: GPS + WiFi-scan
logging + upload to the backend, mirroring the ESP32 firmware. Built in slices;
this covers **slices 1–3** (config, sensing, Force Upload). Slice 4 (on-device
AP + web UI config) comes later.

## Hardware (confirmed on the Pi 2026-06-02)
- Pi 3B, online via **eth0** (10.0.1.242, default route) **and** wlan0 (onboard
  brcmfmac, phy0, 10.0.1.64). Uploads have connectivity independent of the scan
  radio → **no single-radio constraint** (the ESP32's big limitation) while on
  eth0/wlan0.
- **USB dongle: Ralink RT5370** (`148f:5370`, driver `rt2800usb`, phy1 →
  **wlan1**, currently DOWN). Monitor-mode/injection capable — ideal dedicated
  scan radio.
- **`iw` is NOT installed** → apt dep. Also need `gpsd`, `libgps-dev`,
  `libcurl4-openssl-dev`.
- MACs: eth0 `b8:27:eb:8b:9a:58`, wlan0 `b8:27:eb:de:cf:0d` (Pi-stable),
  wlan1 `00:0f:60:01:c8:72` (dongle — removable, do NOT use as hw_id).

## Locked decisions
- **Sequence:** 1→2→3 first (config → sensing → Force Upload), configured via a
  file over SSH. On-device AP + web-UI config is **slice 4, LAST**.
- **Scanning:** active `iw scan` now, behind a swappable `scan_backend`
  interface; **configurable interface** `scan_iface` (default USB dongle
  `wlan1`, fallback `wlan0`) so onboard ↔ USB swap is trivial (user wants both).
  USB **monitor-mode** passive capture = a later backend behind the same iface.
- **hw_id:** an onboard MAC (wlan0 or eth0) uppercased to 12 hex — NOT the
  removable dongle's — so the backend sees one stable device.
- **Upload connectivity:** use whatever the OS is already on (eth0/wlan0); the
  Pi is always online → **no network-switching** (unlike the ESP32).

## Architecture (slices 1–3)
Three pthreads in the `wdm_ui` process feeding the existing globals
(`g_system_state`, `g_gps_data`) — replaces the demo data in `platform_stub.c`,
mirrors the ESP32 task model, each behind a small interface:

### 1. Config (`config.c`, replaces the `nvs_config_get()` stub)
`key=value` file read at boot (`config.txt` beside the app):
```
api_key=wdm_…
server_url=https://wardrivingmapper.com/api/v1/scans
device_name=Pi WarDriver
units=imperial
scan_iface=wlan1
scan_interval_s=10
keep_uploaded=false
```
Edited over SSH for now; slice-4 web UI writes the same file later. Ship a
`config.example.txt`; **never commit real secrets** (standing requirement).

### 2. Sensing
- **GPS** (`gps_gpsd.c`): gpsd owns `/dev/ttyACM0`; a thread uses libgps to fill
  `g_gps_data` (lat/lon/alt/speed/course/hdop/sats/fix/time). Replaces the
  Eiffel-Tower stub → SCAN/NAV screens go live.
- **Scan** (`scan_backend` interface, `scan_iw.c` impl): bring `scan_iface` up,
  run `sudo iw dev <iface> scan` every `scan_interval_s`, parse each BSS
  (bssid, ssid, signal→rssi, freq→channel, RSN/WPA→security), tag with the
  current GPS fix, **75 m position-dedup** (like the ESP32), append to
  `data/wardrive.jsonl`, update `ssids_logged`/`scan_count`/`last_ap_count`.

### 3. Force Upload (`upload.c`)
On hold: set `current_screen = SCREEN_UPLOAD`, then read the log, batch
**100 records**, `POST {server_url}` with `Authorization: Bearer {api_key}`
(libcurl), body `{device_id: device_name, hw_id, name_ts, records:[{bssid, ssid,
rssi, ch, enc, lat, lon, alt, spd, hdop, sats, ts}]}` — the exact shape the
backend + ESP32 use. Parse `accepted/duplicates/rejected`, drive the existing
`SCREEN_UPLOAD` arc (`upload_done/total/batch` fields), archive sent records (or
keep, per `keep_uploaded`). No network-switching (already online).

## Hold actions (`trigger_menu_action`)
- **Force Upload** → `SCREEN_UPLOAD` + kick the upload thread.
- **Wi-Fi Setup** → `SCREEN_SETUP_ACTIVE` showing live connection/config status
  for now (real AP + QR is slice 4).
- **Power Off** → already real (blanks panel + cuts backlight + halts).

## Deps + ops
`scripts/install-service.sh` (+ an apt step): `iw`, `gpsd`, `libgps-dev`,
`libcurl4-openssl-dev`. Enable gpsd on `/dev/ttyACM0`. SD-wear (standing pref):
buffered appends, rotate-on-upload, low write rate.

## Slice 4 (later) — on-device AP + web UI config
hostapd + dnsmasq config AP + a small web server to set `config.txt` + apply
WiFi; the Setup screen shows the real AP SSID/pass/QR. Because eth0 (or one WiFi
radio) keeps the Pi online, the AP can run on a second radio while the Pi stays
connected — sidestepping the ESP32's AP-only-during-setup limitation.
