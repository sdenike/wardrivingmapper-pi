/**
 * gps_gpsd.h — GPS fix source via gpsd (libgps client).
 *
 * gpsd owns /dev/ttyACM0; this thread subscribes over localhost:2947 and fills
 * g_gps_data / g_system_state.gps_fix, replacing platform_stub.c's demo fix.
 */
#pragma once

/* Spawn the detached GPS thread. Returns 0 on success. */
int gps_thread_start(void);
