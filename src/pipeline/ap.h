/**
 * ap.h — one observed access point, the unit that flows scan → log → upload.
 *
 * Deliberately NOT the firmware's scan_result_t (which mirrors ESP wifi enums
 * we don't have on the Pi). scan_parse fills the radio fields; the scan thread
 * stamps the GPS fields at observe time; recordfmt serialises it to the exact
 * `wardrive.jsonl` line the backend + firmware share.
 */
#pragma once

typedef struct {
    char   bssid[18];     /* "AA:BB:CC:DD:EE:FF" (uppercase)        */
    char   ssid[65];      /* raw; JSON-escaped at format time       */
    int    rssi;          /* dBm                                    */
    int    channel;       /* 1..196                                 */
    char   enc[16];       /* Open|WEP|WPA2|WPA/2|WPA3|WPA2/3        */
    /* GPS tag, stamped when the AP is observed: */
    double lat, lon;
    double alt_m;
    double speed_kmh;
    double hdop;
    int    sats;
    char   iso_time[25];  /* "YYYY-MM-DDTHH:MM:SSZ"                 */
} wdm_ap_t;
