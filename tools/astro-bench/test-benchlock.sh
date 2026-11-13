#!/bin/bash
# test-benchlock.sh: tests benchlock.sh's queue with dummy commands on a private lock file (never
# the real one): FIFO order, shared tickets going together, an exclusive ticket holding back later
# shared ones, priorities, a killed waiter, a timeout, nested holds, and mutual exclusion against
# old-style (flock-only) competitors, a zombie waiter, a late higher-priority ticket. Takes about
# 40 s. Exit status 0 when every case passes.
here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d) || exit 1
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$tmp"' EXIT
export KYTY_BENCH_LOCK=$tmp/lock
unset KYTY_BENCH_LOCKED KYTY_BENCH_PRIO
fail=0
check() { if eval "$1"; then echo "  ok: $2"; else echo "  FAILED: $2 ($1)"; fail=1; fi; }
now() { date +%s%N; }

# acq <log> <label> <x|s> <hold> [env...]: a waiter in the background; logs "<label> start|end ns".
# <hold> is seconds, or a file path: hold until that file exists.
acq() {
	local log=$1 label=$2 mode=$3 hold=$4
	shift 4
	(
		unset KYTY_BENCH_LOCKED
		for kv in "$@"; do export "${kv?}"; done
		. "$here/benchlock.sh"
		if benchlock_acquire "$mode" "$label" 2>/dev/null; then
			echo "$label start $(now)" >> "$log"
			if [[ "$hold" == /* ]]; then
				while [ ! -e "$hold" ]; do sleep 0.05; done
			else
				sleep "$hold"
			fi
			echo "$label end $(now)" >> "$log"
		else
			echo "$label failed $(now)" >> "$log"
		fi
	) &
}
t() { awk -v l="$2" -v e="$3" '$1 == l && $2 == e { print $3 }' "$1"; }
drained() { [ -z "$(ls -A "$KYTY_BENCH_LOCK.queue" 2>/dev/null | grep -v '^\.tmp')" ]; }

echo "1. FIFO, shared tickets together, exclusive after them"
log=$tmp/1.log
acq "$log" H x 2; sleep 0.3
acq "$log" A x 1; sleep 0.3
acq "$log" B s 1.5; sleep 0.3
acq "$log" C s 1.5; sleep 0.3
acq "$log" D x 0.5
wait
check '[ "$(t $log A start)" -ge "$(t $log H end)" ]' "A after H"
check '[ "$(t $log B start)" -ge "$(t $log A end)" ] && [ "$(t $log C start)" -ge "$(t $log A end)" ]' "B and C after A"
check '[ "$(t $log C start)" -lt "$(t $log B end)" ]' "B and C (shared) overlap"
check '[ "$(t $log D start)" -ge "$(t $log B end)" ] && [ "$(t $log D start)" -ge "$(t $log C end)" ]' "D after B and C"
check drained "queue empty"

echo "2. priority before arrival"
log=$tmp/2.log
acq "$log" H x 2; sleep 0.3
acq "$log" P9 x 0.5 KYTY_BENCH_PRIO=9; sleep 0.3
acq "$log" P5 x 0.5; sleep 0.3
acq "$log" P1 x 0.5 KYTY_BENCH_PRIO=1
wait
check '[ "$(t $log P1 start)" -lt "$(t $log P5 start)" ] && [ "$(t $log P5 start)" -lt "$(t $log P9 start)" ]' "P1, then P5, then P9"
check drained "queue empty"

echo "3. an exclusive ticket holds back a later shared one even under a shared holder"
log=$tmp/3.log
acq "$log" S1 s 2; sleep 0.3
acq "$log" X x 1; sleep 0.3
acq "$log" S2 s 0.5
wait
check '[ "$(t $log X start)" -ge "$(t $log S1 end)" ]' "X after S1"
check '[ "$(t $log S2 start)" -ge "$(t $log X end)" ]' "S2 after X, not alongside S1"
check drained "queue empty"

echo "4. a killed waiter's ticket is reaped"
log=$tmp/4.log
acq "$log" H x 2; sleep 0.3
acq "$log" W x 0.5; w=$!; sleep 0.3
acq "$log" Z x 0.5; sleep 0.3
kill -9 "$w"
wait
check '[ -z "$(t $log W start)" ]' "W never ran"
check '[ -n "$(t $log Z start)" ] && [ $(( ($(t $log Z start) - $(t $log H end)) / 1000000 )) -lt 3000 ]' "Z ran within 3 s of H"
check drained "queue empty"

echo "4b. a killed waiter its parent has not reaped (a zombie) does not block the queue"
log=$tmp/4b.log
acq "$log" H x 2; sleep 0.3
# The waiter's parent execs sleep, which never reaps it: after kill -9 it stays a zombie.
bash -c '( unset KYTY_BENCH_LOCKED; . "$1/benchlock.sh"; benchlock_acquire x W 2>/dev/null && echo "W start" >> "$2"; sleep 1 ) &
	echo $! > "$3"; exec sleep 5' _ "$here" "$log" "$tmp/4b.pid" &
sleep 0.3
acq "$log" Z x 0.5; sleep 0.3
w=$(cat "$tmp/4b.pid")
kill -9 "$w"
sleep 0.2
check '[[ "$(cat /proc/$w/stat 2>/dev/null)" == *") Z "* ]]' "W is a zombie"
wait
check '[ -n "$(t $log Z start)" ] && [ $(( ($(t $log Z start) - $(t $log H end)) / 1000000 )) -lt 3000 ]' "Z ran within 3 s of H"
check drained "queue empty"

echo "4c. a higher-priority ticket arriving just before the holder lets go goes first (3 tries)"
for i in 1 2 3; do
	log=$tmp/4c.$i.log
	acq "$log" H x "$tmp/4c.$i.release"; sleep 0.3
	acq "$log" P9 x 0.3 KYTY_BENCH_PRIO=9; sleep 1.5 # P9 is at the head, polling
	acq "$log" P1 x 0.3 KYTY_BENCH_PRIO=1; sleep 0.15
	touch "$tmp/4c.$i.release"
	wait
	check '[ "$(t $log P1 start)" -lt "$(t $log P9 start)" ]' "P1 before P9 (try $i)"
done
check drained "queue empty"

echo "5. a timeout removes the ticket"
log=$tmp/5.log
acq "$log" H x 4; sleep 0.3
acq "$log" T x 0.5 KYTY_BENCH_LOCK_TIMEOUT=1
sleep 2.9
check '[ -n "$(t $log T failed)" ]' "T gave up"
check '! ls "$KYTY_BENCH_LOCK.queue" | grep -q -- "-x$"' "no ticket left while H still holds"
wait

echo "6. nested holds"
out=$(
	. "$here/benchlock.sh"
	benchlock_acquire x outer 2>/dev/null || exit 1
	benchlock_acquire x inner 2>/dev/null && echo nested-x-ok
	benchlock_acquire s inner 2>/dev/null && echo nested-s-ok
	( exec 9>&-; KYTY_BENCH_LOCKED=s; . "$here/benchlock.sh"; benchlock_acquire x bad 2>/dev/null || echo s-to-x-refused )
)
check '[[ "$out" == *nested-x-ok* && "$out" == *nested-s-ok* && "$out" == *s-to-x-refused* ]]' "nested x/s no-ops; x inside s refused"

echo "7. mutual exclusion against old-style flock competitors"
cs=$tmp/cs bad=$tmp/bad
old() { # the pre-queue acquisition: plain flock on the same lock file
	(
		exec 9>>"$KYTY_BENCH_LOCK"
		flock -x 9
		mkdir "$cs" 2>/dev/null || touch "$bad"
		sleep 0.2
		rmdir "$cs" 2>/dev/null
	) &
}
new() {
	(
		unset KYTY_BENCH_LOCKED
		. "$here/benchlock.sh"
		benchlock_acquire "$1" new 2>/dev/null || exit 1
		if [ "$1" = x ]; then
			mkdir "$cs" 2>/dev/null || touch "$bad"
			sleep 0.2
			rmdir "$cs" 2>/dev/null
		else
			[ -d "$cs" ] && touch "$bad" # a shared holder never runs inside an exclusive one
			sleep 0.2
		fi
	) &
}
for i in 1 2 3 4; do old; new x; new s; done
wait
check '[ ! -e "$bad" ]' "never two exclusive holders, nor shared inside exclusive"
check drained "queue empty"

[ "$fail" = 0 ] && echo "test-benchlock: all cases passed" || echo "test-benchlock: FAILED"
exit "$fail"
