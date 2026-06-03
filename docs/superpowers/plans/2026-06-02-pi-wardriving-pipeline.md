# Pi Wardriving Pipeline Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task (inline execution — this is hardware-coupled work needing on-device SSH/build/observe). Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the Pi app from "UI on stub data" into a real wardriver — GPS via gpsd, active `iw` Wi-Fi scan logging, and Force-Upload to the backend — mirroring the ESP32 firmware.

**Architecture:** Three pthreads in the `wdm_ui` process write the existing globals (`g_gps_data`, `g_system_state`) and an on-disk JSONL log, replacing `platform_stub.c`'s demo values at runtime. Bug-prone logic (config parse, `iw scan` parse, record JSON, haversine dedup, hw_id format) lives in **pure, host-testable** modules; hardware glue (gpsd, popen iw, libcurl) is thin Linux-only wrappers. `screens.c` is NOT touched (faithful firmware port).

**Tech Stack:** C11, pthreads, libgps (gpsd client), libcurl, `iw` (popen), CMake. Host unit tests are plain-C asserts (no framework), matching the firmware's `tests/` style.

---

## Verified backend contract (read from `backend/src/Controllers/Api.php` + `firmware/`)

- **Endpoint:** `POST {server_url}` — `server_url=https://wardrivingmapper.com/api/v1/scans`
- **Headers:** `Authorization: Bearer <api_key>` · `Content-Type: application/json`
- **Body:** `{"device_id":"<name>","hw_id":"<12 HEX>","name_ts":<epoch|0>,"records":[ <record>, … ]}`
  - `hw_id` = onboard MAC uppercased, colons stripped → 12 hex (wlan0 `b8:27:eb:de:cf:0d` → `B827EBDECF0D`). NOT the dongle.
  - Max **500 records/batch**; we batch **100** (firmware parity).
- **Record (JSONL line) — exact keys** (firmware `wardrive.jsonl` format):
  ```json
  {"bssid":"AA:BB:CC:DD:EE:FF","ssid":"Name","rssi":-67,"ch":6,"enc":"WPA2","lat":42.7325,"lon":-84.5555,"alt":271.3,"spd":12.5,"hdop":0.9,"sats":9,"ts":"2026-05-14T14:23:01Z"}
  ```
  - `bssid` MUST match `^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$` (server uppercases). `ssid` trimmed server-side (empty OK). `rssi` −120..0. `ch` 1..196. `ts` `YYYY-MM-DDTHH:MM:SSZ`. `spd` km/h 0..400. All except bssid are optional/nullable server-side.
  - `enc` vocabulary (mirror firmware scanner.c): `Open`, `WEP`, `WPA2`, `WPA/2`, `WPA3`, `WPA2/3`.
- **Response:** `{ok, accepted, duplicates, rejected, device_name, name_updated_at}`. 401 = bad key.

---

## File Structure

```
pi/
├── src/pipeline/                    (NEW — all pipeline code)
│   ├── config.{c,h}      portable  wdm_config_t + config_load(path) + config_get()
│   ├── geo.{c,h}         portable  wdm_haversine_m() for 75 m dedup
│   ├── hwid.{c,h}        portable  wdm_hwid_from_mac() (pure); Linux: read /sys MAC
│   ├── recordfmt.{c,h}   portable  wdm_record_to_json(scan_result_t) → JSONL line
│   ├── scan_parse.{c,h}  portable  wdm_parse_iw_scan(text) → scan_result_t[]  (THE tricky part)
│   ├── scan_iw.{c,h}     linux     scan thread: ip up + popen(iw scan) + parse + GPS-tag + dedup + log
│   ├── gps_gpsd.{c,h}    linux     gpsd thread (libgps) → g_gps_data
│   ├── upload.{c,h}      linux     libcurl upload thread (Force Upload) → SCREEN_UPLOAD arc
│   └── pipeline.{c,h}    linux     wdm_pipeline_start() / wdm_pipeline_force_upload() umbrella
├── tests/test_pipeline.c (NEW)     host asserts for the 5 portable modules
├── src/main.c            (MODIFY)  run_live(): start pipeline; trigger_menu_action(): wire Upload/Setup
├── src/platform/platform_stub.c (KEEP) still defines globals + tick + button (runtime-overwritten)
├── CMakeLists.txt        (MODIFY)  add portable srcs always; linux srcs + -lgps -lcurl -pthread on Linux; wdm_tests target
├── config.example.txt    (NEW, committed)   sample config
├── config.txt            (NEW on Pi, gitignored)  real secrets
├── .gitignore            (MODIFY)  config.txt, data/, build/
└── scripts/install-service.sh (MODIFY)  apt deps + gpsd enable
```

**Concurrency:** an internal `pthread_mutex` guards the GPS snapshot (gps thread writes / scan thread reads) and the log-append + dedup table. UI reads of the globals stay lock-free — cosmetic-only races, exactly as the existing firmware port already accepts.

**Privilege:** scanning uses `sudo ip link set <iface> up` + `sudo iw dev <iface> scan` (passwordless sudo confirmed on the Pi, already used by Power Off).

---

## Slice 1 — Config + GPS (screens show real data)

### Task 1.1: `config` module (portable)
**Files:** Create `src/pipeline/config.{c,h}`; Test `tests/test_pipeline.c`.

- [ ] Define `wdm_config_t { char api_key[64]; char server_url[160]; char device_name[40]; bool use_metric; char scan_iface[16]; int scan_interval_s; bool keep_uploaded; }`.
- [ ] `bool config_load(const char *path)` — parse `key=value` lines (`#` comments, trim, skip blanks); unknown keys ignored; missing file → defaults + return false. Defaults: server_url=`https://wardrivingmapper.com/api/v1/scans`, device_name=`Pi WarDriver`, use_metric=false, scan_iface=`wlan1`, scan_interval_s=10, keep_uploaded=false.
- [ ] `const wdm_config_t *config_get(void)` → pointer to the loaded singleton.
- [ ] **Test:** feed a temp file with all keys + a comment + a blank + an unknown key; assert each field parses, `units=metric` → use_metric=true, and a missing file yields defaults.

### Task 1.2: gpsd GPS thread (linux)
**Files:** Create `src/pipeline/gps_gpsd.{c,h}`.

- [ ] `int gps_thread_start(void)` spawns a detached pthread: `gps_open("localhost","2947",&g)`, `gps_stream(&g, WATCH_ENABLE|WATCH_JSON, NULL)`; loop `gps_waiting(&g, 5_000_000)` → `gps_read(&g, NULL, 0)`; on a 2D/3D fix fill `g_gps_data` (lat, lon, altitude_m, speed_kmh = `fix.speed*3.6`, course_deg, hdop=`dop.hdop`, satellites=`satellites_used`, is_valid, unix_time, iso_time) under the pipeline mutex; set/clear `g_system_state.gps_fix` + first-fix `gps_ttff_ms`. Reconnect on error with backoff.
- [ ] Guard the libgps API minor differences with `GPSD_API_MAJOR_VERSION` where needed.
- [ ] **Verify on-device:** `gpsd` enabled on `/dev/ttyACM0`; `gpspipe -w -n5` shows a TPV; after deploy, `wdm_ui --info` over SSH shows real lat/lon (not Eiffel Tower) and the NAV screen updates.

### Task 1.3: pipeline umbrella + main wiring (slice-1 scope)
**Files:** Create `src/pipeline/pipeline.{c,h}`; Modify `src/main.c`.

- [ ] `pipeline.c`: `void wdm_pipeline_start(void)` → `config_load("config.txt")` then `gps_thread_start()` (scan/upload added in later slices); declare the shared mutex + `gps_snapshot()`.
- [ ] `main.c run_live()`: after `fb_open`, before loop: `#ifdef __linux__ wdm_pipeline_start(); #endif` (include `pipeline.h`).
- [ ] **Commit** slice 1.

---

## Slice 2 — Scan logging (`iw` → JSONL)

### Task 2.1: `geo` haversine (portable)
**Files:** Create `src/pipeline/geo.{c,h}`; Test.
- [ ] `double wdm_haversine_m(double lat1,double lon1,double lat2,double lon2)`.
- [ ] **Test:** ~111 km per 1° latitude (assert within 1%); identical points → 0.

### Task 2.2: `recordfmt` (portable)
**Files:** Create `src/pipeline/recordfmt.{c,h}`; Test.
- [ ] `int wdm_record_to_json(char *buf,size_t sz,const scan_result_t *r)` → exact firmware line (keys/order above), `%.6f` lat/lon, `%.1f` alt/spd, `%.1f` hdop; JSON-escape ssid (reuse `strutil`/inline minimal escaper for `"` `\` and control chars).
- [ ] **Test:** a known record renders byte-for-byte; an ssid with `"`/`\\` escapes correctly; empty ssid → `"ssid":""`.

### Task 2.3: `scan_parse` (portable — the tricky part, TDD)
**Files:** Create `src/pipeline/scan_parse.{c,h}`; Test with a **captured real `iw scan` fixture**.
- [ ] `int wdm_parse_iw_scan(const char *text, scan_result_t *out, int max)` — split on `BSS <mac>` blocks; per block extract `signal: -67.00 dBm`→rssi(round), `SSID: …`→ssid (blank = hidden→""), `DS Parameter set: channel N` or `primary channel: N` or freq→channel, security from presence of `RSN:`/`WPA:` + `Authentication suites` (`PSK`→WPA2, `SAE`→WPA3, both→WPA2/3; `WEP`→WEP; neither→Open), `freq`→channel fallback (2412→1 … 5180→36…). Returns count.
- [ ] **Test:** capture `iw dev wlan1 scan` output on the Pi into `tests/fixtures/iw_scan.txt`; assert parsed count, and spot-check 2-3 BSSIDs' ssid/rssi/channel/enc.

### Task 2.4: scan thread (linux)
**Files:** Create `src/pipeline/scan_iw.{c,h}`; Modify `pipeline.c`.
- [ ] `scan_backend` struct (fn ptr `scan_once(text_buf)`); default impl `iw`: `sudo ip link set <iface> up` once, then `popen("sudo iw dev <iface> scan")` each cycle into a heap buffer.
- [ ] Thread loop every `scan_interval_s`: scan → `wdm_parse_iw_scan` → for each AP, snapshot GPS (skip if no valid fix, like firmware), 75 m position-dedup (CRC32 of `bssid|ssid` → last lat/lon slot table in RAM), append `wdm_record_to_json` line to `data/wardrive.jsonl` (buffered, `mkdir -p data`), bump `ssids_logged`/`scan_count`/`last_ap_count` in `g_system_state`.
- [ ] `pipeline.c`: start scan thread after GPS. **Verify:** drive/walk test → `wardrive.jsonl` grows; SCAN screen AP count rises.
- [ ] **Commit** slice 2.

### Task 2.5: `hwid` (portable + linux)
**Files:** Create `src/pipeline/hwid.{c,h}`; Test.
- [ ] Pure `void wdm_hwid_from_mac(const char *mac, char *out12)` (strip colons, uppercase). Linux `const char *wdm_hwid(void)` reads `/sys/class/net/wlan0/address` (fallback eth0).
- [ ] **Test:** `b8:27:eb:de:cf:0d` → `B827EBDECF0D`.

---

## Slice 3 — Force Upload (libcurl)

### Task 3.1: upload thread (linux)
**Files:** Create `src/pipeline/upload.{c,h}`; Modify `pipeline.c`, `main.c`.
- [ ] `void upload_start(void)` (no-op if already running): set `g_system_state.current_screen=SCREEN_UPLOAD`, `upload_active=true`; read `data/wardrive.jsonl`, count lines → `upload_total`; batch 100 lines into `{"device_id","hw_id","name_ts":0,"records":[<lines>]}`; `curl_easy` POST with Bearer; parse `accepted/duplicates`; advance `upload_done`/`upload_batch_idx`; on success archive sent lines (truncate log to leftovers, or keep per `keep_uploaded`); set `upload_active=false` at end. Handle 401/5xx/conn-error → stop with a status, leave log intact.
- [ ] `main.c trigger_menu_action()`: `SCREEN_MENU_UPLOAD` → `wdm_pipeline_force_upload()`; `SCREEN_MENU_SETUP` → set `current_screen=SCREEN_SETUP_ACTIVE` (status only, slice 4 later).
- [ ] **Verify:** hold Force Upload → SCREEN_UPLOAD arc runs; backend `accepted>0`; the records appear on wardrivingmapper.com under the device.
- [ ] **Commit** slice 3.

---

## Build / deploy / ops

- **Deps (slice 0, in progress):** `apt install iw gpsd gpsd-clients libgps-dev libcurl4-openssl-dev`; set `/etc/default/gpsd` `DEVICES="/dev/ttyACM0"`, `GPSD_OPTIONS="-n"`, enable `gpsd`. Fold into `scripts/install-service.sh`.
- **Host build/test (Mac):** `cmake -S . -B build && cmake --build build && ./build/wdm_tests` (screenshot path still builds; pipeline threads are Linux-only).
- **Deploy to Pi:** rsync checkout → `shelby@10.0.1.242:/home/shelby/wardrivingmapper-pi`, `cmake --build build -j`, `sudo systemctl restart wdm-ui`. Smoke: `wdm_ui --info`.
- **SD-wear (standing pref):** buffered log appends, archive/rotate on upload, low write cadence.

## Out of scope (slice 4, later)
On-device AP + web UI config (hostapd/dnsmasq + writes `config.txt`), real Setup screen SSID/pass/QR.
