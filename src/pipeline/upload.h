/**
 * upload.h — Force-Upload pipeline (libcurl → backend /api/v1/scans).
 *
 * upload_start() rotates the log aside and POSTs it in 100-record batches with
 * the Bearer api_key, mirroring the firmware. The worker thread never touches
 * LVGL; the render thread polls upload_poll() and drives the SCREEN_UPLOAD arc.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool     active;        /* an upload is in progress                   */
    bool     finished;      /* one-shot: a result is ready to display     */
    bool     ok;            /* result success/failure (when finished)     */
    uint32_t done, total;   /* records sent / total this run              */
    uint32_t batch_idx, batch_total;
    char     host[80];      /* endpoint host, for the screen              */
    char     msg[80];       /* result message                            */
} upload_status_t;

/* Kick an upload (no-op if one is already running). */
void upload_start(void);

/* Snapshot status for the render thread; returns true if there's something to
 * show. Consumes the `finished` edge so a result renders exactly once. */
bool upload_poll(upload_status_t *out);
