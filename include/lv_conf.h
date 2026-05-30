/**
 * lv_conf.h — LVGL 8.4 configuration for the WarDrivingMapper Pi build.
 *
 * Ported from the ESP32 firmware's main/lv_conf.h so the screen code in
 * src/ui/screens.c renders identically. Differences from the firmware:
 *   - LV_COLOR_16_SWAP 0   (host/fbdev is native RGB565; no SPI byte-swap)
 *   - LV_TICK_CUSTOM uses a host clock (wdm_tick.h) instead of esp_timer
 *   - resolution maxes are generous; the real size comes from the display
 *     driver registered in main.c
 */
#if 1  /* Set to 1 to enable content */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/* Colour depth — RGB565, same as the ST7789 / ILI9486 panels */
#define LV_COLOR_DEPTH 16

/* No SPI byte-swap on the host/fbdev path (the firmware needs 1 for its SPI bus) */
#define LV_COLOR_16_SWAP 0

/* Resolution maxes — advisory in v8; real size set by the display driver */
#define LV_HOR_RES_MAX  480
#define LV_VER_RES_MAX  480

/* Memory — plain libc malloc on Linux */
#define LV_MEM_CUSTOM 1
#if LV_MEM_CUSTOM
#  define LV_MEM_CUSTOM_INCLUDE <stdlib.h>
#  define LV_MEM_CUSTOM_ALLOC   malloc
#  define LV_MEM_CUSTOM_FREE    free
#  define LV_MEM_CUSTOM_REALLOC realloc
#endif

#define LV_DISP_DEF_REFR_PERIOD  16  /* ~60fps */
#define LV_INDEV_DEF_READ_PERIOD 30

/* Logging */
#define LV_USE_LOG 0

/* Fonts — must match what the screen code references (14/16/20/28) */
#define LV_FONT_MONTSERRAT_8  1
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_DEFAULT       &lv_font_montserrat_14

/* Widgets we use */
#define LV_USE_LABEL    1
#define LV_USE_BAR      1
#define LV_USE_BTN      1
#define LV_USE_LINE     1
#define LV_USE_ARC      1
#define LV_USE_SPINNER  1

/* Disable unused widgets */
#define LV_USE_CALENDAR  0
#define LV_USE_CHART     0
#define LV_USE_KEYBOARD  0
#define LV_USE_METER     0
#define LV_USE_MSGBOX    0
#define LV_USE_TABVIEW   0
#define LV_USE_TEXTAREA  1    /* required transitively by some widgets */
#define LV_USE_TABLE     0
#define LV_USE_TILEVIEW  0
#define LV_USE_WIN       0
#define LV_USE_SPAN      0

/* Extra modules */
#define LV_USE_QRCODE    1    /* SETUP screen Wi-Fi join QR */

/* Themes */
#define LV_USE_THEME_DEFAULT    1
#define LV_THEME_DEFAULT_DARK   1

/* Tick source — host monotonic clock (see src/platform/wdm_tick.h) */
#define LV_TICK_CUSTOM 1
#if LV_TICK_CUSTOM
#  define LV_TICK_CUSTOM_INCLUDE  "wdm_tick.h"
#  define LV_TICK_CUSTOM_SYS_TIME_EXPR (wdm_tick_ms())
#endif

#define LV_SPRINTF_CUSTOM 0
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR  0

#endif /* LV_CONF_H */
#endif /* Enable content */
