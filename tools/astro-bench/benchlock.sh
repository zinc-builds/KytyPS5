#!/bin/bash
# One machine, several agents: a host-wide lock so benchmarks never overlap each other or a build.
#
#   benchlock.sh run   <cmd...>   exclusive: anything that launches kyty_emulator (bench, profile,
#                                 screenshots). garden.sh, run.sh and batch.sh take it themselves.
#   benchlock.sh build <cmd...>   shared: builds, unit tests, other CPU-heavy work. Shared holders
#                                 run together; a benchmark waits for all of them, and they wait
#                                 for a running benchmark.
#   benchlock.sh status           who holds the lock.
#
# The exclusive lock lives on an inherited file descriptor, so an emulator left running
# (KEEP_RUNNING=1) keeps holding it until it exits: kill your emulator when you are done.
# KYTY_BENCH_LOCK_TIMEOUT: seconds to wait (default 5400). KYTY_BENCH_LOCK: lock file path.
#
# Sourced by the bench scripts for benchlock_acquire; executed for the subcommands above.

KYTY_BENCH_LOCK=${KYTY_BENCH_LOCK:-/tmp/kyty-bench.lock}

benchlock_owner_file() { echo "$KYTY_BENCH_LOCK.owner"; }

# benchlock_acquire <x|s> <what>: take the lock on fd 9 (no-op if this process tree holds it).
benchlock_acquire() {
	local mode=$1 what=$2
	case "$KYTY_BENCH_LOCKED" in
	x | "$mode") return 0 ;;
	s) echo "benchlock: inside a shared (build) hold; cannot take the exclusive lock" >&2; return 1 ;;
	esac
	exec 9>>"$KYTY_BENCH_LOCK" || return 1
	# Turnstile: everyone passes through the gate, and a waiter keeps it while it waits for the
	# lock. flock alone has no queue, so a benchmark could wait forever while builds keep
	# overlapping each other; with the gate, later arrivals queue behind it instead.
	exec 8>>"$KYTY_BENCH_LOCK.gate" || return 1
	local timeout=${KYTY_BENCH_LOCK_TIMEOUT:-5400} start=$SECONDS
	if ! flock -n -x 8 || ! flock -n "-$mode" 9; then
		echo "benchlock: waiting for $KYTY_BENCH_LOCK (holder: $(cat "$(benchlock_owner_file)" 2>/dev/null || echo unknown))" >&2
		if ! flock -w "$timeout" -x 8 ||
			! flock -w "$((timeout - (SECONDS - start) > 0 ? timeout - (SECONDS - start) : 0))" "-$mode" 9; then
			echo "benchlock: gave up after ${timeout}s" >&2
			exec 8>&- 9>&-
			return 1
		fi
	fi
	exec 8>&-
	export KYTY_BENCH_LOCKED=$mode
	echo "$mode $what pid=$$ cwd=$PWD since=$(date +%T)" > "$(benchlock_owner_file)"
	return 0
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
	cmd=$1
	shift
	case "$cmd" in
	run)
		benchlock_acquire x "run: $*" || exit 1
		exec "$@"
		;;
	build)
		benchlock_acquire s "build: $*" || exit 1
		exec "$@"
		;;
	status)
		if flock -n -x "$KYTY_BENCH_LOCK" true 2>/dev/null; then
			echo "free"
		elif flock -n -s "$KYTY_BENCH_LOCK" true 2>/dev/null; then
			echo "shared (builds/tests running)"
		else
			echo "exclusive: $(cat "$(benchlock_owner_file)" 2>/dev/null || echo unknown)"
		fi
		;;
	*)
		sed -n '2,15p' "$0"
		exit 2
		;;
	esac
fi
