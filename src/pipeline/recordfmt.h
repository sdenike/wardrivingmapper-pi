/**
 * recordfmt.h — serialise an observed AP to one wardrive.jsonl line.
 *
 * Byte-compatible with the firmware's log line and the backend's accepted keys:
 *   {"bssid":"…","ssid":"…","rssi":-67,"ch":6,"enc":"WPA2",
 *    "lat":42.732500,"lon":-84.555500,"alt":271.3,"spd":12.5,
 *    "hdop":0.9,"sats":9,"ts":"2026-05-14T14:23:01Z"}
 * No trailing newline (the caller adds it).
 */
#pragma once

#include <stddef.h>
#include "ap.h"

/* Returns the number of chars written (excluding NUL), or <0 on truncation. */
int wdm_record_to_json(char *buf, size_t sz, const wdm_ap_t *ap);
