#!/bin/bash
# Run several garden.sh configurations back to back. Each line of the input:
#   <label> [ENV=VALUE ...] -- [emulator flags...]
# Results are appended to /tmp/bench-batch.txt.
# Holds the exclusive bench lock for the whole batch (see benchlock.sh).
# The first measured run of a batch tends to be slow (cold file cache and clocks), so the batch
# starts with a short, unreported warm-up run of its first line. BATCH_WARMUP=0 skips it.
# A run that garden.sh marks VOID (outside load on the CPUs the layout leaves idle) is repeated
# once; the void one's summary is kept as /tmp/garden-<label>.void.out. A second VOID leaves no
# /tmp/garden-<label>.json (only .void.json): callers treat it as a failed run.
. "$(dirname "$0")/benchlock.sh"
benchlock_acquire x "batch.sh" || exit 1
run_line() { # run_line <line> [extra env]: runs one configuration, prints garden.sh's summary line
	local line=$1 label rest envs flags
	label=${line%% *}; rest=${line#"$label"}
	envs=${rest%%--*}; flags=${rest#*--}; [ "$flags" = "$rest" ] && flags=""
	label=$label$3
	# shellcheck disable=SC2086
	env $envs $2 bash "$(dirname "$0")/garden.sh" "$label" $flags > "/tmp/garden-$label.out" 2>&1
	# A run voided by outside load (garden.sh's idle-CPU monitor) is run once more, same label.
	if head -1 "/tmp/garden-$label.out" | grep -q ' VOID '; then
		echo "batch.sh: $label void (outside load), running it again: $(grep -o 'idle_busy=[^ ]*' "/tmp/garden-$label.out" | head -1)" >&2
		cp "/tmp/garden-$label.out" "/tmp/garden-$label.void.out"
		rm -f "/tmp/garden-$label.json"
		sleep 10
		# shellcheck disable=SC2086
		env $envs $2 bash "$(dirname "$0")/garden.sh" "$label" $flags > "/tmp/garden-$label.out" 2>&1
	fi
	head -1 "/tmp/garden-$label.out"
	sleep 3
}
mapfile -t lines
first=
for line in "${lines[@]}"; do [ -n "$line" ] && { first=$line; break; }; done
if [ -n "$first" ] && [ "${BATCH_WARMUP:-1}" != 0 ]; then
	run_line "$first" "GARDEN_FRAMES=0-150 GARDEN_WINDOW=" "-warmup" > /dev/null
fi
for line in "${lines[@]}"; do
	[ -z "$line" ] && continue
	run_line "$line" >> /tmp/bench-batch.txt
done
