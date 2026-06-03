#include "pipeline.h"
#include "config.h"
#include "gps_gpsd.h"
#include "scan_iw.h"
#include "upload.h"
#include "wdm_platform.h"

#include <pthread.h>
#include <stdlib.h>

#define TAG "pipe"

static pthread_mutex_t s_gps_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_log_lock = PTHREAD_MUTEX_INITIALIZER;

void pipeline_lock(void)   { pthread_mutex_lock(&s_gps_lock); }
void pipeline_unlock(void) { pthread_mutex_unlock(&s_gps_lock); }
void log_lock(void)        { pthread_mutex_lock(&s_log_lock); }
void log_unlock(void)      { pthread_mutex_unlock(&s_log_lock); }

void wdm_pipeline_start(void)
{
    const char *cfg_path = getenv("WDM_CONFIG");
    if (!cfg_path || !*cfg_path) cfg_path = "config.txt";
    if (config_load(cfg_path))
        ESP_LOGI(TAG, "config loaded from %s", cfg_path);
    else
        ESP_LOGW(TAG, "no %s — using defaults (no api_key, scan_iface=%s)",
                 cfg_path, config_get()->scan_iface);

    /* Clear platform_stub.c's demo seed counts — this is a real device now, so
     * the screen should start at zero and count actual scans/logs. */
    g_system_state.ssids_logged  = 0;
    g_system_state.scan_count    = 0;
    g_system_state.last_ap_count = 0;
    g_gps_data.is_valid          = false;   /* no fake fix until gpsd reports one */
    g_gps_data.satellites        = 0;       /* drop the stub count until gpsd reports */
    g_system_state.gps_fix       = false;
    g_events &= ~EVT_GPS_FIX;
    /* Pi-specific: it's USB-powered (no battery gauge) and has no Bluetooth, so
     * drop the ESP32 battery % and BLE icon from the header. */
    g_system_state.battery_pct   = -1.0f;   /* <0 = "USB power, no battery" sentinel */
    g_system_state.is_charging   = false;
    g_system_state.ble_enabled   = false;

    if (gps_thread_start() != 0)  ESP_LOGW(TAG, "GPS thread failed to start");
    if (scan_thread_start() != 0) ESP_LOGW(TAG, "scan thread failed to start");
}

void wdm_pipeline_force_upload(void)
{
    upload_start();
}
