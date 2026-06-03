#!/usr/bin/env bash
#
# Deploy the Pi UI from a dev machine to the Pi over SSH, then rebuild + restart.
#
#   scripts/deploy.sh [user@host]      # default: shelby@10.0.1.242
#
# Why this script exists (two traps it avoids):
#   1. WRONG CWD — running `rsync ./ …` from the wrong directory once synced a
#      sibling repo over this one. This script always uses its own checkout dir.
#   2. MTIME SKIP — `rsync -a` preserves source mtimes, which can land OLDER than
#      the Pi's existing .o files, so `make` skips recompiling changed sources.
#      We `touch` our sources after syncing to force a rebuild of what changed.
#
# Secrets/runtime on the Pi (config.txt, data/) are preserved (excluded + the
# real config.txt is never overwritten).
set -euo pipefail

# Default to the mDNS hostname so it works whether the Pi is on Ethernet or WiFi
# (the IP changes / Ethernet may be unplugged when the device is deployed).
DEST="${1:-shelby@WarDrivingMapper-Pi.local}"
APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"

echo "Syncing $APP_DIR → $DEST:wardrivingmapper-pi/ …"
# --checksum: compare by CONTENT, not mtime. A plain mtime sync skipped edited
# files whose mtime landed older than the Pi's copy (clock skew / a prior deploy
# touch). This bit us repeatedly — never remove it.
rsync -az --checksum --delete \
  --exclude 'build/' --exclude '.git/' --exclude 'data/' --exclude 'config.txt' \
  "$APP_DIR/" "$DEST:wardrivingmapper-pi/"

echo "Building + restarting on $DEST …"
ssh "$DEST" 'set -e
  cd ~/wardrivingmapper-pi
  find src tests -name "*.c" -exec touch {} +   # defeat rsync mtime skew
  cmake -S . -B build >/dev/null
  cmake --build build -j"$(nproc)"
  sudo systemctl restart wdm-ui
  sleep 2
  echo "deployed; wdm-ui is $(systemctl is-active wdm-ui)"'
