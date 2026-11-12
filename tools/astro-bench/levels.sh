#!/bin/bash
# levels.sh [options] <level>...: run garden.sh on a list of levels (the game's "-lvl" names: the
# <File> entries of the dump's data/prein/product_levels.xml), one run each, all in one hold of the
# bench lock, and print one table row per level.
#   -m verify   (default) correctness run: every verifier that exits the emulator on a difference
#               (VERIFY_ENV below), frames 0-400, a screenshot at frame 300
#   -m perf     performance run: frames 200-800 (a level that waits for input still renders)
#   -f A-B      frame window instead of the mode's
#   -s F        screenshot at level frame F (verify default 300; perf none)
#   -e "K=V ..." extra env for every run (e.g. KYTY_INSTALL=<pgo install> KYTY_FRAME_DIAG=1)
#   -t TAG      label prefix (default lv): runs are <TAG>-<mode>-<level>
#   -l FILE     read the levels from FILE (one per line, # comments) as well
# A level that does not reach "StartLevel <name>" within GARDEN_BOOT_TIMEOUT (default here 150 s)
# is reported as not booting. A run that dies is reported with the cause garden.sh printed and the
# last log lines; a verifier's exit shows up there as its EXIT message.
# Output: the table on stdout and in /tmp/levels-<TAG>-<mode>.md; per run /tmp/garden-<label>.out,
# /tmp/bench-<label>.log, /tmp/garden-<label>-shot.png.
here=$(dirname "$0")
# Verifiers that exit the emulator on a difference: gate.sh's EXITING_VERIFY plus the ones it
# leaves out because they cost more (whole-run checks) or belong to other paths.
VERIFY_ENV="KYTY_PREPARE_VERIFY=1 KYTY_BINDING_REUSE_VERIFY=1 KYTY_WORKER_TARGETS_VERIFY=1 KYTY_WORKER_PIPELINE_KEY_VERIFY=1 KYTY_WORKER_BUFFERS=verify KYTY_COLOR_CLEAR_MEMO=verify KYTY_STENCIL_TARGET_FAST=verify KYTY_DECODE_THREAD=verify KYTY_DTC_VERIFY=1 KYTY_WORKER_TEXTURES_VERIFY=1 KYTY_WORKER_STREAM=verify KYTY_ASYNC_WP_VERIFY=1 KYTY_FAST_GUEST_READ=verify KYTY_REGISTER_SNAPSHOT=verify"
mode=verify window= shot= extra= tag=lv levels=()
while [ $# -gt 0 ]; do
	case "$1" in
	-m) mode=$2; shift ;;
	-f) window=$2; shift ;;
	-s) shot=$2; shift ;;
	-e) extra=$2; shift ;;
	-t) tag=$2; shift ;;
	-l) while read -r l _; do [[ -n "$l" && "$l" != \#* ]] && levels+=("$l"); done < "$2"; shift ;;
	-*) echo "levels.sh: unknown option $1" >&2; exit 1 ;;
	*) levels+=("$1") ;;
	esac
	shift
done
[ ${#levels[@]} -gt 0 ] || { echo "usage: levels.sh [-m verify|perf] [-f A-B] [-s F] [-e ENV] [-t TAG] [-l FILE] <level>..." >&2; exit 1; }
case "$mode" in
verify) window=${window:-0-400} shot=${shot:-300} env=$VERIFY_ENV ;;
perf) window=${window:-200-800} env= ;;
*) echo "levels.sh: -m verify|perf" >&2; exit 1 ;;
esac
for l in "${levels[@]}"; do
	printf '%s GARDEN_LEVEL=%s GARDEN_FRAMES=%s GARDEN_BOOT_TIMEOUT=%s %s %s %s -- --amd-cpu\n' \
		"$tag-$mode-$l" "$l" "$window" "${GARDEN_BOOT_TIMEOUT:-150}" "${shot:+GARDEN_SHOT=$shot}" "$env" "$extra"
done | BATCH_WARMUP=0 "$here/batch.sh" > /dev/null
table=/tmp/levels-$tag-$mode.md
{
	echo "| Level | Loaded | Result | FPS | draws/frame | cmd_ns | worst_ms (max) | Screenshot |"
	echo "|---|---|---|---|---|---|---|---|"
	for l in "${levels[@]}"; do
		label=$tag-$mode-$l log=/tmp/bench-$tag-$mode-$l.log out=/tmp/garden-$tag-$mode-$l.out
		loaded=$(grep -o -m1 'Level has started: [a-z0-9_]\+' "$log" 2>/dev/null | tail -1 | cut -d' ' -f4)
		first=$(head -1 "$out" 2>/dev/null)
		if [[ "$first" == "$label: fps="* ]]; then
			result=ok
			fps=$(sed -E 's/.*fps=([0-9.]+).*/\1/' <<<"$first")
			dpf=$(sed -E 's/.*draws\/frame=([0-9]+).*/\1/' <<<"$first")
			cmd=$(grep -o 'cmd_ns=[0-9]*' <<<"$first" | cut -d= -f2)
			worst=$(sed -E 's/.*worst_ms=([0-9.]+) \(max ([0-9.]+)\).*/\1 (\2)/' <<<"$first")
		else
			# The cause garden.sh gave, then the log's last error-looking line.
			cause=$(grep -v -E '^(AMD CPU|benchlock|level start)' "$out" 2>/dev/null | head -1)
			last=$(grep -E 'EXIT|Error|error|verify|VERIFY|assert|Assert|Unhandled' "$log" 2>/dev/null | tail -1 | cut -c1-160)
			result="FAIL: ${cause:-no output}${last:+ — $last}"
			fps= dpf= cmd= worst=
		fi
		png=/tmp/garden-$label-shot.png
		[ -s "$png" ] || png=
		echo "| $l | ${loaded:--} | ${result//|/\\|} | ${fps:--} | ${dpf:--} | ${cmd:--} | ${worst:--} | ${png:--} |"
	done
} | tee "$table"
echo "table: $table" >&2
