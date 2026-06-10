#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

/* Defaults: a fresh device uploads to the public instance, scans the USB
 * dongle, imperial units — overridden by whatever config.txt provides. */
static wdm_config_t s_cfg = {
    .api_key         = "",
    .server_url      = "https://wardrivingmapper.com/api/v1/scans",
    .device_name     = "Pi WarDriver",
    .use_metric      = false,
    .scan_iface      = "wscan",   /* USB dongle (stable name; see .link/netplan) */
    .scan_interval_s = 10,
    .keep_uploaded   = false,
    .ap_pass         = "",      /* generated per-device at first run */
    .assistnow_token = "",      /* u-blox AssistNow token (per-user; set via web UI) */
};

/* An interface name must be shell-safe: it gets interpolated into `iw`/`ip`
 * commands. Allow only the characters a real iface name uses. */
static bool valid_iface(const char *s)
{
    if (!*s || strlen(s) > 15) return false;
    for (const char *p = s; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-' && *p != '.')
            return false;
    return true;
}

/* Trim ASCII whitespace in place; returns the first non-space char. */
static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = '\0';
    return s;
}

static bool parse_bool(const char *v)
{
    return strcasecmp(v, "true") == 0 || strcasecmp(v, "1") == 0 ||
           strcasecmp(v, "yes")  == 0 || strcasecmp(v, "on") == 0;
}

static void copy_field(char *dst, size_t dstsz, const char *src)
{
    strncpy(dst, src, dstsz - 1);
    dst[dstsz - 1] = '\0';
}

void config_set(const char *key, const char *val)
{
    if      (!strcmp(key, "api_key"))         copy_field(s_cfg.api_key, sizeof s_cfg.api_key, val);
    else if (!strcmp(key, "server_url"))      copy_field(s_cfg.server_url, sizeof s_cfg.server_url, val);
    else if (!strcmp(key, "device_name"))     copy_field(s_cfg.device_name, sizeof s_cfg.device_name, val);
    else if (!strcmp(key, "scan_iface"))      { if (valid_iface(val)) copy_field(s_cfg.scan_iface, sizeof s_cfg.scan_iface, val); }
    else if (!strcmp(key, "units"))           s_cfg.use_metric = (strcasecmp(val, "metric") == 0);
    else if (!strcmp(key, "scan_interval_s")) { int n = atoi(val); if (n >= 1 && n <= 3600) s_cfg.scan_interval_s = n; }
    else if (!strcmp(key, "keep_uploaded"))   s_cfg.keep_uploaded = parse_bool(val);
    else if (!strcmp(key, "ap_pass"))         copy_field(s_cfg.ap_pass, sizeof s_cfg.ap_pass, val);
    else if (!strcmp(key, "assistnow_token")) copy_field(s_cfg.assistnow_token, sizeof s_cfg.assistnow_token, val);
    /* unknown keys ignored — forward-compatible with future additions */
}

bool config_load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;

    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *p = trim(line);
        if (*p == '\0' || *p == '#') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        config_set(trim(p), trim(eq + 1));
    }
    fclose(f);
    return true;
}

bool config_save(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "# Written by the on-device Wi-Fi Setup web UI.\n");
    fprintf(f, "api_key=%s\n",         s_cfg.api_key);
    fprintf(f, "server_url=%s\n",      s_cfg.server_url);
    fprintf(f, "device_name=%s\n",     s_cfg.device_name);
    fprintf(f, "units=%s\n",           s_cfg.use_metric ? "metric" : "imperial");
    fprintf(f, "scan_iface=%s\n",      s_cfg.scan_iface);
    fprintf(f, "scan_interval_s=%d\n", s_cfg.scan_interval_s);
    fprintf(f, "keep_uploaded=%s\n",   s_cfg.keep_uploaded ? "true" : "false");
    fprintf(f, "ap_pass=%s\n",         s_cfg.ap_pass);
    fprintf(f, "assistnow_token=%s\n", s_cfg.assistnow_token);
    fclose(f);
    chmod(path, 0600);   /* holds the API key — keep it private */
    return true;
}

const wdm_config_t *config_get(void)
{
    return &s_cfg;
}
