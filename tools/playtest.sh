#!/bin/bash
# playtest.sh [level] [options]: play Astro Bot with the playtest flight recorder on.
#
#   tools/playtest.sh                  start at the title screen (your save)
#   tools/playtest.sh aerial_garden    boot straight into a level (the game's "-lvl" name)
#
# Every hitch, slow second, freeze, new unresolved import and crash is recorded on its own, with
# enough context to diagnose it afterwards; press the marker when something looks or feels wrong:
#   F9 on the keyboard, or the PS button / the DualSense microphone button on the pad
#   (a green "Marker N saved" pill confirms it in the top-left corner for 2.5 s).
# At exit it prints the session directory and the one-page report (tools/playtest-report.py).
#
# It runs the newest PGO build of dev under ~/.cache/kyty-builds that has the recorder, with the
# bench's flags (run.sh: --amd-cpu, the warm shader store, Mailbox presentation at 60 Hz vblanks,
# 1920x1080) and the default thread placement, so what is played is what the bench measures.
# It refuses to start while a benchmark holds the bench lock or another emulator runs, waits for
# builds (queued at priority 1), and holds the lock for the session, so no agent benchmark runs
# while you play.
# Options:
#   --install DIR  another install dir (or a build key under ~/.cache/kyty-builds)
#   --dir DIR      session directory (default ~/.cache/kyty-playtest/<timestamp>)
#   --no-lock      don't take the bench lock (agents' benchmarks may then run alongside)
#   --             the rest goes to kyty_emulator as extra flags
# Environment: KYTY_GAME (default ~/Games/PS5/PPSA21567.zar); PLAYTEST_ENV="K=V ..." extra
# emulator environment (e.g. KYTY_PLAYTEST_INJECT for fault injection).
# Saves: the install's own _SaveData (seeded from the play checkout when the install was built).
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(git -C "$here" rev-parse --show-toplevel 2>/dev/null)
level= lock=1 dir= extra=() install=
while [ $# -gt 0 ]; do
	case "$1" in
	--install) install=$2; shift ;;
	--level) level=$2; shift ;;
	--no-lock) lock=0 ;;
	--dir) dir=$2; shift ;;
	--) shift; extra=("$@"); break ;;
	-h | --help) sed -n '2,27p' "$0"; exit 0 ;;
	-*) echo "playtest.sh: unknown option $1" >&2; exit 2 ;;
	*) level=$1 ;;
	esac
	shift
done
builds=${KYTY_BUILDS:-$HOME/.cache/kyty-builds}
has_recorder() { [ -x "$1/kyty_emulator" ] && grep -q -a -m1 'KYTY_PLAYTEST' "$1/kyty_emulator"; }
if [ -z "$install" ]; then
	# The newest dev commit with a PGO install that has the recorder.
	best= best_n=-1
	for d in "$builds"/*-pgo-*/install; do
		[ -f "$d/../BUILD_MODE" ] || continue
		c=$(sed -n 's/.*commit=\([0-9a-f]\{40\}\) .*/\1/p' "$d/../BUILD_MODE")
		[ -n "$c" ] && git -C "$repo" merge-base --is-ancestor "$c" origin/dev 2>/dev/null || continue
		has_recorder "$d" || continue
		n=$(git -C "$repo" rev-list --count "$c")
		if [ "$n" -gt "$best_n" ] || { [ "$n" = "$best_n" ] && [ "$d/../BUILD_MODE" -nt "$best/../BUILD_MODE" ]; }; then
			best=$d best_n=$n
		fi
	done
	install=$best
	[ -n "$install" ] || { echo "playtest.sh: no PGO build of dev with the recorder under $builds (build one: KYTY_BUILD_MODE=pgo KYTY_PGO_PROFILE=\$(readlink -e ~/.cache/kyty-pgo/current.profdata) tools/astro-bench/refbuild.sh origin/dev)" >&2; exit 2; }
elif [ ! -d "$install" ] && [ -d "$builds/$install/install" ]; then
	install=$builds/$install/install
fi
install=$(cd "$install" 2>/dev/null && pwd) || { echo "playtest.sh: no such install: $install" >&2; exit 2; }
has_recorder "$install" || { echo "playtest.sh: $install/kyty_emulator has no playtest recorder (KYTY_PLAYTEST)" >&2; exit 2; }
bmode=$(cat "$install/../BUILD_MODE" 2>/dev/null || echo "plain (no BUILD_MODE file)")
case "$bmode" in *mode=pgo\ *) ;; *) echo "playtest.sh: note: not a PGO build ($bmode); about 10% slower than the build of record" >&2 ;; esac
# Another emulator would share the cores and the GPU.
for p in $(pgrep -x kyty_emulator); do
	echo "playtest.sh: another kyty_emulator is running (pid $p, cwd $(readlink "/proc/$p/cwd" 2>/dev/null)); not starting" >&2
	exit 1
done
if [ "$lock" = 1 ]; then
	. "$here/astro-bench/benchlock.sh"
	st=$("$here/astro-bench/benchlock.sh" status | head -1)
	case "$st" in exclusive*)
		echo "playtest.sh: an agent benchmark is running (${st#exclusive: }); try again in a few minutes" >&2
		exit 1 ;;
	esac
	# Builds hold it shared: wait for them (a few minutes at most), then hold it for the session.
	[ "$st" = free ] || echo "playtest.sh: waiting for agent builds to finish..." >&2
	# Priority 1: a person is waiting (ahead of every queued agent job).
	KYTY_BENCH_PRIO=1 KYTY_BENCH_LOCK_TIMEOUT=900 benchlock_acquire x "playtest.sh (the user is playing)" 2>/dev/null ||
		{ echo "playtest.sh: could not take the bench lock" >&2; exit 1; }
fi
dir=${dir:-$HOME/.cache/kyty-playtest/$(date +%Y%m%d-%H%M%S)}
mkdir -p "$dir" || exit 1
dir=$(cd "$dir" && pwd)
[ "$lock" = 1 ] && echo "x playtest.sh (the user is playing; session $dir) pid=$$ since=$(date +%T)" > "$(benchlock_owner_file)"
overlay=
if [ -n "$level" ]; then
	overlay=$(mktemp -d "${TMPDIR:-/tmp}/kyty-app0-XXXXXX") || exit 1
	printf '%s\r\n' "-package -sequence product -mode product -odxBinary -perfDisplay false -debugDisplay false -debugInput false -lvl $level" > "$overlay/args.txt"
fi
game=${KYTY_GAME:-$HOME/Games/PS5/PPSA21567.zar}
# run.sh's flags (the bench's), less --perf-overlay: the recorder counts with the overlay hidden
# (F2 shows it).
flags=(--game "$game" --shader-cache --skip-notice-screen --amd-cpu --present-mode Mailbox --vblank-frequency 60
	--screen-width 1920 --screen-height 1080 "${extra[@]}")
fast_env=(KYTY_PERF_LOG=1)
{
	echo "install: $install"
	echo "build_mode: $bmode"
	echo "commit: $(sed -n 's/.*commit=\([0-9a-f]*\).*/\1/p' <<<"$bmode")"
	echo "level: ${level:-(menu)}"
	echo "flags: ${flags[*]}"
	echo "env: ${fast_env[*]} ${PLAYTEST_ENV:-}"
	echo "saves: $install/_SaveData"
	echo "started: $(date -Iseconds)"
} > "$dir/launch.txt"
echo "playtest: session $dir"
echo "playtest: marker = F9, or the PS / microphone button; F2 shows the performance overlay"
start=$(date +%s)
(
	cd "$install" || exit 1
	# The emulator keeps the bench lock's descriptor (fd 9): the lock lasts as long as it runs.
	exec env ${fast_env[@]} ${PLAYTEST_ENV:-} KYTY_PLAYTEST=1 KYTY_PLAYTEST_DIR="$dir" \
		${overlay:+KYTY_APP0_OVERLAY=$overlay} ./kyty_emulator "${flags[@]}" > "$dir/emulator.log" 2>&1
) &
emu=$!
# Wait for the emulator process itself (the subshell execs into it), then sample it.
sampler=
for _ in $(seq 50); do [ -d "/proc/$emu" ] && [ "$(readlink "/proc/$emu/exe" 2>/dev/null)" = "$install/kyty_emulator" ] && break; sleep 0.1; done
if [ -d "/proc/$emu" ]; then
	nice -n 19 python3 -I "$here/playtest-sample.py" "$emu" "$dir/system.jsonl" 2 9>&- 8>&- &
	sampler=$!
fi
wait "$emu"
status=$?
echo "exit_status: $status" >> "$dir/launch.txt"
echo "ended: $(date -Iseconds) ($(( $(date +%s) - start )) s)" >> "$dir/launch.txt"
[ -n "$sampler" ] && kill "$sampler" 2>/dev/null
[ -n "$overlay" ] && rm -rf "$overlay"
if [ "$lock" = 1 ]; then exec 9>&- 8>&-; fi
# Screenshots: PNG next to each PPM, then drop the PPM (6 MB each).
if command -v magick >/dev/null; then
	for f in "$dir"/shots/*.ppm; do
		[ -e "$f" ] || continue
		magick "$f" "${f%.ppm}.png" 2>/dev/null && rm -f "$f"
	done
fi
echo "playtest: emulator exited with status $status"
python3 -I "$here/playtest-report.py" "$dir" || true
echo "playtest: session directory: $dir"
