/**
 * main.c — render harness + live app for the WarDrivingMapper Pi UI.
 *
 *   ./wdm_ui out.png        render the dashboard to a PNG (any host)
 *   ./wdm_ui --fb /dev/fb0  render once and blit to a framebuffer
 *   ./wdm_ui --live         live interactive app on the panel (Pi)
 *   ./wdm_ui --info         dump device state to the terminal
 *
 * Touch model in --live (no physical buttons on the Pi):
 *   tap            -> next screen
 *   swipe L / R    -> previous / next screen
 *   hold (still)   -> on a menu screen, fills the progress bar and activates
 *                     it (Wi-Fi Setup / Force Upload / Power Off), like the
 *                     ESP32 long-press.
 *
 * Flags: --touch <dev> (default /dev/input/event2), --swap-axes, --invert-x,
 *        --bgr, --swap-bytes.  On macOS only the PNG path is compiled in.
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

#define DISP_W LCD_V_RES
#define DISP_H LCD_H_RES

static lv_color_t g_frame[DISP_W * DISP_H];

/* Dirty row range accumulated by flush_cb during one lv_refr_now(). */
static int g_dirty_y1 = 0, g_dirty_y2 = DISP_H - 1;

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px)
{
    if (area->y1 < g_dirty_y1) g_dirty_y1 = area->y1;
    if (area->y2 > g_dirty_y2) g_dirty_y2 = area->y2;
    for (int y = area->y1; y <= area->y2; y++) {
        lv_color_t *dst = &g_frame[y * DISP_W + area->x1];
        for (int x = area->x1; x <= area->x2; x++) *dst++ = *px++;
    }
    lv_disp_flush_ready(drv);
}

static void dump_info(void)
{
    gps_data_t g = g_gps_data; system_state_t s = g_system_state;
    printf("== WarDrivingMapper — device state ==\n");
    printf("  screen      : %u\n", s.current_screen);
    printf("  GPS fix     : %s   sats=%u  hdop=%.1f  ttff=%ums\n",
           g.is_valid ? "yes" : "no", g.satellites, (double)g.hdop, s.gps_ttff_ms);
    printf("  position    : %.6f, %.6f   alt=%.1f m\n", g.lat, g.lon, (double)g.altitude_m);
    printf("  speed/course: %.1f km/h   %.0f deg\n", (double)g.speed_kmh, (double)g.course_deg);
    printf("  GPS time    : %s\n", g.iso_time);
    printf("  networks    : %u unique logged\n", s.ssids_logged);
    printf("  scan        : #%u   %d APs visible\n", s.scan_count, s.last_ap_count);
    printf("  battery     : %.0f%%  %.2f V%s\n", (double)s.battery_pct,
           (double)s.battery_voltage, s.is_charging ? "  (charging)" : "");
    printf("  storage     : %.0f / %.0f MB free\n",
           s.sd_free_bytes / 1048576.0, s.sd_total_bytes / 1048576.0);
    printf("  trusted SSID: %s\n", s.trusted_visible ? "in range" : "no");
    printf("  (values are STUB/demo until gpsd + iw scan are wired in)\n");
}

/* ── Linux framebuffer + touch (the Pi panel) ────────────────────── */
#ifdef __linux__
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <linux/input.h>

extern volatile uint32_t g_btn_held_ms;   /* platform_stub.c */

typedef struct { int fd; uint32_t xres, yres, line_len; } fb_target_t;

static int fb_open(fb_target_t *t, const char *dev)
{
    t->fd = open(dev, O_RDWR);
    if (t->fd < 0) { perror("open framebuffer"); return -1; }
    struct fb_var_screeninfo vi; struct fb_fix_screeninfo fi;
    if (ioctl(t->fd, FBIOGET_VSCREENINFO, &vi) < 0 ||
        ioctl(t->fd, FBIOGET_FSCREENINFO, &fi) < 0) { perror("fb ioctl"); close(t->fd); return -1; }
    if (vi.bits_per_pixel != 16) { fprintf(stderr, "need 16bpp, got %u\n", vi.bits_per_pixel); close(t->fd); return -1; }
    t->xres = vi.xres; t->yres = vi.yres; t->line_len = fi.line_length;
    fprintf(stderr, "fb %s: %ux%u line=%u\n", dev, vi.xres, vi.yres, fi.line_length);
    return 0;
}

/* Blit framebuffer rows [y1,y2] from g_frame. */
static void fb_blit_rows(fb_target_t *t, int y1, int y2, int swap_rb, int swap_bytes)
{
    if (y1 < 0) y1 = 0;
    if (y2 >= (int)t->yres) y2 = (int)t->yres - 1;
    if (y2 < y1) return;
    int W = (int)t->xres < DISP_W ? (int)t->xres : DISP_W;
    uint16_t row[DISP_W];
    for (int y = y1; y <= y2; y++) {
        for (int x = 0; x < W; x++) {
            uint16_t c = g_frame[y * DISP_W + x].full;
            if (swap_rb) { uint16_t r=(c>>11)&0x1F,g=(c>>5)&0x3F,b=c&0x1F; c=(uint16_t)((b<<11)|(g<<5)|r); }
            if (swap_bytes) c = (uint16_t)((c >> 8) | (c << 8));
            row[x] = c;
        }
        lseek(t->fd, (off_t)y * t->line_len, SEEK_SET);
        if (write(t->fd, row, (size_t)W * 2) < 0) { perror("write fb"); return; }
    }
}

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

/* Screen cycle order — matches the ESP32 firmware nav sequence. */
static const uint8_t NAV_CYCLE[] = {
    SCREEN_SCAN, SCREEN_NAV, SCREEN_MENU_SETUP, SCREEN_MENU_UPLOAD, SCREEN_MENU_POWEROFF
};
#define NAV_N      ((int)(sizeof(NAV_CYCLE) / sizeof(NAV_CYCLE[0])))
#define SWIPE_PX   300    /* raw ADS7846 units to count as a swipe / "moved" */
#define TAP_MS     400    /* released within this with no move = a tap */

static int is_menu_screen(uint8_t s)
{
    return s == SCREEN_MENU_SETUP || s == SCREEN_MENU_UPLOAD || s == SCREEN_MENU_POWEROFF;
}
static uint32_t hold_full_ms(uint8_t s)
{
    return s == SCREEN_MENU_POWEROFF ? HOLD_POWEROFF_MS : LONG_MS;
}

static void trigger_menu_action(uint8_t s)
{
    /* The hold mechanic + progress bar are live now; the actions below are
     * placeholders until those features are wired (and the boot overlay is
     * persisted, so Power Off won't strand the panel). */
    switch (s) {
        case SCREEN_MENU_SETUP:   fprintf(stderr, "ACTION: enter Wi-Fi Setup (not implemented yet)\n"); break;
        case SCREEN_MENU_UPLOAD:  fprintf(stderr, "ACTION: Force Upload (not implemented yet)\n"); break;
        case SCREEN_MENU_POWEROFF:fprintf(stderr, "ACTION: Power Off (placeholder — not shutting down yet)\n"); break;
        default: break;
    }
}

static int run_live(const char *fb_dev, const char *touch_dev,
                    int swap_rb, int swap_bytes, int swap_axes, int invert_x)
{
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig);
    fb_target_t fb;
    if (fb_open(&fb, fb_dev) < 0) return 1;
    int tfd = open(touch_dev, O_RDONLY | O_NONBLOCK);
    if (tfd < 0) fprintf(stderr, "warn: no touch %s (%s)\n", touch_dev, strerror(errno));
    fprintf(stderr, "live: tap=next, swipe=prev/next, hold=select. Ctrl-C to quit.\n");

    int nav_idx = 0, touching = 0, moved = 0, fired = 0;
    int sx = 0, sy = 0, cx = 0, cy = 0;
    uint32_t down_ms = 0, last_refresh = 0;
    int need_render = 1;   /* force initial full paint */

    while (!g_stop) {
        /* ── drain touch events ───────────────────────────── */
        if (tfd >= 0) {
            struct input_event ev;
            while (read(tfd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
                if (ev.type == EV_ABS && ev.code == ABS_X) cx = ev.value;
                else if (ev.type == EV_ABS && ev.code == ABS_Y) cy = ev.value;
                else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
                    if (ev.value) { touching = 1; sx = cx; sy = cy; down_ms = wdm_tick_ms(); moved = 0; fired = 0; }
                    else if (touching) {
                        touching = 0;
                        int dx = cx - sx, dy = cy - sy;
                        int hx = swap_axes ? dy : dx, hy = swap_axes ? dx : dy;
                        if (invert_x) hx = -hx;
                        uint32_t dur = wdm_tick_ms() - down_ms;
                        if (moved && abs(hx) > SWIPE_PX && abs(hx) > abs(hy))
                            nav_idx = (nav_idx + (hx > 0 ? 1 : -1) + NAV_N) % NAV_N;   /* swipe */
                        else if (!moved && !fired && dur < TAP_MS)
                            nav_idx = (nav_idx + 1) % NAV_N;                            /* tap = next */
                        g_system_state.current_screen = NAV_CYCLE[nav_idx];
                        g_btn_held_ms = 0;
                        need_render = 1;
                    }
                }
            }
        }

        /* ── hold tracking while finger is down ───────────── */
        if (touching) {
            int dx = cx - sx, dy = cy - sy;
            if (abs(dx) > SWIPE_PX || abs(dy) > SWIPE_PX) moved = 1;
            if (!moved) {
                uint32_t held = wdm_tick_ms() - down_ms;
                g_btn_held_ms = held;
                uint8_t s = g_system_state.current_screen;
                if (is_menu_screen(s) && !fired && held >= hold_full_ms(s)) {
                    fired = 1;
                    trigger_menu_action(s);
                    g_btn_held_ms = 0;   /* bar snaps closed = "activated" */
                }
                need_render = 1;   /* animate the bar */
            } else if (g_btn_held_ms) { g_btn_held_ms = 0; need_render = 1; }
        }

        /* ── render: on change/hold, else every 500ms ─────── */
        uint32_t now = wdm_tick_ms();
        if (need_render || now - last_refresh > 500) {
            last_refresh = now;
            need_render = 0;
            g_dirty_y1 = DISP_H; g_dirty_y2 = -1;
            ui_refresh();
            lv_refr_now(NULL);
            if (g_dirty_y2 >= g_dirty_y1)
                fb_blit_rows(&fb, g_dirty_y1, g_dirty_y2, swap_rb, swap_bytes);
        }
        lv_timer_handler();
        usleep(12000);
    }
    if (tfd >= 0) close(tfd);
    close(fb.fd);
    fprintf(stderr, "live: stopped\n");
    return 0;
}

static int blit_once(const char *fb_dev, int swap_rb, int swap_bytes)
{
    fb_target_t fb;
    if (fb_open(&fb, fb_dev) < 0) return 1;
    fb_blit_rows(&fb, 0, DISP_H - 1, swap_rb, swap_bytes);
    close(fb.fd);
    fprintf(stderr, "blitted dashboard to %s\n", fb_dev);
    return 0;
}
#endif /* __linux__ */

static void setup_display(void)
{
    lv_init();
    static lv_color_t draw_buf[DISP_W * 48];
    static lv_disp_draw_buf_t db;
    lv_disp_draw_buf_init(&db, draw_buf, NULL, DISP_W * 48);
    static lv_disp_drv_t drv;
    lv_disp_drv_init(&drv);
    drv.hor_res = DISP_W; drv.ver_res = DISP_H;
    drv.flush_cb = flush_cb; drv.draw_buf = &db;
    lv_disp_t *disp = lv_disp_drv_register(&drv);
    lv_theme_t *th = lv_theme_default_init(disp,
        lv_palette_main(LV_PALETTE_INDIGO), lv_palette_main(LV_PALETTE_CYAN),
        true, &lv_font_montserrat_14);
    lv_disp_set_theme(disp, th);
}

int main(int argc, char **argv)
{
    const char *png_out = NULL, *fb_dev = NULL, *touch_dev = "/dev/input/event2";
    int live = 0, swap_rb = 0, swap_bytes = 0, swap_axes = 0, invert_x = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--info"))                   { dump_info(); return 0; }
        else if (!strcmp(argv[i], "--live"))              live = 1;
        else if (!strcmp(argv[i], "--fb") && i+1<argc)    fb_dev = argv[++i];
        else if (!strcmp(argv[i], "--touch") && i+1<argc) touch_dev = argv[++i];
        else if (!strcmp(argv[i], "--bgr"))               swap_rb = 1;
        else if (!strcmp(argv[i], "--swap-bytes"))        swap_bytes = 1;
        else if (!strcmp(argv[i], "--swap-axes"))         swap_axes = 1;
        else if (!strcmp(argv[i], "--invert-x"))          invert_x = 1;
        else                                              png_out = argv[i];
    }
    if (live && !fb_dev) fb_dev = "/dev/fb0";
    if (!live && !png_out && !fb_dev) png_out = "screenshot.png";

    setup_display();
    create_ui();
    for (int i = 0; i < 3; i++) { ui_refresh(); lv_refr_now(NULL); lv_timer_handler(); }

    if (live) {
#ifdef __linux__
        return run_live(fb_dev, touch_dev, swap_rb, swap_bytes, swap_axes, invert_x);
#else
        fprintf(stderr, "--live is only supported on Linux\n"); return 3;
#endif
    }

    int rc = 0;
    if (fb_dev) {
#ifdef __linux__
        rc = blit_once(fb_dev, swap_rb, swap_bytes);
#else
        fprintf(stderr, "--fb is only supported on Linux\n"); rc = 3;
#endif
    }
    if (png_out) {
        static uint8_t rgb[DISP_W * DISP_H * 3];
        for (int i = 0; i < DISP_W * DISP_H; i++) {
            uint16_t c = g_frame[i].full;
            uint8_t r=(c>>11)&0x1F, g=(c>>5)&0x3F, b=c&0x1F;
            rgb[i*3+0]=(uint8_t)((r<<3)|(r>>2));
            rgb[i*3+1]=(uint8_t)((g<<2)|(g>>4));
            rgb[i*3+2]=(uint8_t)((b<<3)|(b>>2));
        }
        if (!stbi_write_png(png_out, DISP_W, DISP_H, 3, rgb, DISP_W*3)) {
            fprintf(stderr, "ERROR: failed to write %s\n", png_out); return 1;
        }
        printf("wrote %s  (%dx%d)\n", png_out, DISP_W, DISP_H);
    }
    return rc;
}
