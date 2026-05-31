#!/usr/bin/env bash
#
# optimize-sdcard.sh — reduce SD-card wear for a 24/7 Pi device.
#
# Raspberry Pi OS (Trixie) already sets root `noatime` and uses zram swap, so
# this ONLY adds what's missing — all additive / low-risk (no root-fs or swap
# surgery, nothing that can stop the Pi booting):
#   - tmpfs for /tmp and /var/tmp (RAM, not SD)
#   - journald -> volatile (logs in RAM), size-capped
#   - swappiness=100 (zram is fast RAM, lean on it before evicting cache)
#   - disable chatty background writers (man-db, apt-daily timers)
#   - Log2Ram: /var/log in RAM, flushed hourly + on shutdown
#
# Idempotent. Backs up edited files. Needs sudo. Reboot to fully apply
# (tmpfs mounts + Log2Ram take effect on boot).
set -euo pipefail
ts=$(date +%s 2>/dev/null || echo bak)
bk() { [ -f "$1" ] && sudo cp -n "$1" "$1.wdmbak.$ts" 2>/dev/null || true; }

echo "==> tmpfs for /tmp and /var/tmp"
bk /etc/fstab
grep -qE '[[:space:]]/tmp[[:space:]]' /etc/fstab || \
  echo 'tmpfs /tmp     tmpfs defaults,noatime,nosuid,nodev,size=64M 0 0' | sudo tee -a /etc/fstab >/dev/null
grep -qE '[[:space:]]/var/tmp[[:space:]]' /etc/fstab || \
  echo 'tmpfs /var/tmp tmpfs defaults,noatime,nosuid,nodev,size=32M 0 0' | sudo tee -a /etc/fstab >/dev/null

echo "==> journald -> volatile (RAM), capped at 32M"
sudo mkdir -p /etc/systemd/journald.conf.d
printf '[Journal]\nStorage=volatile\nRuntimeMaxUse=32M\n' | \
  sudo tee /etc/systemd/journald.conf.d/wdm-volatile.conf >/dev/null

echo "==> swappiness=100 (zram swap is RAM, not SD)"
echo 'vm.swappiness=100' | sudo tee /etc/sysctl.d/99-wdm.conf >/dev/null
sudo sysctl -q vm.swappiness=100 || true

echo "==> disable chatty background writers"
for t in man-db.timer apt-daily.timer apt-daily-upgrade.timer; do
  sudo systemctl disable --now "$t" 2>/dev/null || true
done

echo "==> Log2Ram (/var/log in RAM)"
if ! command -v log2ram >/dev/null 2>&1 && [ ! -f /etc/log2ram.conf ]; then
  sudo install -d -m0755 /usr/share/keyrings
  if sudo wget -qO /usr/share/keyrings/azlux-archive-keyring.gpg https://azlux.fr/repo.gpg; then
    echo "deb [signed-by=/usr/share/keyrings/azlux-archive-keyring.gpg] http://packages.azlux.fr/debian/ stable main" | \
      sudo tee /etc/apt/sources.list.d/azlux.list >/dev/null
    sudo apt-get update -qq >/dev/null 2>&1 || true
    sudo apt-get install -y -qq log2ram >/dev/null 2>&1 || echo "  (apt install failed)"
  fi
fi
if [ -f /etc/log2ram.conf ]; then
  bk /etc/log2ram.conf
  sudo sed -i 's/^SIZE=.*/SIZE=48M/' /etc/log2ram.conf
  echo "  log2ram configured (SIZE=48M)"
else
  echo "  NOTE: log2ram not installed (repo unreachable) — re-run later; the rest still applies."
fi

echo "Done. Reboot to apply tmpfs + Log2Ram."
