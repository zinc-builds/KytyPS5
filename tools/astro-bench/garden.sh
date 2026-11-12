#!/bin/bash
# Benchmark Astro Bot's Sky Garden: get into the level, then record KYTY_PERF_LOG lines for a
# fixed window once it is running.
# Usage: garden.sh <label> [extra kyty_emulator flags...]
# Result: /tmp/bench-<label>.log and a summary line on stdout. With a binary that prints METRICS
#   lines (benchMetrics.h), the summary line ends with per-draw costs over the same frame window
#   and the full set goes to /tmp/garden-<label>.json (metrics.py documents every field):
#   cmd_ipd/cmd_cpd  command-thread user-mode instructions/cycles per draw (the tightest numbers)
#   cmd_ns = cmd_uns + cmd_sns  its CPU ns per draw, user + sys
#   wait_prep/wait_rec  its ns per draw waiting for the prepare workers / the recorder
#   prep_ns  mean worker time per prepared draw; rej_f  rejected prepared draws per frame;
#   drains_kd  queue drains per 1,000 draws
# GARDEN_ROUTE=lvl (default): boot straight into the level. The game's own "-lvl <level>" boot
#   argument is added to /app0/args.txt through KYTY_APP0_OVERLAY (needs an emulator with it).
# GARDEN_ROUTE=menu: the original route (title, save 1, galaxy map, fly to Sky Garden, dive in)
#   driven by vpad.py. Numbers from the two routes are not interchangeable; see FINDINGS.md.
# GARDEN_LEVEL=<name> (lvl route): boot another level (the game's "-lvl" name, e.g. a <File> from
#   product_levels.xml; levels.sh runs a list of them). Runs of another level stay out of the
#   result store, whose references are Sky Garden's. GARDEN_BOOT_TIMEOUT=<s> (90): time to its start.
label=$1
shift
# Exclusive host-wide lock; the emulator inherits it (see benchlock.sh).
. "$(dirname "$0")/benchlock.sh"
benchlock_acquire x "garden.sh $label" || exit 1
log=/tmp/bench-$label.log
# A FIFO without a reader (vpad.py gone) would block forever while holding the bench lock.
pad() { timeout 5 bash -c 'echo "$1" > /tmp/vpad.fifo' _ "$*" || { echo "vpad.py is not reading /tmp/vpad.fifo" >&2; exit 1; }; }
wait_s() { timeout "$1" tail -f /dev/null; }
wait_log() { # wait_log <regex> <timeout_s>
	local end=$((SECONDS + $2))
	until grep -q -E "$1" "$log" 2>/dev/null; do
		((SECONDS > end)) && { echo "timeout waiting for: $1" >&2; return 1; }
		pgrep -f '^\./kyty_emulator --game' >/dev/null || { echo "emulator exited ($(tail -1 "$log" 2>/dev/null))" >&2; return 1; }
		wait_s 1
	done
	return 0
}

for _ in $(seq 20); do pgrep -f '^\./kyty_emulator --game' >/dev/null || break; wait_s 1; done
pgrep -f '^\./kyty_emulator --game' >/dev/null && { echo "emulator already running" >&2; exit 1; }
route=${GARDEN_ROUTE:-lvl}
level=${GARDEN_LEVEL:-aerial_garden}
[ "$route" = menu ] && [ "$level" != aerial_garden ] && { echo "GARDEN_LEVEL needs GARDEN_ROUTE=lvl" >&2; exit 1; }
case "$route" in
lvl)
	# The game's args.txt plus -lvl. Kept out of the install dir: only this run sees it.
	overlay=$(mktemp -d /tmp/kyty-app0-XXXXXX) || exit 1
	printf '%s\r\n' "-package -sequence product -mode product -odxBinary -perfDisplay false -debugDisplay false -debugInput false -lvl $level" > "$overlay/args.txt"
	export KYTY_APP0_OVERLAY=$overlay
	;;
menu) [ -p /tmp/vpad.fifo ] || { echo "vpad.py not running" >&2; exit 1; } ;;
*) echo "GARDEN_ROUTE must be lvl or menu" >&2; exit 1 ;;
esac
rm -f "$log"
# GARDEN_SHOT=<F>: save the frame shown at level frame F as /tmp/garden-<label>-shot.png (the
# emulator's KYTY_SHOT_FILE capture: the guest image, no overlay; needs a binary that has it).
# KYTY_SHOT_COUNT=<n> also saves the next n-1 presented frames as <shot>.ppm-<k>.ppm (the SHOT
# lines in the log give each one's flip number).
shot_png=/tmp/garden-$label-shot.png
rm -f "$shot_png"
if [ -n "$GARDEN_SHOT" ]; then
	export KYTY_SHOT_FILE=/tmp/kyty-shot-$label.req
	rm -f "$KYTY_SHOT_FILE"
fi
# On any failure (a timeout below, a signal), stop the emulator: left running, it would keep
# holding the bench lock. KEEP_RUNNING only applies to a run that reaches the level.
# Kill only this run's emulator (a child of run.sh): a pattern kill could hit the emulator of
# whoever takes the lock next.
cleanup() {
	[ -n "$overlay" ] && rm -rf "$overlay"
	[ -n "$KYTY_SHOT_FILE" ] && rm -f "$KYTY_SHOT_FILE" "/tmp/garden-$label-shot.ppm"
	[ -n "$run_pid" ] || return
	pkill -P "$run_pid" -x kyty_emulator; wait_s 3; pkill -9 -P "$run_pid" -x kyty_emulator
}
trap 'cleanup' EXIT
nohup "$(dirname "$0")/run.sh" "$label" "$@" >/dev/null 2>&1 &
run_pid=$!
wait_s 5
if [ "$route" = lvl ]; then
	# The game logs StartLevel with its short name (bumper_enemy_snowy_canyon: snowy_canyon), so
	# any StartLevel counts: with -lvl the first level started is the one asked for.
	wait_log "StartLevel |Level has started: title_controller_ship" "${GARDEN_BOOT_TIMEOUT:-90}" || exit 1
	grep -q "StartLevel " "$log" || {
		echo "booted to the title: this binary has no KYTY_APP0_OVERLAY (built before 45d400a8): run with GARDEN_ROUTE=menu" >&2
		exit 1
	}
else
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
fi
# The StartLevel line itself: PERF lines strictly after it belong to the level.
start_re="StartLevel "
[ "$route" = menu ] && start_re="StartLevel aerial_garden" # the world map may start first
start=$(grep -n -m1 "$start_re" "$log" | cut -d: -f1)
summary='{f+=$1; d+=$2; s+=$3; w+=$4; ill+=$5; wm+=$6; if ($6>wx) wx=$6; lo+=$7; n++}
	END {if (fw != "") f = fw * n; printf "%s: fps=%.2f draws/frame=%.0f draws/s=%.0f wt_faults/s=%.0f sigill/s=%.0f worst_ms=%.1f (max %.1f) low1=%.1f (n=%d)%s\n", l, f/n, d/n, s/n, w/n, ill/n, wm/n, wx, lo/n, n, note}'
fields() { sed -E 's/.*fps=([0-9.]+).*worst_ms=([0-9.]+) low1=([0-9.]+) draws_frame=([0-9]+) draws_s=([0-9]+).*wt_faults_s=([0-9]+).*sigill_s=([0-9]+).*/\1 \4 \5 \6 \7 \2 \3/'; }
perf_since_start() { tail -n +"$((start + 1))" "$log" | grep '^PERF'; }
last_frames() { perf_since_start | grep -o 'frames=[0-9]*' | tail -1 | cut -d= -f2; }
# Frame window (default): the game is frame-locked and the scripted opening flight is the same in
# every run, so game frames [F0, F1) after level start always show the same scene, whatever the
# build's speed. fps = frames / seconds between the PERF lines at F0 and F1. Needs a binary whose
# PERF lines carry frames= and t=; older ones fall back to the time window.
for _ in $(seq 10); do [ -n "$(last_frames)" ] && break; wait_s 1; done
base=$(perf_since_start | grep -m1 -o 'frames=[0-9]*' | cut -d= -f2)
window=${GARDEN_FRAMES:-1000-1600}
[ -n "$GARDEN_SHOT" ] && [ -n "$base" ] && printf '%s %s\n' "$((base + GARDEN_SHOT))" "/tmp/garden-$label-shot.ppm" > "$KYTY_SHOT_FILE"
if [ -n "$base" ] && [ -z "$GARDEN_WINDOW" ] && [ "$window" != time ]; then
	f0=$((base + ${window%-*})) f1=$((base + ${window#*-}))
	end=$((SECONDS + ${window#*-} / 5 + 30)) # even at 5 FPS
	until [ "$(last_frames)" -ge "$f1" ] 2>/dev/null; do
		((SECONDS > end)) && { echo "timeout waiting for frame $f1" >&2; exit 1; }
		pgrep -f '^\./kyty_emulator --game' >/dev/null || { echo "emulator exited ($(tail -1 "$log"))" >&2; exit 1; }
		wait_s 1
	done
	perf_since_start | awk -v f0="$f0" -v f1="$f1" '
		{ match($0, /frames=[0-9]+/); fr = substr($0, RSTART + 7, RLENGTH - 7) + 0
		  match($0, / t=[0-9.]+/); t = substr($0, RSTART + 3, RLENGTH - 3) + 0 }
		fr >= f0 && a == "" { a = fr; ta = t; inwin = 1 }
		inwin { print }
		fr >= f1 { printf "FRAMEFPS %.4f\n", (fr - a) / (t - ta); exit }' > "/tmp/garden-$label.window"
	fw=$(grep -m1 '^FRAMEFPS' "/tmp/garden-$label.window" | cut -d' ' -f2)
	# Per-draw costs over the same window (metrics.py; needs a binary with METRICS lines), as
	# fields appended to the summary line and as /tmp/garden-<label>.json.
	rm -f "/tmp/garden-$label.json"
	install=${KYTY_INSTALL:-$(git -C "$(dirname "$0")" rev-parse --show-toplevel)/_Build/linux/install}
	kenv=$(env | grep -E '^KYTY_' | grep -vE '^KYTY_(BENCH_LOCKED|APP0_OVERLAY|INSTALL|SHOT_FILE)=' | sort | tr '\n' ' ')
	# Build identity: the binary's hash, and the build mode and profile that refbuild.sh/build.sh
	# record next to the install (plain when there is no BUILD_MODE file).
	exe=$(sha256sum "$install/kyty_emulator" 2>/dev/null | cut -c1-16)
	bmode=$(cat "$install/../BUILD_MODE" 2>/dev/null)
	mode=$(sed -n 's/.*\bmode=\([a-z]*\).*/\1/p' <<<"$bmode") profile=$(sed -n 's/.*\bsha256=\([0-9a-f]*\).*/\1/p' <<<"$bmode")
	extra=$(python3 -I "$(dirname "$0")/metrics.py" "$log" "$start" "$f0" "$f1" "/tmp/garden-$label.json" \
		label="$label" window="$window" route="$route" install="$install" flags="$*" env="${kenv% }" \
		time="$(date -Iseconds)" exe="$exe" build_mode="${mode:-plain}" profile="$profile" level="$level")
	# Every measured run goes into the shared result store (results.sh), so references are reused.
	case "$label" in *-warmup) ;; *) [ "$level" = aerial_garden ] && [ -f "/tmp/garden-$label.json" ] && python3 -I "$(dirname "$0")/results.py" add "/tmp/garden-$label.json" ;; esac
	grep '^PERF' "/tmp/garden-$label.window" | fields |
		awk -v l="$label" -v fw="$fw" -v note=" frames=$window$extra" "$summary"
else
	wait_s "${GARDEN_SETTLE:-25}"       # opening flight / shader warm-up
	from=$(wc -l < "$log")
	wait_s "${GARDEN_WINDOW:-45}"
	to=$(wc -l < "$log")
	sed -n "${from},${to}p" "$log" | grep '^PERF' | fields |
		awk -v l="$label" -v fw="" -v note=" time window" "$summary"
fi
grep -E '^AMD CPU compatibility' "$log" | head -3
if [ -n "$GARDEN_SHOT" ]; then
	# The emulator prints SHOT once it has saved the frame (which may lie past the window). A
	# binary without KYTY_SHOT_FILE never does: stop ~2 s of frames after the shot's frame.
	for _ in $(seq 60); do
		[ "$(grep -c '^SHOT ' "$log")" -ge "${KYTY_SHOT_COUNT:-1}" ] && break
		[ "$(last_frames)" -ge "$((base + GARDEN_SHOT + 120 + ${KYTY_SHOT_COUNT:-1}))" ] 2>/dev/null && break
		wait_s 1
	done
	if [ -s "/tmp/garden-$label-shot.ppm" ] && magick "/tmp/garden-$label-shot.ppm" "$shot_png" 2>/dev/null; then
		echo "screenshot: $shot_png (level frame $GARDEN_SHOT)"
	else
		echo "screenshot: none (frame $GARDEN_SHOT not reached, or a binary without KYTY_SHOT_FILE)" >&2
	fi
fi
echo "level start at log line $start" >&2
if [ -n "$KEEP_RUNNING" ]; then trap - EXIT; fi
