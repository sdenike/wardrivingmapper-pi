/**
 * platform_stub.c — fake device state for headless screenshots.
 *
 * Provides the globals + hooks the ported screen code reads, populated with
 * realistic "scanning, GPS fix, SD ready" values so a render looks alive.
 * On the real Pi these get replaced by gpsd / iw-scan / config-file backends.
 */
#include "wdm_platform.h"

/* Event bits: scanning normally with a GPS fix and storage ready. */
EventGroupHandle_t g_events = EVT_GPS_FIX | EVT_SD_READY | EVT_WIFI_CONNECTED;

gps_data_t g_gps_data = {
    /* Obvious demo coordinates (Eiffel Tower) — NOT a real location. */
    .lat        = 48.858400,
    .lon        = 2.294500,
    .altitude_m = 271.4f,
    .speed_kmh  = 0.0f,
    .course_deg = 0.0f,
    .hdop       = 0.8f,
    .satellites = 11,
    .is_valid   = true,
    .unix_time  = 1780000000,
    .iso_time   = "2026-05-30T03:42:10Z",
};

system_state_t g_system_state = {
    .ssids_logged    = 1287,
    .scan_count      = 642,
    .last_ap_count   = 18,
    .sd_ready        = true,
    .gps_fix         = true,
    .battery_pct     = 84.0f,
    .is_charging     = false,
    .setup_mode      = false,
    .upload_active   = false,
    .uptime_ms       = 9123000,
    .sd_free_bytes   = 11ULL * 1024 * 1024 * 1024,
    .sd_total_bytes  = 12ULL * 1024 * 1024 * 1024,
    .current_screen  = SCREEN_SCAN,
    .battery_voltage = 3.96f,
    .trusted_visible = false,
    .ble_enabled     = true,
    .ble_connected   = false,
    .gps_ttff_ms     = 7000,
    .gps_start_mode  = GPS_START_WARM,
};

void nvs_config_get(wardrive_config_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    snprintf(out->ap_ssid, sizeof(out->ap_ssid), "WarDrivingMapper");
    snprintf(out->ap_pass, sizeof(out->ap_pass), "configure1");
    out->use_metric = false;   /* imperial, matches firmware default */
}

uint32_t button_get_held_ms(void)
{
    return 0;   /* no button held in a static render */
}

uint32_t wdm_tick_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
