#!/bin/bash
# ab.sh <label> [options]: one-command A/B of this checkout's install against its merge-base with
# origin/dev, ending in one verdict line (results.py verdict).
#   -n N            runs per side (default 2)
#   --pair          always run the reference in the same batch (needed for an FPS claim of
#                   record); without it, a fresh stored reference is reused when one exists
#   --ref SHA       reference commit (default: git merge-base HEAD origin/dev), built by refbuild.sh
#   --ref-install D / --new-install D   use these installs instead (default: refbuild, checkout)
#   --env "K=V ..." env for both sides; --ref-env / --new-env for one side (e.g. a kill switch)
#   --window A-B    frame window (default 200-500; 1000-1600 for numbers of record)
#   --max-age H     stored reference runs count if younger than H hours (default 24)
#   --shot F        run 1 of each side saves a screenshot at level frame F (GARDEN_SHOT)
#   --extra LINE    one more batch.sh line, run in the same lock hold (repeatable; gate.sh)
#   --hitch A-B     one more run a side over level frames A-B with KYTY_FRAME_DIAG=1, in the same
#                   lock hold: /tmp/bench-<label>-hitch-{ref,new}.log for hitches.py (gate.sh)
#   --hitch-new-only  the hitch run on the new side only (gate.sh compares it with a stored baseline)
#   --hitch-shot F  the hitch runs also save level frame F (gate.sh: instead of run 1 of a pair)
#   --sequential    run one pair at a time and stop as soon as a decision metric (cmd_ns, cpd) is
#                   a signal (>= 3 sd either way), else add a pair, up to -n
#   --min-ref N     stored reference runs needed to reuse them (default 3)
#   --flags "F"     emulator flags of every run (default --amd-cpu; plain words, no quotes: batch.sh
#                   splits lines on whitespace). Stored references count only with the same flags.
# The whole A/B (pairs, --extra lines, hitch runs) is one hold of the bench lock.
# Window: 200-500 ends each run ~23 s sooner (~35 s instead of ~59 s) and gives the same verdicts
# as 1000-1600 (PR #87); its FPS and CPU-ns noise is 2-3x larger, its cycles/draw noise the same.
# Reuse: with >= 3 stored clean runs of the reference (same binary, window, env, flags) younger
# than --max-age, only the new side runs. Instructions and cycles per draw do not drift between
# batches, so those verdicts stand; CPU ns get a drift allowance; FPS and waits drift with clock
# speed and are marked x. An FPS claim of record: ab.sh <label> --pair --window 1000-1600 -n 2.
# Build modes (KYTY_BUILD_MODE, refbuild.sh): both sides must have the same BUILD_MODE line mode
# and profile; for pgo pass --new-install "$(tools/astro-bench/build.sh)".
# Results: /tmp/garden-<label>-{ref,new}<i>.{out,json}, the store (results.sh), and the verdict.
AB_WINDOW_DEFAULT=200-500
dir=$(dirname "$0")
repo=$(git -C "$dir" rev-parse --show-toplevel)
label=$1
[[ -n "$label" && "$label" != -* ]] || { sed -n '2,17p' "$0" >&2; exit 2; }
shift
n=2 pair=0 max_age=24 ref_sha= ref_install= new_install= env= ref_env= new_env= shot= hitch=
hitch_new_only=0 hitch_shot= sequential=0 min_ref=3 flags=--amd-cpu
extra=()
window=${GARDEN_FRAMES:-$AB_WINDOW_DEFAULT}
while [ $# -gt 0 ]; do
	case "$1" in
	-n) n=$2; shift ;;
	--pair) pair=1 ;;
	--ref) ref_sha=$2; shift ;;
	--ref-install) ref_install=$2; shift ;;
	--new-install) new_install=$2; shift ;;
	--env) env=$2; shift ;;
	--ref-env) ref_env=$2; shift ;;
	--new-env) new_env=$2; shift ;;
	--window) window=$2; shift ;;
	--max-age) max_age=$2; shift ;;
	--shot) shot=$2; shift ;;
	--extra) extra+=("$2"); shift ;;
	--hitch) hitch=$2; shift ;;
	--hitch-new-only) hitch_new_only=1 ;;
	--hitch-shot) hitch_shot=$2; shift ;;
	--sequential) sequential=1 ;;
	--min-ref) min_ref=$2; shift ;;
	--flags) flags=$2; shift ;;
	*) echo "ab.sh: unknown option $1" >&2; exit 2 ;;
	esac
	shift
done
[[ "$flags" == *[\"\']* ]] && { echo "ab.sh: --flags takes plain words (no quotes)" >&2; exit 2; }
[ "$KYTY_BENCH_LOCKED" = x ] && { echo "ab.sh: run it outside batch.sh (it builds and takes the lock itself)" >&2; exit 1; }
if [ -z "$ref_install" ]; then
	ref_sha=${ref_sha:-$(git -C "$repo" merge-base HEAD origin/dev)} || exit 1
	ref_sha=$(git -C "$repo" rev-parse --verify "$ref_sha^{commit}") || exit 1
	ref_install=$("$dir/refbuild.sh" "$ref_sha") || { echo "ab.sh: refbuild.sh $ref_sha failed" >&2; exit 1; }
fi
new_install=${new_install:-$repo/_Build/linux/install}
[ -x "$new_install/kyty_emulator" ] || { echo "ab.sh: no kyty_emulator in $new_install" >&2; exit 1; }
built=$(stat -c %Y "$repo/_Build/linux/kyty_emulator" 2>/dev/null || echo 0)
if [ "$new_install" = "$repo/_Build/linux/install" ] && [ "$built" -gt "$(stat -c %Y "$new_install/kyty_emulator")" ]; then
	echo "ab.sh: _Build/linux/kyty_emulator is newer than the install; run cmake --install first" >&2
	exit 1
fi
# Both sides in the same build mode with the same profile (BUILD_MODE next to the install).
bm() { # "<mode> [<profile hash>]"; no BUILD_MODE file: a plain build
	[ -f "$1/../BUILD_MODE" ] || { echo plain; return; }
	sed -n 's/.*\bmode=\([a-z]*\).*/\1/p; s/.*\bsha256=\([0-9a-f]*\).*/\1/p' "$1/../BUILD_MODE" | xargs
}
if [ "$(bm "$ref_install")" != "$(bm "$new_install")" ]; then
	echo "ab.sh: build modes differ: ref '$(bm "$ref_install")' vs new '$(bm "$new_install")' (BUILD_MODE files)" >&2
	exit 1
fi
# The env string garden.sh records: KYTY_* settings, sorted (minus the bench's own).
# Later assignments win, as in `env A=1 A=0`.
norm_env() {
	tr ' ' '\n' <<<"$*" | grep -E '^KYTY_' | grep -vE '^KYTY_(BENCH_LOCKED|APP0_OVERLAY|INSTALL|SHOT_FILE)=' |
		awk -F= '{v[$1] = $0} END {for (k in v) print v[k]}' | sort | tr '\n' ' ' | sed 's/ $//'
}
inherited=$(env | grep -E '^KYTY_' | tr '\n' ' ')
ref_key=$(norm_env "$inherited $env $ref_env")
exe_hash() { sha256sum "$1/kyty_emulator" | cut -c1-16; }
ref_exe=$(exe_hash "$ref_install")
reuse=0
if [ "$pair" = 0 ] && [ -n "$ref_sha" ]; then
	count=$(python3 -I "$dir/results.py" ref "$ref_sha" --window "$window" --env "$ref_key" --flags="$flags" \
		--exe "$ref_exe" --route "${GARDEN_ROUTE:-lvl}" --max-age "$max_age" --count 2>/dev/null)
	[ "${count:-0}" -ge "$min_ref" ] && reuse=1
fi
pair_lines() { # pair_lines <i>: the batch lines of pair i
	local s=
	[ "$1" = 1 ] && [ -n "$shot" ] && s=GARDEN_SHOT=$shot
	[ "$reuse" = 0 ] && echo "$label-ref$1 KYTY_INSTALL=$ref_install GARDEN_FRAMES=$window $s $env $ref_env -- $flags"
	echo "$label-new$1 KYTY_INSTALL=$new_install GARDEN_FRAMES=$window $s $env $new_env -- $flags"
}
tail_lines=("${extra[@]}")
if [ -n "$hitch" ]; then
	[ "$hitch_new_only" = 0 ] && tail_lines+=("$label-hitch-ref KYTY_INSTALL=$ref_install KYTY_FRAME_DIAG=1 GARDEN_FRAMES=$hitch ${hitch_shot:+GARDEN_SHOT=$hitch_shot} $env $ref_env -- $flags")
	tail_lines+=("$label-hitch-new KYTY_INSTALL=$new_install KYTY_FRAME_DIAG=1 GARDEN_FRAMES=$hitch ${hitch_shot:+GARDEN_SHOT=$hitch_shot} $env $new_env -- $flags")
fi
for i in $(seq "$n"); do rm -f "/tmp/garden-$label-ref$i.json" "/tmp/garden-$label-new$i.json" "/tmp/garden-$label-ref$i.out" "/tmp/garden-$label-new$i.out"; done
# One hold for everything below: pairs, extra lines, hitch runs (batch.sh nests in it).
. "$dir/benchlock.sh"
benchlock_acquire x "ab.sh $label" || exit 1
# results <upto>: the verdict over pairs 1..upto (empty on a missing result)
news=() refs=() failed=()
collect() {
	news=() refs=() failed=()
	for i in $(seq "$1"); do
		for side in ref new; do
			[ "$side" = ref ] && [ "$reuse" = 1 ] && continue
			if [ -f "/tmp/garden-$label-$side$i.json" ]; then
				[ "$side" = ref ] && refs+=("/tmp/garden-$label-$side$i.json") || news+=("/tmp/garden-$label-$side$i.json")
			else
				failed+=("$label-$side$i")
			fi
		done
	done
}
run_verdict() {
	if [ "$reuse" = 1 ]; then
		python3 -I "$dir/results.py" verdict --label "$label" --reused --new "${news[@]}" \
			--ref-stored "$ref_sha" --window "$window" --env "$ref_key" --flags="$flags" --exe "$ref_exe" \
			--route "${GARDEN_ROUTE:-lvl}" --max-age "$max_age"
	else
		python3 -I "$dir/results.py" verdict --label "$label" --ref "${refs[@]}" --new "${news[@]}"
	fi
}
done_pairs=0
step=$n
[ "$sequential" = 1 ] && step=1
while [ "$done_pairs" -lt "$n" ]; do
	lines=()
	upto=$((done_pairs + step))
	((upto > n)) && upto=$n
	for i in $(seq $((done_pairs + 1)) "$upto"); do mapfile -t -O "${#lines[@]}" lines < <(pair_lines "$i"); done
	# batch.sh warms up its first line's install only; a cold shader store (a new binary)
	# compiles during the first frames, so the other install gets its own short warm-up.
	if [ "$done_pairs" = 0 ] && [ "$reuse" = 0 ]; then
		lines=("${lines[0]}" "$label-new-warmup KYTY_INSTALL=$new_install GARDEN_FRAMES=0-150 $env $new_env -- $flags" "${lines[@]:1}")
	fi
	echo "ab.sh: pairs $((done_pairs + 1))-$upto: ${#lines[@]} runs (window $window, reference ${ref_sha:0:12}$([ "$reuse" = 1 ] && echo ": stored runs reused"))" >&2
	printf '%s\n' "${lines[@]}" | BATCH_WARMUP=$([ "$done_pairs" = 0 ] && echo 1 || echo 0) "$dir/batch.sh" || exit 1
	for i in $(seq $((done_pairs + 1)) "$upto"); do
		for side in ref new; do [ "$side" = ref ] && [ "$reuse" = 1 ] && continue; head -1 "/tmp/garden-$label-$side$i.out" >&2; done
	done
	done_pairs=$upto
	collect "$done_pairs"
	[ ${#failed[@]} != 0 ] && break
	if [ "$sequential" = 1 ] && [ "$done_pairs" -lt "$n" ]; then
		v=$(run_verdict 2>/dev/null)
		if grep -qE '(cmd_ns|cpd) [-+][0-9.]+%±[0-9.]+!' <<<"$v"; then
			echo "ab.sh: a decision metric is a signal after $done_pairs pair(s); stopping (sequential)" >&2
			break
		fi
	fi
done
if [ ${#tail_lines[@]} != 0 ]; then
	printf '%s\n' "${tail_lines[@]}" | BATCH_WARMUP=0 "$dir/batch.sh"
fi
# A run that failed (or a binary without METRICS lines) leaves no JSON: no verdict on a partial batch.
if [ ${#failed[@]} != 0 ]; then
	echo "ab.sh: no result from ${failed[*]} (see /tmp/garden-<label>.out); no verdict" >&2
	exit 1
fi
echo "AB_PAIRS=$done_pairs" >&2
run_verdict
