#!/usr/bin/env bash
# Kill everything step3_launch_5band.sh started (5 fake producers +
# sdrangelsrv). Run from the sdrangel-udmabufinput repo root.

set -uo pipefail

SCRATCH="codex-handoff/plan-03/temp/5band"
PIDFILE="$SCRATCH/pids.txt"

if [ ! -f "$PIDFILE" ]; then
  echo "no pidfile at $PIDFILE -- nothing to do" >&2
  exit 0
fi

count=0
while read -r pid; do
  if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
    kill "$pid" 2>/dev/null && count=$((count + 1))
  fi
done < "$PIDFILE"

echo "sent SIGTERM to $count process(es) from $PIDFILE"
