#!/bin/bash
# Run several garden.sh configurations back to back. Each line of the input:
#   <label> [ENV=VALUE ...] -- [emulator flags...]
# Results are appended to /tmp/bench-batch.txt.
while read -r line; do
	[ -z "$line" ] && continue
	label=${line%% *}; rest=${line#"$label"}
	envs=${rest%%--*}; flags=${rest#*--}; [ "$flags" = "$rest" ] && flags=""
	# shellcheck disable=SC2086
	env $envs bash "$(dirname "$0")/garden.sh" "$label" $flags > "/tmp/garden-$label.out" 2>&1; head -1 "/tmp/garden-$label.out" >> /tmp/bench-batch.txt
	sleep 3
done
