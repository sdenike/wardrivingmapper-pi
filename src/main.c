/**
 * main.c — render harness for the WarDrivingMapper Pi UI.
 *
 *   ./wdm_ui out.png            render the dashboard to a PNG (headless, any host)
 *   ./wdm_ui --fb /dev/fb1      render and blit to a Linux framebuffer (the panel)
 *   ./wdm_ui --fb /dev/fb1 --bgr --swap-bytes   color/byte-order fixups if needed
 *
 * The same screens.c drives both. On macOS only the PNG path is compiled in.
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

/* Canvas = the ported layout: LCD_V_RES wide x LCD_H_RES tall (Elecrow 480x320). */
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

/* ── Linux framebuffer output (the Pi panel) ─────────────────────── */
#ifdef __linux__
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/fb.h>

static int blit_to_fb(const char *dev, int swap_rb, int swap_bytes)
{
    int fd = open(dev, O_RDWR);
    if (fd < 0) { perror("open framebuffer"); return 1; }

    struct fb_var_screeninfo vi;
    struct fb_fix_screeninfo fi;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &vi) < 0 ||
        ioctl(fd, FBIOGET_FSCREENINFO, &fi) < 0) {
        perror("FBIOGET_*SCREENINFO"); close(fd); return 1;
    }
    fprintf(stderr, "fb %s: %ux%u %ubpp  line=%u  R@%u G@%u B@%u\n",
            dev, vi.xres, vi.yres, vi.bits_per_pixel, fi.line_length,
            vi.red.offset, vi.green.offset, vi.blue.offset);

    if (vi.bits_per_pixel != 16) {
        fprintf(stderr, "expected 16bpp RGB565; got %ubpp — not blitting.\n",
                vi.bits_per_pixel);
        close(fd); return 2;
    }

    int W = (int)vi.xres < DISP_W ? (int)vi.xres : DISP_W;
    int H = (int)vi.yres < DISP_H ? (int)vi.yres : DISP_H;
    uint16_t *row = malloc((size_t)vi.xres * 2);
    if (!row) { close(fd); return 1; }

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint16_t c = g_frame[y * DISP_W + x].full;
            if (swap_rb) {           /* RGB565 <-> BGR565 */
                uint16_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
                c = (uint16_t)((b << 11) | (g << 5) | r);
            }
            if (swap_bytes) c = (uint16_t)((c >> 8) | (c << 8));
            row[x] = c;
        }
        if (lseek(fd, (off_t)y * fi.line_length, SEEK_SET) < 0 ||
            write(fd, row, (size_t)W * 2) < 0) {
            perror("write fb"); free(row); close(fd); return 1;
        }
    }
    free(row);
    close(fd);
    return 0;
}
#endif /* __linux__ */

int main(int argc, char **argv)
{
    const char *png_out = NULL;
    const char *fb_dev  = NULL;
    int swap_rb = 0, swap_bytes = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--fb") && i + 1 < argc)      fb_dev = argv[++i];
        else if (!strcmp(argv[i], "--bgr"))                swap_rb = 1;
        else if (!strcmp(argv[i], "--swap-bytes"))         swap_bytes = 1;
        else                                               png_out = argv[i];
    }
    if (!png_out && !fb_dev) png_out = "screenshot.png";

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

    lv_theme_t *th = lv_theme_default_init(
        disp, lv_palette_main(LV_PALETTE_INDIGO),
        lv_palette_main(LV_PALETTE_CYAN), true, &lv_font_montserrat_14);
    lv_disp_set_theme(disp, th);

    create_ui();
    for (int i = 0; i < 3; i++) { ui_refresh(); lv_refr_now(NULL); lv_timer_handler(); }

    int rc = 0;
    if (fb_dev) {
#ifdef __linux__
        rc = blit_to_fb(fb_dev, swap_rb, swap_bytes);
        if (rc == 0) fprintf(stderr, "blitted dashboard to %s\n", fb_dev);
#else
        fprintf(stderr, "--fb is only supported on Linux\n");
        rc = 3;
#endif
    }

    if (png_out) {
        static uint8_t rgb[DISP_W * DISP_H * 3];
        for (int i = 0; i < DISP_W * DISP_H; i++) {
            uint16_t c = g_frame[i].full;
            uint8_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
            rgb[i*3+0] = (uint8_t)((r << 3) | (r >> 2));
            rgb[i*3+1] = (uint8_t)((g << 2) | (g >> 4));
            rgb[i*3+2] = (uint8_t)((b << 3) | (b >> 2));
        }
        if (!stbi_write_png(png_out, DISP_W, DISP_H, 3, rgb, DISP_W * 3)) {
            fprintf(stderr, "ERROR: failed to write %s\n", png_out);
            return 1;
        }
        printf("wrote %s  (%dx%d)\n", png_out, DISP_W, DISP_H);
    }
    return rc;
}
