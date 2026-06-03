#include "scan_iw.h"
#include "pipeline.h"
#include "config.h"
#include "geo.h"
#include "ap.h"
#include "scan_parse.h"
#include "recordfmt.h"
#include "wdm_platform.h"

#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#define TAG       "scan"
#define MAX_APS   512

/* ── position-aware dedup table ───────────────────────────────────
 * Open-addressed, linear-probe. Key = CRC32("bssid|ssid"); slot holds the
 * last-logged lat/lon so the same AP within 75 m of where it was logged is
 * skipped, but the same AP a block away is logged again (firmware parity). */
#define DEDUP_SLOTS (1u << 16)        /* 65536 slots, ~1.5 MB */
typedef struct { uint32_t key; double lat, lon; } dslot_t;
static dslot_t *s_dedup;

/* Latest-scan SSID snapshot for the Setup web UI's network picker. */
#define SNAP_MAX 64
static pthread_mutex_t s_snap_lock = PTHREAD_MUTEX_INITIALIZER;
static char s_snap[SNAP_MAX][33];
static int  s_snap_n;
static volatile int s_paused;     /* set while the dongle is the setup hotspot */

void scan_pause(bool paused) { s_paused = paused ? 1 : 0; }

static void snapshot_ssids(const wdm_ap_t *aps, int n)
{
    pthread_mutex_lock(&s_snap_lock);
    s_snap_n = 0;
    for (int i = 0; i < n && s_snap_n < SNAP_MAX; i++) {
        if (aps[i].ssid[0] == '\0') continue;          /* skip hidden */
        int dup = 0;
        for (int j = 0; j < s_snap_n; j++)
            if (strcmp(s_snap[j], aps[i].ssid) == 0) { dup = 1; break; }
        if (!dup) { strncpy(s_snap[s_snap_n], aps[i].ssid, 32); s_snap[s_snap_n][32] = '\0'; s_snap_n++; }
    }
    pthread_mutex_unlock(&s_snap_lock);
}

int scan_snapshot_ssids(char out[][33], int max)
{
    pthread_mutex_lock(&s_snap_lock);
    int k = s_snap_n < max ? s_snap_n : max;
    for (int i = 0; i < k; i++) memcpy(out[i], s_snap[i], 33);
    pthread_mutex_unlock(&s_snap_lock);
    return k;
}

static uint32_t crc32_str(const char *s)
{
    uint32_t c = 0xFFFFFFFFu;
    for (; *s; s++) {
        c ^= (uint8_t)*s;
        for (int i = 0; i < 8; i++) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int)(c & 1)));
    }
    c ^= 0xFFFFFFFFu;
    return c ? c : 1;                  /* reserve 0 for "empty slot" */
}

/* Decide whether to log this AP and update the table. true = log it. */
static bool dedup_should_log(const char *bssid, const char *ssid, double lat, double lon)
{
    if (!s_dedup) return true;
    char buf[64 + 33];
    snprintf(buf, sizeof buf, "%s|%s", bssid, ssid);
    uint32_t key = crc32_str(buf);
    uint32_t i = key & (DEDUP_SLOTS - 1);
    for (uint32_t probe = 0; probe < DEDUP_SLOTS; probe++) {
        dslot_t *s = &s_dedup[i];
        if (s->key == 0) {                          /* new AP → log + remember */
            s->key = key; s->lat = lat; s->lon = lon;
            return true;
        }
        if (s->key == key) {                        /* seen before */
            if (wdm_haversine_m(s->lat, s->lon, lat, lon) < DEDUP_RADIUS_M) return false;
            s->lat = lat; s->lon = lon;             /* moved → log + update spot */
            return true;
        }
        i = (i + 1) & (DEDUP_SLOTS - 1);
    }
    return true;                                    /* table full (shouldn't happen) */
}

/* ── iw scan backend ──────────────────────────────────────────────
 * Run `iw dev <iface> scan` and slurp stdout into a growing heap buffer.
 * Returns a malloc'd NUL-terminated string (caller frees) or NULL. */
static char *iw_scan_once(const char *iface)
{
    char cmd[128];
    snprintf(cmd, sizeof cmd, "sudo iw dev %s scan 2>/dev/null", iface);
    FILE *p = popen(cmd, "r");
    if (!p) return NULL;

    size_t cap = 64 * 1024, len = 0;
    char *buf = malloc(cap);
    if (!buf) { pclose(p); return NULL; }
    size_t n;
    char chunk[8192];
    while ((n = fread(chunk, 1, sizeof chunk, p)) > 0) {
        if (len + n + 1 > cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); pclose(p); return NULL; }
            buf = nb;
        }
        memcpy(buf + len, chunk, n);
        len += n;
    }
    buf[len] = '\0';
    pclose(p);
    return buf;
}

static void iso_now(char *out, size_t sz)
{
    time_t t = time(NULL);
    struct tm tm; gmtime_r(&t, &tm);
    strftime(out, sz, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

/* Append already-formatted JSONL records under the log lock. */
static int append_records(wdm_ap_t *aps, int n, const gps_data_t *fix)
{
    log_lock();
    FILE *f = fopen(WDM_LOG_PATH, "a");
    if (!f) { log_unlock(); ESP_LOGW(TAG, "cannot open %s", WDM_LOG_PATH); return 0; }

    int logged = 0;
    char line[600];
    char iso[25]; iso_now(iso, sizeof iso);
    for (int i = 0; i < n; i++) {
        wdm_ap_t *a = &aps[i];
        if (!dedup_should_log(a->bssid, a->ssid, fix->lat, fix->lon)) continue;
        a->lat = fix->lat; a->lon = fix->lon;
        a->alt_m = fix->altitude_m; a->speed_kmh = fix->speed_kmh;
        a->hdop = fix->hdop; a->sats = fix->satellites;
        memcpy(a->iso_time, iso, sizeof a->iso_time);
        int w = wdm_record_to_json(line, sizeof line, a);
        if (w > 0) { fwrite(line, 1, (size_t)w, f); fputc('\n', f); logged++; }
    }
    fflush(f);
    fclose(f);
    log_unlock();
    return logged;
}

static void *scan_thread(void *arg)
{
    (void)arg;
    const wdm_config_t *cfg = config_get();
    s_dedup = calloc(DEDUP_SLOTS, sizeof *s_dedup);

    ESP_LOGI(TAG, "scanning on %s every %ds", cfg->scan_iface, cfg->scan_interval_s);

    wdm_ap_t *aps = malloc(sizeof(wdm_ap_t) * MAX_APS);
    if (!aps) return NULL;

    for (;;) {
        /* Paused while the dongle is the setup hotspot — don't touch the radio. */
        if (s_paused) { sleep(1); continue; }

        /* Ensure the scan radio is up each cycle (robust after the hotspot hands
         * it back in station mode). Needs no IP — we only scan on it. */
        char up[96];
        snprintf(up, sizeof up, "sudo ip link set %s up 2>/dev/null", cfg->scan_iface);
        int sysrc = system(up);
        (void)sysrc;

        char *text = iw_scan_once(cfg->scan_iface);
        int n = text ? wdm_parse_iw_scan(text, aps, MAX_APS) : 0;
        free(text);
        snapshot_ssids(aps, n);          /* feed the Setup web UI picker */

        /* Snapshot the GPS fix; without a real fix we count but don't log
         * (matches firmware — no Null-Island records). */
        pipeline_lock();
        gps_data_t fix = g_gps_data;
        pipeline_unlock();

        int logged = 0;
        if (n > 0 && fix.is_valid) logged = append_records(aps, n, &fix);

        g_system_state.last_ap_count = n;
        g_system_state.scan_count++;
        g_system_state.ssids_logged += (uint32_t)logged;
        g_system_state.scanner_heartbeat++;
        g_system_state.sd_ready = true;
        /* Real SD-card usage (the screen had the ESP32 stub's fake 1/12 GB). */
        struct statvfs vfs;
        if (statvfs(".", &vfs) == 0) {
            g_system_state.sd_total_bytes = (uint64_t)vfs.f_blocks * vfs.f_frsize;
            g_system_state.sd_free_bytes  = (uint64_t)vfs.f_bavail * vfs.f_frsize;
        }
        ESP_LOGI(TAG, "scan #%u: %d APs visible, %d logged, gps=%s sats=%u",
                 g_system_state.scan_count, n, logged,
                 fix.is_valid ? "fix" : "no-fix", (unsigned)fix.satellites);

        int wait = cfg->scan_interval_s > 0 ? cfg->scan_interval_s : 10;
        sleep((unsigned)wait);
    }
    return NULL;
}

int scan_thread_start(void)
{
    mkdir(WDM_DATA_DIR, 0755);   /* harmless if it exists */
    pthread_t th;
    if (pthread_create(&th, NULL, scan_thread, NULL) != 0) return -1;
    pthread_detach(th);
    return 0;
}
