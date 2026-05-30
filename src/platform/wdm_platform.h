/**
 * wdm_platform.h — Linux/host compatibility layer for the Pi UI.
 *
 * src/ui/screens.c is the ESP32 firmware's main/display.c ported VERBATIM
 * (LVGL 8.4 widget code). That code was written against ESP-IDF + FreeRTOS.
 * Rather than edit ~1300 lines of widget code, this header re-defines the
 * handful of ESP/FreeRTOS symbols it references as Linux no-ops/equivalents,
 * and supplies the shared structs + constants that lived in the firmware's
 * config.h / scanner.h / nvs_config.h / touch.h.
 *
 * Keep the struct definitions in sync with firmware/main/config.h.
 *
 * Single-threaded host build:
 *   - locks are no-ops
 *   - ESP_LOGx -> stderr
 *   - esp_timer -> CLOCK_MONOTONIC
 *   - event group / button / touch -> stubbed in platform_stub.c
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "wdm_tick.h"   /* uint32_t wdm_tick_ms(void) */

/* ─── Board geometry ───────────────────────────────────────────────
 * The ported layout is 320 px wide (panel long edge) x LCD_H_RES tall
 * (short edge, the rendered height in landscape). Override with
 * -DLCD_H_RES / -DLCD_V_RES to target a panel profile:
 *   1.47B -> H=172, 2.8" -> H=240. V stays 320. The Pi Elecrow 3.5"
 *   (480x320) geometry is a later decision; the first screenshot uses
 *   the 2.8" profile (the larger known-good layout). */
#ifndef LCD_V_RES
#define LCD_V_RES 480          /* rendered WIDTH  (panel long edge) */
#endif
#ifndef LCD_H_RES
#define LCD_H_RES 320          /* rendered HEIGHT (panel short edge) */
#endif

/* ─── Version / build stamp (firmware: git-describe) ──────────────── */
#ifndef WDM_FW_VERSION
#define WDM_FW_VERSION "v0.1.0-pi"
#endif
#define WDM_BUILD_STAMP (__DATE__ " " __TIME__)

/* ─── Button hold thresholds (firmware: scanner.h) ─────────────────
 * LONG_MS only sets the hold-bar range on the non-poweroff menu screens
 * (a hidden modal in the SCAN screenshot); 1000 matches firmware intent. */
#define HOLD_POWEROFF_MS 3000
#define LONG_MS          1000

/* Buffer sizes (firmware: nvs_config.h) — used for on-stack sanitize buffers */
#define WD_MAX_SSID_LEN 33
#define WD_MAX_PASS_LEN 65
#define WD_MAX_URL_LEN  128

/* ─── Shared data structures (firmware: config.h — keep in sync) ─── */
typedef struct {
    double  lat;
    double  lon;
    float   altitude_m;
    float   speed_kmh;
    float   course_deg;
    float   hdop;
    uint8_t satellites;
    bool    is_valid;
    time_t  unix_time;
    char    iso_time[25];
} gps_data_t;

typedef struct {
    char    bssid[18];
    char    ssid[33];
    int8_t  rssi;
    uint8_t channel;
    uint8_t auth_mode;
    uint8_t pairwise_cipher;
    uint8_t group_cipher;
    uint8_t phy_flags;
    uint8_t channel_width;
    uint8_t ftm_flags;
    bool    wps;
    char    country_code[4];
    double  lat;
    double  lon;
    float   altitude_m;
    float   speed_kmh;
    float   course_deg;
    float   hdop;
    uint8_t sats;
    char    iso_time[25];
} scan_result_t;

typedef enum {
    SCREEN_SCAN          = 0,
    SCREEN_NAV           = 1,
    SCREEN_MENU_SETUP    = 2,
    SCREEN_MENU_POWEROFF = 3,
    SCREEN_SETUP_ACTIVE  = 4,
    SCREEN_UPLOAD        = 5,
    SCREEN_MENU_UPLOAD   = 6,
    SCREEN_BLE_PAIR      = 7,
} device_screen_t;

typedef struct {
    uint32_t ssids_logged;
    uint32_t scan_count;
    int      last_ap_count;
    bool     sd_ready;
    bool     gps_fix;
    float    battery_pct;
    bool     is_charging;
    bool     setup_mode;
    bool     upload_active;
    uint32_t upload_done;
    uint32_t upload_total;
    uint64_t uptime_ms;
    uint64_t sd_free_bytes;
    uint64_t sd_total_bytes;
    int32_t  last_scan_err;
    int32_t  wifi_init_err;
    int32_t  sd_mount_err;
    uint32_t scanner_heartbeat;
    uint8_t  current_screen;
    float    battery_voltage;
    uint32_t upload_batch_idx;
    uint32_t upload_batch_total;
    char     upload_ssid[33];
    bool     trusted_visible;
    bool     ble_enabled;
    bool     ble_connected;
    uint32_t gps_ttff_ms;
    uint8_t  gps_start_mode;
} system_state_t;

#define GPS_START_SEARCHING 0
#define GPS_START_HOT       1
#define GPS_START_WARM      2
#define GPS_START_COLD      3

/* Globals (defined in platform_stub.c) */
extern gps_data_t     g_gps_data;
extern system_state_t g_system_state;

/* ─── Slim device config (firmware: nvs_config.h) ─────────────────
 * Only the fields the screen code reads (ap_ssid/ap_pass/use_metric). */
typedef struct {
    char ap_ssid[33];
    char ap_pass[65];
    bool use_metric;
} wardrive_config_t;

void nvs_config_get(wardrive_config_t *out);   /* platform_stub.c */

/* ─── Button + touch hooks (firmware: scanner.h / touch.h) ───────── */
uint32_t button_get_held_ms(void);             /* platform_stub.c */
static inline void wdm_touch_init(void) {}
static inline void wdm_touch_poll(void) {}

/* ═══ FreeRTOS / ESP-IDF compatibility shims ══════════════════════ */
typedef int      SemaphoreHandle_t;
typedef uint32_t TickType_t;
typedef uint32_t EventBits_t;
typedef uint32_t EventGroupHandle_t;

#define pdTRUE  1
#define pdFALSE 0
#define portMAX_DELAY 0xFFFFFFFFu
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define configASSERT(x)   ((void)0)

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)              { return 1; }

/* Mutex handles the ported code passes to the (no-op) lock shims. */
#define g_gps_mutex   0
#define g_state_mutex 0
static inline int  xSemaphoreTake(SemaphoreHandle_t m, TickType_t t)     { (void)m; (void)t; return pdTRUE; }
static inline void xSemaphoreGive(SemaphoreHandle_t m)                   { (void)m; }
static inline TickType_t xTaskGetTickCount(void)                         { return (TickType_t)wdm_tick_ms(); }
static inline void vTaskDelay(TickType_t t)                              { (void)t; }

/* Event group — bits live in g_events (platform_stub.c). */
extern EventGroupHandle_t g_events;
static inline EventBits_t xEventGroupGetBits(EventGroupHandle_t g)       { return (EventBits_t)g; }

/* Event bits (firmware: config.h) */
#define EVT_GPS_FIX          (1u << 0)
#define EVT_SD_READY         (1u << 1)
#define EVT_SETUP_MODE       (1u << 2)
#define EVT_UPLOAD_ACTIVE    (1u << 3)
#define EVT_LOW_BATTERY      (1u << 4)
#define EVT_WIFI_CONNECTED   (1u << 5)
#define EVT_UPLOAD_REQUEST   (1u << 6)
#define EVT_SCAN_PAUSE       (1u << 7)
#define EVT_SCAN_RESUME      (1u << 8)
#define EVT_SCAN_PAUSED      (1u << 9)
#define EVT_LOG_ROTATE       (1u << 10)
#define EVT_LOG_ROTATED      (1u << 11)
#define EVT_LOG_REOPEN       (1u << 12)
#define EVT_LOG_FLUSH_NOW    (1u << 13)
#define EVT_BLE_PAIR_REQUEST (1u << 14)

/* esp_timer -> monotonic microseconds */
static inline int64_t esp_timer_get_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* esp_log -> stderr */
#define ESP_LOGI(tag, fmt, ...) fprintf(stderr, "[I %s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) fprintf(stderr, "[W %s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) fprintf(stderr, "[E %s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) ((void)0)

/* heap_caps -> stub (only appears in log args in the ported code) */
#define MALLOC_CAP_INTERNAL 0
#define MALLOC_CAP_SPIRAM   0
#define MALLOC_CAP_DMA      0
static inline size_t heap_caps_get_free_size(uint32_t caps) { (void)caps; return 0; }
