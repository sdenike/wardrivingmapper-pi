/**
 * setup_ap.h — on-device Wi-Fi config (the "Wi-Fi Setup" hold).
 *
 * Turns the onboard radio (wlan0) into a hotspot via NetworkManager and serves
 * a small web page from the Pi itself: join a Wi-Fi network + edit every
 * wardriver setting, from any browser, no phone app required. The dongle
 * (wlan1) keeps wardriving-scanning throughout and supplies the network list.
 *
 * Linux-only. The render thread drives the screen; these run on the main thread
 * (start/stop) and a dedicated web-server thread.
 */
#pragma once

#include <stdbool.h>

/* Bring up the hotspot + web server (idempotent). */
void setup_mode_start(void);

/* Tear down the web server + hotspot and return wlan0 to client mode. */
void setup_mode_stop(void);

/* True while the hotspot/web server is up. */
bool setup_mode_active(void);

/* Set by the web server when the user joins a network or it times out; the main
 * loop polls this and calls setup_mode_stop() from the main thread (the web
 * thread can't stop itself — it would join itself). Cleared by setup_mode_stop. */
bool setup_should_stop(void);
