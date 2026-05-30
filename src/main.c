/**
 * main.c — headless render harness for the WarDrivingMapper Pi UI.
 *
 * Registers an in-memory LVGL display (no SDL / display server), builds the
 * ported dashboard, refreshes it from the stub state, then dumps the frame
 * to a PNG. Runs identically on macOS and the Pi — the same screens.c that
 * will later drive the real Elecrow panel via fbdev.
 *
 *   ./wdm_screenshot [out.png]
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "wdm_platform.h"
#include "screens.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

/* Canvas = the ported layout: 320 wide (LCD_V_RES) x LCD_H_RES tall. */
#define DISP_W LCD_V_RES
#define DISP_H LCD_H_RES

static lv_color_t g_frame[DISP_W * DISP_H];

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px)
{
    for (int y = area->y1; y <= area->y2; y++) {
        lv_color_t *dst = &g_frame[y * DISP_W + area->x1];
        for (int x = area->x1; x <= area->x2; x++) {
            *dst++ = *px++;
        }
    }
    lv_disp_flush_ready(drv);
}

int main(int argc, char **argv)
{
    const char *out = (argc > 1) ? argv[1] : "screenshot.png";

    lv_init();

    static lv_color_t draw_buf[DISP_W * 48];
    static lv_disp_draw_buf_t db;
    lv_disp_draw_buf_init(&db, draw_buf, NULL, DISP_W * 48);

    static lv_disp_drv_t drv;
    lv_disp_drv_init(&drv);
    drv.hor_res  = DISP_W;
    drv.ver_res  = DISP_H;
    drv.flush_cb = flush_cb;
    drv.draw_buf = &db;
    lv_disp_t *disp = lv_disp_drv_register(&drv);

    /* Same theme the firmware's lvgl_init() applied. */
    lv_theme_t *th = lv_theme_default_init(
        disp,
        lv_palette_main(LV_PALETTE_INDIGO),
        lv_palette_main(LV_PALETTE_CYAN),
        true, &lv_font_montserrat_14);
    lv_disp_set_theme(disp, th);

    create_ui();
    for (int i = 0; i < 3; i++) {
        ui_refresh();
        lv_refr_now(NULL);
        lv_timer_handler();
    }

    /* RGB565 -> RGB888 */
    static uint8_t rgb[DISP_W * DISP_H * 3];
    for (int i = 0; i < DISP_W * DISP_H; i++) {
        uint16_t c = g_frame[i].full;
        uint8_t r = (c >> 11) & 0x1F;
        uint8_t g = (c >> 5)  & 0x3F;
        uint8_t b =  c        & 0x1F;
        rgb[i*3+0] = (uint8_t)((r << 3) | (r >> 2));
        rgb[i*3+1] = (uint8_t)((g << 2) | (g >> 4));
        rgb[i*3+2] = (uint8_t)((b << 3) | (b >> 2));
    }

    if (!stbi_write_png(out, DISP_W, DISP_H, 3, rgb, DISP_W * 3)) {
        fprintf(stderr, "ERROR: failed to write %s\n", out);
        return 1;
    }
    printf("wrote %s  (%dx%d)\n", out, DISP_W, DISP_H);
    return 0;
}
