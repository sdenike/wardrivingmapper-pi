#pragma once
#include <stdint.h>
#include <stdbool.h>

/**
 * screens.h — public entry points for the ported LVGL dashboard
 * (src/ui/screens.c, ported from the ESP32 firmware's display.c).
 *
 * The hardware glue (panel init, flush callback, render task) was dropped;
 * main.c registers an LVGL display and drives create_ui()/ui_refresh().
 */

/* Build the main screen + all overlay panels. Call once after the LVGL
 * display is registered. */
void create_ui(void);

/* Repaint all dynamic labels from g_system_state / g_gps_data. Call each
 * frame (or once before a screenshot). */
void ui_refresh(void);

/* Cross-module display helpers (ported verbatim from firmware display.h).
 * Used later by the upload + BLE-pair flows; harmless if unused now. */
void display_show_upload_progress(uint32_t records_done, uint32_t records_total,
                                  uint32_t batch_idx, uint32_t batch_total,
                                  const char *trusted_ssid, const char *endpoint_host);
void display_show_upload_result(bool ok, const char *message);
void display_set_upload_resume(bool is_resume);
void display_show_pair_prompt(uint32_t code);
void display_hide_pair_prompt(void);
