#!/usr/bin/env bash
# Dev launcher: baresip v4.12 from ../../deps with Rejack and project-local state.
# Usage: rejack/run/start.sh [extra baresip args, e.g. -t 10]
set -eu
RUN=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$RUN/../.." && pwd)
export LD_LIBRARY_PATH="$ROOT/deps/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export REJACK_EMERGENCY_ENV="$RUN/emergency.env"     # copy of conf/emergency.env.example, filled in
export REJACK_OUTBOX="$RUN/outbox"
export REJACK_EMERGENCY_LOG="$RUN/emergency.log"
mkdir -p "$RUN/recordings" "$RUN/outbox"
# generate config from the template with this checkout's absolute path
sed "s|@ROOT@|$ROOT|g" "$RUN/config.in" > "$RUN/config"
unset DISPLAY                                        # silence X11 noise over SSH
exec "$ROOT/deps/bin/baresip" -f "$RUN" "$@"
