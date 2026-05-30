/**
 * display.c — ST7789 display driver + LVGL dashboard UI
 *
 * Uses esp_lcd (ESP-IDF native) for the panel driver,
 * LVGL v8 for rendering.
 *
 * Screen layout (landscape: 320w × 172h):
 *   [0–28]    Header bar: mode name + battery %
 *   [30–50]   GPS row
 *   [52–72]   Coordinates
 *   [74–94]   SSIDs logged
 *   [96–116]  Scan count + SD status
 *   [118–138] Alert bar (GPS no fix, low bat, etc.)
 *   [140–172] Footer: uptime + button hint
 */

#include <string.h>
#include <stdio.h>

#define LV_CONF_INCLUDE_SIMPLE 1
#include "lvgl.h"
#include "extra/libs/qrcode/lv_qrcode.h"

#include <math.h>
#include "strutil.h"
#include "wdm_platform.h"
#include "screens.h"

static const char *TAG = "DISPLAY";


// LVGL mutex — must be held when calling any lv_* function from outside display_task
static SemaphoreHandle_t lvgl_mutex;

// Set true at the end of create_ui() once both alternate screens
// (upload + pair) are fully constructed. Cross-task display_* helpers
// (display_set_upload_resume, display_show_upload_progress,
// display_show_upload_result, display_show_pair_prompt) check this
// before touching LVGL — if upload_task or ble.c fires before the
// boot-time UI build has finished, the helper returns silently and
// the upload/pair proceeds without UI rather than racing into a
// half-constructed LVGL state. Defense in depth alongside holding
// lvgl_mutex during create_ui.
static volatile bool s_display_ready = false;


// ============================================================
// UI WIDGETS
// ============================================================
static lv_obj_t *lbl_mode;
static lv_obj_t *lbl_battery;
static lv_obj_t *lbl_trusted;   // small "home" icon shown when trusted SSID is in range
static lv_obj_t *lbl_ble;       // bluetooth icon — solid when connected, blinks while advertising
static lv_obj_t *lbl_gps_status;
static lv_obj_t *lbl_gps_sats;
static lv_obj_t *lbl_coords;
static lv_obj_t *lbl_ssid_count;
static lv_obj_t *lbl_scan_info;
static lv_obj_t *lbl_sd_status;
static lv_obj_t *lbl_alert;
static lv_obj_t *lbl_uptime;
static lv_obj_t *header_bar;
static lv_obj_t *alert_bar;
static lv_obj_t *row_gps;
// Setup-mode widgets — overlay the scan widgets when in setup mode
static lv_obj_t *setup_panel;
static lv_obj_t *setup_ssid_value;
static lv_obj_t *setup_pass_value;
static lv_obj_t *setup_qr;
static char      setup_qr_last[128] = "";
// Menu-screen widgets — shown when cycling through choices via short-press BOOT
static lv_obj_t *menu_panel;
static lv_obj_t *menu_icon;
static lv_obj_t *menu_title;
static lv_obj_t *menu_hint;

// Hold-action progress modal — floats above any active screen on
// lv_layer_top() so the bar fill is unmistakable. ui_refresh() shows it
// while button_get_held_ms() > 0 AND current_screen is one of the three
// menu screens, then sets title/bar range/fill color per screen so the
// user knows which action they're about to trigger:
//   SCREEN_MENU_SETUP   — "Hold to enter Wi-Fi Setup", purple, 1 s fill
//   SCREEN_MENU_UPLOAD  — "Hold to force upload",      cyan,   1 s fill
//   SCREEN_MENU_POWEROFF — "Hold to power off",        red,    3 s fill
static lv_obj_t *hold_overlay;   // full-screen backdrop (semi-transparent)
static lv_obj_t *hold_box;       // centered card
static lv_obj_t *hold_title;
static lv_obj_t *hold_bar;       // lv_bar — range/value/color set per refresh
static lv_obj_t *hold_hint;
// NAV screen widgets — alternate dashboard showing speed/altitude/heading
static lv_obj_t *nav_panel;
static lv_obj_t *nav_speed_val;
static lv_obj_t *nav_alt_val;
static lv_obj_t *nav_head_val;
static lv_obj_t *nav_coord_val;
static lv_obj_t *nav_ttff_val;   // shows "TTFF 3s HOT" once gps_task latches first-fix
// Upload screen widgets — built lazily and shown via lv_scr_load
static lv_obj_t *s_upload_scr    = NULL;
static lv_obj_t *s_upload_title  = NULL;
static lv_obj_t *s_upload_ssid   = NULL;
static lv_obj_t *s_upload_arc    = NULL;
static lv_obj_t *s_upload_pct    = NULL;  // "NN%" label centered inside arc
static lv_obj_t *s_upload_count  = NULL;
static lv_obj_t *s_upload_host   = NULL;
static lv_obj_t *s_upload_result = NULL;  // hidden, shown on completion

// Handle to the main screen so we can switch back to it when leaving
// SCREEN_UPLOAD (which uses a separate LVGL screen object).
static lv_obj_t *s_main_scr = NULL;

// Forward decl — defined in the SCREEN_BLE_PAIR section below.
static lv_obj_t *s_pair_scr;

// Pre-built alternate screens (upload progress + BLE pair prompt). Both
// are created once during boot from create_ui() so the lv_obj_create()
// allocations happen when free internal heap is at maximum (~200 KB).
// Lazy-building these on first use was crashing the device during an
// upload: by then WiFi + lwIP + mbedtls had eaten enough internal RAM
// that malloc() returned NULL inside lv_obj_class_create_obj, and LVGL
// doesn't check the return — lv_obj_class_init_obj then dereferenced
// NULL and the device hit Guru Meditation in lv_obj_mark_layout_as_dirty.
// Building at boot sidesteps the OOM entirely; the screens just sit in
// memory until lv_scr_load activates them.
static void build_upload_screen(void);
static void build_pair_screen(void);

void create_ui(void)
{
    lv_obj_t *scr = lv_scr_act();
    s_main_scr = scr;
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0D0D1A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Menu-panel vertical offset. The SETUP / FORCE UPLOAD / POWER OFF
    // menu is only ~3 lines of text and looked stuck-to-the-top on the
    // taller 2.8" panel; pushing it down by half the extra screen height
    // centers it visually. Applied ONLY to menu_panel — the SCAN
    // dashboard labels, NAV panel, and Wi-Fi Setup panel are designed
    // to hug the top of the body so they follow the GPS row directly.
    //
    // The display is rotated landscape: panel's PORTRAIT short edge
    // (LCD_H_RES) becomes the rendered HEIGHT. So the body-area math
    // uses LCD_H_RES, not LCD_V_RES (= the 320 long edge).
    //   1.47B (LCD_H_RES = 172) → MENU_VOFF = 0  (no layout change)
    //   2.8"  (LCD_H_RES = 240) → MENU_VOFF = 34 (menu drops 34 px)
    const int MENU_VOFF = (LCD_H_RES - 172) / 2;

    // ── Header bar ──────────────────────────────────────────
    // NOTE (Pi landscape 480x320): fonts + row pitch scaled up from the
    // ESP32 240px-tall layout to fill the taller panel. See docs/design.md.
    header_bar = lv_obj_create(scr);
    lv_obj_set_size(header_bar, LCD_V_RES, 36);
    lv_obj_set_pos(header_bar, 0, 0);
    lv_obj_set_style_bg_color(header_bar, lv_color_hex(0x1A5C2A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(header_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(header_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(header_bar, 0, LV_PART_MAIN);
    lv_obj_clear_flag(header_bar, LV_OBJ_FLAG_SCROLLABLE);

    lbl_mode = lv_label_create(header_bar);
    lv_label_set_text(lbl_mode, LV_SYMBOL_WIFI "  SCANNING");
    lv_obj_set_style_text_color(lbl_mode, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_mode, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(lbl_mode, LV_ALIGN_LEFT_MID, 10, 0);

    lbl_battery = lv_label_create(header_bar);
    lv_label_set_text(lbl_battery, "BAT ---%");
    lv_obj_set_style_text_color(lbl_battery, lv_color_hex(0xCCCCCC), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_battery, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_align(lbl_battery, LV_ALIGN_RIGHT_MID, -10, 0);

    // Trusted-SSID indicator — sits just left of the battery, hidden by default.
    lbl_trusted = lv_label_create(header_bar);
    lv_label_set_text(lbl_trusted, LV_SYMBOL_HOME);
    lv_obj_set_style_text_color(lbl_trusted, lv_color_hex(0xA6E3A1), LV_PART_MAIN);  // soft green
    lv_obj_set_style_text_font(lbl_trusted, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_align(lbl_trusted, LV_ALIGN_RIGHT_MID, -106, 0);   // left of battery text
    lv_obj_add_flag(lbl_trusted, LV_OBJ_FLAG_HIDDEN);

    // BLE indicator — always present. ui_refresh() makes it solid while a
    // central (the iOS app) is connected, and blinks it while the device
    // is only advertising. Sits just left of the trusted-SSID icon.
    lbl_ble = lv_label_create(header_bar);
    lv_label_set_text(lbl_ble, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_color(lbl_ble, lv_color_hex(0x89B4FA), LV_PART_MAIN);  // soft blue
    lv_obj_set_style_text_font(lbl_ble, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_align(lbl_ble, LV_ALIGN_RIGHT_MID, -136, 0);

    // ── GPS row ─────────────────────────────────────────────
    row_gps = lv_obj_create(scr);
    lv_obj_set_size(row_gps, LCD_V_RES, 30);
    lv_obj_set_pos(row_gps, 0, 50);
    // Solid bg matching screen so label updates don't leave stale glyphs
    lv_obj_set_style_bg_color(row_gps, lv_color_hex(0x0D0D1A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row_gps, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(row_gps, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(row_gps, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row_gps, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row_gps, LV_OBJ_FLAG_SCROLLABLE);

    // GPS row uses an LV_SYMBOL_GPS icon inline (set in ui_refresh), so
    // we don't need a separate "GPS" header label anymore.
    lbl_gps_status = lv_label_create(row_gps);
    lv_label_set_text(lbl_gps_status, LV_SYMBOL_GPS " Searching");
    lv_obj_set_style_text_color(lbl_gps_status, lv_color_hex(0xFFA500), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_gps_status, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_pos(lbl_gps_status, 10, 4);

    // Right-aligned sat count — pinned to a fixed x so it doesn't shift
    // when the animated dots on lbl_gps_status change width.
    lbl_gps_sats = lv_label_create(row_gps);
    lv_label_set_text(lbl_gps_sats, "0 sats");
    lv_obj_set_style_text_color(lbl_gps_sats, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_gps_sats, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_align(lbl_gps_sats, LV_ALIGN_RIGHT_MID, -10, 0);

    // ── Coordinates ─────────────────────────────────────────
    lbl_coords = lv_label_create(scr);
    lv_label_set_text(lbl_coords, "          ---.------,  ---.------");
    lv_obj_set_style_text_color(lbl_coords, lv_color_hex(0x00CCFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_coords, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_pos(lbl_coords, 10, 96);

    // ── SSID count ──────────────────────────────────────────
    lbl_ssid_count = lv_label_create(scr);
    lv_label_set_text(lbl_ssid_count, LV_SYMBOL_LIST "  0 unique networks logged");
    lv_obj_set_style_text_color(lbl_ssid_count, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_ssid_count, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_pos(lbl_ssid_count, 10, 144);

    // ── Scan info ───────────────────────────────────────────
    lbl_scan_info = lv_label_create(scr);
    lv_label_set_text(lbl_scan_info, "Scan #0  |  0 APs visible");
    lv_obj_set_style_text_color(lbl_scan_info, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_scan_info, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_pos(lbl_scan_info, 10, 192);

    // ── SD status ───────────────────────────────────────────
    lbl_sd_status = lv_label_create(scr);
    lv_label_set_text(lbl_sd_status, LV_SYMBOL_SAVE "  Initializing...");
    lv_obj_set_style_text_color(lbl_sd_status, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_sd_status, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_pos(lbl_sd_status, 10, 230);

    // ── Setup-mode panel ────────────────────────────────────
    // Covers the body area between header and footer; hidden in scan mode.
    setup_panel = lv_obj_create(scr);
    lv_obj_set_size(setup_panel, LCD_V_RES, 138);
    lv_obj_set_pos(setup_panel, 0, 30);
    lv_obj_set_style_bg_color(setup_panel, lv_color_hex(0x0D0D1A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(setup_panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(setup_panel, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(setup_panel, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(setup_panel, 6, LV_PART_MAIN);
    lv_obj_clear_flag(setup_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(setup_panel, LV_OBJ_FLAG_HIDDEN);

    // QR code on the left — encodes WIFI:T:WPA;S:...;P:...;; for scan-to-join
    setup_qr = lv_qrcode_create(setup_panel, 110, lv_color_black(), lv_color_white());
    lv_obj_align(setup_qr, LV_ALIGN_LEFT_MID, 0, -2);
    lv_qrcode_update(setup_qr, "WIFI:T:WPA;S:WarDrivingMapper;P:configure1;;", 36);

    // Right column — labels stacked, all centered within the 200px-wide
    // strip to the right of the QR code.
    #define SETUP_COL_X 120
    #define SETUP_COL_W 200
    lv_obj_t *setup_hint = lv_label_create(setup_panel);
    lv_label_set_text(setup_hint, LV_SYMBOL_WIFI "  Scan to join:");
    lv_obj_set_style_text_color(setup_hint, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_set_style_text_font(setup_hint, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_width(setup_hint, SETUP_COL_W);
    lv_obj_set_style_text_align(setup_hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(setup_hint, LV_ALIGN_TOP_LEFT, SETUP_COL_X, 0);

    setup_ssid_value = lv_label_create(setup_panel);
    lv_label_set_text(setup_ssid_value, "WarDrivingMapper");
    lv_obj_set_style_text_color(setup_ssid_value, lv_color_hex(0xCBA6F7), LV_PART_MAIN);
    lv_obj_set_style_text_font(setup_ssid_value, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_width(setup_ssid_value, SETUP_COL_W);
    lv_obj_set_style_text_align(setup_ssid_value, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(setup_ssid_value, LV_ALIGN_TOP_LEFT, SETUP_COL_X, 18);

    lv_obj_t *pass_hint = lv_label_create(setup_panel);
    lv_label_set_text(pass_hint, "Password:");
    lv_obj_set_style_text_color(pass_hint, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_set_style_text_font(pass_hint, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_width(pass_hint, SETUP_COL_W);
    lv_obj_set_style_text_align(pass_hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(pass_hint, LV_ALIGN_TOP_LEFT, SETUP_COL_X, 46);

    setup_pass_value = lv_label_create(setup_panel);
    lv_label_set_text(setup_pass_value, "configure1");
    lv_obj_set_style_text_color(setup_pass_value, lv_color_hex(0xA6E3A1), LV_PART_MAIN);
    lv_obj_set_style_text_font(setup_pass_value, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_width(setup_pass_value, SETUP_COL_W);
    lv_obj_set_style_text_align(setup_pass_value, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(setup_pass_value, LV_ALIGN_TOP_LEFT, SETUP_COL_X, 62);

    lv_obj_t *setup_url = lv_label_create(setup_panel);
    lv_label_set_text(setup_url, LV_SYMBOL_HOME "  192.168.4.1");
    lv_obj_set_style_text_color(setup_url, lv_color_hex(0x89DCEB), LV_PART_MAIN);
    lv_obj_set_style_text_font(setup_url, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_width(setup_url, SETUP_COL_W);
    lv_obj_set_style_text_align(setup_url, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(setup_url, LV_ALIGN_TOP_LEFT, SETUP_COL_X, 96);

    // Build stamp footer — answers "is this device on the latest firmware?"
    // at a glance, without serial-monitor access. Filed under the URL so it
    // sits under Jake's eye while he's troubleshooting the AP connection.
    lv_obj_t *setup_build = lv_label_create(setup_panel);
    char build_label[64];
    // WDM_FW_VERSION already begins with "v" (from git describe), so the
    // format is plain "%s" — earlier "v%s" produced "vv0.4.0...".
    snprintf(build_label, sizeof(build_label),
             "%s  %s", WDM_FW_VERSION, WDM_BUILD_STAMP);
    lv_label_set_text(setup_build, build_label);
    lv_obj_set_style_text_color(setup_build, lv_color_hex(0x6c7086), LV_PART_MAIN);
    // montserrat_14 — _12 is in lv_conf.h but the LVGL component build
    // doesn't pick it up for reasons that aren't worth a side quest.
    lv_obj_set_style_text_font(setup_build, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_width(setup_build, SETUP_COL_W);
    lv_obj_set_style_text_align(setup_build, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(setup_build, LV_ALIGN_TOP_LEFT, SETUP_COL_X, 118);

    // ── Menu panel ──────────────────────────────────────────
    // Shown when cycling through menu screens; same body area as setup panel.
    menu_panel = lv_obj_create(scr);
    lv_obj_set_size(menu_panel, LCD_V_RES, 138);
    lv_obj_set_pos(menu_panel, 0, 30 + MENU_VOFF);
    lv_obj_set_style_bg_color(menu_panel, lv_color_hex(0x0D0D1A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(menu_panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(menu_panel, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(menu_panel, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(menu_panel, 8, LV_PART_MAIN);
    lv_obj_clear_flag(menu_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(menu_panel, LV_OBJ_FLAG_HIDDEN);

    menu_icon = lv_label_create(menu_panel);
    lv_label_set_text(menu_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(menu_icon, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(menu_icon, lv_color_hex(0xCBA6F7), LV_PART_MAIN);
    lv_obj_align(menu_icon, LV_ALIGN_TOP_MID, 0, 8);

    menu_title = lv_label_create(menu_panel);
    lv_label_set_text(menu_title, "Wi-Fi Setup");
    lv_obj_set_style_text_font(menu_title, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(menu_title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(menu_title, LV_ALIGN_TOP_MID, 0, 42);

    menu_hint = lv_label_create(menu_panel);
    lv_label_set_text(menu_hint, "Hold BOOT to enter");
    lv_obj_set_style_text_font(menu_hint, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(menu_hint, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_align(menu_hint, LV_ALIGN_BOTTOM_MID, 0, -8);

    // ── NAV panel — alternate dashboard ─────────────────────
    nav_panel = lv_obj_create(scr);
    lv_obj_set_size(nav_panel, LCD_V_RES, 138);
    lv_obj_set_pos(nav_panel, 0, 30);
    lv_obj_set_style_bg_color(nav_panel, lv_color_hex(0x0D0D1A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(nav_panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(nav_panel, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(nav_panel, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(nav_panel, 6, LV_PART_MAIN);
    lv_obj_clear_flag(nav_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(nav_panel, LV_OBJ_FLAG_HIDDEN);

    // Speed (big — top half)
    lv_obj_t *nav_speed_lbl = lv_label_create(nav_panel);
    lv_label_set_text(nav_speed_lbl, "SPEED");
    lv_obj_set_style_text_font(nav_speed_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(nav_speed_lbl, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_align(nav_speed_lbl, LV_ALIGN_TOP_LEFT, 6, 0);

    nav_speed_val = lv_label_create(nav_panel);
    // ASCII placeholders only — em-dash U+2014, ellipsis U+2026, and
    // degree U+00B0 all live outside LVGL Montserrat's bundled glyph
    // range and render as a .notdef block on the LCD.
    lv_label_set_text(nav_speed_val, "-");
    lv_obj_set_style_text_font(nav_speed_val, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(nav_speed_val, lv_color_hex(0xCBA6F7), LV_PART_MAIN);
    lv_obj_align(nav_speed_val, LV_ALIGN_TOP_LEFT, 6, 16);

    // Altitude (left column, lower)
    lv_obj_t *nav_alt_lbl = lv_label_create(nav_panel);
    lv_label_set_text(nav_alt_lbl, LV_SYMBOL_UP "  ALT");
    lv_obj_set_style_text_font(nav_alt_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(nav_alt_lbl, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_align(nav_alt_lbl, LV_ALIGN_TOP_LEFT, 6, 54);

    nav_alt_val = lv_label_create(nav_panel);
    lv_label_set_text(nav_alt_val, "- m");
    lv_obj_set_style_text_font(nav_alt_val, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(nav_alt_val, lv_color_hex(0x89DCEB), LV_PART_MAIN);
    lv_obj_align(nav_alt_val, LV_ALIGN_TOP_LEFT, 6, 70);

    // Heading (right column, lower)
    lv_obj_t *nav_head_lbl = lv_label_create(nav_panel);
    lv_label_set_text(nav_head_lbl, "HEADING");
    lv_obj_set_style_text_font(nav_head_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(nav_head_lbl, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_align(nav_head_lbl, LV_ALIGN_TOP_LEFT, 170, 54);

    nav_head_val = lv_label_create(nav_panel);
    lv_label_set_text(nav_head_val, "-");
    lv_obj_set_style_text_font(nav_head_val, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(nav_head_val, lv_color_hex(0xA6E3A1), LV_PART_MAIN);
    lv_obj_align(nav_head_val, LV_ALIGN_TOP_LEFT, 170, 70);

    // Coordinates strip — positioned so it clears the on-screen footer
    // (footer lives at y=152..172, panel starts at y=30, so anything past
    // panel-local y≈115 gets covered).
    nav_coord_val = lv_label_create(nav_panel);
    lv_label_set_text(nav_coord_val, LV_SYMBOL_GPS "  no fix");
    lv_obj_set_style_text_font(nav_coord_val, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(nav_coord_val, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_align(nav_coord_val, LV_ALIGN_TOP_LEFT, 6, 100);

    // TTFF diagnostic — empty until gps_task latches first fix, then it
    // stays for the rest of the session. Top-right corner of the panel
    // (directly under the header bar) so it doesn't collide with the
    // coordinate strip at the bottom-left (y=100). Earlier it sat at
    // y=100 on the right and the lat/lon text ran over it.
    nav_ttff_val = lv_label_create(nav_panel);
    lv_label_set_text(nav_ttff_val, "");
    lv_obj_set_style_text_font(nav_ttff_val, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(nav_ttff_val, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_align(nav_ttff_val, LV_ALIGN_TOP_RIGHT, -6, 0);

    // ── Alert bar ───────────────────────────────────────────
    alert_bar = lv_obj_create(scr);
    lv_obj_set_size(alert_bar, LCD_V_RES, 18);
    // Anchor to the bottom of the screen, just above the 20 px footer.
    // Use LCD_H_RES — the panel is rotated landscape, so the portrait
    // SHORT edge is what becomes the rendered HEIGHT. LCD_V_RES is the
    // 320 long edge, not what we want here. 134 was the magic number
    // for the 172 px-tall 1.47B; LCD_H_RES - 38 generalises it.
    lv_obj_set_pos(alert_bar, 0, LCD_H_RES - 38);
    lv_obj_set_style_bg_color(alert_bar, lv_color_hex(0x1A1A00), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(alert_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(alert_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(alert_bar, 0, LV_PART_MAIN);
    lv_obj_clear_flag(alert_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(alert_bar, LV_OBJ_FLAG_HIDDEN);

    lbl_alert = lv_label_create(alert_bar);
    lv_label_set_text(lbl_alert, "");
    lv_obj_set_style_text_color(lbl_alert, lv_color_hex(0xFFCC00), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_alert, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(lbl_alert, LV_ALIGN_LEFT_MID, 6, 0);

    // ── Footer ──────────────────────────────────────────────
    lv_obj_t *footer = lv_obj_create(scr);
    lv_obj_set_size(footer, LCD_V_RES, 20);
    // Anchor to the very bottom of the screen. LCD_H_RES is the rendered
    // HEIGHT (landscape rotation makes the portrait short edge become
    // the rendered height). 152 was the magic number for the 172 px-tall
    // 1.47B; LCD_H_RES - 20 generalises to any board.
    lv_obj_set_pos(footer, 0, LCD_H_RES - 20);
    // Match the screen's body bg (0x0D0D1A "Crust" Catppuccin) so the
    // footer doesn't show as a slightly-lighter band — the previous
    // 0x111111 was close to but visibly distinct from the body.
    lv_obj_set_style_bg_color(footer, lv_color_hex(0x0D0D1A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(footer, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(footer, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(footer, 0, LV_PART_MAIN);
    lv_obj_clear_flag(footer, LV_OBJ_FLAG_SCROLLABLE);

    lbl_uptime = lv_label_create(footer);
    lv_label_set_text(lbl_uptime, "00:00:00  |  Press BOOT to toggle mode");
    lv_obj_set_style_text_color(lbl_uptime, lv_color_hex(0x555555), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_uptime, &lv_font_montserrat_14, LV_PART_MAIN);
    // Centered along the footer so the uptime + version + build stamp
    // sit symmetrically rather than hugging the left edge.
    lv_obj_align(lbl_uptime, LV_ALIGN_CENTER, 0, 0);

    // ── Hold-action progress modal ──────────────────────────
    // Lives on lv_layer_top() so it floats above whichever main-screen
    // panel is currently visible. Hidden by default; ui_refresh() shows
    // it whenever BOOT is being held on a menu screen, then adapts the
    // title text, bar range, and indicator color per screen.
    hold_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(hold_overlay, LCD_V_RES, LCD_H_RES);
    lv_obj_set_pos(hold_overlay, 0, 0);
    lv_obj_set_style_bg_color(hold_overlay, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(hold_overlay, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_border_width(hold_overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(hold_overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(hold_overlay, 0, LV_PART_MAIN);
    lv_obj_clear_flag(hold_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(hold_overlay, LV_OBJ_FLAG_HIDDEN);

    hold_box = lv_obj_create(hold_overlay);
    lv_obj_set_size(hold_box, 260, 110);
    lv_obj_align(hold_box, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(hold_box, lv_color_hex(0x181825), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(hold_box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(hold_box, lv_color_hex(0xF38BA8), LV_PART_MAIN);
    lv_obj_set_style_border_width(hold_box, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(hold_box, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(hold_box, 10, LV_PART_MAIN);
    lv_obj_clear_flag(hold_box, LV_OBJ_FLAG_SCROLLABLE);

    hold_title = lv_label_create(hold_box);
    lv_label_set_text(hold_title, "Hold to power off");
    lv_obj_set_style_text_font(hold_title, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(hold_title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(hold_title, LV_ALIGN_TOP_MID, 0, 4);

    hold_bar = lv_bar_create(hold_box);
    lv_obj_set_size(hold_bar, 220, 16);
    lv_obj_align(hold_bar, LV_ALIGN_CENTER, 0, 4);
    lv_bar_set_range(hold_bar, 0, HOLD_POWEROFF_MS);
    lv_bar_set_value(hold_bar, 0, LV_ANIM_OFF);
    // Track (background) — muted Catppuccin "surface1"
    lv_obj_set_style_bg_color(hold_bar, lv_color_hex(0x313244), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(hold_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(hold_bar, lv_color_hex(0x45475A), LV_PART_MAIN);
    lv_obj_set_style_border_width(hold_bar, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(hold_bar, 3, LV_PART_MAIN);
    // Indicator (fill) — Catppuccin "red" so "powering off" reads as warning
    lv_obj_set_style_bg_color(hold_bar, lv_color_hex(0xF38BA8), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(hold_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(hold_bar, 3, LV_PART_INDICATOR);

    hold_hint = lv_label_create(hold_box);
    lv_label_set_text(hold_hint, "Release to cancel");
    // Use 14 (already shipped) instead of 12 — Montserrat 12 isn't enabled
    // in CONFIG_LV_FONT_MONTSERRAT_*, and the few KB to add it isn't worth
    // the flash for a single hint line.
    lv_obj_set_style_text_font(hold_hint, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(hold_hint, lv_color_hex(0x9399B2), LV_PART_MAIN);
    lv_obj_align(hold_hint, LV_ALIGN_BOTTOM_MID, 0, -4);

    // ── Pre-build alternate screens ─────────────────────────
    // Build the upload + pair screens now, while WiFi is still idle and
    // free internal heap is at maximum. Lazy-building these on first
    // use crashed Jake's device mid-upload — by then WiFi + mbedtls had
    // eaten enough RAM that lv_obj_create(NULL) returned NULL and LVGL's
    // init path dereferenced it. Both builders are idempotent.
    build_upload_screen();
    build_pair_screen();

    // Anchor lv_scr_load back to the main screen now — the screen-create
    // calls above implicitly load whichever screen was created last as
    // the active one, which would show a black/blank upload UI until the
    // first dashboard repaint catches up.
    if (s_main_scr) lv_scr_load(s_main_scr);
}

// ============================================================
// UI REFRESH — called every 500ms from display_task loop
// ============================================================
void ui_refresh(void)
{
    system_state_t state;
    gps_data_t     gps;

    // Snapshot shared state under mutexes
    if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        memcpy(&state, &g_system_state, sizeof(system_state_t));
        xSemaphoreGive(g_state_mutex);
    } else return;

    // If the logical screen isn't SCREEN_UPLOAD but the LVGL active screen is
    // still the upload screen (because upload_task previously loaded it),
    // switch back to the main screen so panel visibility on the main screen
    // is what the user sees. Without this, after an upload completes the user
    // is stuck on the upload screen until the next upload.
    if ((device_screen_t)state.current_screen != SCREEN_UPLOAD
        && s_upload_scr != NULL && lv_scr_act() == s_upload_scr
        && s_main_scr != NULL) {
        lv_scr_load(s_main_scr);
    }

    // Same handling for the pair prompt: leave the BLE pair screen up while
    // current_screen == SCREEN_BLE_PAIR; restore main when it changes back.
    if ((device_screen_t)state.current_screen != SCREEN_BLE_PAIR
        && s_pair_scr != NULL && lv_scr_act() == s_pair_scr
        && s_main_scr != NULL) {
        lv_scr_load(s_main_scr);
    }

    // While the pair prompt is up, skip the regular dashboard refresh.
    if ((device_screen_t)state.current_screen == SCREEN_BLE_PAIR) {
        return;
    }

    if (xSemaphoreTake(g_gps_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        memcpy(&gps, &g_gps_data, sizeof(gps_data_t));
        xSemaphoreGive(g_gps_mutex);
    }

    char buf[80];
    EventBits_t evts = xEventGroupGetBits(g_events);

    // ── Header: mode + colour ──────────────────────────────
    if (state.setup_mode) {
        lv_label_set_text(lbl_mode, LV_SYMBOL_SETTINGS "  SETUP");
        lv_obj_set_style_bg_color(header_bar, lv_color_hex(0x2D1A5C), LV_PART_MAIN);
    } else if ((device_screen_t)state.current_screen == SCREEN_NAV) {
        lv_label_set_text(lbl_mode, LV_SYMBOL_GPS "  NAV");
        lv_obj_set_style_bg_color(header_bar,
            (evts & EVT_GPS_FIX) ? lv_color_hex(0x1A4A5C) : lv_color_hex(0x3A2800),
            LV_PART_MAIN);
    } else {
        lv_label_set_text(lbl_mode, LV_SYMBOL_WIFI "  SCANNING");
        lv_obj_set_style_bg_color(header_bar,
            (evts & EVT_GPS_FIX) ? lv_color_hex(0x1A5C2A) : lv_color_hex(0x3A2800),
            LV_PART_MAIN);
    }

    // ── Mode-specific panel swap ────────────────────────────
    device_screen_t scr_state = (device_screen_t)state.current_screen;
    bool show_scan  = (scr_state == SCREEN_SCAN);
    bool show_nav   = (scr_state == SCREEN_NAV);
    bool show_menu  = (scr_state == SCREEN_MENU_SETUP
                    || scr_state == SCREEN_MENU_UPLOAD
                    || scr_state == SCREEN_MENU_POWEROFF);
    bool show_setup = (scr_state == SCREEN_SETUP_ACTIVE);

    if (show_nav) {
        // Units default to imperial (mph, ft); user can opt into metric via web UI.
        // File-scope static so we don't put a 2.5 KB wardrive_config_t on the
        // display_task stack every refresh (would overflow under LVGL pressure).
        static wardrive_config_t s_units_cfg;
        nvs_config_get(&s_units_cfg);
        bool metric = s_units_cfg.use_metric;

        if (evts & EVT_GPS_FIX) {
            float speed_disp = metric ? gps.speed_kmh : (gps.speed_kmh * 0.621371f);
            const char *speed_unit = metric ? "km/h" : "mph";
            float stop_threshold = metric ? 1.6f : 1.0f;  // ~1 mph either way
            if (speed_disp < stop_threshold) {
                lv_label_set_text(nav_speed_val, "stopped");
            } else {
                snprintf(buf, sizeof(buf), "%.1f %s", speed_disp, speed_unit);
                lv_label_set_text(nav_speed_val, buf);
            }
            float alt_disp = metric ? gps.altitude_m : (gps.altitude_m * 3.28084f);
            const char *alt_unit = metric ? "m" : "ft";
            snprintf(buf, sizeof(buf), "%.0f %s", alt_disp, alt_unit);
            lv_label_set_text(nav_alt_val, buf);

            // Heading — degree value + cardinal letter
            // Heading — bearing + cardinal letter. ASCII 'deg' instead of
            // the U+00B0 degree sign because Montserrat's bundled glyph
            // range stops at 0x7F (the degree sign renders as a .notdef
            // block).
            const char *dir = "-";
            float h = gps.course_deg;
            if (h >= 337.5f || h < 22.5f)       dir = "N";
            else if (h < 67.5f)                  dir = "NE";
            else if (h < 112.5f)                 dir = "E";
            else if (h < 157.5f)                 dir = "SE";
            else if (h < 202.5f)                 dir = "S";
            else if (h < 247.5f)                 dir = "SW";
            else if (h < 292.5f)                 dir = "W";
            else                                  dir = "NW";
            snprintf(buf, sizeof(buf), "%.0f deg %s", h, dir);
            lv_label_set_text(nav_head_val, buf);

            snprintf(buf, sizeof(buf), LV_SYMBOL_GPS "  %.5f, %.5f  HDOP %.1f",
                     gps.lat, gps.lon, gps.hdop);
            lv_label_set_text(nav_coord_val, buf);
            lv_obj_set_style_text_color(nav_coord_val, lv_color_hex(0x89DCEB), LV_PART_MAIN);
        } else {
            // Pre-fix placeholders — ASCII only. Em-dashes (U+2014) and
            // ellipsis (U+2026) render as .notdef boxes on the LCD, so
            // we use plain hyphens and three literal dots instead. Speed
            // gets a real explanation instead of the cryptic 'no fix'.
            lv_label_set_text(nav_speed_val, "No satellites connected");
            lv_label_set_text(nav_alt_val,   "-");
            lv_label_set_text(nav_head_val,  "-");
            lv_label_set_text(nav_coord_val, LV_SYMBOL_GPS "  Waiting for satellites...");
            lv_obj_set_style_text_color(nav_coord_val, lv_color_hex(0x888888), LV_PART_MAIN);
        }

        // TTFF + start-mode indicator. Empty until gps_task latches the
        // first valid fix; afterwards it stays for the rest of the
        // session as a diagnostic ("did the backup cell help?").
        if (state.gps_start_mode != GPS_START_SEARCHING) {
            const char *mode = "?";
            uint32_t color = 0x888888;
            switch (state.gps_start_mode) {
                case GPS_START_HOT:  mode = "HOT";  color = 0x00DD44; break;
                case GPS_START_WARM: mode = "WARM"; color = 0xFFCC00; break;
                case GPS_START_COLD: mode = "COLD"; color = 0xFFA500; break;
            }
            snprintf(buf, sizeof(buf), "TTFF %lus %s",
                     (unsigned long)(state.gps_ttff_ms / 1000), mode);
            lv_label_set_text(nav_ttff_val, buf);
            lv_obj_set_style_text_color(nav_ttff_val, lv_color_hex(color),
                                        LV_PART_MAIN);
        } else {
            lv_label_set_text(nav_ttff_val, "");
        }
    }

    if (show_setup) {
        // Populate AP creds from saved config
        wardrive_config_t cfg;
        nvs_config_get(&cfg);
        const char *qr_ssid = cfg.ap_ssid[0] ? cfg.ap_ssid : "WarDrivingMapper";
        const char *qr_pass = cfg.ap_pass[0] ? cfg.ap_pass : "configure1";
        // The visible labels go through the LCD's ASCII-only font; sanitize
        // typographic codepoints (curly quotes etc) that the user might have
        // pasted into NVS so the label doesn't render as .notdef blocks.
        // The QR payload below keeps the raw bytes — phones scanning it
        // need the literal SSID/password as configured.
        char ssid_safe[WD_MAX_SSID_LEN];
        char pass_safe[WD_MAX_PASS_LEN];
        wd_str_sanitize_for_lcd(ssid_safe, sizeof(ssid_safe), qr_ssid);
        wd_str_sanitize_for_lcd(pass_safe, sizeof(pass_safe), qr_pass);

        // Auto-scale SSID + password font down for longer strings so
        // they don't overflow the 200 px wide right column. The default
        // "WarDrivingMapper" (16 chars) at montserrat_20 was clipping
        // its last letter — proportional fonts mean wide letters like
        // W and M push past the 200 px bound even when the average
        // char width says it should fit. Buckets erring on the side of
        // legibility. Cached so we only call lv_obj_set_style_text_font
        // on actual change — avoids per-refresh allocator churn.
        const lv_font_t *want_ssid_font;
        size_t ssid_len = strlen(ssid_safe);
        if      (ssid_len <=  8) want_ssid_font = &lv_font_montserrat_28;
        else if (ssid_len <= 13) want_ssid_font = &lv_font_montserrat_20;
        else if (ssid_len <= 18) want_ssid_font = &lv_font_montserrat_16;
        else                     want_ssid_font = &lv_font_montserrat_14;

        const lv_font_t *want_pass_font;
        size_t pass_len = strlen(pass_safe);
        if      (pass_len <=  8) want_pass_font = &lv_font_montserrat_28;
        else if (pass_len <= 13) want_pass_font = &lv_font_montserrat_20;
        else if (pass_len <= 18) want_pass_font = &lv_font_montserrat_16;
        else                     want_pass_font = &lv_font_montserrat_14;

        static const lv_font_t *last_ssid_font = NULL;
        static const lv_font_t *last_pass_font = NULL;
        if (want_ssid_font != last_ssid_font) {
            lv_obj_set_style_text_font(setup_ssid_value, want_ssid_font, LV_PART_MAIN);
            last_ssid_font = want_ssid_font;
        }
        if (want_pass_font != last_pass_font) {
            lv_obj_set_style_text_font(setup_pass_value, want_pass_font, LV_PART_MAIN);
            last_pass_font = want_pass_font;
        }

        lv_label_set_text(setup_ssid_value, ssid_safe);
        lv_label_set_text(setup_pass_value, pass_safe);

        // Rebuild the QR only when the encoded string actually changes —
        // re-rendering every refresh wastes cycles and flickers the canvas.
        char qr_payload[128];
        snprintf(qr_payload, sizeof(qr_payload),
                 "WIFI:T:WPA;S:%s;P:%s;;", qr_ssid, qr_pass);
        if (strcmp(qr_payload, setup_qr_last) != 0) {
            lv_qrcode_update(setup_qr, qr_payload, strlen(qr_payload));
            strncpy(setup_qr_last, qr_payload, sizeof(setup_qr_last) - 1);
        }
    }

    if (show_menu) {
        if (scr_state == SCREEN_MENU_SETUP) {
            lv_label_set_text(menu_icon,  LV_SYMBOL_WIFI);
            lv_obj_set_style_text_color(menu_icon, lv_color_hex(0xCBA6F7), LV_PART_MAIN);
            lv_label_set_text(menu_title, "Wi-Fi Setup");
            lv_label_set_text(menu_hint,  "Hold BOOT to enter   |   short = next");
        } else if (scr_state == SCREEN_MENU_UPLOAD) {
            lv_label_set_text(menu_icon,  LV_SYMBOL_UPLOAD);
            lv_obj_set_style_text_color(menu_icon, lv_color_hex(0x89DCEB), LV_PART_MAIN);
            lv_label_set_text(menu_title, "Force Upload");
            lv_label_set_text(menu_hint,  "Hold BOOT to start   |   short = next");
        } else {
            lv_label_set_text(menu_icon,  LV_SYMBOL_POWER);
            lv_obj_set_style_text_color(menu_icon, lv_color_hex(0xF38BA8), LV_PART_MAIN);
            lv_label_set_text(menu_title, "Power Off");
            // Idle prompt; the lv_bar modal (shown via the
            // hold-to-power-off block lower in ui_refresh) takes over
            // visually as soon as the user starts holding BOOT.
            lv_label_set_text(menu_hint,
                "Press and hold BOOT to power off");
        }
    }

    if (show_setup) lv_obj_clear_flag(setup_panel, LV_OBJ_FLAG_HIDDEN);
    else            lv_obj_add_flag(setup_panel,   LV_OBJ_FLAG_HIDDEN);
    if (show_menu)  lv_obj_clear_flag(menu_panel,  LV_OBJ_FLAG_HIDDEN);
    else            lv_obj_add_flag(menu_panel,    LV_OBJ_FLAG_HIDDEN);
    if (show_nav)   lv_obj_clear_flag(nav_panel,   LV_OBJ_FLAG_HIDDEN);
    else            lv_obj_add_flag(nav_panel,     LV_OBJ_FLAG_HIDDEN);

    if (show_scan) {
        lv_obj_clear_flag(row_gps,        LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_coords,     LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_ssid_count, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_scan_info,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_sd_status,  LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(row_gps,        LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_coords,     LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_ssid_count, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_scan_info,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_sd_status,  LV_OBJ_FLAG_HIDDEN);
    }
    // ── Battery ───────────────────────────────────────────
    // Battery readout. When plugged in we surface "USB" alongside the
    // percent — earlier visual of "⚡ USB" without a percent was preferred
    // by the user but lost the live state; this keeps both.
    if (state.is_charging) {
        // Charging / USB powered — bolt plus the live percentage.
        snprintf(buf, sizeof(buf), LV_SYMBOL_CHARGE " USB %.0f%%", state.battery_pct);
    } else {
        // Pick battery icon by percentage bucket
        const char *icon = LV_SYMBOL_BATTERY_EMPTY;
        if      (state.battery_pct >= 87.5f) icon = LV_SYMBOL_BATTERY_FULL;
        else if (state.battery_pct >= 62.5f) icon = LV_SYMBOL_BATTERY_3;
        else if (state.battery_pct >= 37.5f) icon = LV_SYMBOL_BATTERY_2;
        else if (state.battery_pct >= 12.5f) icon = LV_SYMBOL_BATTERY_1;
        snprintf(buf, sizeof(buf), "%s %.0f%%", icon, state.battery_pct);
    }
    lv_label_set_text(lbl_battery, buf);
    lv_obj_set_style_text_color(lbl_battery,
        state.is_charging      ? lv_color_hex(0x00DDFF) :
        (evts & EVT_LOW_BATTERY) ? lv_color_hex(0xFF4444) :
                                   lv_color_hex(0xCCCCCC),
        LV_PART_MAIN);

    // Trusted SSID indicator — green home icon when in range of a trusted net.
    if (state.trusted_visible) lv_obj_clear_flag(lbl_trusted, LV_OBJ_FLAG_HIDDEN);
    else                       lv_obj_add_flag(lbl_trusted,   LV_OBJ_FLAG_HIDDEN);

    // BLE indicator — solid while a central (the iOS app) is connected,
    // blinks at 1 Hz while only advertising so the user can see the
    // device is discoverable. Hidden entirely when BLE is disabled in NVS.
    //
    // Blink is driven off the wall clock (esp_timer), not a per-refresh
    // toggle, so the blink rate stays at 1 Hz regardless of how often
    // ui_refresh runs. (At the previous 500 ms refresh, per-call toggle
    // happened to match 1 Hz; bumping refresh to 150 ms made the icon
    // strobe ~7×/s, which the user noticed and disliked.)
    if (!state.ble_enabled) {
        lv_obj_add_flag(lbl_ble, LV_OBJ_FLAG_HIDDEN);
    } else if (state.ble_connected) {
        lv_obj_clear_flag(lbl_ble, LV_OBJ_FLAG_HIDDEN);
    } else {
        bool blink_on = ((esp_timer_get_time() / 500000) % 2) == 0;
        if (blink_on) lv_obj_clear_flag(lbl_ble, LV_OBJ_FLAG_HIDDEN);
        else          lv_obj_add_flag(lbl_ble,   LV_OBJ_FLAG_HIDDEN);
    }

    // ── GPS ───────────────────────────────────────────────
    static uint8_t dot_tick = 0;
    dot_tick = (dot_tick + 1) & 3;
    const char *dots = (dot_tick == 0) ? "   " :
                       (dot_tick == 1) ? ".  " :
                       (dot_tick == 2) ? ".. " : "...";

    if (evts & EVT_GPS_FIX) {
        // Append the latched start-mode tag once gps_task records a TTFF.
        // Keeps the at-a-glance answer to "is my XH414 backup cell helping?"
        // on the most-visible status line of the most-visible screen.
        const char *mode = NULL;
        switch (state.gps_start_mode) {
            case GPS_START_HOT:  mode = "HOT";  break;
            case GPS_START_WARM: mode = "WARM"; break;
            case GPS_START_COLD: mode = "COLD"; break;
            default:             mode = NULL;   break;
        }
        if (mode && state.gps_ttff_ms > 0) {
            snprintf(buf, sizeof(buf), LV_SYMBOL_GPS " FIX %.1f  %s %lus",
                     gps.hdop, mode,
                     (unsigned long)(state.gps_ttff_ms / 1000));
        } else {
            snprintf(buf, sizeof(buf), LV_SYMBOL_GPS " FIX  HDOP %.1f", gps.hdop);
        }
        lv_label_set_text(lbl_gps_status, buf);
        lv_obj_set_style_text_color(lbl_gps_status,
            lv_color_hex(0x00DD44), LV_PART_MAIN);

        snprintf(buf, sizeof(buf), "%d sats", gps.satellites);
        lv_label_set_text(lbl_gps_sats, buf);
        lv_obj_set_style_text_color(lbl_gps_sats, lv_color_hex(0x00DD44), LV_PART_MAIN);
        lv_obj_align(lbl_gps_sats, LV_ALIGN_RIGHT_MID, -8, 0);

        snprintf(buf, sizeof(buf), "%.5f\xC2\xB0%c   %.5f\xC2\xB0%c",
                 fabs(gps.lat), gps.lat >= 0 ? 'N' : 'S',
                 fabs(gps.lon), gps.lon >= 0 ? 'E' : 'W');
        lv_label_set_text(lbl_coords, buf);
        lv_obj_set_style_text_color(lbl_coords, lv_color_hex(0x00CCFF), LV_PART_MAIN);
    } else {
        snprintf(buf, sizeof(buf), LV_SYMBOL_GPS " Searching%s", dots);
        lv_label_set_text(lbl_gps_status, buf);
        lv_obj_set_style_text_color(lbl_gps_status,
            lv_color_hex(0xFFA500), LV_PART_MAIN);

        snprintf(buf, sizeof(buf), "%d sats", gps.satellites);
        lv_label_set_text(lbl_gps_sats, buf);
        lv_obj_set_style_text_color(lbl_gps_sats, lv_color_hex(0x888888), LV_PART_MAIN);
        lv_obj_align(lbl_gps_sats, LV_ALIGN_RIGHT_MID, -8, 0);

        lv_label_set_text(lbl_coords, LV_SYMBOL_WARNING "  No GPS fix");
        lv_obj_set_style_text_color(lbl_coords, lv_color_hex(0x888888), LV_PART_MAIN);
    }

    // ── SSID count ───────────────────────────────────────
    snprintf(buf, sizeof(buf), LV_SYMBOL_LIST "  %lu unique networks logged",
             (unsigned long)state.ssids_logged);
    lv_label_set_text(lbl_ssid_count, buf);

    // ── Scan info ────────────────────────────────────────
    snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI "  Scan #%lu  |  %d APs visible",
             (unsigned long)state.scan_count, state.last_ap_count);
    lv_label_set_text(lbl_scan_info, buf);

    // ── Storage status (onboard LittleFS) ─────────────────
    if (state.sd_ready) {
        const float mb = 1024.0f * 1024.0f;
        float total_mb = (float)state.sd_total_bytes / mb;
        float used_mb  = (float)(state.sd_total_bytes - state.sd_free_bytes) / mb;
        snprintf(buf, sizeof(buf), LV_SYMBOL_SAVE "  %.2f / %.1f MB used",
                 used_mb, total_mb);
        lv_label_set_text(lbl_sd_status, buf);
        lv_obj_set_style_text_color(lbl_sd_status,
            lv_color_hex(0x00BB44), LV_PART_MAIN);
    } else if (state.sd_mount_err != 0) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_WARNING "  Storage ERR 0x%lx",
                 (unsigned long)state.sd_mount_err);
        lv_label_set_text(lbl_sd_status, buf);
        lv_obj_set_style_text_color(lbl_sd_status,
            lv_color_hex(0xFF4444), LV_PART_MAIN);
    } else {
        lv_label_set_text(lbl_sd_status,
            LV_SYMBOL_WARNING "  Storage offline");
        lv_obj_set_style_text_color(lbl_sd_status,
            lv_color_hex(0xFF4444), LV_PART_MAIN);
    }

    // ── Alert bar — only for actionable conditions in scan mode ──
    if (!state.setup_mode && (evts & EVT_LOW_BATTERY)) {
        lv_label_set_text(lbl_alert, LV_SYMBOL_WARNING "  Low battery!");
        lv_obj_clear_flag(alert_bar, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(alert_bar, LV_OBJ_FLAG_HIDDEN);
    }

    // ── Uptime + version ─────────────────────────────────
    // Footer reads "12:34:56  v0.4.0-1-gc4ee3c6". WDM_FW_VERSION
    // already includes the leading "v" from `git describe`, so the
    // format string is plain "%s" — adding "v%s" produces a literal
    // "vv" that the user noticed cropped both edges of the 320 px
    // footer. The compile-time date/time stamp used to live here too
    // but it pushed the line past the screen width; it still appears
    // on the Wi-Fi Setup screen where there's room.
    uint64_t s = esp_timer_get_time() / 1000000;
    snprintf(buf, sizeof(buf),
             "%02llu:%02llu:%02llu  %s",
             s / 3600, (s % 3600) / 60, s % 60,
             WDM_FW_VERSION);
    lv_label_set_text(lbl_uptime, buf);

    // ── Hold-action progress modal ──────────────────────────
    // Show whenever (a) one of the three menu screens is active AND
    // (b) BOOT is currently held. The bar range, title, and fill color
    // adapt to the screen so the user always knows which action is about
    // to fire. The display_task loop bumps its refresh rate while the
    // BOOT button is held, so the bar fill is smooth instead of chunking
    // at the 150 ms idle cadence.
    {
        // Grace period before the modal pops. A typical tap-to-cycle press
        // is ~50-150 ms; without this the modal would flash on every short
        // press, looking like the device is misreading the input. 250 ms
        // is well past tap latency but still feels instant when the user
        // is deliberately pressing-and-holding (the bar appears already
        // ~25 % filled on a SETUP / UPLOAD hold, which is fine — it
        // accurately reflects how much of the 1 s threshold has elapsed).
        static const uint32_t MODAL_GRACE_MS = 250;

        // Track which screen we last styled the modal for. Updating the
        // local style (title text, bar range, bar fill color, border color)
        // every refresh tick fragments LVGL's heap fast — each lv_obj_set_style_*
        // call allocates/updates a style node, and at 30 ms refresh that's
        // ~100 churn cycles across a 3 s hold. We only need to re-style when
        // the active menu actually changes; the bar VALUE still updates each
        // tick (cheap, no allocation).
        static device_screen_t last_styled_screen = -1;
        static bool was_visible = false;

        uint32_t held_ms = button_get_held_ms();
        bool is_menu = (scr_state == SCREEN_MENU_SETUP
                     || scr_state == SCREEN_MENU_UPLOAD
                     || scr_state == SCREEN_MENU_POWEROFF);
        bool show_overlay = is_menu && (held_ms >= MODAL_GRACE_MS);

        if (show_overlay) {
            uint32_t full_ms = (scr_state == SCREEN_MENU_POWEROFF)
                ? HOLD_POWEROFF_MS : LONG_MS;

            // Re-style only on a screen change. Idempotent on the bar VALUE
            // path so smooth fill still works.
            if (scr_state != last_styled_screen) {
                const char *title;
                uint32_t    fill_hex;

                if (scr_state == SCREEN_MENU_SETUP) {
                    title    = "Hold to enter Wi-Fi Setup";
                    fill_hex = 0xCBA6F7;   // Catppuccin "mauve"
                } else if (scr_state == SCREEN_MENU_UPLOAD) {
                    title    = "Hold to force upload";
                    fill_hex = 0x89DCEB;   // Catppuccin "sky"
                } else {  // SCREEN_MENU_POWEROFF
                    title    = "Hold to power off";
                    fill_hex = 0xF38BA8;   // Catppuccin "red" — danger
                }

                lv_label_set_text(hold_title, title);
                lv_bar_set_range(hold_bar, 0, (int32_t)full_ms);
                lv_obj_set_style_bg_color(hold_bar, lv_color_hex(fill_hex),
                                          LV_PART_INDICATOR);
                lv_obj_set_style_border_color(hold_box, lv_color_hex(fill_hex),
                                              LV_PART_MAIN);
                last_styled_screen = scr_state;
            }

            uint32_t v = (held_ms > full_ms) ? full_ms : held_ms;
            lv_bar_set_value(hold_bar, (int32_t)v, LV_ANIM_OFF);

            if (!was_visible) {
                lv_obj_clear_flag(hold_overlay, LV_OBJ_FLAG_HIDDEN);
                was_visible = true;
            }
        } else if (was_visible) {
            lv_obj_add_flag(hold_overlay, LV_OBJ_FLAG_HIDDEN);
            lv_bar_set_value(hold_bar, 0, LV_ANIM_OFF);
            was_visible = false;
        }
    }
}

// ============================================================
// SCREEN_UPLOAD — built once, updated by display_show_upload_progress
// ============================================================

static void build_upload_screen(void)
{
    // Idempotent — create_ui() builds the screen at boot, but the legacy
    // lazy callers (display_set_upload_resume, display_show_upload_progress,
    // display_show_upload_result) still invoke this on the way through.
    // Skip if already built.
    if (s_upload_scr) return;

    ESP_LOGI(TAG, "build_upload_screen: starting  free heap: internal=%u psram=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    s_upload_scr = lv_obj_create(NULL);
    if (!s_upload_scr) {
        // Allocation failure path. Previous behaviour was to keep going
        // and deref NULL inside the next LVGL call, which Guru Meditation'd.
        // Log the heap state and bail gracefully instead — the caller
        // (display_set_upload_resume) holds the lvgl mutex and will release
        // it; no upload UI for this cycle, but the device stays alive.
        ESP_LOGE(TAG,
                 "build_upload_screen: lv_obj_create returned NULL "
                 "(free internal=%u psram=%u). Upload UI unavailable this cycle.",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        return;
    }
    lv_obj_set_style_bg_color(s_upload_scr, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_upload_scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_upload_scr, LV_OBJ_FLAG_SCROLLABLE);

    // Layout is tight: panel is 172 px tall, so widgets stack:
    //   title 4–22, ssid 24–38, arc 42–122, count 128–142, host 152–166.
    // A previous version also had a linear bar (158–172) and a batch label
    // (off-screen at 176); both were dropped after they collided with host.

    s_upload_title = lv_label_create(s_upload_scr);
    lv_obj_set_width(s_upload_title, 220);
    lv_label_set_long_mode(s_upload_title, LV_LABEL_LONG_CLIP);
    lv_label_set_text(s_upload_title, LV_SYMBOL_UPLOAD " Uploading");
    lv_obj_align(s_upload_title, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_style_text_color(s_upload_title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_upload_title, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_upload_title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    // Fixed-width labels prevent "(c Pixel g)" bleed-through when text
    // shrinks: with auto-size, LVGL's invalidation of the OLD bounding box
    // can lose to display_task's render cycle under mbedtls CPU pressure and
    // leave the wider previous text's outer pixels on screen. A stable box
    // means the whole label area is repainted every update.
    s_upload_ssid = lv_label_create(s_upload_scr);
    lv_obj_set_width(s_upload_ssid, 280);
    lv_label_set_long_mode(s_upload_ssid, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_upload_ssid, "");
    lv_obj_align(s_upload_ssid, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_text_color(s_upload_ssid, lv_color_hex(0xB4B4B4), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_upload_ssid, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_upload_ssid, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    // Circular progress indicator. Originally an animated spinner with a
    // fixed 30% fill; switched to a true 0–100 progress arc so the user
    // can see how far the upload has gotten at a glance (the spinner
    // version also stalled visibly under mbedtls CPU pressure).
    s_upload_arc = lv_arc_create(s_upload_scr);
    lv_obj_set_size(s_upload_arc, 80, 80);
    lv_obj_align(s_upload_arc, LV_ALIGN_TOP_MID, 0, 42);
    lv_arc_set_bg_angles(s_upload_arc, 0, 360);
    lv_arc_set_rotation(s_upload_arc, 270);   // start fill at 12 o'clock
    lv_arc_set_range(s_upload_arc, 0, 100);
    lv_arc_set_value(s_upload_arc, 0);
    lv_obj_remove_style(s_upload_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_upload_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_upload_arc, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_upload_arc, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_upload_arc, lv_color_hex(0x00C8FF), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_upload_arc, lv_color_hex(0x222222), LV_PART_MAIN);

    s_upload_pct = lv_label_create(s_upload_scr);
    lv_obj_set_width(s_upload_pct, 60);
    lv_label_set_long_mode(s_upload_pct, LV_LABEL_LONG_CLIP);
    lv_label_set_text(s_upload_pct, "0%");
    lv_obj_align_to(s_upload_pct, s_upload_arc, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_color(s_upload_pct, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_upload_pct, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_upload_pct, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    s_upload_count = lv_label_create(s_upload_scr);
    lv_obj_set_width(s_upload_count, 280);
    lv_label_set_long_mode(s_upload_count, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_upload_count, "0 / 0 records");
    lv_obj_align(s_upload_count, LV_ALIGN_TOP_MID, 0, 128);
    lv_obj_set_style_text_color(s_upload_count, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_upload_count, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_upload_count, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    s_upload_host = lv_label_create(s_upload_scr);
    lv_obj_set_width(s_upload_host, 280);
    lv_label_set_long_mode(s_upload_host, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_upload_host, "");
    lv_obj_align(s_upload_host, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_text_color(s_upload_host, lv_color_hex(0x787878), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_upload_host, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_upload_host, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    s_upload_result = lv_label_create(s_upload_scr);
    lv_label_set_text(s_upload_result, "");
    lv_obj_align(s_upload_result, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_color(s_upload_result, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_upload_result, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_add_flag(s_upload_result, LV_OBJ_FLAG_HIDDEN);
}

void display_show_upload_progress(uint32_t records_done, uint32_t records_total,
                                  uint32_t batch_idx, uint32_t batch_total,
                                  const char *trusted_ssid,
                                  const char *endpoint_host)
{
    // Stash in shared state regardless of LVGL availability so other readers
    // (and ui_refresh) see the latest snapshot.
    if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        g_system_state.upload_done        = records_done;
        g_system_state.upload_total       = records_total;
        g_system_state.upload_batch_idx   = batch_idx;
        g_system_state.upload_batch_total = batch_total;
        if (trusted_ssid) {
            strncpy(g_system_state.upload_ssid, trusted_ssid,
                    sizeof(g_system_state.upload_ssid) - 1);
            g_system_state.upload_ssid[sizeof(g_system_state.upload_ssid) - 1] = '\0';
        }
        xSemaphoreGive(g_state_mutex);
    }

    // Serialise LVGL updates with display_task's lv_refr_now. Without this,
    // a shorter replacement label (e.g. "Pixel" overwriting "(connecting)")
    // leaves old pixels visible outside the new bounding box — the user sees
    // "(c Pixel g)" because LVGL's dirty-region tracking gets clobbered by
    // the concurrent renderer.
    if (!lvgl_mutex) return;
    // Bail if display setup hasn't finished yet. upload_task fires on
    // boot whenever there's a leftover wardrive.uploading.jsonl, and
    // can beat create_ui() to the punch — without this guard the
    // helper would block on the mutex (display_task holds it during
    // setup) but then race into a half-constructed LVGL state once
    // the mutex is released. Skipping the UI update is fine — the
    // upload itself runs independently.
    if (!s_display_ready) return;
    if (xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;

    if (!s_upload_scr) build_upload_screen();
    if (!s_upload_scr) {
        // Allocation failed in build_upload_screen — keep the device
        // alive by skipping the upload UI rather than dereferencing.
        xSemaphoreGive(lvgl_mutex);
        return;
    }

    char buf[64];
    snprintf(buf, sizeof(buf), "%lu / %lu records",
             (unsigned long)records_done, (unsigned long)records_total);
    lv_label_set_text(s_upload_count, buf);

    int pct = records_total ? (int)((records_done * 100ULL) / records_total) : 0;
    lv_arc_set_value(s_upload_arc, pct);
    snprintf(buf, sizeof(buf), "%d%%", pct);
    lv_label_set_text(s_upload_pct, buf);

    // Both strings can be user-supplied (trusted SSID from NVS, endpoint
    // host parsed from user-configured api_url). Sanitize typographic
    // codepoints to ASCII so they don't render as .notdef glyphs.
    if (trusted_ssid)  {
        char ssid_safe[WD_MAX_SSID_LEN];
        wd_str_sanitize_for_lcd(ssid_safe, sizeof(ssid_safe), trusted_ssid);
        lv_label_set_text(s_upload_ssid, ssid_safe);
    }
    if (endpoint_host) {
        char host_safe[WD_MAX_URL_LEN];
        wd_str_sanitize_for_lcd(host_safe, sizeof(host_safe), endpoint_host);
        lv_label_set_text(s_upload_host, host_safe);
    }

    lv_obj_add_flag(s_upload_result, LV_OBJ_FLAG_HIDDEN);
    lv_scr_load(s_upload_scr);

    xSemaphoreGive(lvgl_mutex);
}

void display_show_upload_result(bool ok, const char *message)
{
    if (!lvgl_mutex) return;
    // Bail if display setup hasn't finished yet. upload_task fires on
    // boot whenever there's a leftover wardrive.uploading.jsonl, and
    // can beat create_ui() to the punch — without this guard the
    // helper would block on the mutex (display_task holds it during
    // setup) but then race into a half-constructed LVGL state once
    // the mutex is released. Skipping the UI update is fine — the
    // upload itself runs independently.
    if (!s_display_ready) return;
    if (xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;

    if (!s_upload_scr) build_upload_screen();
    if (!s_upload_scr) {
        xSemaphoreGive(lvgl_mutex);
        return;
    }

    lv_obj_clear_flag(s_upload_result, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_upload_result,
                      message ? message : (ok ? "Upload complete" : "Upload failed"));
    lv_obj_set_style_text_color(s_upload_result,
        ok ? lv_color_hex(0x00C800) : lv_color_hex(0xDC3C3C), LV_PART_MAIN);
    lv_scr_load(s_upload_scr);

    xSemaphoreGive(lvgl_mutex);
}

void display_set_upload_resume(bool is_resume)
{
    if (!lvgl_mutex) return;
    // Bail if display setup hasn't finished yet. upload_task fires on
    // boot whenever there's a leftover wardrive.uploading.jsonl, and
    // can beat create_ui() to the punch — without this guard the
    // helper would block on the mutex (display_task holds it during
    // setup) but then race into a half-constructed LVGL state once
    // the mutex is released. Skipping the UI update is fine — the
    // upload itself runs independently.
    if (!s_display_ready) return;
    if (xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;

    if (!s_upload_scr) build_upload_screen();
    if (!s_upload_scr) {
        xSemaphoreGive(lvgl_mutex);
        return;
    }
    lv_label_set_text(s_upload_title,
        is_resume ? LV_SYMBOL_UPLOAD " Resuming"
                  : LV_SYMBOL_UPLOAD " Uploading");

    xSemaphoreGive(lvgl_mutex);
}

// ============================================================
// SCREEN_BLE_PAIR — built once, populated via display_show_pair_prompt
// ============================================================
// s_pair_scr declared near top (forward decl). Initialise here.
static lv_obj_t *s_pair_code_label = NULL;
static uint8_t   s_pair_prev_screen = SCREEN_SCAN;

static void build_pair_screen(void)
{
    // Idempotent — see comment in build_upload_screen for the boot-time
    // pre-allocation strategy.
    if (s_pair_scr) return;
    s_pair_scr = lv_obj_create(NULL);
    if (!s_pair_scr) {
        ESP_LOGE(TAG,
                 "build_pair_screen: lv_obj_create returned NULL "
                 "(free internal=%u psram=%u). Pair prompt unavailable.",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        return;
    }
    lv_obj_set_style_bg_color(s_pair_scr, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_pair_scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_pair_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(s_pair_scr, 0, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(s_pair_scr);
    lv_label_set_text(title, LV_SYMBOL_BLUETOOTH "  Pair with iPhone?");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    s_pair_code_label = lv_label_create(s_pair_scr);
    lv_label_set_text(s_pair_code_label, "------");
    lv_obj_set_style_text_font(s_pair_code_label, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_pair_code_label, lv_color_hex(0x10B981), LV_PART_MAIN);
    lv_obj_align(s_pair_code_label, LV_ALIGN_CENTER, 0, -16);

    lv_obj_t *prompt = lv_label_create(s_pair_scr);
    lv_label_set_text(prompt, "Same code on phone?");
    lv_obj_set_style_text_color(prompt, lv_color_hex(0xA1A1AA), LV_PART_MAIN);
    lv_obj_set_style_text_font(prompt, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(prompt, LV_ALIGN_CENTER, 0, 22);

    lv_obj_t *yes = lv_label_create(s_pair_scr);
    lv_label_set_text(yes, "Hold BOOT = Yes");
    lv_obj_set_style_text_color(yes, lv_color_hex(0x10B981), LV_PART_MAIN);
    lv_obj_set_style_text_font(yes, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(yes, LV_ALIGN_BOTTOM_MID, 0, -22);

    lv_obj_t *no = lv_label_create(s_pair_scr);
    lv_label_set_text(no, "Tap  BOOT = No");
    lv_obj_set_style_text_color(no, lv_color_hex(0xDC3C3C), LV_PART_MAIN);
    lv_obj_set_style_text_font(no, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(no, LV_ALIGN_BOTTOM_MID, 0, -4);
}

void display_show_pair_prompt(uint32_t code)
{
    // Stash the requested screen in shared state so ui_refresh leaves it alone.
    if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        s_pair_prev_screen = g_system_state.current_screen;
        g_system_state.current_screen = SCREEN_BLE_PAIR;
        xSemaphoreGive(g_state_mutex);
    }

    if (!lvgl_mutex) return;
    // Bail if display setup hasn't finished yet. upload_task fires on
    // boot whenever there's a leftover wardrive.uploading.jsonl, and
    // can beat create_ui() to the punch — without this guard the
    // helper would block on the mutex (display_task holds it during
    // setup) but then race into a half-constructed LVGL state once
    // the mutex is released. Skipping the UI update is fine — the
    // upload itself runs independently.
    if (!s_display_ready) return;
    if (xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;

    if (!s_pair_scr) build_pair_screen();
    if (!s_pair_scr) {
        xSemaphoreGive(lvgl_mutex);
        return;
    }

    char buf[16];
    snprintf(buf, sizeof(buf), "%06lu", (unsigned long)code);
    lv_label_set_text(s_pair_code_label, buf);
    lv_scr_load(s_pair_scr);

    xSemaphoreGive(lvgl_mutex);
}

void display_hide_pair_prompt(void)
{
    if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (g_system_state.current_screen == SCREEN_BLE_PAIR) {
            g_system_state.current_screen = s_pair_prev_screen;
        }
        xSemaphoreGive(g_state_mutex);
    }
    // ui_refresh will load the main screen on its next tick.
}

// ============================================================
// DISPLAY TASK
// ============================================================
