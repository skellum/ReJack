#!/usr/bin/env bash
# rejack/systemd/install.sh — install ReJack as a systemd service.  Run:  sudo rejack/systemd/install.sh
# Also installs the udev rule (hidraw access, line levels).
# Runs as the user who invoked sudo (override: REJACK_USER=name). Builds tools/tjprobe if missing.
set -euo pipefail
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
USER_NAME=${REJACK_USER:-${SUDO_USER:-}}
[ -n "$USER_NAME" ] && [ "$USER_NAME" != root ] || { echo "set REJACK_USER (not root)"; exit 1; }
id -nG "$USER_NAME" | grep -qw audio || echo "warning: $USER_NAME is not in the audio group (needed for the dongle)"
[ -x "$ROOT/deps/bin/baresip" ] || { echo "missing $ROOT/deps/bin/baresip — build per rejack/README.md"; exit 1; }
[ -f "$ROOT/rejack/run/accounts" ] || { echo "missing rejack/run/accounts — see pbx/README.md"; exit 1; }
if [ ! -x "$ROOT/tools/tjprobe" ]; then
	sudo -u "$USER_NAME" gcc -O2 -Wall -o "$ROOT/tools/tjprobe" "$ROOT/tools/tjprobe.c"
fi
install -m 0644 "$ROOT/rejack/systemd/99-magicjack.rules" /etc/udev/rules.d/99-magicjack.rules
udevadm control --reload
udevadm trigger --action=change --subsystem-match=hidraw --subsystem-match=sound
sed -e "s|@ROOT@|$ROOT|g" -e "s|@USER@|$USER_NAME|g" "$ROOT/rejack/systemd/rejack.service.in" \
	> /etc/systemd/system/rejack.service
systemctl daemon-reload
systemctl enable --now rejack.service
sleep 4
systemctl --no-pager --lines=12 status rejack.service || true
echo "logs: journalctl -u rejack -f"
