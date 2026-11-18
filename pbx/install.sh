#!/usr/bin/env bash
# pbx/install.sh — install the ReJack PBX config into the system Asterisk.  Run:  sudo pbx/install.sh
# Backs up the stock pjsip.conf / extensions.conf / rtp.conf, renders pjsip.conf with pbx/secrets.env,
# installs as asterisk:asterisk 0640 and restarts Asterisk.
set -euo pipefail
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
DIR=$(cd "$(dirname "$0")" && pwd)
ETC=/etc/asterisk
SECRETS="$DIR/secrets.env"
[ -f "$SECRETS" ] || { echo "missing $SECRETS (see secrets.env.example)"; exit 1; }
# shellcheck disable=SC1090
. "$SECRETS"
for v in LAN_CIDR PW_100 PW_101 PW_102; do [ -n "${!v:-}" ] || { echo "$v not set in $SECRETS"; exit 1; }; done

BK="$ETC/rejack-backup-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$BK"
for f in pjsip.conf extensions.conf rtp.conf; do [ -f "$ETC/$f" ] && cp -p "$ETC/$f" "$BK/"; done
echo "backup: $BK"

tmp=$(mktemp)
sed -e "s|@LAN_CIDR@|$LAN_CIDR|" -e "s|@PW_100@|$PW_100|" -e "s|@PW_101@|$PW_101|" -e "s|@PW_102@|$PW_102|" \
    "$DIR/asterisk/pjsip.conf.in" > "$tmp"
install -o asterisk -g asterisk -m 0640 "$tmp" "$ETC/pjsip.conf"
rm -f "$tmp"
install -o asterisk -g asterisk -m 0640 "$DIR/asterisk/extensions.conf" "$ETC/extensions.conf"
install -o asterisk -g asterisk -m 0640 "$DIR/asterisk/rtp.conf" "$ETC/rtp.conf"

systemctl restart asterisk
sleep 3
asterisk -rx "pjsip show endpoints" | grep -E 'Endpoint:|Contact:' || true
echo "installed. Logs: sudo asterisk -rvvv"
