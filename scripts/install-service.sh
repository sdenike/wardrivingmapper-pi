#!/usr/bin/env bash
#
# Install the WarDrivingMapper Pi UI as a systemd service so the panel runs
# the live dashboard automatically (and restarts on failure / on boot).
#
# Run on the Pi:  bash scripts/install-service.sh
# Requires sudo. Uses the invoking user + this checkout's path — nothing is
# hardcoded, so it works for any user/location.
set -euo pipefail

APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$APP_DIR/build/wdm_ui"

# Wardriver dependencies: cmake/build, iw (scan), gpsd + libgps (GPS), libcurl
# (upload). Idempotent — apt skips already-installed packages.
echo "Installing dependencies…"
sudo DEBIAN_FRONTEND=noninteractive apt-get update -qq
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
    build-essential cmake iw gpsd gpsd-clients libgps-dev libcurl4-openssl-dev python3-serial

# Point gpsd at the USB GPS by its STABLE /dev/serial/by-id path, NOT /dev/ttyACMx.
# The ttyACM number is assigned by plug ORDER, so it renumbers when the dongle is
# moved to another USB port or replugged (a replug onto a USB extender bumped ours
# from ttyACM0 to ttyACM1, and gpsd — pinned to ttyACM0 — silently stopped getting
# fixes). The by-id symlink is keyed to the device's USB identity, so it follows
# the receiver across any port / renumber / reboot.
GPS_DEV="$(ls /dev/serial/by-id/ 2>/dev/null | grep -iE 'u-?blox|GNSS|GPS' | head -1)"
[[ -n "$GPS_DEV" ]] && GPS_DEV="/dev/serial/by-id/$GPS_DEV"
[[ -z "$GPS_DEV" && -e /dev/ttyACM0 ]] && GPS_DEV="/dev/ttyACM0"   # fallback if by-id absent
if [[ -n "$GPS_DEV" ]]; then
    sudo tee /etc/default/gpsd >/dev/null <<GPSD
START_DAEMON="true"
USBAUTO="true"
DEVICES="$GPS_DEV"
GPSD_OPTIONS="-n"
GPSD
    sudo systemctl enable gpsd.socket >/dev/null 2>&1 || true
    sudo systemctl restart gpsd.socket || true
fi

# GPS startup aiding: the USB u-blox 7 cold-starts every boot with no retained
# almanac or time (live SKY shows az=0 / no elevation; the receiver clock
# free-runs at the 2019 GPS-rollover epoch), so the first fix is slow even
# outdoors. Push UBX-AID-INI — the legacy aiding message, correct for this
# protocol-14 receiver (MGA is proto 15+) — carrying the Pi's NTP time so the
# receiver starts warm. Runs once per boot, after gpsd.
sudo install -m 0755 "$APP_DIR/scripts/gps-aid.py" /usr/local/sbin/gps-aid.py
sudo mkdir -p /var/lib/wdm
sudo tee /etc/systemd/system/gps-aid.service >/dev/null <<'AID'
[Unit]
Description=u-blox GPS startup aiding (AID-INI time hint -> faster cold-start TTFF)
After=gpsd.service time-sync.target
Wants=gpsd.service
[Service]
Type=oneshot
ExecStart=/usr/local/sbin/gps-aid.py
[Install]
WantedBy=multi-user.target
AID
sudo systemctl enable gps-aid.service >/dev/null 2>&1 || true

# GPS aiding CACHE (SD-backed warm start): persists almanac/ephemeris/AOP to the
# SD card while the receiver has them and replays them on boot — the software
# equivalent of the ESP32's XH414 backup cell, so the u-blox warm-starts instead
# of cold-starting every power-up. SUPERSEDES gps-aid.service (it sends AID-INI
# itself), so its --install disables the standalone aider. Needs python3-serial
# (installed above) for the one-shot direct-serial restore write. See gps-cache.py.
sudo install -m 0755 "$APP_DIR/scripts/gps-cache.py" /usr/local/sbin/gps-cache.py
sudo /usr/local/sbin/gps-cache.py --install || true

# Pin WiFi interface names by DRIVER so the radios never swap across reboots.
# Use NON-wlanN names: renaming INTO the kernel's own wlanN namespace races with
# the kernel's assignment (worsened by the onboard's flaky firmware-load timing)
# and was unreliable. Onboard brcmfmac = wmain (connectivity); USB rt2800usb
# dongle = wscan (scanner + setup AP). Connectivity (netplan) must bind the
# onboard by MAC to wmain — that part is site-specific, done outside this script.
sudo rm -f /etc/systemd/network/10-wlan-onboard.link /etc/systemd/network/11-wlan-dongle.link
sudo tee /etc/systemd/network/10-wmain.link >/dev/null <<'LINK'
[Match]
Driver=brcmfmac
[Link]
Name=wmain
LINK
sudo tee /etc/systemd/network/11-wscan.link >/dev/null <<'LINK'
[Match]
Driver=rt2800usb
[Link]
Name=wscan
LINK

# ── Kiosk panel hardening ───────────────────────────────────────────────────
# The Elecrow SPI panel is a framebuffer console (fbtft /dev/fb0), so the kernel
# text console + login getty paint a blinking cursor / log text onto it, which
# bleeds through the LVGL UI. Kill the cursor globally and stop the login getty;
# fbcon itself is unbound from the panel by wdm-console-off.sh (below) at app
# start (doing it here at install time wouldn't survive a reboot, and an early
# boot service gets re-bound by vconsole setup).
CMDLINE=/boot/firmware/cmdline.txt
[[ -f $CMDLINE ]] || CMDLINE=/boot/cmdline.txt
if [[ -f $CMDLINE ]]; then
    sudo cp -n "$CMDLINE" "$CMDLINE.wdmbak" || true
    grep -q 'vt.global_cursor_default=0' "$CMDLINE" \
        || sudo sed -i '1 s/$/ vt.global_cursor_default=0/' "$CMDLINE"
fi
sudo systemctl disable getty@tty1 >/dev/null 2>&1 || true

sudo tee /usr/local/sbin/wdm-console-off.sh >/dev/null <<'COFF'
#!/bin/sh
# Detach the kernel text console (fbcon) + blink cursor from the SPI panel fb so
# the LVGL UI owns /dev/fb0 with no cursor / kernel-log bleed-through.
for v in /sys/class/vtconsole/vtcon*; do
    if grep -q "frame buffer" "$v/name" 2>/dev/null; then
        echo 0 > "$v/bind" 2>/dev/null || true
    fi
done
echo 0 > /sys/class/graphics/fbcon/cursor_blink 2>/dev/null || true
exit 0
COFF
sudo chmod +x /usr/local/sbin/wdm-console-off.sh

# Panel orientation: the panel mounts upside-down in the case, so flip it 180
# degrees — piscreen rotate=270 (still landscape 480x320). The app then runs
# with --invert-x (in the unit below) so swipe direction matches. Only touches
# an existing piscreen overlay line; the overlay itself is set up per docs.
CONFIG=/boot/firmware/config.txt
[[ -f $CONFIG ]] || CONFIG=/boot/config.txt
if [[ -f $CONFIG ]] && grep -q 'dtoverlay=piscreen' "$CONFIG"; then
    sudo cp -n "$CONFIG" "$CONFIG.wdmbak" || true
    sudo sed -i 's/dtoverlay=piscreen,rotate=[0-9]*/dtoverlay=piscreen,rotate=270/' "$CONFIG"
fi

if [[ ! -x "$BIN" ]]; then
    echo "Building first ($BIN missing)…"
    cmake -S "$APP_DIR" -B "$APP_DIR/build" >/dev/null
    cmake --build "$APP_DIR/build" -j"$(nproc)"
fi

sudo tee /etc/systemd/system/wdm-ui.service >/dev/null <<UNIT
[Unit]
Description=WarDrivingMapper Pi UI (live panel)
After=multi-user.target

[Service]
Type=simple
User=$USER
SupplementaryGroups=video input
WorkingDirectory=$APP_DIR
# Detach the console/cursor from the SPI panel before the UI draws ('+' = run as
# root, since the service user can't write the vtcon bind node).
ExecStartPre=+/usr/local/sbin/wdm-console-off.sh
# --invert-x pairs with the 180-degree-flipped panel (config.txt rotate=270),
# correcting left/right swipe direction for the case-mounted orientation.
ExecStart=$BIN --live --invert-x
Restart=on-failure
RestartSec=2
# Let the Setup web UI bind port 80 without running as root.
AmbientCapabilities=CAP_NET_BIND_SERVICE

[Install]
WantedBy=multi-user.target
UNIT

sudo systemctl daemon-reload
sudo systemctl enable --now wdm-ui
echo "installed; is-active=$(systemctl is-active wdm-ui)"
