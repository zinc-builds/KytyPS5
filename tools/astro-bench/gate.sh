#!/bin/bash
# gate.sh <label> --class diag|switch|full --why "<reason>" [options]: the merge gate, one lock
# hold, a block to paste into the PR body. The class is declared, printed with its reason, and
# decides what runs (bench procedure, "fewer runs"):
#   diag    diagnostic-only code, off by default: a screening A/B of the new build (diagnostic off)
#           against stored references of the merge-base (up to 2 pairs), and one run with it on
#           (--diag-env), the verify modes and the screenshot together. No hitch run.
#   switch  a behaviour change behind a kill switch: the same binary with the switch's off value
#           (--switch "KYTY_X=0") as the reference arm and the default as the new arm, a verify
#           run, and one hitch run (with the screenshot) on the new arm against the stored dev
#           hitch baseline.
#   full    anything else: the A/B against the merge-base (stored references when there are),
#           the verify run, the hitch run against the baseline.
# Every A/B is sequential: one pair, stop when cmd_ns or cycles per draw (cpd) is a signal (>= 3
# sd), else another pair, up to -n (default 3); window 200-500. The verdict judges cmd_ns and cpd:
# worse by >= 3 sd fails unless --accept-cost "<reason>". FPS is printed, never judged (outside
# load moves it more than any change: 47.7-60 within one build).
#   verify: the project's verify modes (below, plus --verify-env), frames 0-$GATE_VERIFY_FRAMES;
#     it passes if the run reaches its last frame (a verify mode that finds a difference exits).
#   hitches: one run over level frames $GATE_HITCH_FRAMES (0-2600) with KYTY_FRAME_DIAG=1
#     (hitches.py), compared with the hitch baseline of the merge-base or the nearest of the 10 dev
#     commits before it that has one (same build mode and profile; the SHA used is printed). It
#     fails with more frames over 100 ms than the baseline plus $GATE_HITCH_SLACK (2). With no
#     baseline, the reference arm runs a hitch run too. That run also saves the screenshot (level
#     frame $GATE_SHOT_FRAME, 600).
#   screenshots: fail when the new frame is missing, near-black or near-uniform; the PR author
#     compares it with the baseline's (or the reference arm's) by eye and ticks the line.
# Options: -n N, --switch "K=V", --diag-env "K=V ...", --verify-env "K=V ..." (only verifiers that
#   exit on a difference: EXITING_VERIFY), --no-verify, --no-hitch, --accept-cost "<reason>",
#   --new-install D, and ab.sh's --ref, --ref-install, --env, --ref-env, --new-env, --flags (the
#   emulator flags of every run, default --amd-cpu).
#
# gate.sh --baseline [<commit>] (default origin/dev): what the gate reads instead of re-running
#   dev. For one dev commit, in the build mode KYTY_BUILD_MODE/KYTY_PGO_PROFILE select (as
#   refbuild.sh): 2 per-draw runs (frames 200-500, into the result store) and one hitch run with
#   the screenshot, stored as its hitch baseline. Run once by whoever merges to dev (~4 min).
VERIFY_ENV="KYTY_PREPARE_VERIFY=1 KYTY_BINDING_REUSE_VERIFY=1 KYTY_WORKER_TARGETS_VERIFY=1 KYTY_WORKER_PIPELINE_KEY_VERIFY=1 KYTY_COLLECT_OBSERVE=1"
# Verifiers that exit on a difference. Others (KYTY_STREAM_VERIFY, KYTY_TILER_VERIFY, ...) only
# print, so a run that reaches its last frame proves nothing for them: run those by hand.
EXITING_VERIFY="$VERIFY_ENV KYTY_WORKER_BUFFERS=verify KYTY_COLOR_CLEAR_MEMO=verify KYTY_STENCIL_TARGET_FAST=verify KYTY_DECODE_THREAD=verify KYTY_OCCLUSION_PROXY_SKIP=verify"
dir=$(dirname "$0")
repo=$(git -C "$dir" rev-parse --show-toplevel)
shot=${GATE_SHOT_FRAME:-600}
vframes=${GATE_VERIFY_FRAMES:-600}
hframes=${GATE_HITCH_FRAMES:-0-2600}
hslack=${GATE_HITCH_SLACK:-2}
bm() { # "<mode> [<profile hash>]" of an install; no BUILD_MODE file: plain
	[ -f "$1/../BUILD_MODE" ] || { echo plain; return; }
	sed -n 's/.*\bmode=\([a-z]*\).*/\1/p; s/.*\bsha256=\([0-9a-f]*\).*/\1/p' "$1/../BUILD_MODE" | xargs
}
o100() { sed -n 's/.* over100=\([0-9]*\) .*/\1/p' <<<"$1"; }
# ran <label> [timed]: the run reached its last frame; with "timed", also not VOID (outside load
# makes timings, not correctness, worthless: verify and diagnostic runs accept a VOID).
ran() {
	local v
	v=$(head -1 "/tmp/garden-$1.out" 2>/dev/null)
	[[ "$v" == "$1: fps="* ]] && { [ "$2" != timed ] || [[ "$v" != *" VOID "* ]]; }
}

if [ "$1" = --baseline ]; then
	sha=$(git -C "$repo" rev-parse --verify "${2:-origin/dev}^{commit}") || exit 1
	inst=$("$dir/refbuild.sh" "$sha") || { echo "gate.sh: refbuild.sh $sha failed" >&2; exit 1; }
	read -r mode profile <<<"$(bm "$inst")"
	b=base-${sha:0:8}-$mode${profile:+-$profile}
	rm -f "/tmp/garden-$b-hitch-shot.png" "/tmp/bench-$b-hitch.log" /tmp/garden-"$b"-{perf1,perf2,hitch}.{out,json}
	printf '%s\n' "$b-perf1 KYTY_INSTALL=$inst GARDEN_FRAMES=200-500 -- --amd-cpu" \
		"$b-perf2 KYTY_INSTALL=$inst GARDEN_FRAMES=200-500 -- --amd-cpu" \
		"$b-hitch KYTY_INSTALL=$inst KYTY_FRAME_DIAG=1 GARDEN_FRAMES=$hframes GARDEN_SHOT=$shot -- --amd-cpu" |
		"$dir/batch.sh" || exit 1
	for r in perf1 perf2 hitch; do head -1 "/tmp/garden-$b-$r.out"; done
	for r in perf1 perf2; do # garden.sh stored these in the result store (references for gates)
		ran "$b-$r" timed && [ -f "/tmp/garden-$b-$r.json" ] ||
			{ echo "gate.sh: $b-$r failed or was void (no per-draw result); baseline not stored" >&2; exit 1; }
	done
	ran "$b-hitch" timed || { echo "gate.sh: the hitch run failed or was void; baseline not stored" >&2; exit 1; }
	h=$(python3 -I "$dir/hitches.py" "$b-hitch" --frames "$hframes" 2>&1)
	[ -n "$(o100 "$h")" ] || { echo "gate.sh: no hitch result: $h" >&2; exit 1; }
	keep=$HOME/.cache/kyty-builds/baselines
	mkdir -p "$keep"
	png=
	[ -f "/tmp/garden-$b-hitch-shot.png" ] && png=$keep/$b-shot$shot.png && cp "/tmp/garden-$b-hitch-shot.png" "$png"
	python3 -I "$dir/results.py" add-hitch "$h" rev="$sha" exe="$(sha256sum "$inst/kyty_emulator" | cut -c1-16)" \
		build_mode="$mode" profile="$profile" frames="$hframes" flags=--amd-cpu shot="$png" shot_frame="$shot" || exit 1
	echo "gate.sh: baseline of ${sha:0:12} ($mode${profile:+ $profile}) stored: $h${png:+; screenshot $png}"
	exit 0
fi

label=$1
[[ -n "$label" && "$label" != -* ]] || { sed -n '2,36p' "$0" >&2; exit 2; }
shift
flags=--amd-cpu n=3 verify=1 hitch=1 accept_cost= verify_env= new_install= new_env= env= pass=() class= why= switch= diag_env= ref=
while [ $# -gt 0 ]; do
	case "$1" in
	-n) n=$2; shift ;;
	--class) class=$2; shift ;;
	--why) why=$2; shift ;;
	--switch) switch=$2; shift ;;
	--diag-env) diag_env=$2; shift ;;
	--verify-env) verify_env=$2; shift ;;
	--no-verify) verify=0 ;;
	--no-hitch) hitch=0 ;;
	--accept-cost) accept_cost=$2; shift ;;
	--new-install) new_install=$2; shift ;;
	--new-env) new_env=$2; pass+=("$1" "$2"); shift ;;
	--env) env=$2; pass+=("$1" "$2"); shift ;;
	--ref) ref=$2; pass+=("$1" "$2"); shift ;;
	--ref-install | --ref-env) pass+=("$1" "$2"); shift ;;
	--flags) flags=$2; pass+=("$1" "$2"); shift ;;
	*) echo "gate.sh: unknown option $1" >&2; exit 2 ;;
	esac
	shift
done
case "$class" in
diag) [ -n "$diag_env" ] || { echo "gate.sh: --class diag needs --diag-env \"KYTY_X=1\" (the diagnostic on)" >&2; exit 2; }
	hitch=0 ;; # the verify modes run in the diagnostic's run
switch) [[ "$switch" == KYTY_*=* ]] || { echo "gate.sh: --class switch needs --switch \"KYTY_X=0\" (the env that turns the change off)" >&2; exit 2; } ;;
full) ;;
*) echo "gate.sh: --class diag|switch|full is required (and --why \"<reason>\"); see the header" >&2; exit 2 ;;
esac
[ -n "$why" ] || { echo "gate.sh: --why \"<reason>\" is required: it goes into the PR block for review" >&2; exit 2; }
for kv in $verify_env; do
	[[ " $EXITING_VERIFY " == *" $kv "* ]] || {
		echo "gate.sh: --verify-env $kv: not a verifier setting that exits on a difference ($EXITING_VERIFY); check it in its own run" >&2
		exit 2
	}
done
new_install=${new_install:-$repo/_Build/linux/install}
read -r mode profile <<<"$(bm "$new_install")"
# The hitch baseline: the merge-base or one of the 10 dev commits before it.
merge_base=$(git -C "$repo" rev-parse --verify "${ref:-$(git -C "$repo" merge-base HEAD origin/dev)}^{commit}")
base_json= base_sha=
if [ "$hitch" = 1 ]; then
	base_json=$(python3 -I "$dir/results.py" hitch-base $(git -C "$repo" rev-list --first-parent -n 11 "$merge_base") \
		--mode "$mode" --profile "${profile:-}" --flags="$flags" --frames "$hframes" --shot-frame "$shot" 2>/dev/null)
	base_sha=$(python3 -I -c 'import json,sys; print(json.loads(sys.argv[1])["rev"])' "$base_json" 2>/dev/null)
fi
extra=()
[ "$verify" = 1 ] && [ "$class" != diag ] && extra+=(--extra "$label-verify KYTY_INSTALL=$new_install GARDEN_FRAMES=0-$vframes $env $new_env $VERIFY_ENV $verify_env -- $flags")
[ "$class" = diag ] && extra+=(--extra "$label-diag KYTY_INSTALL=$new_install GARDEN_FRAMES=0-$((shot + 100)) GARDEN_SHOT=$shot $env $new_env $diag_env $([ "$verify" = 1 ] && echo "$VERIFY_ENV $verify_env") -- $flags")
if [ "$hitch" = 1 ]; then
	extra+=(--hitch "$hframes" --hitch-shot "$shot")
	[ -n "$base_sha" ] && extra+=(--hitch-new-only)
fi
[ "$class" = switch ] && pass+=(--ref-install "$new_install" --ref-env "$switch")
rm -f /tmp/garden-"$label"-{verify,diag,hitch-ref,hitch-new}.out /tmp/bench-"$label"-hitch-{ref,new}.log \
	/tmp/garden-"$label"-{diag,hitch-ref,hitch-new}-shot.png
maxn=$n
[ "$class" = diag ] && ((maxn > 2)) && maxn=2
verdict=$("$dir/ab.sh" "$label" --sequential -n "$maxn" --window 200-500 --min-ref 2 --max-age 168 \
	--new-install "$new_install" "${pass[@]}" "${extra[@]}") || { echo "gate.sh: ab.sh failed" >&2; exit 1; }

status=PASS
echo "### Merge gate: $label ($(git -C "$repo" rev-parse --short HEAD), $(date '+%F %H:%M'))"
echo
echo "**Class:** $class${switch:+ (reference arm: same binary with $switch)}${diag_env:+ (diagnostic on: $diag_env)}. **Why:** $why"
echo
echo "**Per-draw** (frames 200-500, sequential up to $maxn pairs; FPS printed, not judged):"
echo
echo '```'
for i in $(seq "$maxn"); do
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
for run in $([ "$verify" = 1 ] && [ "$class" != diag ] && echo verify) $([ "$class" = diag ] && echo diag); do
	v=$(head -1 "/tmp/garden-$label-$run.out" 2>/dev/null)
	what=$([ "$run" = verify ] && echo "$VERIFY_ENV${verify_env:+ $verify_env}" || echo "$diag_env$([ "$verify" = 1 ] && echo " $VERIFY_ENV${verify_env:+ $verify_env}")")
	if ran "$label-$run"; then
		echo "**${run^}:** PASS, frames 0-$([ "$run" = verify ] && echo "$vframes" || echo $((shot + 100))) clean with $what"
	else
		status=FAIL
		echo "**${run^}:** FAIL with $what:"
		echo '```'
		head -5 "/tmp/garden-$label-$run.out" 2>/dev/null
		tail -5 "/tmp/bench-$label-$run.log" 2>/dev/null
		echo '```'
	fi
done
ref_png= new_png=
if [ "$hitch" = 1 ]; then
	hnew=$(python3 -I "$dir/hitches.py" "$label-hitch-new" --frames "$hframes" 2>&1)
	w=$(o100 "$hnew")
	new_png=/tmp/garden-$label-hitch-new-shot.png
	done_new=0
	ran "$label-hitch-new" timed && done_new=1
	if [ -n "$base_sha" ]; then
		href=$(python3 -I -c 'import json,sys; print(json.loads(sys.argv[1])["line"])' "$base_json")
		r=$(o100 "$href")
		ref_png=$(python3 -I -c 'import json,sys; print(json.loads(sys.argv[1]).get("shot", ""))' "$base_json")
		from="stored baseline of ${base_sha:0:12}$([ "$base_sha" != "$merge_base" ] && echo " ($(git -C "$repo" rev-list --count "$base_sha..$merge_base") dev commits before the merge-base)")"
		done_ref=1
	else
		href=$(python3 -I "$dir/hitches.py" "$label-hitch-ref" --frames "$hframes" 2>&1)
		r=$(o100 "$href")
		ref_png=/tmp/garden-$label-hitch-ref-shot.png
		from="the reference arm (no stored baseline within 10 dev commits; gate.sh --baseline stores one)"
		done_ref=0
		ran "$label-hitch-ref" timed && done_ref=1
	fi
	if [ "$done_ref" = 0 ] || [ "$done_new" = 0 ]; then
		status=FAIL
		hverdict="FAIL: a hitch run did not complete or was void (ref $done_ref, new $done_new; see /tmp/garden-$label-hitch-*.out)"
	elif [ -z "$r" ] || [ -z "$w" ]; then
		status=FAIL
		hverdict="FAIL (no result)"
	elif [ "$w" -gt $((r + hslack)) ]; then
		status=FAIL
		hverdict="FAIL: frames over 100 ms $r -> $w (allowed: up to $((r + hslack)))"
	else
		hverdict="PASS: frames over 100 ms $r -> $w (allowed: up to $((r + hslack)))"
	fi
	echo "**Hitches** (new arm, level frames $hframes, KYTY_FRAME_DIAG=1, against $from; level-load"
	echo "frames that upload >= 1 GiB are listed apart, not counted): $hverdict"
	echo
	echo '```'
	echo "$href"
	echo "$hnew"
	echo '```'
	[[ "$hverdict" == FAIL* ]] && python3 -I "$dir/hitches.py" "$label-hitch-new" --frames "$hframes" --list 2>&1 | tail -n +2 | head -20
elif [ "$class" = diag ]; then
	new_png=/tmp/garden-$label-diag-shot.png
	ref_png=$(python3 -I "$dir/results.py" hitch-base $(git -C "$repo" rev-list --first-parent -n 11 "$merge_base") \
		--mode "$mode" --profile "${profile:-}" --flags="$flags" --shot-frame "$shot" 2>/dev/null | python3 -I -c 'import json,sys; print(json.load(sys.stdin).get("shot", ""))' 2>/dev/null)
fi
# Near-black or near-uniform frames fail (a blank capture passed by existence once).
stats=$(magick "$new_png" -colorspace gray -format '%[fx:mean] %[fx:standard_deviation]' info: 2>/dev/null)
if [ -z "$new_png" ]; then
	echo "**Screenshots:** none (--no-hitch)"
elif [ -f "$new_png" ] && { [ -z "$stats" ] || awk '{exit !($1 < 0.03 || $2 < 0.02)}' <<<"$stats"; }; then
	status=FAIL
	echo "**Screenshots:** FAIL, the new frame is near-black or near-uniform (gray mean, sd: ${stats:-unreadable}): $new_png"
elif [ -f "$new_png" ]; then
	if [ -n "$ref_png" ] && [ -f "$ref_png" ]; then
		side=/tmp/garden-$label-shots.jpg
		rm -f "$side"
		if magick "$ref_png" "$new_png" +append -resize 3840x "$side" 2>/dev/null; then
			echo "**Screenshots** (level frame $shot, reference | new; compare by eye): $side"
		else
			echo "**Screenshots** (level frame $shot; side-by-side failed, compare by eye):"
		fi
		echo "(full size: $ref_png, $new_png. Two runs of one build differ a little at the same frame:"
		echo "camera, particles, birds; look for missing or wrong geometry, textures or colors.)"
		echo "- [ ] I compared reference | new at level frame $shot by eye: no missing or wrong geometry, textures or colors."
	else
		echo "**Screenshots:** new only, $new_png (no reference frame: compare with a dev screenshot by hand)"
		echo "- [ ] I compared the new frame at level frame $shot with a dev screenshot by eye: no missing or wrong geometry, textures or colors."
	fi
else
	status=FAIL
	echo "**Screenshots:** FAIL, the new build saved no frame ($new_png)"
fi
echo
echo "**Gate: $status**"
[ "$status" = PASS ]
