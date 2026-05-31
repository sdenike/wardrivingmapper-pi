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

[Install]
WantedBy=multi-user.target
UNIT

sudo systemctl daemon-reload
sudo systemctl enable --now wdm-ui
echo "installed; is-active=$(systemctl is-active wdm-ui)"
