/**
 * scan_parse.h — parse `iw dev <iface> scan` text into observed APs.
 *
 * Pure and host-testable (the hard, bug-prone part of scanning). The Linux
 * scan thread runs `iw` and stamps GPS; this just turns its text into structs.
 */
#pragma once

#include "ap.h"

/* Parse `text` into up to `max` APs. Returns the count written (radio fields
 * only; GPS fields are left zeroed for the caller to stamp). */
int wdm_parse_iw_scan(const char *text, wdm_ap_t *out, int max);

/* Exposed for tests: 2412 -> 1, 5180 -> 36, etc. 0 if unknown. */
int wdm_freq_to_channel(int mhz);
