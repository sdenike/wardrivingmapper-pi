#!/usr/bin/env python3
"""
gps-diag.py — instrument the WarDrivingMapper Pi's GPS cold-start so we can see
WHERE the time goes and WHY a receiver that "sees 10+ satellites" still never
gets a fix.

Two questions this answers, with hard numbers on a boot-relative timeline:

  1. How long does startup take, per stage?  boot -> gpsd active -> AID-INI
     injected -> first SKY report -> FIRST FIX.  (per-service start offsets +
     measured TTFF.)

  2. WHY no lock when satellites are visible?  The discriminator is signal
     quality and the "used" count, NOT the "seen" count.  This dongle keeps no
     almanac / ephemeris / time across boots (see gps-aid.py), so every boot is
     a COLD start: the receiver must DOWNLOAD each satellite's ephemeris off the
     50 bps nav stream — roughly 30 s of *uninterrupted* signal per satellite.
     If C/N0 is marginal (RF noise from the Pi / SPI LCD / USB dongle, or a poor
     sky view) it tracks the carrier forever but never finishes a subframe, so
     `used` stays 0 and `mode` never leaves 1 ("no fix") even with 10+ sats
     SEEN.  This logs the C/N0 distribution + used count every interval so that
     failure mode is unmistakable.  The screen's "10+ satellites" is
     satellites_VISIBLE; this shows satellites_USED, which is what a fix needs.

All elapsed times are measured against the boot clock (/proc/uptime) and a
monotonic clock, NEVER the wall clock: gpsd/NTP yank the wall clock by years
mid-run (the receiver free-runs at the ~2019 GPS week-rollover epoch until it
has time), which would make wall-clock deltas meaningless.  Wall-clock JUMPS are
detected and logged as their own events — the jump tells us when time aiding /
a fix actually took effect.

Data source: gpsd's JSON stream via `gpspipe -w` (gpsd-clients, already a Pi
dep).  Receiver-internal state (its own TTFF + time-valid flags) is polled
best-effort from UBX-NAV-STATUS via ubxtool — the same transport gps-aid.py uses
(through gpsd, which keeps /dev/ttyACM0 open).

Usage:
    ./gps-diag.py --once          # one snapshot of the current state, then exit
    ./gps-diag.py                 # follow live; 5 s samples to stdout + logfile
    sudo ./gps-diag.py --install  # install+enable a boot service, then reboot to
                                  # capture a full cold start from t=0
    journalctl -u gps-diag.service --no-pager   # read the captured timeline
    sudo ./gps-diag.py --uninstall              # remove the service when done
"""
import argparse
import json
import os
import re
import select
import signal
import subprocess
import sys
import time

PROTVER = "14"                       # u-blox 7 protocol; match gps-aid.py
LOGFILE = "/var/log/wdm/gps-diag.log"
WATCHED_UNITS = ("gpsd.service", "gps-aid.service", "wdm-ui.service")
INSTALL_PATH = "/usr/local/sbin/gps-diag.py"
UNIT_PATH = "/etc/systemd/system/gps-diag.service"

# C/N0 (carrier-to-noise, dB-Hz) thresholds for interpreting signal quality.
# Below ~30 dB-Hz a u-blox can track the carrier but rarely decodes the nav
# message cleanly enough to pull ephemeris; >=40 is a healthy open-sky signal.
CN0_DECODE = 30
CN0_GOOD = 40


# ── time sources that survive a wall-clock jump ─────────────────────────────
def uptime_s():
    """Seconds since boot (CLOCK_BOOTTIME-ish), immune to wall-clock jumps."""
    try:
        with open("/proc/uptime") as f:
            return float(f.read().split()[0])
    except Exception:
        return float("nan")


def fmt_dur(sec):
    """Compact m:ss-ish duration for log readability."""
    if sec != sec:                   # NaN
        return "?"
    sec = int(sec)
    if sec < 60:
        return "%ds" % sec
    return "%dm%02ds" % (sec // 60, sec % 60)


# ── one-time startup context ────────────────────────────────────────────────
def service_start_offsets():
    """For each watched unit, seconds-after-boot it became active (or None).

    Uses systemd's monotonic activation timestamp, which is measured against the
    same boot clock as /proc/uptime — so these compose into one clean timeline
    regardless of how wrong the wall clock was at boot."""
    out = {}
    for unit in WATCHED_UNITS:
        try:
            r = subprocess.run(
                ["systemctl", "show", unit,
                 "-p", "ActiveEnterTimestampMonotonic",
                 "-p", "ExecMainStartTimestampMonotonic"],
                capture_output=True, text=True, timeout=5)
            vals = dict(line.split("=", 1) for line in r.stdout.splitlines() if "=" in line)
            # Prefer when the service's main process started; fall back to active-enter.
            usec = vals.get("ExecMainStartTimestampMonotonic", "0")
            if usec in ("", "0"):
                usec = vals.get("ActiveEnterTimestampMonotonic", "0")
            usec = int(usec)
            out[unit] = (usec / 1e6) if usec > 0 else None
        except Exception:
            out[unit] = None
    return out


def ntp_synced():
    try:
        r = subprocess.run(["timedatectl", "show", "-p", "NTPSynchronized", "--value"],
                           capture_output=True, text=True, timeout=5)
        return r.stdout.strip() == "yes"
    except Exception:
        return False


def boot_total_s():
    """Total time to reach the default target, per systemd-analyze (or None)."""
    try:
        r = subprocess.run(["systemd-analyze", "time"], capture_output=True, text=True, timeout=5)
        m = re.search(r"=\s*([\d.]+)s", r.stdout)
        return float(m.group(1)) if m else None
    except Exception:
        return None


def aid_ini_summary():
    """Last line gps-aid.service logged this boot — what time it injected + OK/retry."""
    try:
        r = subprocess.run(["journalctl", "-b", "-u", "gps-aid.service", "--no-pager", "-o", "cat"],
                           capture_output=True, text=True, timeout=5)
        lines = [l for l in r.stdout.splitlines() if "AID-INI" in l]
        return lines[-1] if lines else None
    except Exception:
        return None


# ── receiver-internal state (best effort) ───────────────────────────────────
NAV_STATUS_FIX = {0: "none", 1: "DR", 2: "2D", 3: "3D", 4: "GPS+DR", 5: "time-only"}


def poll_nav_status():
    """UBX-NAV-STATUS -> dict(gpsFix, time_known, ttff_ms) or None.

    flags bit2=wknSet bit3=towSet: both set means the receiver actually has time
    (i.e. the AID-INI aiding 'took').  ttff is the receiver's own first-fix
    stopwatch in ms — 0 until it has ever fixed this power cycle."""
    try:
        r = subprocess.run(["ubxtool", "-P", PROTVER, "-p", "NAV-STATUS"],
                           capture_output=True, text=True, timeout=8)
    except Exception:
        return None
    fix = flags = ttff = None
    for line in r.stdout.splitlines():
        m = re.search(r"gpsFix\s+(\d+)", line)
        if m:
            fix = int(m.group(1))
        m = re.search(r"\bflags\s+x?([0-9a-fA-F]+)", line)
        if m:
            flags = int(m.group(1), 16)
        m = re.search(r"\bttff\s+(\d+)", line)
        if m:
            ttff = int(m.group(1))
    if fix is None and flags is None:
        return None
    return {
        "gpsFix": NAV_STATUS_FIX.get(fix, str(fix)),
        "time_known": (flags is not None and (flags & 0x0C) == 0x0C),
        "ttff_ms": ttff,
    }


# ── gpsd JSON reduction ─────────────────────────────────────────────────────
def summarize_sky(sky):
    """(seen, tracked, used, [cn0...]) from a SKY report.

    seen    = satellites reported at all
    tracked = satellites with a real C/N0 (signal actually being received)
    used    = satellites in the position solution (this is what a fix needs)"""
    sats = sky.get("satellites", []) or []
    cn0 = [s["ss"] for s in sats if isinstance(s.get("ss"), (int, float)) and s["ss"] > 0]
    used = sum(1 for s in sats if s.get("used"))
    return len(sats), len(cn0), used, sorted(cn0)


def cn0_stats(cn0):
    if not cn0:
        return "none"
    n = len(cn0)
    med = cn0[n // 2] if n % 2 else (cn0[n // 2 - 1] + cn0[n // 2]) / 2
    decode = sum(1 for c in cn0 if c >= CN0_DECODE)
    good = sum(1 for c in cn0 if c >= CN0_GOOD)
    return "min=%g med=%g max=%g  (>=%d:%d  >=%d:%d)" % (
        cn0[0], med, cn0[-1], CN0_DECODE, decode, CN0_GOOD, good)


MODE_NAME = {0: "unknown", 1: "NO FIX", 2: "2D fix", 3: "3D fix"}


class Logger:
    def __init__(self, also_file):
        self.fh = None
        if also_file:
            try:
                os.makedirs(os.path.dirname(LOGFILE), exist_ok=True)
                self.fh = open(LOGFILE, "a", buffering=1)
            except Exception as e:
                sys.stderr.write("gps-diag: cannot open %s (%s); stdout only\n" % (LOGFILE, e))

    def __call__(self, msg):
        line = "[+%s] %s" % (fmt_dur(uptime_s()), msg)
        print(line, flush=True)
        if self.fh:
            self.fh.write(line + "\n")


def banner(log):
    log("===== gps-diag run start =====")
    bt = boot_total_s()
    log("boot: userspace ready at +%s%s | NTP synced=%s" % (
        fmt_dur(uptime_s()), (" (systemd-analyze total %.1fs)" % bt) if bt else "", ntp_synced()))
    for unit, off in service_start_offsets().items():
        log("  %-18s started at boot+%s" % (unit, fmt_dur(off) if off is not None else "?/not-active"))
    aid = aid_ini_summary()
    if aid:
        log("  gps-aid: %s" % aid)
    ns = poll_nav_status()
    if ns:
        log("  receiver: gpsFix=%s time_known=%s ttff=%s" % (
            ns["gpsFix"], ns["time_known"],
            ("%dms" % ns["ttff_ms"]) if ns["ttff_ms"] else "none-yet"))
    log("legend: SEEN=reported  TRK=has C/N0  USED=in solution  | C/N0 in dB-Hz "
        "(>=%d decodable, >=%d strong)" % (CN0_DECODE, CN0_GOOD))


def sample_line(tpv, sky):
    mode = (tpv or {}).get("mode", 0)
    seen, trk, used, cn0 = summarize_sky(sky or {})
    hdop = (sky or {}).get("hdop")
    rxtime = (tpv or {}).get("time", "—")
    return "%-7s seen=%-2d trk=%-2d used=%-2d  C/N0 %s  hdop=%s  rx_utc=%s" % (
        MODE_NAME.get(mode, "?"), seen, trk, used, cn0_stats(cn0),
        ("%.1f" % hdop) if isinstance(hdop, (int, float)) else "—", rxtime)


def open_gpspipe():
    return subprocess.Popen(["gpspipe", "-w"], stdout=subprocess.PIPE, text=True, bufsize=1)


def run(interval, max_seconds, also_file):
    log = Logger(also_file)
    banner(log)

    proc = open_gpspipe()
    tpv = None
    sky = None
    got_fix = False
    last_wall = time.time()
    last_mono = time.monotonic()
    start_mono = last_mono
    next_sample = start_mono + interval
    next_nav = start_mono + interval        # first NAV-STATUS poll a sample in
    sky_seen_logged = False

    def stop(*_):
        try:
            proc.terminate()
        except Exception:
            pass
        log("===== gps-diag run end (uptime +%s) =====" % fmt_dur(uptime_s()))
        sys.exit(0)
    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)

    while True:
        now = time.monotonic()
        if max_seconds and (now - start_mono) >= max_seconds:
            log("reached --max-seconds=%d; stopping" % max_seconds)
            break

        # wall-clock jump detector: compare wall delta vs monotonic delta.
        wall = time.time()
        drift = (wall - last_wall) - (now - last_mono)
        if abs(drift) >= 3:
            log(">>> WALL CLOCK JUMPED %+.0fs (now %s) — time source kicked in"
                % (drift, time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(wall))))
        last_wall, last_mono = wall, now

        timeout = max(0.0, min(next_sample, next_nav) - now)
        ready, _, _ = select.select([proc.stdout], [], [], timeout)
        if ready:
            line = proc.stdout.readline()
            if not line:                     # gpspipe died — gpsd down? restart it
                log("gpspipe ended; restarting in 2s (is gpsd up?)")
                time.sleep(2)
                proc = open_gpspipe()
                continue
            try:
                obj = json.loads(line)
            except ValueError:
                continue
            cls = obj.get("class")
            if cls == "TPV":
                tpv = obj
                mode = obj.get("mode", 0)
                if not got_fix and mode >= 2:
                    got_fix = True
                    log("*** FIRST FIX at boot+%s — %s ***   %s"
                        % (fmt_dur(uptime_s()), MODE_NAME.get(mode), sample_line(tpv, sky)))
            elif cls == "SKY":
                sky = obj
                if not sky_seen_logged:
                    sky_seen_logged = True
                    log("first SKY report at boot+%s — %s" % (fmt_dur(uptime_s()), sample_line(tpv, sky)))

        now = time.monotonic()
        if now >= next_sample:
            log(sample_line(tpv, sky))
            next_sample = now + interval
        if now >= next_nav:
            ns = poll_nav_status()
            if ns:
                log("  rx: gpsFix=%s time_known=%s ttff=%s"
                    % (ns["gpsFix"], ns["time_known"],
                       ("%dms" % ns["ttff_ms"]) if ns["ttff_ms"] else "none-yet"))
            next_nav = now + max(interval * 6, 30)   # NAV-STATUS is slow-changing

    try:
        proc.terminate()
    except Exception:
        pass


def run_once(also_file):
    """One snapshot: drain a few gpsd reports, print state, exit."""
    log = Logger(also_file)
    banner(log)
    proc = open_gpspipe()
    tpv = sky = None
    deadline = time.monotonic() + 6        # gpsd emits TPV+SKY ~once/sec
    while time.monotonic() < deadline and not (tpv and sky):
        ready, _, _ = select.select([proc.stdout], [], [], deadline - time.monotonic())
        if not ready:
            break
        try:
            obj = json.loads(proc.stdout.readline())
        except ValueError:
            continue
        if obj.get("class") == "TPV":
            tpv = obj
        elif obj.get("class") == "SKY":
            sky = obj
    proc.terminate()
    log("snapshot: %s" % sample_line(tpv, sky))
    seen, trk, used, _ = summarize_sky(sky or {})
    if used == 0 and trk >= 4:
        log("DIAGNOSIS HINT: %d sats tracked but 0 used — classic 'tracking, can't "
            "decode ephemeris'. Check the C/N0 above: if it's mostly <%d dB-Hz the "
            "signal is too weak/noisy to pull ephemeris (antenna sky view / Pi RF "
            "noise), which a cold-start receiver cannot work around." % (trk, CN0_DECODE))


# ── service install / uninstall (this is a temporary debug tool) ────────────
UNIT = """[Unit]
Description=WarDrivingMapper GPS cold-start diagnostic (TEMPORARY — remove when root-caused)
After=gpsd.service
Wants=gpsd.service

[Service]
Type=simple
ExecStart=%s --service
Restart=on-failure
RestartSec=2

[Install]
WantedBy=multi-user.target
""" % INSTALL_PATH


def require_root(action):
    if os.geteuid() != 0:
        sys.exit("gps-diag: --%s needs root (try: sudo %s --%s)" % (action, sys.argv[0], action))


def install():
    require_root("install")
    subprocess.run(["install", "-m", "0755", os.path.abspath(__file__), INSTALL_PATH], check=True)
    with open(UNIT_PATH, "w") as f:
        f.write(UNIT)
    subprocess.run(["systemctl", "daemon-reload"], check=True)
    subprocess.run(["systemctl", "enable", "gps-diag.service"], check=True)
    print("installed gps-diag.service (starts on boot, after gpsd).")
    print("Reboot to capture a full cold start:   sudo reboot")
    print("Then read the timeline:                journalctl -u gps-diag.service --no-pager")
    print("Remove when done:                      sudo %s --uninstall" % INSTALL_PATH)


def uninstall():
    require_root("uninstall")
    subprocess.run(["systemctl", "disable", "--now", "gps-diag.service"], check=False)
    for p in (UNIT_PATH, INSTALL_PATH):
        try:
            os.remove(p)
        except FileNotFoundError:
            pass
    subprocess.run(["systemctl", "daemon-reload"], check=False)
    print("removed gps-diag.service.")


def main():
    ap = argparse.ArgumentParser(description="GPS cold-start diagnostic logger for the WarDrivingMapper Pi")
    ap.add_argument("--once", action="store_true", help="print one snapshot of current GPS state and exit")
    ap.add_argument("--interval", type=float, default=5.0, help="seconds between samples (default 5)")
    ap.add_argument("--max-seconds", type=int, default=0, help="stop after N seconds (0 = run until killed)")
    ap.add_argument("--no-file", action="store_true", help="log to stdout only, not %s" % LOGFILE)
    ap.add_argument("--service", action="store_true", help="run as the boot service (forever, journald-logged)")
    ap.add_argument("--install", action="store_true", help="install+enable the boot service (root)")
    ap.add_argument("--uninstall", action="store_true", help="remove the boot service (root)")
    args = ap.parse_args()

    if args.install:
        return install()
    if args.uninstall:
        return uninstall()
    also_file = not args.no_file and not args.service   # service mode -> journald only
    if args.once:
        return run_once(also_file)
    run(args.interval, args.max_seconds, also_file)


if __name__ == "__main__":
    main()
