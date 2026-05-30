#pragma once
#include <stdint.h>

/* LVGL tick source for the Linux host/Pi build.
 * Referenced by lv_conf.h via LV_TICK_CUSTOM_SYS_TIME_EXPR.
 * Returns milliseconds from a monotonic clock. Defined in platform_stub.c. */
uint32_t wdm_tick_ms(void);
