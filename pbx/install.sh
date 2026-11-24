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
for v in LAN_CIDR PW_100 PW_101 PW_102 VM_PIN_100 VM_PIN_101 VM_PIN_102; do [ -n "${!v:-}" ] || { echo "$v not set in $SECRETS"; exit 1; }; done

# SIP trunk (optional): set TRUNK_HOST, TRUNK_USER, TRUNK_PASS, TRUNK_DID in secrets.env to enable it.
TRUNK_PERMIT=127.0.0.1/255.255.255.255
if [ -n "${TRUNK_HOST:-}" ]; then
  for v in TRUNK_USER TRUNK_PASS TRUNK_DID; do [ -n "${!v:-}" ] || { echo "$v not set in $SECRETS (needed with TRUNK_HOST)"; exit 1; }; done
  case "$TRUNK_PASS" in *'|'*|*'&'*|*'\'*) echo "TRUNK_PASS: avoid | & \\ (sed substitution)"; exit 1;; esac
  ip=$(getent ahostsv4 "$TRUNK_HOST" | awk 'NR==1{print $1}')
  [ -n "$ip" ] || { echo "cannot resolve TRUNK_HOST=$TRUNK_HOST"; exit 1; }
  TRUNK_PERMIT="$ip/255.255.255.255"
  TRUNK_PORT=${TRUNK_PORT:-5060}; TRUNK_LOCAL_PORT=${TRUNK_LOCAL_PORT:-5160}
fi

BK="$ETC/rejack-backup-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$BK"
for f in pjsip.conf pjsip_trunk.conf extensions.conf extensions_trunk.conf rtp.conf voicemail.conf modules.conf; do [ -f "$ETC/$f" ] && cp -p "$ETC/$f" "$BK/"; done
echo "backup: $BK"

tmp=$(mktemp)
sed -e "s|@LAN_CIDR@|$LAN_CIDR|" -e "s|@TRUNK_PERMIT@|$TRUNK_PERMIT|" -e "s|@PW_100@|$PW_100|" -e "s|@PW_101@|$PW_101|" -e "s|@PW_102@|$PW_102|" \
    "$DIR/asterisk/pjsip.conf.in" > "$tmp"
install -o asterisk -g asterisk -m 0640 "$tmp" "$ETC/pjsip.conf"
rm -f "$tmp"
tmp=$(mktemp)
sed -e "s|@VM_PIN_100@|$VM_PIN_100|" -e "s|@VM_PIN_101@|$VM_PIN_101|" -e "s|@VM_PIN_102@|$VM_PIN_102|" \
    "$DIR/asterisk/voicemail.conf.in" > "$tmp"
install -o asterisk -g asterisk -m 0640 "$tmp" "$ETC/voicemail.conf"
rm -f "$tmp"
tmp=$(mktemp); tmp2=$(mktemp)
if [ -n "${TRUNK_HOST:-}" ]; then
  subst=(-e "s|@TRUNK_USER@|$TRUNK_USER|g" -e "s|@TRUNK_HOST@|$TRUNK_HOST|g" -e "s|@TRUNK_PASS@|$TRUNK_PASS|g" -e "s|@TRUNK_DID@|$TRUNK_DID|g" \
         -e "s|@TRUNK_PORT@|$TRUNK_PORT|g" -e "s|@TRUNK_LOCAL_PORT@|$TRUNK_LOCAL_PORT|g")
  sed "${subst[@]}" "$DIR/asterisk/pjsip_trunk.conf.in" > "$tmp"
  sed "${subst[@]}" "$DIR/asterisk/extensions_trunk.conf.in" > "$tmp2"
  echo "trunk: $TRUNK_USER@$TRUNK_HOST:$TRUNK_PORT from local port $TRUNK_LOCAL_PORT ($TRUNK_PERMIT)"
else
  echo "; no SIP trunk (TRUNK_HOST not set in pbx/secrets.env)" > "$tmp"
  printf '%s\n' '; no SIP trunk (TRUNK_HOST not set in pbx/secrets.env)' '[from-trunk]' '[trunk-out]' \
    '; 911 TEST ONLY without a trunk: congestion, so ReJack'"'"'s 911 fallback runs' '[emergency]' \
    'exten => 911,1,NoOp(TEST 911 from ${CALLERID(num)} — no trunk, not a real emergency call)' \
    ' same => n,Congestion(5)' > "$tmp2"
  echo "trunk: none"
fi
install -o asterisk -g asterisk -m 0640 "$tmp" "$ETC/pjsip_trunk.conf"
install -o asterisk -g asterisk -m 0640 "$tmp2" "$ETC/extensions_trunk.conf"
rm -f "$tmp" "$tmp2"
install -o asterisk -g asterisk -m 0640 "$DIR/asterisk/modules.conf" "$ETC/modules.conf"
install -o asterisk -g asterisk -m 0640 "$DIR/asterisk/extensions.conf" "$ETC/extensions.conf"
install -o asterisk -g asterisk -m 0640 "$DIR/asterisk/rtp.conf" "$ETC/rtp.conf"

systemctl restart asterisk
sleep 3
asterisk -rx "pjsip show endpoints" | grep -E 'Endpoint:|Contact:' || true
asterisk -rx "voicemail show users" || true
[ -n "${TRUNK_HOST:-}" ] && { sleep 2; asterisk -rx "pjsip show registrations" || true; }
echo "installed. Logs: sudo asterisk -rvvv"
