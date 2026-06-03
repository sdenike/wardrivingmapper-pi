#include "gps_gpsd.h"
#include "pipeline.h"
#include "wdm_platform.h"

#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <gps.h>

#define TAG "gps"

/* Fill g_gps_data's iso_time from a unix epoch (UTC). */
static void fmt_iso(time_t t, char *out, size_t sz)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, sz, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static void apply_fix(const struct gps_data_t *g, uint32_t *ttff_ms, uint32_t start_ms)
{
    /* Key on the RETAINED fix mode, not the per-read LATLON_SET flag: gpsd sets
     * LATLON_SET only on position (TPV) reports, never on satellite (SKY) reports.
     * Requiring it made every SKY update look like "no fix", which flickered the
     * fix indicator and zeroed the satellite count on screen. libgps retains
     * satellites_used/_visible across reads, so they're valid on any read. */
    int used = g->satellites_used;
    int seen = g->satellites_visible;

    if (g->fix.mode >= MODE_2D) {
        double alt;
        time_t tsec;
#if GPSD_API_MAJOR_VERSION >= 9
        alt  = g->fix.altMSL;
        tsec = g->fix.time.tv_sec;
#else
        alt  = g->fix.altitude;
        tsec = (time_t)g->fix.time;
#endif
        if (tsec <= 0) tsec = time(NULL);

        pipeline_lock();
        g_gps_data.lat        = g->fix.latitude;
        g_gps_data.lon        = g->fix.longitude;
        g_gps_data.altitude_m = (float)(alt == alt ? alt : 0.0);   /* NaN guard */
        g_gps_data.speed_kmh  = (float)((g->fix.speed == g->fix.speed) ? g->fix.speed * 3.6 : 0.0);
        g_gps_data.course_deg = (float)((g->fix.track == g->fix.track) ? g->fix.track : 0.0);
        g_gps_data.hdop       = (float)((g->dop.hdop == g->dop.hdop) ? g->dop.hdop : 0.0);
        if (used > 0)      g_gps_data.satellites = (uint8_t)used;
        else if (seen > 0) g_gps_data.satellites = (uint8_t)seen;
        g_gps_data.is_valid   = true;
        g_gps_data.unix_time  = tsec;
        fmt_iso(tsec, g_gps_data.iso_time, sizeof g_gps_data.iso_time);
        pipeline_unlock();

        g_system_state.gps_fix = true;
        g_events |= EVT_GPS_FIX;
        if (*ttff_ms == 0) {
            *ttff_ms = wdm_tick_ms() - start_ms;
            g_system_state.gps_ttff_ms = *ttff_ms;
            ESP_LOGI(TAG, "first fix: %.6f,%.6f sats=%d ttff=%ums",
                     g->fix.latitude, g->fix.longitude, used, *ttff_ms);
        }
    } else {
        pipeline_lock();
        g_gps_data.is_valid = false;
        if (seen > 0) g_gps_data.satellites = (uint8_t)seen;  /* tracked count while acquiring */
        pipeline_unlock();
        g_system_state.gps_fix = false;
        g_events &= ~EVT_GPS_FIX;
    }
}

static void *gps_thread(void *arg)
{
    (void)arg;
    uint32_t start_ms = wdm_tick_ms();
    uint32_t ttff_ms = 0;

    for (;;) {
        struct gps_data_t gpsd;
        if (gps_open("localhost", "2947", &gpsd) != 0) {
            ESP_LOGW(TAG, "gps_open failed; retry in 5s");
            sleep(5);
            continue;
        }
        gps_stream(&gpsd, WATCH_ENABLE | WATCH_JSON, NULL);
        ESP_LOGI(TAG, "connected to gpsd");

        for (;;) {
            if (!gps_waiting(&gpsd, 5000000)) continue;   /* 5 s, microseconds */
#if GPSD_API_MAJOR_VERSION >= 7
            int rc = gps_read(&gpsd, NULL, 0);
#else
            int rc = gps_read(&gpsd);
#endif
            if (rc < 0) { ESP_LOGW(TAG, "gps_read error %d; reconnecting", rc); break; }
            if (rc == 0) continue;                          /* no new data yet */
            apply_fix(&gpsd, &ttff_ms, start_ms);
        }

        gps_stream(&gpsd, WATCH_DISABLE, NULL);
        gps_close(&gpsd);
        sleep(2);
    }
    return NULL;
}

int gps_thread_start(void)
{
    pthread_t th;
    if (pthread_create(&th, NULL, gps_thread, NULL) != 0) return -1;
    pthread_detach(th);
    return 0;
}
