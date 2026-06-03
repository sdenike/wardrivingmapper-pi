/**
 * scan_iw.h — active Wi-Fi scan backend using `iw`.
 *
 * Brings the configured radio up and runs `iw dev <iface> scan` on an interval,
 * GPS-tags each AP, suppresses re-logs within 75 m (firmware-parity dedup), and
 * appends JSONL records to data/wardrive.jsonl. The scan_backend seam lets a
 * USB monitor-mode capture drop in later behind the same interface.
 */
#pragma once

#include <stdbool.h>

/* Spawn the detached scan thread. Returns 0 on success. */
int scan_thread_start(void);

/* Copy up to `max` unique named SSIDs from the most recent scan into out[]
 * (newest scan, signal-ordered as iw returned them). Returns the count. Used
 * by the Setup web UI to populate its Wi-Fi network picker. Thread-safe. */
int scan_snapshot_ssids(char out[][33], int max);

/* Pause/resume scanning. Setup mode pauses it while the dongle (the scan radio)
 * is repurposed as the config hotspot, then resumes it. */
void scan_pause(bool paused);
