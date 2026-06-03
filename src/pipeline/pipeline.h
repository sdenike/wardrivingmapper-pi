/**
 * pipeline.h — umbrella that turns the UI process into a wardriver.
 *
 * wdm_pipeline_start() loads config.txt and spins up the GPS + scan threads;
 * wdm_pipeline_force_upload() kicks the upload thread (the Force Upload hold).
 * Linux-only (gpsd / iw / libcurl); the host screenshot build never calls it.
 */
#pragma once

#include "wdm_platform.h"   /* gps_data_t */

void wdm_pipeline_start(void);
void wdm_pipeline_force_upload(void);

/* Shared lock around g_gps_data: held by the GPS thread while it writes and by
 * the scan thread while it snapshots, so a record never gets a half-updated fix. */
void pipeline_lock(void);
void pipeline_unlock(void);

/* Shared lock around the on-disk log: the scan thread appends under it, the
 * upload thread reads + rewrites under it, so they never collide. */
void log_lock(void);
void log_unlock(void);

/* Log lives beside the app (service WorkingDirectory = the repo checkout). */
#define WDM_DATA_DIR "data"
#define WDM_LOG_PATH "data/wardrive.jsonl"
