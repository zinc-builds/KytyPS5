#!/bin/bash
# gate.sh <label> [options]: the merge gate in one lock hold. It prints a block to paste into the
# PR body:
#   1. perf: a same-batch A/B against the merge-base (ab.sh --pair, frames 1000-1600, 2 runs a
#      side: the FPS of record), with the verdict line;
#   2. verify: one run of the new build with the project's verify modes on (below, plus
#      --verify-env), frames 0-$GATE_VERIFY_FRAMES; it passes if the run reaches its last frame
#      (a verify mode that finds a difference exits the emulator);
#   3. screenshots: run 1 of each side saves level frame $GATE_SHOT_FRAME (default 600, before
#      the perf window) in-emulator; gate.sh makes a side-by-side image to compare by eye (two
#      runs of one build differ a little at the same frame, so there is no pixel threshold).
# Options: -n N (runs per side), --verify-env "K=V ..." (only verifiers that exit the emulator on
#   a difference, so that reaching the last frame means clean: see EXITING_VERIFY), --no-verify,
#   and ab.sh's --ref,
#   --ref-install, --new-install, --env, --ref-env, --new-env. Takes ~7 min (5 runs + 2 warm-ups).
VERIFY_ENV="KYTY_PREPARE_VERIFY=1 KYTY_BINDING_REUSE_VERIFY=1 KYTY_WORKER_TARGETS_VERIFY=1 KYTY_WORKER_PIPELINE_KEY_VERIFY=1"
# Verifiers that exit on a difference. Others (KYTY_STREAM_VERIFY, KYTY_TILER_VERIFY, ...) only
# print, so a run that reaches its last frame proves nothing for them: run those by hand.
EXITING_VERIFY="$VERIFY_ENV KYTY_WORKER_BUFFERS=verify KYTY_COLOR_CLEAR_MEMO=verify KYTY_STENCIL_TARGET_FAST=verify KYTY_DECODE_THREAD=verify"
dir=$(dirname "$0")
repo=$(git -C "$dir" rev-parse --show-toplevel)
label=$1
[[ -n "$label" && "$label" != -* ]] || { sed -n '2,13p' "$0" >&2; exit 2; }
shift
n=2 verify=1 verify_env= new_install= new_env= env= pass=()
while [ $# -gt 0 ]; do
	case "$1" in
	-n) n=$2; shift ;;
	--verify-env) verify_env=$2; shift ;;
	--no-verify) verify=0 ;;
	--new-install) new_install=$2; shift ;;
	--new-env) new_env=$2; pass+=("$1" "$2"); shift ;;
	--env) env=$2; pass+=("$1" "$2"); shift ;;
	--ref | --ref-install | --ref-env) pass+=("$1" "$2"); shift ;;
	*) echo "gate.sh: unknown option $1" >&2; exit 2 ;;
	esac
	shift
done
for kv in $verify_env; do
	[[ " $EXITING_VERIFY " == *" $kv "* ]] || {
		echo "gate.sh: --verify-env $kv: not a verifier setting that exits on a difference ($EXITING_VERIFY); check it in its own run" >&2
		exit 2
	}
done
new_install=${new_install:-$repo/_Build/linux/install}
shot=${GATE_SHOT_FRAME:-600}
vframes=${GATE_VERIFY_FRAMES:-600}
extra=()
[ "$verify" = 1 ] && extra=(--extra "$label-verify KYTY_INSTALL=$new_install GARDEN_FRAMES=0-$vframes $env $new_env $VERIFY_ENV $verify_env -- --amd-cpu")
rm -f "/tmp/garden-$label-verify.out"
verdict=$("$dir/ab.sh" "$label" --pair -n "$n" --window 1000-1600 --shot "$shot" --new-install "$new_install" \
	"${pass[@]}" "${extra[@]}") || { echo "gate.sh: ab.sh failed" >&2; exit 1; }

status=PASS
echo "### Merge gate: $label ($(git -C "$repo" rev-parse --short HEAD), $(date '+%F %H:%M'))"
echo
echo "**Perf** (same batch, frames 1000-1600, $n runs a side):"
echo
echo '```'
for i in $(seq "$n"); do
	head -1 "/tmp/garden-$label-ref$i.out" 2>/dev/null
	head -1 "/tmp/garden-$label-new$i.out" 2>/dev/null
done
echo "$verdict"
echo '```'
if [ "$verify" = 1 ]; then
	v=$(head -1 "/tmp/garden-$label-verify.out" 2>/dev/null)
	if [[ "$v" == "$label-verify: fps="* ]]; then
		echo "**Verify:** PASS, frames 0-$vframes clean with $VERIFY_ENV${verify_env:+ $verify_env}"
	else
		status=FAIL
		echo "**Verify:** FAIL with $VERIFY_ENV${verify_env:+ $verify_env}:"
		echo '```'
		cat "/tmp/garden-$label-verify.out" 2>/dev/null | head -5
		tail -5 "/tmp/bench-$label-verify.log" 2>/dev/null
		echo '```'
	fi
fi
ref_png=/tmp/garden-$label-ref1-shot.png new_png=/tmp/garden-$label-new1-shot.png
if [ -f "$new_png" ]; then
	if [ -f "$ref_png" ]; then
		side=/tmp/garden-$label-shots.jpg
		rm -f "$side"
		if magick "$ref_png" "$new_png" +append -resize 3840x "$side" 2>/dev/null; then
			echo "**Screenshots** (level frame $shot, ref | new; compare by eye): $side"
		else
			echo "**Screenshots** (level frame $shot; side-by-side failed, compare by eye):"
		fi
		echo "(full size: $ref_png, $new_png. Two runs of one build differ a little at the same frame:"
		echo "camera, particles, birds; look for missing or wrong geometry, textures or colors.)"
	else
		echo "**Screenshots:** new only, $new_png (the reference binary predates KYTY_SHOT_FILE:"
		echo "compare with a dev screenshot by hand)"
	fi
else
	status=FAIL
	echo "**Screenshots:** FAIL, the new build saved no frame ($new_png)"
fi
echo
echo "**Gate: $status**"
[ "$status" = PASS ]
