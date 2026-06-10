/**
 * config.h — key=value device config for the Pi wardriver.
 *
 * Replaces the firmware's NVS store. A plain `config.txt` (key=value, one per
 * line, '#' comments) sits beside the app and is edited over SSH for now; the
 * slice-4 web UI will write the same file later. Holds the upload credentials,
 * the scan radio selection, and display units.
 */
#pragma once

#include <stdbool.h>

typedef struct {
    char api_key[64];        /* wdm_… Bearer token for the backend            */
    char server_url[160];    /* full POST endpoint (…/api/v1/scans)           */
    char device_name[40];    /* device_id sent with each upload               */
    bool use_metric;         /* false = imperial (matches firmware default)   */
    char scan_iface[16];     /* radio to scan on (default USB dongle wlan1)   */
    int  scan_interval_s;    /* seconds between scans                         */
    bool keep_uploaded;      /* keep records after upload instead of archiving */
    char ap_pass[33];        /* setup-hotspot password (per-device random)    */
    char assistnow_token[80];/* u-blox AssistNow Online token (per-user; ""=off) */
} wdm_config_t;

/* Load config from `path`. Unknown keys are ignored; absent file leaves the
 * singleton at defaults and returns false. Returns true if the file was read. */
bool config_load(const char *path);

/* Apply one key=value to the singleton (used by config_load + the web UI). */
void config_set(const char *key, const char *val);

/* Serialise the singleton back to `path` as key=value. Returns false on error. */
bool config_save(const char *path);

/* The loaded singleton (defaults until config_load runs). Never NULL. */
const wdm_config_t *config_get(void);
