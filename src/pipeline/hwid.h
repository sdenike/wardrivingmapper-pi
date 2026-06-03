/**
 * hwid.h — stable hardware id for the backend's device identity.
 *
 * The backend keys a device on (user_id, hw_id) so renames don't fork it. We
 * use the Pi's ETHERNET MAC (eth0) — always present and rock-stable, unlike the
 * onboard WiFi (whose firmware load can fail on boot) or the removable USB scan
 * dongle (whose name can shift). Falls back to wlan0. Uppercased, colons
 * stripped to 12 hex, matching the firmware's `%02X…` efuse-MAC format.
 */
#pragma once

#include <stddef.h>

/* Pure: "b8:27:eb:de:cf:0d" -> "B827EBDECF0D". out12 needs >=13 bytes. */
void wdm_hwid_from_mac(const char *mac, char *out12, size_t outsz);

/* Cached onboard hw_id read from /sys (Linux). Empty string if unavailable. */
const char *wdm_hwid(void);
