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
#   4. hitches: one more run a side over level frames $GATE_HITCH_FRAMES (default 0-2600) with
#      KYTY_FRAME_DIAG=1 (hitches.py): frames over 50 and 100 ms and the worst frame, level-load
#      frames (>= 1 GiB uploaded) apart. It fails if the new side has more frames over 100 ms than
#      the reference plus $GATE_HITCH_SLACK (default 2: one run a side differs by up to ~2 such
#      frames on one binary). The perf window alone missed #120's 0.4-2 s GC frames after the
#      level load.
#   5. perf direction: it fails when cmd_ns or cycles per draw (cpd) are worse by >= 3 sd (a "!"),
#      unless --accept-cost "<reason>" says the change is meant to cost that.
#   6. screenshots: it fails when the new frame is missing, near-black or near-uniform, and asks
#      the PR author to confirm the by-eye comparison (a checkbox line to tick in the PR).
# Options: -n N (runs per side), --verify-env "K=V ..." (only verifiers that exit the emulator on
#   a difference, so that reaching the last frame means clean: see EXITING_VERIFY), --no-verify,
#   --no-hitch, --accept-cost "<reason>", and ab.sh's --ref,
#   --ref-install, --new-install, --env, --ref-env, --new-env. Takes ~9 min (7 runs + 2 warm-ups).
VERIFY_ENV="KYTY_PREPARE_VERIFY=1 KYTY_BINDING_REUSE_VERIFY=1 KYTY_WORKER_TARGETS_VERIFY=1 KYTY_WORKER_PIPELINE_KEY_VERIFY=1"
# Verifiers that exit on a difference. Others (KYTY_STREAM_VERIFY, KYTY_TILER_VERIFY, ...) only
# print, so a run that reaches its last frame proves nothing for them: run those by hand.
EXITING_VERIFY="$VERIFY_ENV KYTY_WORKER_BUFFERS=verify KYTY_COLOR_CLEAR_MEMO=verify KYTY_STENCIL_TARGET_FAST=verify KYTY_DECODE_THREAD=verify"
dir=$(dirname "$0")
repo=$(git -C "$dir" rev-parse --show-toplevel)
label=$1
[[ -n "$label" && "$label" != -* ]] || { sed -n '2,13p' "$0" >&2; exit 2; }
shift
n=2 verify=1 hitch=1 accept_cost= verify_env= new_install= new_env= env= pass=()
while [ $# -gt 0 ]; do
	case "$1" in
	-n) n=$2; shift ;;
	--verify-env) verify_env=$2; shift ;;
	--no-verify) verify=0 ;;
	--no-hitch) hitch=0 ;;
	--accept-cost) accept_cost=$2; shift ;;
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
hframes=${GATE_HITCH_FRAMES:-0-2600}
hslack=${GATE_HITCH_SLACK:-2}
[ "$hitch" = 1 ] && { extra+=(--hitch "$hframes"); rm -f "/tmp/bench-$label-hitch-ref.log" "/tmp/bench-$label-hitch-new.log"; }
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
# A cost of >= 3 sd in the decision metrics (cmd_ns, cycles per draw) fails unless accepted.
worse=$(grep -oE '(cmd_ns|cpd) \+[0-9.]+%±[0-9.]+!' <<<"$verdict" | tr '\n' ' ')
if [ -n "$worse" ]; then
	if [ -n "$accept_cost" ]; then
		echo "**Perf direction:** worse by >= 3 sd ($worse), accepted: $accept_cost"
	else
		status=FAIL
		echo "**Perf direction:** FAIL, worse by >= 3 sd: $worse(pass --accept-cost \"<reason>\" if intended)"
	fi
elif ! grep -q '^AB ' <<<"$verdict"; then
	status=FAIL
	echo "**Perf direction:** FAIL, no verdict line"
else
	echo "**Perf direction:** PASS (no decision metric worse by >= 3 sd)"
fi
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
if [ "$hitch" = 1 ]; then
	href=$(python3 -I "$dir/hitches.py" "$label-hitch-ref" --frames "$hframes" 2>&1)
	hnew=$(python3 -I "$dir/hitches.py" "$label-hitch-new" --frames "$hframes" 2>&1)
	o100() { sed -n 's/.* over100=\([0-9]*\) .*/\1/p' <<<"$1"; }
	r=$(o100 "$href") w=$(o100 "$hnew")
	# Only runs that reached the window's end count (batch.sh does not stop on a failed run).
	done_ref=0 done_new=0
	[[ "$(head -1 "/tmp/garden-$label-hitch-ref.out" 2>/dev/null)" == "$label-hitch-ref: fps="* ]] && done_ref=1
	[[ "$(head -1 "/tmp/garden-$label-hitch-new.out" 2>/dev/null)" == "$label-hitch-new: fps="* ]] && done_new=1
	if [ "$done_ref" = 0 ] || [ "$done_new" = 0 ]; then
		status=FAIL
		hverdict="FAIL: a hitch run did not complete (ref $done_ref, new $done_new; see /tmp/garden-$label-hitch-*.out)"
	elif [ -z "$r" ] && [ -n "$w" ] && [[ "$href" == *"no GFRAME"* ]]; then
		hverdict="new side only: the reference predates KYTY_FRAME_DIAG"
	elif [ -z "$r" ] || [ -z "$w" ]; then
		status=FAIL
		hverdict="FAIL (no result)"
	elif [ "$w" -gt $((r + hslack)) ]; then
		status=FAIL
		hverdict="FAIL: frames over 100 ms $r -> $w (allowed: up to $((r + hslack)))"
	else
		hverdict="PASS: frames over 100 ms $r -> $w (allowed: up to $((r + hslack)))"
	fi
	echo "**Hitches** (one run a side, level frames $hframes, KYTY_FRAME_DIAG=1; level-load frames"
	echo "that upload >= 1 GiB are listed apart, not counted): $hverdict"
	echo
	echo '```'
	echo "$href"
	echo "$hnew"
	echo '```'
	[[ "$hverdict" == FAIL* ]] && python3 -I "$dir/hitches.py" "$label-hitch-new" --frames "$hframes" --list 2>&1 | tail -n +2 | head -20
fi
ref_png=/tmp/garden-$label-ref1-shot.png new_png=/tmp/garden-$label-new1-shot.png
# Near-black or near-uniform frames fail (a blank capture passed by existence once).
stats=$(magick "$new_png" -colorspace gray -format '%[fx:mean] %[fx:standard_deviation]' info: 2>/dev/null)
if [ -f "$new_png" ] && { [ -z "$stats" ] || awk '{exit !($1 < 0.03 || $2 < 0.02)}' <<<"$stats"; }; then
	status=FAIL
	echo "**Screenshots:** FAIL, the new frame is near-black or near-uniform (gray mean, sd: ${stats:-unreadable}): $new_png"
elif [ -f "$new_png" ]; then
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
		echo "- [ ] I compared ref | new at level frame $shot by eye: no missing or wrong geometry, textures or colors."
	else
		echo "**Screenshots:** new only, $new_png (the reference binary predates KYTY_SHOT_FILE:"
		echo "compare with a dev screenshot by hand)"
		echo "- [ ] I compared the new frame at level frame $shot with a dev screenshot by eye: no missing or wrong geometry, textures or colors."
	fi
else
	status=FAIL
	echo "**Screenshots:** FAIL, the new build saved no frame ($new_png)"
fi
echo
echo "**Gate: $status**"
[ "$status" = PASS ]
