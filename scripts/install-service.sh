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
    build-essential cmake iw gpsd gpsd-clients libgps-dev libcurl4-openssl-dev

# Point gpsd at the USB GPS and let it poll without a client, then enable it.
if [[ -e /dev/ttyACM0 ]]; then
    sudo tee /etc/default/gpsd >/dev/null <<'GPSD'
START_DAEMON="true"
USBAUTO="true"
DEVICES="/dev/ttyACM0"
GPSD_OPTIONS="-n"
GPSD
    sudo systemctl enable gpsd.socket >/dev/null 2>&1 || true
    sudo systemctl restart gpsd.socket || true
fi

# Pin WiFi interface names by DRIVER so the radios never swap wlan0/wlan1 across
# reboots (the onboard WiFi can fail its boot firmware load and let the USB
# dongle grab wlan0, which breaks scan_iface/hw_id/connectivity). Onboard
# brcmfmac = wlan0 (client + DeNike); USB rt2800usb dongle = wlan1 (scanner +
# setup AP). The dongle is ALWAYS wlan1, so scanning works even if the onboard
# radio fails to come up.
sudo tee /etc/systemd/network/10-wlan-onboard.link >/dev/null <<'LINK'
[Match]
Driver=brcmfmac
[Link]
Name=wlan0
LINK
sudo tee /etc/systemd/network/11-wlan-dongle.link >/dev/null <<'LINK'
[Match]
Driver=rt2800usb
[Link]
Name=wlan1
LINK

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
ExecStart=$BIN --live
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
