#!/bin/bash
# Benchmark Astro Bot's Sky Garden: boot, load save 1, fly to Sky Garden, dive in, then record
# KYTY_PERF_LOG lines for a fixed window once the level is running. Needs vpad.py running.
# Usage: garden.sh <label> [extra kyty_emulator flags...]
# Result: /tmp/bench-<label>.log and a summary line on stdout.
label=$1
shift
# Exclusive host-wide lock; the emulator inherits it (see benchlock.sh).
. "$(dirname "$0")/benchlock.sh"
benchlock_acquire x "garden.sh $label" || exit 1
log=/tmp/bench-$label.log
pad() { echo "$*" > /tmp/vpad.fifo; }
wait_s() { timeout "$1" tail -f /dev/null; }
wait_log() { # wait_log <regex> <timeout_s>
	local end=$((SECONDS + $2))
	until grep -q -E "$1" "$log" 2>/dev/null; do
		((SECONDS > end)) && { echo "timeout waiting for: $1" >&2; return 1; }
		pgrep -f '^\./kyty_emulator --game' >/dev/null || { echo "emulator exited" >&2; return 1; }
		wait_s 1
	done
	return 0
}

for _ in $(seq 20); do pgrep -f '^\./kyty_emulator --game' >/dev/null || break; wait_s 1; done
pgrep -f '^\./kyty_emulator --game' >/dev/null && { echo "emulator already running" >&2; exit 1; }
[ -p /tmp/vpad.fifo ] || { echo "vpad.py not running" >&2; exit 1; }
rm -f "$log"
# On any failure (a timeout below, a signal), stop the emulator: left running, it would keep
# holding the bench lock. KEEP_RUNNING only applies to a run that reaches the level.
# Kill only this run's emulator (a child of run.sh): a pattern kill could hit the emulator of
# whoever takes the lock next.
cleanup() { pkill -P "$run_pid" -x kyty_emulator; wait_s 3; pkill -9 -P "$run_pid" -x kyty_emulator; }
trap 'cleanup' EXIT
nohup "$(dirname "$0")/run.sh" "$label" "$@" >/dev/null 2>&1 &
run_pid=$!
wait_s 5
wait_log 'Level has started: title_controller_ship' 90 || exit 1
wait_s 4; pad press a 0.3          # title: press any button
wait_s 5; pad press a 0.3          # save slot 1
wait_log 'Level has started: worldmap' 60 || exit 1
wait_s 6; pad stick L 0 1 2.0      # fly up to the Gorilla Nebula
wait_s 3; pad press a 0.3          # enter
wait_s 12; pad stick L 0 1 1.4     # fly up to Sky Garden
wait_s 3; pad hold a 3.5           # dive in (hold)
for nudge in 0.6 0.9 0.6; do         # the ship sometimes stops short of the planet
	wait_log 'StartLevel aerial_garden' 25 2>/dev/null && break
	pad stick L 0 1 "$nudge"; wait_s 3; pad hold a 4
done
wait_log 'StartLevel aerial_garden' 60 || exit 1
start=$(wc -l < "$log")
wait_s "${GARDEN_SETTLE:-25}"       # opening flight / shader warm-up
from=$(wc -l < "$log")
wait_s "${GARDEN_WINDOW:-45}"
to=$(wc -l < "$log")
sed -n "${from},${to}p" "$log" | grep '^PERF' |
	sed -E 's/.*fps=([0-9.]+).*worst_ms=([0-9.]+) low1=([0-9.]+) draws_frame=([0-9]+) draws_s=([0-9]+).*wt_faults_s=([0-9]+).*sigill_s=([0-9]+).*/\1 \4 \5 \6 \7 \2 \3/' |
	awk -v l="$label" '{f+=$1; d+=$2; s+=$3; w+=$4; ill+=$5; wm+=$6; if ($6>wx) wx=$6; lo+=$7; n++}
	     END {printf "%s: fps=%.2f draws/frame=%.0f draws/s=%.0f wt_faults/s=%.0f sigill/s=%.0f worst_ms=%.1f (max %.1f) low1=%.1f (n=%d)\n", l, f/n, d/n, s/n, w/n, ill/n, wm/n, wx, lo/n, n}'
grep -E '^AMD CPU compatibility' "$log" | head -3
echo "level start at log line $start" >&2
if [ -n "$KEEP_RUNNING" ]; then trap - EXIT; fi
