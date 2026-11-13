#!/bin/bash
# idlecpus.sh "<Hybrid CPU: line>" [<emulator pid>]: the CPUs garden.sh's outside-load monitor
# watches (rule 7 of the bench procedure): the SMT siblings of the GPU command thread's and the
# draw prepare workers' cores, plus KYTY_RESERVE_CPUS, minus every CPU the run itself may use (the
# pinned CPUs on the line, KYTY_RECORDER_CPU, KYTY_DECODE_CPU, and, with a pid, every CPU in any of
# the emulator's threads' affinity lists). Prints them comma-separated on stdout (nothing if none)
# and, on stderr, one line per candidate it dropped and why.
# CPU lists may be ranges: sysfs writes a core's siblings as "10-11".
expand() { # "0-3,8,10-11" -> "0 1 2 3 8 10 11"
	local part out=
	for part in ${1//,/ }; do
		if [[ "$part" =~ ^([0-9]+)-([0-9]+)$ ]]; then
			out+="$(seq -s ' ' "${BASH_REMATCH[1]}" "${BASH_REMATCH[2]}") "
		elif [[ "$part" =~ ^[0-9]+$ ]]; then
			out+="$part "
		fi
	done
	echo "$out"
}
hy=$1 pid=$2
[[ "$hy" == "Hybrid CPU: emulator on"* ]] || { echo "idlecpus: not a Hybrid CPU line: $hy" >&2; exit 2; }
gpu=$(sed -n 's/.*GPU command thread on CPU \([0-9]*\).*/\1/p' <<<"$hy")
prep=$(sed -n 's/.*draw prepare workers on CPUs \([0-9,]*\).*/\1/p' <<<"$hy")
over=$(sed -n 's/.*overflow worker on CPU \(-\?[0-9]*\).*/\1/p' <<<"$hy")
pinned=" $gpu $(expand "$prep") $over $(expand "${KYTY_RECORDER_CPU:-}") $(expand "${KYTY_DECODE_CPU:-}") "
threads=" "
if [ -n "$pid" ]; then
	for list in $(sed -n 's/^Cpus_allowed_list:\s*//p' /proc/"$pid"/task/*/status 2>/dev/null); do
		threads+="$(expand "$list") "
	done
fi
cand="$(expand "${KYTY_RESERVE_CPUS:-}")"
for c in $gpu $(expand "$prep"); do
	cand+=" $(expand "$(cat "/sys/devices/system/cpu/cpu$c/topology/thread_siblings_list" 2>/dev/null)")"
done
out= seen=" "
for c in $cand; do
	[[ "$seen" == *" $c "* ]] && continue
	seen+="$c "
	if [[ "$pinned" == *" $c "* ]]; then
		continue # a pipeline thread's own CPU, not a candidate
	elif [[ "$threads" == *" $c "* ]]; then
		echo "cpu $c: in an emulator thread's affinity" >&2
		continue
	fi
	out+=${out:+,}$c
done
echo "$out"
