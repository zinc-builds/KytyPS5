#!/bin/bash
# test-idlecpus.sh: self-test of the CPUs garden.sh's outside-load monitor watches (idlecpus.sh).
# Fails if this machine's default layout (or the newest real "Hybrid CPU:" line in a bench log)
# leaves nothing to watch: then every run would print idle_busy=unchecked and gates would fail.
# No emulator, no bench lock; a few seconds.
here=$(cd "$(dirname "$0")" && pwd)
fail=0
check() { if eval "$1"; then echo "  ok: $2"; else echo "  FAILED: $2 ($1)"; fail=1; fi; }
ids() { "$here/idlecpus.sh" "$@" 2>/dev/null; }
if [ ! -d /sys/devices/cpu_atom ]; then
	echo "test-idlecpus: not a hybrid CPU (no /sys/devices/cpu_atom): garden.sh runs no monitor here"
	exit 0
fi
# The layout the emulator picks by default (hybridAffinity.h GetPlan): the GPU thread on the
# first thread of the last performance core, prepare workers on the first threads of the cores
# below it, the overflow worker on the last prepare worker's sibling.
perf=$(tr ',' ' ' < /sys/devices/cpu_core/cpus 2>/dev/null)
perf=$(for p in $perf; do [[ "$p" == *-* ]] && seq "${p%-*}" "${p#*-}" || echo "$p"; done | sort -n)
firsts=$(for c in $perf; do s=$(cat "/sys/devices/system/cpu/cpu$c/topology/thread_siblings_list"); f=${s%%[-,]*}; [ "$f" = "$c" ] && echo "$c"; done | sort -rn)
gpu=$(head -1 <<<"$firsts")
prep=$(sed -n '2,3p' <<<"$firsts" | paste -sd,)
last=${prep##*,}
over=$(s=$(cat "/sys/devices/system/cpu/cpu$last/topology/thread_siblings_list"); for c in ${s//[-,]/ }; do [ "$c" != "$last" ] && echo "$c"; done | head -1)
default="Hybrid CPU: emulator on 6 performance threads, GPU command thread on CPU $gpu, draw prepare workers on CPUs $prep, overflow worker on CPU $over"
echo "1. this machine's default layout: $default"
w=$(ids "$default")
echo "   watched: ${w:-none}"
check '[ -n "$w" ]' "the default layout leaves CPUs to watch"
for c in $gpu ${prep//,/ } $over; do
	check '[[ ",$w," != *",'"$c"',"* ]]' "pinned CPU $c is not watched"
done

echo "2. the newest real Hybrid CPU line in /tmp/bench-*.log"
log=$(grep -l -m1 '^Hybrid CPU: emulator on' $(ls -t /tmp/bench-*.log 2>/dev/null | head -50) /dev/null 2>/dev/null | head -1)
if [ -n "$log" ]; then
	hy=$(grep -m1 '^Hybrid CPU: emulator on' "$log")
	w=$(ids "$hy")
	echo "   $log: watched ${w:-none}"
	check '[ -n "$w" ]' "a real run's layout leaves CPUs to watch"
else
	echo "   none found (skipped)"
fi

echo "3. exclusions and list formats"
first=${w%%,*}
check '[[ ",$(KYTY_RECORDER_CPU=$first ids "$default")," != *",$first,"* ]]' "KYTY_RECORDER_CPU=$first is not watched"
check '[[ ",$(KYTY_DECODE_CPU=$first ids "$default")," != *",$first,"* ]]' "KYTY_DECODE_CPU=$first is not watched"
check '[[ ",$(KYTY_RESERVE_CPUS=0-1 ids "$default")," == *",0,1,"* ]]' "KYTY_RESERVE_CPUS=0-1 (a range) is watched"
taskset -c "$first" sleep 30 &
sp=$!
sleep 0.2
check '[[ ",$(ids "$default" "$sp")," != *",$first,"* ]]' "a CPU in the emulator's threads' affinity is not watched"
kill "$sp" 2>/dev/null
check '! "$here/idlecpus.sh" "not a hybrid line" >/dev/null 2>&1' "a malformed line is an error"

[ "$fail" = 0 ] && echo "test-idlecpus: all cases passed" || echo "test-idlecpus: FAILED"
exit "$fail"
