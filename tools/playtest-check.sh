#!/bin/bash
# playtest-check.sh [--hold shared|exclusive | --user-emulator] [--level NAME] [--play S]: the
# user's path through tools/playtest.sh, end to end. Re-run it after any change to playtest.sh,
# benchlock.sh or the recorder's tools.
#
# It starts playtest.sh the way the user does: from a clean environment (env -i with only HOME,
# PATH and the display/session variables; no KYTY_* set) and without holding the bench lock, so
# playtest.sh takes it itself. It waits for the level to start, plays --play seconds (default 20)
# with no input, stops that emulator by its PID (SIGTERM) and checks:
#   playtest.sh started the emulator and exited after it, printing the report and the session dir;
#   the session has frames, a StartLevel event, report.json, and system samples with a VRAM reading
#   (when nvidia-smi exists).
# --hold shared      an agent build holds the lock first (a 45 s sleep under benchlock.sh build):
#                    playtest.sh must wait, say so, and start when it ends.
# --hold exclusive   an agent benchmark holds it first, with a stand-in agent emulator (a copy of
#                    sleep named kyty_emulator, run from the build cache dir) for 45 s: same.
# --user-emulator    a stand-in for the user's own emulator (the same copy of sleep, run from a
#                    directory outside the agent worktrees and the build cache): playtest.sh must
#                    refuse at once with "Astro Bot is already running"; no game is launched.
# Run it outside any lock hold of your own. Exit status 0 when every check passes.
set -u
here=$(cd "$(dirname "$0")" && pwd)
hold= level=aerial_garden play=20 user=0
while [ $# -gt 0 ]; do
	case "$1" in
	--hold) hold=$2; shift ;;
	--user-emulator) user=1 ;;
	--level) level=$2; shift ;;
	--play) play=$2; shift ;;
	-h | --help) sed -n '2,25p' "$0"; exit 0 ;;
	*) echo "playtest-check.sh: unknown argument $1" >&2; exit 2 ;;
	esac
	shift
done
case "$hold" in "" | shared | exclusive) ;; *) echo "playtest-check.sh: --hold shared|exclusive" >&2; exit 2 ;; esac
if [ -n "${KYTY_BENCH_LOCKED:-}" ]; then
	echo "playtest-check.sh: run it outside your own bench lock hold (playtest.sh takes the lock itself)" >&2
	exit 2
fi
builds=${KYTY_BUILDS:-$HOME/.cache/kyty-builds}
tmp=$(mktemp -d "${TMPDIR:-/tmp}/playtest-check-XXXXXX") || exit 1
dir=$HOME/.cache/kyty-playtest/check-${hold:-${user/1/user}}-$(date +%Y%m%d-%H%M%S)
dir=${dir/check-0-/check-free-}
out=$tmp/playtest.out
fails=0
check() { # check <what> <command...>
	local what=$1
	shift
	if "$@"; then echo "PASS $what"; else echo "FAIL $what"; fails=$((fails + 1)); fi
}
holder= standin=
cleanup() {
	[ -n "$standin" ] && kill "$standin" 2>/dev/null
	[ -n "$holder" ] && kill "$holder" 2>/dev/null
	rm -rf "$tmp"
}
trap cleanup EXIT
# A process named kyty_emulator that only sleeps.
cp "$(command -v sleep)" "$tmp/kyty_emulator" || exit 1

if [ "$user" = 1 ]; then
	mkdir -p "$tmp/user" && (cd "$tmp/user" && exec "$tmp/kyty_emulator" 60) 9>&- 8>&- &
	standin=$!
	sleep 0.5
fi
if [ -n "$hold" ]; then
	# Hold the lock as an agent would, at priority 1 so the hold is not queued behind real work.
	if [ "$hold" = shared ]; then
		KYTY_BENCH_PRIO=1 "$here/astro-bench/benchlock.sh" build sleep 45 9>&- 8>&- &
	else
		KYTY_BENCH_PRIO=1 "$here/astro-bench/benchlock.sh" run bash -c "cd '$builds' && exec '$tmp/kyty_emulator' 45" 9>&- 8>&- &
	fi
	holder=$!
	for _ in $(seq 600); do
		case "$("$here/astro-bench/benchlock.sh" status | head -1)" in free) sleep 0.5 ;; *) break ;; esac
	done
	grep -q "pid=$holder " "$("$here/astro-bench/benchlock.sh" status >/dev/null; echo "${KYTY_BENCH_LOCK:-/tmp/kyty-bench.lock}.owner")" ||
		{ echo "playtest-check.sh: the stand-in hold did not get the lock (queue busy?)" >&2; exit 1; }
	echo "playtest-check.sh: lock held $hold by the stand-in (pid $holder)"
fi

# The user's launch: a login shell from a clean environment.
clean=(HOME="$HOME" PATH="$PATH")
for v in DISPLAY WAYLAND_DISPLAY XAUTHORITY XDG_RUNTIME_DIR XDG_SESSION_TYPE DBUS_SESSION_BUS_ADDRESS; do
	[ -n "${!v:-}" ] && clean+=("$v=${!v}")
done
t0=$SECONDS
env -i "${clean[@]}" bash -lc "exec '$here/playtest.sh' '$level' --dir '$dir'" > "$out" 2>&1 9>&- 8>&- &
launcher=$!

if [ "$user" = 1 ]; then
	for _ in $(seq 100); do [ -d "/proc/$launcher" ] || break; sleep 0.1; done
	wait "$launcher"
	rc=$?
	cat "$out"
	check "refused with status 1 (got $rc)" [ "$rc" = 1 ]
	check "said the user's emulator is running" grep -q "Astro Bot is already running; close it first" "$out"
	check "launched nothing" [ ! -e "$dir/launch.txt" ]
	exit $((fails > 0))
fi

# The emulator playtest.sh starts: the kyty_emulator with this session dir in its environment.
emu=
deadline=$((SECONDS + 1200))
while [ -z "$emu" ] && [ -d "/proc/$launcher" ] && ((SECONDS < deadline)); do
	for p in $(pgrep -x kyty_emulator); do
		tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null | grep -qx "KYTY_PLAYTEST_DIR=$dir" && emu=$p
	done
	sleep 1
done
waited=$((SECONDS - t0))
cat "$out"
check "playtest.sh started the emulator (after ${waited}s)" [ -n "$emu" ]
if [ -n "$hold" ]; then
	check "said it was waiting for the benchmark" grep -q "An automated benchmark is running" "$out"
	check "started after the hold ended (waited ${waited}s)" [ "$waited" -ge 30 ]
fi
if [ -n "$emu" ]; then
	for _ in $(seq 300); do grep -q '"StartLevel"' "$dir/events.jsonl" 2>/dev/null && break; sleep 1; done
	check "the level started" grep -q '"StartLevel"' "$dir/events.jsonl"
	sleep "$play"
	tr '\0' '\n' < "/proc/$emu/environ" 2>/dev/null | grep -qx "KYTY_PLAYTEST_DIR=$dir" && kill -TERM "$emu"
fi
for _ in $(seq 240); do [ -d "/proc/$launcher" ] || break; sleep 0.5; done
if [ -d "/proc/$launcher" ]; then
	echo "playtest-check.sh: playtest.sh did not exit 120 s after the emulator was stopped" >&2
	fails=$((fails + 1))
else
	wait "$launcher"
	echo "--- playtest.sh output after the start ---"
	tail -n +2 "$out" | sed -n '/emulator exited/,$p'
fi
check "printed the session directory at exit" grep -q "playtest: session directory: $dir" "$out"
check "wrote report.json" [ -s "$dir/report.json" ]
check "recorded over 600 frames ($(($(wc -l < "$dir/frames.csv" 2>/dev/null || echo 1) - 1)))" \
	[ "$(wc -l < "$dir/frames.csv" 2>/dev/null || echo 0)" -gt 601 ]
if command -v nvidia-smi >/dev/null; then
	check "system samples have a VRAM reading" grep -q '"vram_used_mb":' "$dir/system.jsonl"
fi
echo "playtest-check.sh: session $dir; $fails failed"
exit $((fails > 0))
