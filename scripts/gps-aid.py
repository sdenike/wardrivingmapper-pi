#!/usr/bin/env python3
"""
gps-aid.py — speed up u-blox 7 cold starts on the WarDrivingMapper Pi.

The USB u-blox 7 dongle does a full COLD start every boot: it retains no
almanac and no time (live SKY reports show az=0 / no elevation, and the
receiver clock free-runs at the GPS week-rollover epoch ~2019-04-07). With no
time hint it must search the whole time/frequency space, so the first fix is
slow even outdoors.

This pushes UBX-AID-INI — the legacy aiding message, which is the correct one
for protocol 14 / u-blox 7 (NOT UBX-MGA-*, which is protocol 15+ / u-blox 8) —
carrying the Pi's NTP-accurate UTC time, and optionally a coarse last-known
position. That lets the receiver start warm instead of stone cold.

Transport: `ubxtool -c` (verified to reach the receiver via gpsd, which keeps
ownership of /dev/ttyACM0 — no need to stop the daemon).

Verify the MECHANISM indoors (no fix needed): the receiver's reported time
should jump from ~2019 to the current year right after this runs. The fix-time
BENEFIT is an outdoor test (watch the app's "first fix ... ttff=" log line).

Position aiding (if a state file is present) is sent with a deliberately coarse
100 km accuracy: this device moves between sessions, and GPS visibility barely
changes within 100 km, so a stale-but-coarse hint helps acquisition without
misleading the receiver.
"""
import os
import re
import struct
import subprocess
import sys
import time

LEAP_SECONDS = 18            # GPS-UTC offset; constant since 2017-01-01 (no leap since)
GPS_EPOCH_UNIX = 315964800   # unix time of 1980-01-06T00:00:00Z (GPS week 0)
SECONDS_PER_WEEK = 604800
TACC_SYNCED_MS = 2000        # NTP-locked: tight window (absorbs leap/drift slop)
TACC_FREE_MS = 30 * 60 * 1000  # no NTP (field): honest 30-min window, still beats 2019
POS_ACC_CM = 100 * 1000 * 100  # 100 km, in centimetres
STATE_FILE = os.environ.get("WDM_GPS_STATE", "/var/lib/wdm/last_pos")
PROTVER = "14"               # match the receiver so ubxtool frames/decodes correctly

UBX_AID_INI = (0x0B, 0x01)


def gps_week_tow(now_unix):
    """Full GPS week number and time-of-week (ms) for a unix timestamp."""
    g = now_unix - GPS_EPOCH_UNIX + LEAP_SECONDS
    wn = int(g // SECONDS_PER_WEEK)
    tow_ms = int(round((g - wn * SECONDS_PER_WEEK) * 1000))
    return wn, tow_ms


def read_last_pos(path):
    """Return (lat, lon, alt_m) from a 'lat lon alt' state file, or None."""
    try:
        with open(path) as f:
            lat, lon, alt = (float(x) for x in f.read().split()[:3])
        return lat, lon, alt
    except Exception:
        return None


def clock_trusted():
    """True if the system clock is NTP-synchronised (so we can claim tight tAcc)."""
    try:
        r = subprocess.run(["timedatectl", "show", "-p", "NTPSynchronized", "--value"],
                           capture_output=True, text=True, timeout=5)
        return r.stdout.strip() == "yes"
    except Exception:
        return False


def build_aid_ini(now_unix, tacc_ms, pos=None):
    """Pack a 48-byte UBX-AID-INI payload: time always, position if known."""
    wn, tow_ms = gps_week_tow(now_unix)
    lat_i = lon_i = alt_i = 0
    pos_acc = 0
    flags = 0x02                       # bit1: time valid (GPS time via wn + tow)
    if pos:
        lat, lon, alt = pos
        lat_i = int(round(lat * 1e7))  # 1e-7 deg
        lon_i = int(round(lon * 1e7))
        alt_i = int(round(alt * 100))  # cm
        pos_acc = POS_ACC_CM
        flags |= 0x01                  # bit0: position valid
        flags |= 0x20                  # bit5: position is lat/lon/alt (not ECEF)
    payload = struct.pack(
        "<iiiIHHIiIIiII",
        lat_i, lon_i, alt_i, pos_acc,  # ecefXOrLat, ecefYOrLon, ecefZOrAlt, posAcc
        0, wn, tow_ms, 0,              # tmCfg, wn, tow(ms), towNs
        tacc_ms, 0,                   # tAccMs, tAccNs
        0, 0,                         # clkDOrFreq, clkDAccOrFreqAcc
        flags,
    )
    assert len(payload) == 48, "AID-INI payload must be 48 bytes, got %d" % len(payload)
    return payload, wn, tow_ms


def send_ubx(cls_id, payload):
    """Send one UBX frame via ubxtool (ubxtool adds sync, length, checksum)."""
    cmd = "0x%02x,0x%02x" % cls_id
    if payload:
        cmd += "," + ",".join("0x%02x" % b for b in payload)
    return subprocess.run(
        ["ubxtool", "-P", PROTVER, "-c", cmd],
        capture_output=True, text=True, timeout=15,
    )


def receiver_week():
    """Poll UBX-NAV-TIMEGPS; return (week, valid_flags) the receiver reports.

    Parsed off the one line that carries leapS (unique to NAV-TIMEGPS) so a
    streamed NAV-SOL/NAV-DOP line can't be mistaken for it. (None, None) on
    any failure."""
    try:
        r = subprocess.run(["ubxtool", "-P", PROTVER, "-p", "NAV-TIMEGPS"],
                           capture_output=True, text=True, timeout=10)
    except Exception:
        return None, None
    for line in r.stdout.splitlines():
        if "week" in line and "leapS" in line and "valid" in line:
            mw = re.search(r"week\s+(\d+)", line)
            mv = re.search(r"valid\s+x([0-9a-fA-F]+)", line)
            if mw and mv:
                return int(mw.group(1)), int(mv.group(1), 16)
    return None, None


def main():
    pos = read_last_pos(STATE_FILE)
    synced = clock_trusted()
    tacc_ms = TACC_SYNCED_MS if synced else TACC_FREE_MS
    # gpsd may not have the device open the instant a boot service fires, so
    # send-then-confirm and retry until the receiver reports a valid week.
    for attempt in range(1, 7):
        now = time.time()
        payload, wn, _ = build_aid_ini(now, tacc_ms, pos)
        send_ubx(UBX_AID_INI, payload)
        time.sleep(1.5)
        week, valid = receiver_week()
        ok = week is not None and week > 1000 and valid is not None and (valid & 0x02)
        print("AID-INI attempt %d: sent wn=%d utc=%s clock=%s pos=%s -> receiver week=%s valid=%s [%s]" % (
            attempt, wn,
            time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(now)),
            "ntp" if synced else "free",
            "yes" if pos else "none",
            week, ("0x%x" % valid) if valid is not None else None,
            "OK" if ok else "retry",
        ))
        if ok:
            return 0
    sys.stderr.write("gps-aid: receiver never confirmed a valid week after 6 tries\n")
    return 1


if __name__ == "__main__":
    sys.exit(main())
