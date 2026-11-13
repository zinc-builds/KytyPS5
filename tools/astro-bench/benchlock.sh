#!/bin/bash
# One machine, several agents: a host-wide lock so benchmarks never overlap each other or a build.
#
#   benchlock.sh run   <cmd...>   exclusive: anything that launches kyty_emulator (bench, profile,
#                                 screenshots). garden.sh, run.sh and batch.sh take it themselves.
#   benchlock.sh build <cmd...>   shared: builds, unit tests, other CPU-heavy work. Shared holders
#                                 run together; a benchmark waits for all of them, and they wait
#                                 for a running benchmark.
#   benchlock.sh status           who holds the lock, and the queue.
#
# Waiters queue: each drops a ticket in $KYTY_BENCH_LOCK.queue/ and only the head of the queue
# tries for the lock (flock on $KYTY_BENCH_LOCK, which stays the actual mutual exclusion).
# Order: effective priority, then arrival. The effective priority starts at KYTY_BENCH_PRIO (1 =
# critical path, 5 = default, 9 = background) and improves by one step for every KYTY_BENCH_AGE
# seconds waited (default 1200, 20 min; floor 1), so no class starves: a priority-9 ticket is
# ahead of priority-5 tickets that arrive 4 steps (80 min) after it, and reaches 1 in 160 min.
# KYTY_BENCH_AGE must be the same for every waiter on the host (each one orders the queue). Shared
# tickets right behind each other at the head go together; an exclusive ticket at the head holds
# back every shared ticket behind it, so neither side can starve the other. A ticket is removed as
# soon as its waiter holds the lock or gives up, and any waiter removes the tickets of processes
# that are gone. Copies of this script from before the queue still take the lock with flock alone
# (they can jump the queue, never break the exclusion).
#
# The exclusive lock lives on an inherited file descriptor (9), so an emulator left running
# (KEEP_RUNNING=1) keeps holding it until it exits: kill your emulator when you are done.
# KYTY_BENCH_LOCKED (set while held) makes nested calls no-ops.
# KYTY_BENCH_LOCK_TIMEOUT: seconds to wait in all (default 5400); on a timeout benchlock_acquire
# prints "timed out waiting for the bench lock" and returns 75, and the scripts exit with it.
# KYTY_BENCH_LOCK: lock file path.
#
# Sourced by the bench scripts for benchlock_acquire; executed for the subcommands above.

KYTY_BENCH_LOCK=${KYTY_BENCH_LOCK:-/tmp/kyty-bench.lock}

benchlock_owner_file() { echo "$KYTY_BENCH_LOCK.owner"; }
benchlock_queue_dir() { echo "$KYTY_BENCH_LOCK.queue"; }

# The start time of process <pid> in clock ticks since boot ("" if it is gone or a zombie): with
# the pid it names one process, so a reused pid does not keep a dead waiter's ticket alive, nor
# does a killed waiter whose parent has not reaped it yet.
benchlock_starttime() {
	local stat
	{ read -r stat < "/proc/$1/stat"; } 2>/dev/null || return 0
	stat=${stat##*) }
	set -- $stat
	case $1 in Z | X) return 0 ;; esac
	echo "${20}"
}

# Nanoseconds since the epoch (EPOCHREALTIME: no fork).
benchlock_now_ns() {
	if [ -n "$EPOCHREALTIME" ]; then
		local t=${EPOCHREALTIME/[.,]/}
		echo "$((10#$t * 1000))"
	else
		date +%s%N
	fi
}

# Prints the live tickets in queue order ("<file> <effective prio> <mode> <pid> <prio> <what...>"),
# removing dead ones. A ticket's name is <prio>-<arrival ns>-<pid>-<x|s>; its content "<pid>
# <starttime> <what>". Order: effective priority (KYTY_BENCH_PRIO minus one per KYTY_BENCH_AGE
# seconds waited, floor 1), then arrival.
benchlock_tickets() {
	local dir f name pid start what prio ns eff now age=${KYTY_BENCH_AGE:-1200}
	[[ "$age" =~ ^[1-9][0-9]*$ ]] || age=1200
	dir=$(benchlock_queue_dir)
	now=$(benchlock_now_ns)
	for f in "$dir"/[1-9]-*; do
		[ -e "$f" ] || continue
		name=${f##*/}
		read -r pid start what < "$f" 2>/dev/null || { rm -f "$f"; continue; }
		if [ -z "$pid" ] || [ "$(benchlock_starttime "$pid")" != "$start" ]; then
			rm -f "$f"
			continue
		fi
		prio=${name%%-*}
		ns=${name#*-}
		ns=${ns%%-*}
		eff=$((prio - (now - 10#$ns) / (age * 1000000000)))
		((eff < 1)) && eff=1
		echo "$eff $ns $name $eff ${name##*-} $pid $prio $what"
	done | sort -k1,1n -k2,2 | cut -d' ' -f3-
}

# Whether ticket <name> may try for the lock now: it is first, or it is shared and only shared
# tickets are ahead of it.
benchlock_may_try() {
	local mine=$1 mode=$2 name prio m rest
	while read -r name prio m rest; do
		[ "$name" = "$mine" ] && return 0
		[ "$mode" = s ] && [ "$m" = s ] && continue
		return 1
	done < <(benchlock_tickets)
	return 1 # our own ticket is gone: the caller re-creates it
}

# benchlock_acquire <x|s> <what>: take the lock on fd 9 (no-op if this process tree holds it).
benchlock_acquire() {
	local mode=$1 what=$2
	case "$KYTY_BENCH_LOCKED" in
	x | "$mode") return 0 ;;
	s) echo "benchlock: inside a shared (build) hold; cannot take the exclusive lock" >&2; return 1 ;;
	esac
	exec 9>>"$KYTY_BENCH_LOCK" || return 1
	local timeout=${KYTY_BENCH_LOCK_TIMEOUT:-5400} prio=${KYTY_BENCH_PRIO:-5}
	[[ "$prio" =~ ^[1-9]$ ]] || { echo "benchlock: KYTY_BENCH_PRIO must be 1-9" >&2; exec 9>&-; return 1; }
	# The ticket names this process: BASHPID, not $$, which a subshell shares with its parent.
	local dir ticket name me=$BASHPID start deadline=$((SECONDS + timeout)) said= ahead eff
	dir=$(benchlock_queue_dir)
	mkdir -p "$dir" 2>/dev/null && chmod 1777 "$dir" 2>/dev/null
	start=$(benchlock_starttime "$me")
	while :; do
		if [ -z "$ticket" ] || [ ! -e "$ticket" ]; then
			# Arrival time in ns, zero-padded so names sort in queue order.
			name=$prio-$(printf '%020d' "$(date +%s%N)")-$me-$mode
			ticket=$dir/$name
			echo "$me $start $what cwd=$PWD since=$(date +%T)" > "$dir/.tmp.$me" && mv -f "$dir/.tmp.$me" "$ticket" || {
				echo "benchlock: cannot write a ticket in $dir" >&2
				exec 9>&-
				return 1
			}
		fi
		if benchlock_may_try "$name" "$mode"; then
			# At the head: poll the lock without blocking (a waiter blocked inside flock would win
			# against a higher-priority ticket arriving meanwhile), and once it is ours check the
			# queue again: a ticket that arrived between the two goes first.
			if flock -n "-$mode" 9; then
				if benchlock_may_try "$name" "$mode"; then
					rm -f "$ticket"
					break
				fi
				flock -u 9
			fi
			sleep 0.2
		else
			sleep 1
		fi
		if ((SECONDS > deadline)); then
			rm -f "$ticket"
			echo "benchlock: timed out waiting for the bench lock after ${timeout}s (KYTY_BENCH_LOCK_TIMEOUT): $what did not start" >&2
			exec 9>&-
			return 75
		fi
		# Say where we are when it changes, at most once a minute.
		read -r ahead eff < <(benchlock_tickets | awk -v me="$name" '$1 == me { e = $2; exit } { n++ } END { print n + 0, e }')
		if [ "$ahead" != "${said%%:*}" ] && { [ -z "$said" ] || ((SECONDS - ${said#*:} >= 60)); }; then
			echo "benchlock: waiting ($mode, priority $prio${eff:+, now $eff}), $ahead ahead in the queue; holder: $(cat "$(benchlock_owner_file)" 2>/dev/null || echo unknown)" >&2
			said=$ahead:$SECONDS
		fi
	done
	export KYTY_BENCH_LOCKED=$mode
	echo "$mode $what pid=$$ cwd=$PWD since=$(date +%T)" > "$(benchlock_owner_file)"
	return 0
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
	cmd=$1
	shift
	case "$cmd" in
	run)
		benchlock_acquire x "run: $*" || exit $?
		exec "$@"
		;;
	build)
		benchlock_acquire s "build: $*" || exit $?
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
		i=0
		while read -r name eff m pid prio what; do
			i=$((i + 1))
			echo "  $i. priority $prio$([ "$eff" != "$prio" ] && echo " (aged to $eff)"), $( [ "$m" = x ] && echo exclusive || echo shared), pid $pid: $what"
		done < <(benchlock_tickets)
		[ "$i" = 0 ] && echo "  queue empty"
		;;
	*)
		sed -n '2,31p' "$0"
		exit 2
		;;
	esac
fi
