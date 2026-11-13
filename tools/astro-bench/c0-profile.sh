#!/bin/bash
# Track C0 capture: drive into Sky Garden, then sample per-thread CPU time, page faults and an
# on-CPU profile of every thread for a fixed window, with KYTY_MEM_DIAG=1 counters in the log.
# Usage: c0-profile.sh <label> [extra emulator flags...]   (needs vpad.py)
# Output: /tmp/c0-<label>/ (threads.txt, perf-stat.txt, perf.data, the link map, the bench log)
label=$1
shift
here=$(cd "$(dirname "$0")" && pwd)
. "$here/benchlock.sh"
benchlock_acquire x "c0-profile.sh $label" || exit $?
out=/tmp/c0-$label
rm -rf "$out"
mkdir -p "$out"
repo=$(git -C "$here" rev-parse --show-toplevel)
cp "$repo/_Build/linux/kyty_emulator_clang_lld.map" "$out/" 2>/dev/null
window=${C0_WINDOW:-10}

KEEP_RUNNING=1 KYTY_MEM_DIAG=1 GARDEN_WINDOW=${GARDEN_WINDOW:-20} bash "$here/garden.sh" "$label" "$@" > "$out/garden.txt" 2>&1
pid=$(pgrep -f '^\./kyty_emulator --game' | head -1)
[ -n "$pid" ] || { echo "emulator not running" >&2; cat "$out/garden.txt"; exit 1; }

snap() { # tid name utime+stime(ticks) minflt majflt
	for t in /proc/"$pid"/task/*; do
		local stat
		stat=$(cat "$t/stat" 2>/dev/null) || continue
		local name=${stat#*(}; name=${name%%)*}; name=${name// /_}
		local rest=${stat##*) }
		# fields after ") ": state(1) ... minflt(8) majflt(10) utime(12) stime(13)
		set -- $rest
		echo "${t##*/} $name $((${12} + ${13})) $8 ${10}"
	done
}
log=/tmp/bench-$label.log
l0=$(wc -l < "$log")
snap > "$out/snap0.txt"
perf stat --per-thread -e page-faults,context-switches,cpu-migrations -p "$pid" -o "$out/perf-stat.txt" -- sleep "$window" &
perf record -F 997 -p "$pid" -s -o "$out/perf.data" -- sleep "$window" > /dev/null 2>&1
wait
snap > "$out/snap1.txt"
l1=$(wc -l < "$log")
sed -n "${l0},${l1}p" "$log" | grep -E '^(PERF|MEMDIAG)' > "$out/diag.txt"
grep -E '^ *[0-9a-f]+-[0-9a-f]+ r-xp .*kyty_emulator' /proc/"$pid"/maps | head -1 > "$out/maps.txt"
kill "$pid"
sleep 3
kill -9 "$pid" 2>/dev/null
cp "$log" "$out/"
frames=$(grep '^PERF' "$out/diag.txt" | sed -E 's/.*fps=([0-9.]+).*/\1/' | awk '{s+=$1; n++} END {if (n) printf "%.2f", s/n}')
echo "fps=$frames window=${window}s" > "$out/threads.txt"
hz=$(getconf CLK_TCK)
join -j 1 <(sort "$out/snap0.txt") <(sort "$out/snap1.txt") |
	awk -v hz="$hz" -v w="$window" -v fps="$frames" '{cpu=($7-$3)/hz; flt=$8-$4;
	    if (cpu > 0.05) printf "%-8s %-16s cpu=%5.1f%% ms/frame=%5.1f minflt/s=%.0f\n", $1, $2, 100*cpu/w, 1000*cpu/(w*fps), flt/w}' |
	sort -t= -k2 -rn >> "$out/threads.txt"
cat "$out/garden.txt" | head -2
cat "$out/threads.txt"
