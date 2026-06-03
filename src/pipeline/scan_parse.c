#include "scan_parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

int wdm_freq_to_channel(int mhz)
{
    if (mhz >= 2412 && mhz <= 2472) return (mhz - 2407) / 5;   /* 2.4 GHz 1..13 */
    if (mhz == 2484)                return 14;
    if (mhz >= 5160 && mhz <= 5885) return (mhz - 5000) / 5;   /* 5 GHz 32..177 */
    if (mhz >= 5955 && mhz <= 7115) return (mhz - 5950) / 5;   /* 6 GHz */
    return 0;
}

/* Per-AP scratch accumulated across a block's lines. */
typedef struct {
    int  has;            /* a BSS block is open */
    int  has_rsn, has_wpa, has_psk, has_sae, has_privacy;
    int  ssid_set;       /* an SSID line has been seen (may be empty) */
    int  freq;           /* for channel fallback */
    int  channel;        /* from DS Parameter / primary channel */
    wdm_ap_t ap;
} acc_t;

/* Map accumulated security flags to the firmware's enc vocabulary. */
static void finish_enc(acc_t *a)
{
    const char *enc;
    if (a->has_rsn && a->has_wpa)        enc = "WPA/2";
    else if (a->has_rsn && a->has_sae && a->has_psk) enc = "WPA2/3";
    else if (a->has_rsn && a->has_sae)   enc = "WPA3";
    else if (a->has_rsn)                 enc = "WPA2";
    else if (a->has_wpa)                 enc = "WPA";
    else if (a->has_privacy)             enc = "WEP";
    else                                 enc = "Open";
    strncpy(a->ap.enc, enc, sizeof a->ap.enc - 1);
}

/* Flush the open block into out[] if it has a usable BSSID. */
static int flush(acc_t *a, wdm_ap_t *out, int idx, int max)
{
    if (!a->has || a->ap.bssid[0] == '\0' || idx >= max) { a->has = 0; return idx; }
    a->ap.channel = a->channel ? a->channel : wdm_freq_to_channel(a->freq);
    finish_enc(a);
    out[idx] = a->ap;
    a->has = 0;
    return idx + 1;
}

/* trim leading ws */
static const char *skip_ws(const char *s) { while (*s == ' ' || *s == '\t') s++; return s; }

int wdm_parse_iw_scan(const char *text, wdm_ap_t *out, int max)
{
    if (!text || !out || max <= 0) return 0;

    acc_t a; memset(&a, 0, sizeof a);
    int idx = 0;
    char line[600];

    const char *p = text;
    while (*p && idx < max) {
        /* copy one line (without the newline) into `line` */
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len >= sizeof line) len = sizeof line - 1;
        memcpy(line, p, len);
        line[len] = '\0';
        p = nl ? nl + 1 : p + strlen(p);

        const char *t = skip_ws(line);

        /* A block starts with "BSS <mac>" at column 0. Match the RAW line (not
         * the trimmed one) and require a hex BSSID, so the indented "BSS Load:"
         * information element isn't mistaken for a new access point. */
        if (line[0] == 'B' && strncmp(line, "BSS ", 4) == 0 && isxdigit((unsigned char)line[4])) {
            idx = flush(&a, out, idx, max);            /* close previous block */
            memset(&a, 0, sizeof a);
            a.has = 1;
            /* BSSID is the token after "BSS ", up to '(' or space */
            const char *m = line + 4;
            size_t j = 0;
            while (*m && *m != '(' && *m != ' ' && j + 1 < sizeof a.ap.bssid)
                a.ap.bssid[j++] = (char)toupper((unsigned char)*m++);
            a.ap.bssid[j] = '\0';
            continue;
        }
        if (!a.has) continue;

        if (strncmp(t, "signal:", 7) == 0) {
            a.ap.rssi = (int)lround(atof(skip_ws(t + 7)));
        } else if (strncmp(t, "freq:", 5) == 0) {
            a.freq = atoi(skip_ws(t + 5));
        } else if (strncmp(t, "DS Parameter set: channel", 25) == 0) {
            a.channel = atoi(skip_ws(t + 25));
        } else if (strncmp(t, "* primary channel:", 18) == 0) {
            if (!a.channel) a.channel = atoi(skip_ws(t + 18));
        } else if (strncmp(t, "SSID:", 5) == 0) {
            const char *v = t + 5;
            if (*v == ' ') v++;
            /* take the first SSID; upgrade an earlier empty one to a real name */
            if (!a.ssid_set || (a.ap.ssid[0] == '\0' && *v)) {
                strncpy(a.ap.ssid, v, sizeof a.ap.ssid - 1);
                a.ap.ssid[sizeof a.ap.ssid - 1] = '\0';
            }
            a.ssid_set = 1;
        } else if (strncmp(t, "capability:", 11) == 0) {
            if (strstr(t, "Privacy")) a.has_privacy = 1;
        } else if (strncmp(t, "RSN:", 4) == 0) {
            a.has_rsn = 1;
        } else if (strncmp(t, "WPA:", 4) == 0) {
            a.has_wpa = 1;
        } else if (strstr(t, "Authentication suites:")) {
            if (strstr(t, "PSK")) a.has_psk = 1;
            if (strstr(t, "SAE")) a.has_sae = 1;
        }
    }
    idx = flush(&a, out, idx, max);                    /* close the last block */
    return idx;
}
