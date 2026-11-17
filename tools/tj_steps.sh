#!/usr/bin/env bash
# tj_steps.sh — step through TigerJet register writes one at a time, with a human watching the hardware.
#
# For each step: press Enter -> the write happens -> BEEP -> watch the dongle LED and the phone's
# "No Line" indicator for WATCH seconds -> BEEP -> type what you saw (Enter = no change).
# Input pins are recorded automatically before/after every step. Everything goes to the log file.
#
# Usage: tools/tj_steps.sh <snapshot.bin> [steps-file]
#   snapshot.bin  from `tjprobe activate` — restored first so we start from the original state
#   steps-file    lines of "REG VAL  description" (hex). Default: the type-2 activation, one write per step.
set -u
DIR=$(cd "$(dirname "$0")" && pwd)
TJ="$DIR/tjprobe"
SNAP=${1:?usage: $0 snapshot.bin [steps-file]}
WATCH=${WATCH:-10}
LOG=${LOG:-$DIR/tj_steps-$(date +%Y%m%d-%H%M%S).log}

default_steps() {
cat <<'EOF'
00 c0  ctl: reset/enable phase (TjIpSys step 1)
02 20  reg 0x02 = 0x20
00 40  ctl: run phase
02 00  reg 0x02 = 0x00
3c 32  reg 0x3c
30 80  codec 0x30
31 3e  codec 0x31
32 00  codec 0x32
33 7d  codec 0x33
EOF
}

steps() { if [ $# -gt 0 ] && [ -f "$1" ]; then grep -v -E '^\s*(#|$)' "$1"; else default_steps; fi; }

beep() { printf '\a'; }
say()  { printf '%s\n' "$*" | tee -a "$LOG"; }

say "# tj_steps $(date -Is)  snapshot=$SNAP  watch=${WATCH}s"
say "# Restoring the original registers first..."
"$TJ" restore "$SNAP" 2>&1 | grep -v '^#' | tee -a "$LOG" >/dev/null
say "BASELINE  $("$TJ" inputs | tail -1)"
echo
echo "Before we start: note what the LED and the phone show right now (expect: LED solid, 'No Line')."
read -r -p "Baseline observation (Enter = LED solid, No Line): " obs
say "  observed: ${obs:-LED solid, No Line}"

n=0
while read -r reg val desc; do
	n=$((n + 1))
	echo
	echo "=== Step $n: write 0x$reg = 0x$val  ($desc) ==="
	read -r -p "Press Enter, then turn around and watch (${WATCH}s, beep at start and end)... " _ </dev/tty
	out=$("$TJ" write "$reg" "$val" | tail -2)
	beep
	for ((i = WATCH; i > 0; i--)); do printf '\r  watching... %2d ' "$i"; sleep 1; done
	beep
	printf '\r                  \n'
	say "STEP $n  0x$reg=0x$val  ($desc)"
	say "  $(echo "$out" | head -1)"
	say "  $(echo "$out" | tail -1)"
	read -r -p "What changed? (Enter = nothing): " obs </dev/tty
	say "  observed: ${obs:-no change}"
done < <(steps "${2:-}")

echo
say "# Done. Final state left as-is. Undo: $TJ restore $SNAP   (or replug the dongle)"
echo "Log: $LOG"
