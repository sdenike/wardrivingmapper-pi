#!/usr/bin/env python3
"""
gps-cache.py — SD-backed GPS aiding cache for the WarDrivingMapper Pi.

This is the software equivalent of the ESP32's XH414 backup battery. The USB
u-blox 7 keeps NO almanac / ephemeris / time across power cycles (see
gps-aid.py), so every boot is a full COLD start: it must re-download each
satellite's ephemeris off the 50 bps nav stream — ~30 s of clean signal per SV.
Where the antenna sees weak signal (this device tracks at ~22 dB-Hz indoors) it
can track 10+ satellites yet never finish a download, so it never gets a fix.

The fix: persist the receiver's orbital data to the SD card while it HAS it, and
replay it on the next boot. A receiver handed known orbits only has to *range*,
not *decode* — and ranging works at signal levels where decoding never will
(this is why assisted GPS locks indoors). So restoring the cache should let the
Pi fix even at today's weak signal, exactly the way the battery-backed ESP32 does.

What is cached (validity drives the restore freshness gates):
  almanac (AID-ALM)  weeks   — which sats exist + coarse orbits (acquisition)
  AOP     (AID-AOP)  ~3 days — AssistNow Autonomous: receiver-PREDICTED orbits
  ephemeris(AID-EPH) ~2-4 h  — precise orbits => true hot start, fix in seconds
  HUI     (AID-HUI)  weeks   — health / UTC / ionosphere
  last position              — written here, finally feeding gps-aid.py's
                               AID-INI position hint (previously dead: nothing
                               ever wrote /var/lib/wdm/last_pos)
AssistNow Autonomous (AOP) is enabled in the receiver (CFG-NAVX5 aopCfg bit0) so
it predicts orbits for ~3 days from data it has already collected.

Transport (both verified on-device):
  SAVE   — `ubxtool -R` capture THROUGH gpsd. Read-only, no daemon stop, so it
           never interrupts wardriving GPS. Runs on a 30-min timer + on shutdown.
  RESTORE— ONE direct-serial write (pyserial). ubxtool costs ~2.3 s/call and
           only honours the last -c, so injecting dozens of frames through it
           would take ~90 s; a single serial write is <1 s. Restore briefly
           stops gpsd for exclusive access and ALWAYS restarts it (try/finally).

  gps-cache.py save        # capture orbits -> SD   (safe anytime; needs a fix to be useful)
  gps-cache.py restore     # inject SD cache + AOP + AID-INI  (boot; needs root)
  sudo gps-cache.py --install     # boot-restore service + save timer + shutdown save
  sudo gps-cache.py --uninstall   # remove them, re-enable plain gps-aid.service
"""
import argparse
import glob
import importlib.util
import os
import struct
import subprocess
import sys
import time

PROTVER = "14"                                   # u-blox 7; matches gps-aid.py
CACHE_DIR = os.environ.get("WDM_GPS_CACHE", "/var/lib/wdm")
INSTALL_PATH = "/usr/local/sbin/gps-cache.py"


def resolve_device():
    """Receiver device path, found by USB IDENTITY not port number. The stable
    /dev/serial/by-id symlink follows the dongle across any USB port, any
    ttyACM0<->ttyACM1 renumber (which a replug triggers), and reboots — so direct
    serial restore never writes to the wrong node. Falls back to ttyACM0."""
    env = os.environ.get("WDM_GPS_DEV")
    if env:
        return env
    for pat in ("*u-blox*", "*u_blox*", "*GNSS*", "*GPS*"):
        hits = sorted(glob.glob("/dev/serial/by-id/" + pat))
        if hits:
            return hits[0]
    return "/dev/ttyACM0"


DEVICE = resolve_device()

# UBX (class, id) of the messages we cache + the config message that turns on AOP.
AID_INI = (0x0B, 0x01)
AID_HUI = (0x0B, 0x02)
AID_ALM = (0x0B, 0x30)
AID_EPH = (0x0B, 0x31)
AID_AOP = (0x0B, 0x33)
CFG_NAVX5 = (0x06, 0x23)
NAV_STATUS = (0x01, 0x03)
NAV_POSLLH = (0x01, 0x02)
AOPCFG_OFFSET = 27                               # byte 27 bit0 = useAOP (calibrated on-device)

# key -> (msg, restore-freshness seconds, min payload length that means "real data")
# Empty poll responses are 8 B (svid + week=0 / how=0); real almanac=40, eph=104,
# AOP~60, HUI=72. The min-real gate keeps empties out of the cache.
CACHE = {
    "hui": (AID_HUI, 30 * 86400, 9),
    "alm": (AID_ALM, 60 * 86400, 9),
    "aop": (AID_AOP, 3 * 86400, 9),
    "eph": (AID_EPH, 2 * 3600, 9),
}
# Inject order: time-context first (AOP-enable, AID-INI), then orbits coarse->fine.
RESTORE_ORDER = ("hui", "alm", "aop", "eph")


def _load_gps_aid():
    """Import the sibling gps-aid.py (hyphen => importlib) to reuse its AID-INI
    builder, so the time/position hint stays single-sourced."""
    for d in (os.path.dirname(os.path.abspath(__file__)), "/usr/local/sbin"):
        p = os.path.join(d, "gps-aid.py")
        if os.path.exists(p):
            spec = importlib.util.spec_from_file_location("gps_aid", p)
            m = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(m)
            return m
    return None


GPS_AID = _load_gps_aid()


# ── UBX framing ─────────────────────────────────────────────────────────────
def ubx_frame(cls, mid, payload=b""):
    """Wrap a payload in a full UBX frame (sync, len, Fletcher-8 checksum)."""
    body = bytes([cls, mid]) + struct.pack("<H", len(payload)) + payload
    a = b = 0
    for x in body:
        a = (a + x) & 0xFF
        b = (b + a) & 0xFF
    return b"\xB5\x62" + body + bytes([a, b])


def parse_frames(data):
    """Yield (cls, id, full_frame_bytes, payload) for every checksum-valid UBX
    frame in a raw byte buffer (skips NMEA and partial frames)."""
    out = []
    i, n = 0, len(data)
    while i < n - 7:
        if data[i] == 0xB5 and data[i + 1] == 0x62:
            cls, mid = data[i + 2], data[i + 3]
            ln = data[i + 4] | (data[i + 5] << 8)
            if i + 8 + ln <= n:
                a = b = 0
                for x in data[i + 2:i + 6 + ln]:
                    a = (a + x) & 0xFF
                    b = (b + a) & 0xFF
                if a == data[i + 6 + ln] and b == data[i + 7 + ln]:
                    out.append((cls, mid, data[i:i + 8 + ln], data[i + 6:i + 6 + ln]))
                    i += 8 + ln
                    continue
        i += 1
    return out


def run(args, **kw):
    return subprocess.run(args, capture_output=True, text=True, **kw)


def capture(presets, secs=4):
    """Poll `presets` and return parsed UBX frames from the raw receiver stream,
    via `ubxtool -R` (through gpsd — read-only, no daemon stop)."""
    tmp = "/tmp/gps-cache-capture.bin"
    try:
        os.remove(tmp)
    except FileNotFoundError:
        pass
    cmd = ["ubxtool", "-P", PROTVER, "-R", tmp, "-w", str(secs)]
    for p in presets:
        cmd += ["-p", p]
    run(cmd)
    try:
        return parse_frames(open(tmp, "rb").read())
    except FileNotFoundError:
        return []


def atomic_write(path, data):
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


# ── SAVE (through gpsd, no interruption) ────────────────────────────────────
def save_last_pos():
    """Write 'lat lon alt' to last_pos from the current fix, if any (best effort).
    This is what gps-aid.py reads for its AID-INI position hint."""
    fix = None
    pos = None
    for cls, mid, _full, pl in capture(["NAV-STATUS", "NAV-POSLLH"], secs=2):
        if (cls, mid) == NAV_STATUS and len(pl) >= 5:
            fix = pl[4]                                   # gpsFix: >=2 means 2D/3D
        elif (cls, mid) == NAV_POSLLH and len(pl) >= 20:
            lon, lat, _h, hmsl = struct.unpack_from("<iiii", pl, 4)
            pos = (lat / 1e7, lon / 1e7, hmsl / 1000.0)
    if fix and fix >= 2 and pos:
        atomic_write(os.path.join(CACHE_DIR, "last_pos"),
                     ("%.6f %.6f %.1f\n" % pos).encode())
        return pos
    return None


def save():
    os.makedirs(CACHE_DIR, exist_ok=True)
    frames = capture(["AID-ALM", "AID-AOP", "AID-EPH", "AID-HUI"], secs=5)
    grouped = {}
    for cls, mid, full, pl in frames:
        for key, (msg, _age, min_real) in CACHE.items():
            if (cls, mid) == msg and len(pl) >= min_real:
                grouped.setdefault(key, bytearray()).extend(full)
    summary = {}
    for key in CACHE:
        blob = grouped.get(key)
        if blob:                                          # never clobber good cache with empties
            atomic_write(os.path.join(CACHE_DIR, "aid_%s.ubx" % key), bytes(blob))
            summary[key] = len(parse_frames(blob))
    pos = save_last_pos()
    if summary or pos:
        print("gps-cache save: " + ", ".join("%s=%df" % (k, v) for k, v in summary.items())
              + (" last_pos=%.5f,%.5f" % (pos[0], pos[1]) if pos else ""))
    else:
        print("gps-cache save: no real aiding data yet (receiver has no fix/orbits to save)")
    return 0


# ── RESTORE (one direct-serial write; boot only) ────────────────────────────
def aop_enable_frame():
    """Capture the live CFG-NAVX5 and return a copy with useAOP set (preserves
    every other tuned field — safe read-modify-write)."""
    for cls, mid, _full, pl in capture(["CFG-NAVX5"], secs=2):
        if (cls, mid) == CFG_NAVX5 and len(pl) > AOPCFG_OFFSET:
            pl = bytearray(pl)
            pl[AOPCFG_OFFSET] |= 0x01
            return ubx_frame(*CFG_NAVX5, bytes(pl))
    return None


def aid_ini_frame():
    """Fresh AID-INI (time always; position if last_pos exists) via gps-aid.py."""
    if not GPS_AID:
        return None
    pos = GPS_AID.read_last_pos(GPS_AID.STATE_FILE)
    synced = GPS_AID.clock_trusted()
    tacc = GPS_AID.TACC_SYNCED_MS if synced else GPS_AID.TACC_FREE_MS
    payload, _wn, _ = GPS_AID.build_aid_ini(time.time(), tacc, pos)
    return ubx_frame(*AID_INI, payload), bool(pos), synced


def gpsd_stop():
    run(["systemctl", "stop", "gpsd.socket", "gpsd.service"])
    time.sleep(0.5)                                       # let the device handle release


def gpsd_start():
    run(["systemctl", "start", "gpsd.socket"])
    run(["systemctl", "start", "gpsd.service"])           # harmless if socket-activated


def inject_direct(buf):
    """Stop gpsd, write the whole aiding buffer to the receiver in one shot, and
    ALWAYS bring gpsd back."""
    import serial
    gpsd_stop()
    try:
        ser = serial.Serial(DEVICE, 9600, timeout=1)      # baud is ignored on USB CDC
        try:
            ser.write(buf)
            ser.flush()
            time.sleep(1.0)                               # let the receiver consume + ACK
            ser.read(ser.in_waiting or 0)                 # drain (ACKs go nowhere useful)
        finally:
            ser.close()
    finally:
        gpsd_start()


def restore():
    if os.geteuid() != 0:
        sys.exit("gps-cache restore needs root (opens %s + bounces gpsd)" % DEVICE)
    os.makedirs(CACHE_DIR, exist_ok=True)
    buf = bytearray()
    detail = {}

    aop = aop_enable_frame()                              # captured while gpsd still up
    if aop:
        buf += aop
        detail["aop_enable"] = True
    ini = aid_ini_frame()
    if ini:
        buf += ini[0]
        detail["aid_ini"] = "pos+time" if ini[1] else "time"
        detail["clock"] = "ntp" if ini[2] else "free"

    for key in RESTORE_ORDER:
        path = os.path.join(CACHE_DIR, "aid_%s.ubx" % key)
        if not os.path.exists(path):
            continue
        age = time.time() - os.path.getmtime(path)
        max_age = CACHE[key][1]
        if age > max_age:
            detail[key] = "skip(stale %.1fh)" % (age / 3600.0)
            continue
        data = open(path, "rb").read()
        buf += data
        detail[key] = "%df(%.1fh)" % (len(parse_frames(data)), age / 3600.0)

    if not buf:
        print("gps-cache restore: nothing to inject (no cache yet) — cold start unaided")
        return 0
    inject_direct(bytes(buf))
    print("gps-cache restore: wrote %d bytes -> %s" % (len(buf), detail))
    return 0


# ── AssistNow Online (optional; per-user token from the Setup web UI) ────────
# Fetches CURRENT orbits for ALL satellites from u-blox over WiFi, so the receiver
# can fix on the FIRST boot in a brand-new location with no prior good-signal
# bootstrap. The token is per-operator (config.txt, written by the web UI) and is
# NEVER shipped in the repo. With no token the feature is simply skipped.
ASSISTNOW_HOSTS = ("online-live1.services.u-blox.com", "online-live2.services.u-blox.com")


def resolve_config():
    """Path to the wardriver's config.txt (holds assistnow_token). The C app reads
    ./config.txt from its checkout; we run from /usr/local/sbin, so find it by the
    usual deploy layout (or $WDM_CONFIG)."""
    env = os.environ.get("WDM_CONFIG")
    if env and os.path.exists(env):
        return env
    for pat in ("/home/*/wardrivingmapper-pi/config.txt",
                "/root/wardrivingmapper-pi/config.txt",
                os.path.join(os.path.dirname(os.path.abspath(__file__)), "config.txt")):
        hits = sorted(glob.glob(pat))
        if hits:
            return hits[0]
    return None


def read_config():
    cfg = {}
    path = resolve_config()
    if path:
        try:
            for line in open(path):
                line = line.strip()
                if line and not line.startswith("#") and "=" in line:
                    k, v = line.split("=", 1)
                    cfg[k.strip()] = v.strip()
        except OSError:
            pass
    return cfg


def fetch_assistnow(token, pos=None):
    """Download current GPS orbits from u-blox AssistNow Online in legacy AID format
    (proto-14 / u-blox 7). Returns raw UBX AID bytes ready to inject, or None
    (offline / bad token). The token is never logged."""
    import urllib.request
    query = "token=%s;gnss=gps;datatype=eph,alm,aux;format=aid" % token
    if pos:
        query += ";lat=%.4f;lon=%.4f;pacc=100000" % (pos[0], pos[1])
    for host in ASSISTNOW_HOSTS:
        try:
            with urllib.request.urlopen("https://%s/GetOnlineData.ashx?%s" % (host, query),
                                        timeout=12) as r:
                data = r.read()
            if data[:2] == b"\xB5\x62":            # a valid UBX AID stream
                return data
            sys.stderr.write("assistnow: %s returned %d non-UBX bytes (token invalid?)\n"
                             % (host, len(data)))
        except Exception as e:                     # offline / DNS / TLS / HTTP — all non-fatal
            sys.stderr.write("assistnow: %s unreachable (%s)\n" % (host, e))
    return None


def assistnow():
    if os.geteuid() != 0:
        sys.exit("gps-cache assistnow needs root (opens %s + bounces gpsd)" % DEVICE)
    token = read_config().get("assistnow_token", "").strip()
    if not token:
        print("gps-cache assistnow: no assistnow_token set (add one in the Setup web UI) — skipping")
        return 0
    pos = GPS_AID.read_last_pos(GPS_AID.STATE_FILE) if GPS_AID else None
    data = fetch_assistnow(token, pos)
    if not data:
        print("gps-cache assistnow: nothing fetched (offline or bad token) — SD cache still covers boot")
        return 0
    buf = bytearray()
    aop = aop_enable_frame()
    if aop:
        buf += aop
    ini = aid_ini_frame()
    if ini:
        buf += ini[0]
    buf += data
    inject_direct(bytes(buf))
    print("gps-cache assistnow: injected %d AID frames (%d bytes) from u-blox AssistNow"
          % (len(parse_frames(data)), len(buf)))
    time.sleep(3)                                  # let gpsd re-attach after the bounce
    save()                                         # bake the fresh orbits into the SD cache
    return 0


# ── systemd wiring (this supersedes plain gps-aid.service for boot aiding) ───
UNIT_RESTORE = """[Unit]
Description=WarDrivingMapper GPS aiding restore (SD cache + AOP + AID-INI -> warm start)
After=gpsd.service time-sync.target
Wants=gpsd.service
Before=wdm-ui.service

[Service]
Type=oneshot
ExecStart=%s restore

[Install]
WantedBy=multi-user.target
""" % INSTALL_PATH

UNIT_SAVE = """[Unit]
Description=WarDrivingMapper GPS aiding save (almanac/ephemeris/AOP -> SD)
After=gpsd.service

[Service]
Type=oneshot
ExecStart=%s save
""" % INSTALL_PATH

TIMER_SAVE = """[Unit]
Description=Periodically save GPS aiding to SD (cheap; survives abrupt power loss)

[Timer]
OnBootSec=10min
OnUnitActiveSec=30min

[Install]
WantedBy=timers.target
"""

UNIT_SHUTDOWN = """[Unit]
Description=Save GPS aiding to SD on shutdown
After=gpsd.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/bin/true
ExecStop=%s save

[Install]
WantedBy=multi-user.target
""" % INSTALL_PATH

UNIT_ASSISTNOW = """[Unit]
Description=WarDrivingMapper AssistNow Online (fetch fresh GPS orbits over WiFi)
After=network-online.target gpsd.service
Wants=network-online.target gpsd.service

[Service]
Type=oneshot
ExecStart=%s assistnow

[Install]
WantedBy=multi-user.target
""" % INSTALL_PATH

UNITS = {
    "/etc/systemd/system/gps-cache.service": UNIT_RESTORE,
    "/etc/systemd/system/gps-cache-save.service": UNIT_SAVE,
    "/etc/systemd/system/gps-cache-save.timer": TIMER_SAVE,
    "/etc/systemd/system/gps-cache-shutdown.service": UNIT_SHUTDOWN,
    "/etc/systemd/system/gps-assistnow.service": UNIT_ASSISTNOW,
}


def install():
    if os.geteuid() != 0:
        sys.exit("--install needs root (try: sudo %s --install)" % sys.argv[0])
    run(["install", "-m", "0755", os.path.abspath(__file__), INSTALL_PATH])
    for path, body in UNITS.items():
        with open(path, "w") as f:
            f.write(body)
    run(["systemctl", "daemon-reload"])
    # gps-cache restore now sends AID-INI itself, so retire the standalone aider.
    run(["systemctl", "disable", "--now", "gps-aid.service"])
    for unit in ("gps-cache.service", "gps-cache-save.timer", "gps-cache-shutdown.service",
                 "gps-assistnow.service"):
        run(["systemctl", "enable", unit])
    run(["systemctl", "start", "gps-cache-save.timer"])
    run(["systemctl", "start", "gps-cache-shutdown.service"])
    print("installed gps-cache: boot-restore + 30-min save timer + shutdown save.")
    print("  (disabled gps-aid.service — gps-cache restore now sends AID-INI too)")
    print("Seed the cache once the device gets a fix:  sudo %s save" % INSTALL_PATH)
    print("It then auto-restores on every boot. Remove: sudo %s --uninstall" % INSTALL_PATH)
    print("AssistNow Online runs when a token is set in the Setup web UI (else skipped).")


def uninstall():
    if os.geteuid() != 0:
        sys.exit("--uninstall needs root")
    for unit in ("gps-cache.service", "gps-cache-save.timer", "gps-cache-save.service",
                 "gps-cache-shutdown.service", "gps-assistnow.service"):
        run(["systemctl", "disable", "--now", unit])
    for path in list(UNITS) + [INSTALL_PATH]:
        try:
            os.remove(path)
        except FileNotFoundError:
            pass
    run(["systemctl", "enable", "gps-aid.service"])       # restore prior behavior
    run(["systemctl", "daemon-reload"])
    print("removed gps-cache; re-enabled gps-aid.service.")


def main():
    ap = argparse.ArgumentParser(description="SD-backed GPS aiding cache for the WarDrivingMapper Pi")
    ap.add_argument("action", nargs="?", choices=["save", "restore", "status", "assistnow"],
                    help="save: orbits->SD | restore: SD->receiver (boot) | assistnow: fetch orbits over WiFi | status")
    ap.add_argument("--install", action="store_true", help="install boot-restore + save timer + shutdown save (root)")
    ap.add_argument("--uninstall", action="store_true", help="remove services, re-enable gps-aid.service (root)")
    args = ap.parse_args()

    if args.install:
        return install()
    if args.uninstall:
        return uninstall()
    if args.action == "save":
        return save()
    if args.action == "restore":
        return restore()
    if args.action == "status":
        return status()
    if args.action == "assistnow":
        return assistnow()
    ap.print_help()
    return 1


def status():
    print("GPS aiding cache in %s:" % CACHE_DIR)
    found = False
    for key, (_msg, max_age, _mr) in CACHE.items():
        path = os.path.join(CACHE_DIR, "aid_%s.ubx" % key)
        if os.path.exists(path):
            found = True
            age = time.time() - os.path.getmtime(path)
            blob = open(path, "rb").read()
            fresh = "FRESH" if age <= max_age else "STALE"
            print("  aid_%-3s %3d frames  age %5.1fh  (max %4.0fh) %s"
                  % (key, len(parse_frames(blob)), age / 3600.0, max_age / 3600.0, fresh))
    lp = os.path.join(CACHE_DIR, "last_pos")
    if os.path.exists(lp):
        found = True
        print("  last_pos: %s (age %.1fh)" % (open(lp).read().strip(),
              (time.time() - os.path.getmtime(lp)) / 3600.0))
    tok = read_config().get("assistnow_token", "").strip()
    print("  AssistNow: %s" % ("token set — Online fetch enabled" if tok
                               else "no token (Online disabled; SD cache only)"))
    if not found:
        print("  (cache empty — run 'gps-cache.py save' after the receiver has a fix)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
