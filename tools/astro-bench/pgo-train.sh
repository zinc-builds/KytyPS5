#!/bin/bash
# pgo-train.sh [commit]: train a PGO profile for <commit> (default origin/dev) on the bench and
# print its path, ~/.cache/kyty-pgo/<sha>.profdata (default training set) or
# <sha>-<set>.profdata (another set). A trained commit and set is reused.
#   1. builds the instrumented emulator (KYTY_BUILD_MODE=pgogen refbuild.sh, shared build cache),
#   2. under one exclusive bench-lock hold, for each training level: a short warm-up run (shader
#      cache), then a garden.sh run with KYTY_PGO_DUMP (the emulator writes its counters every 10 s),
#   3. merges each level's newest whole dump into one profile (counts add up across levels).
# KYTY_PGO_LEVELS: the training set, "level[:A-B] ..." (garden.sh's GARDEN_LEVEL names and frame
#   windows; default window 1000-1600 for aerial_garden, 200-1000 for others). Default: the set in
#   DEFAULT_LEVELS below. KYTY_PGO_SET names a non-default set (default: a hash of the list).
#   <profile>.levels records the set.
# It moves current.profdata (a link) to the new profile when <commit> is on origin/dev, the set is
# the default one, and <commit> is not older than the commit current.profdata was trained on;
# KYTY_PGO_CURRENT=0 never moves it.
# Use the profile through KYTY_BUILD_MODE=pgo with refbuild.sh and build.sh (see build.sh).
# One trainer per commit (a lock in the profile store); a second caller waits and reuses it.
# Runs land in /tmp/garden-pgo-{warm,train}-<sha8>.out. About 10 minutes cold; see "Build modes"
# in docs/mt-rewrite/PLAN-60fps.md for the measured times and when to retrain.
# KYTY_PGO_DIR: profile store (default ~/.cache/kyty-pgo).
set -eo pipefail
repo=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
bench=$repo/tools/astro-bench
. "$bench/buildmode.sh"
rev=${1:-origin/dev}
sha=$(git -C "$repo" rev-parse --verify --quiet "$rev^{commit}") || { echo "pgo-train: unknown commit $rev" >&2; exit 1; }
DEFAULT_LEVELS="aerial_garden"
levels=${KYTY_PGO_LEVELS:-$DEFAULT_LEVELS}
levels=$(xargs <<<"$levels") # normalize spaces
dir=$KYTY_PGO_DIR
if [ "$levels" = "$DEFAULT_LEVELS" ]; then
	name=$sha default_set=1
else
	name=$sha-${KYTY_PGO_SET:-$(sha1sum <<<"$levels" | cut -c1-8)} default_set=0
	[[ "$name" =~ ^[0-9a-f]{40}-[A-Za-z0-9._-]+$ ]] || { echo "pgo-train: bad KYTY_PGO_SET" >&2; exit 1; }
fi
out=$dir/$name.profdata
# A named set reused with other levels would silently return the old profile: checked up front,
# whenever an existing profile is reused (a concurrent trainer may have just published it; its
# .levels is published first), and again under the trainer lock.
check_levels() {
	if [ -f "$out.levels" ] && [ "$(cat "$out.levels")" != "$levels" ]; then
		echo "pgo-train: $name was trained on \"$(cat "$out.levels")\", not \"$levels\"; pick another KYTY_PGO_SET" >&2
		exit 1
	fi
}
check_levels
mkdir -p "$dir"
log() { echo "pgo-train: $*" >&2; }

# current.profdata -> <sha>.profdata: move it forward along origin/dev only.
update_current() {
	[ "${KYTY_PGO_CURRENT:-1}" = 0 ] || [ "$default_set" = 0 ] && return 0
	git -C "$repo" merge-base --is-ancestor "$sha" origin/dev 2>/dev/null || return 0
	exec 6>"$dir/current.lock"
	flock 6
	local cur
	cur=$(readlink "$dir/current.profdata" 2>/dev/null || true)
	cur=${cur%.profdata}
	if [ -n "$cur" ] && [ "$cur" != "$sha" ] && git -C "$repo" merge-base --is-ancestor "$sha" "$cur" 2>/dev/null; then
		exec 6>&-
		return 0 # current is newer
	fi
	ln -sfn "$sha.profdata" "$dir/current.profdata.tmp.$$"
	mv -T "$dir/current.profdata.tmp.$$" "$dir/current.profdata"
	exec 6>&-
	log "current.profdata -> $sha.profdata"
}

if [ -f "$out" ]; then check_levels; update_current; echo "$out"; exit 0; fi
[ -n "$KYTY_BENCH_LOCKED" ] && { echo "pgo-train: call it outside any bench-lock hold (it builds, then runs)" >&2; exit 1; }
exec 7>"$dir/$name.lock"
flock 7 # one trainer per commit and set; later callers wait and reuse its profile
check_levels
if [ -f "$out" ]; then update_current; echo "$out"; exit 0; fi

t0=$SECONDS
inst=$(KYTY_BUILD_MODE=pgogen "$bench/refbuild.sh" "$sha")
t_build=$((SECONDS - t0))
grep -q KYTY_PGO_DUMP "$inst/kyty_emulator" || { echo "pgo-train: $sha predates KYTY_PGO_DUMP; cannot train it" >&2; exit 1; }
log "instrumented build ${t_build}s (lock wait included): $inst"

tmp=$dir/$name.tmp.$$
rm -rf "$tmp"; mkdir -p "$tmp"
trap 'rm -rf "$tmp"' EXIT
. "$bench/benchlock.sh"
benchlock_acquire x "pgo-train.sh $sha" >&2 || exit $?
t1=$SECONDS
# The exclusive lock means no benchmark is running: an emulator now is the user playing.
if pgrep -x kyty_emulator >/dev/null; then
	echo "pgo-train: kyty_emulator is running outside the bench (the user is playing?); try later" >&2
	exit 1
fi
label=${sha:0:8}
# A fresh binary starts with a cold shader store: warm it first, so the profile is the level, not
# shader compiles. The exit-hook profiles of these runs go to the scratch dir, not the install.
export KYTY_INSTALL=$inst LLVM_PROFILE_FILE=$tmp/exit-%p.profraw
i=0
for entry in $levels; do
	i=$((i + 1))
	lvl=${entry%%:*} win=${entry#*:}
	[ "$win" = "$entry" ] && { [ "$lvl" = aerial_garden ] && win=1000-1600 || win=200-1000; }
	tag=$label-$i-$lvl
	GARDEN_LEVEL=$lvl GARDEN_FRAMES=0-150 GARDEN_WINDOW='' "$bench/garden.sh" "pgo-warm-$tag" --amd-cpu \
		> "/tmp/garden-pgo-warm-$tag.out" 2>&1 || log "warm-up run failed (/tmp/garden-pgo-warm-$tag.out); training anyway"
	sleep 3
	GARDEN_LEVEL=$lvl GARDEN_FRAMES=$win KYTY_PGO_DUMP=$tmp/train-$i "$bench/garden.sh" "pgo-train-$tag" --amd-cpu \
		> "/tmp/garden-pgo-train-$tag.out" 2>&1 || {
		echo "pgo-train: training run on $lvl failed: $(tail -3 "/tmp/garden-pgo-train-$tag.out")" >&2
		exit 1
	}
	log "$lvl: $(head -1 "/tmp/garden-pgo-train-$tag.out" | cut -c1-120)"
	sleep 3
done
exec 9>&-
unset KYTY_BENCH_LOCKED
t_run=$((SECONDS - t1))
log "runs ${t_run}s"

# The two dump files of a run alternate; a kill during a write truncates one. Take each run's
# newest file that reads, then add the runs up.
picked=()
for n in $(seq "$i"); do
	ok=
	for f in $(ls -t "$tmp"/train-"$n".*.profraw 2>/dev/null); do
		if llvm-profdata merge -o "$tmp/check.profdata" "$f" 2>/dev/null; then ok=$f; break; fi
		log "skipping unreadable dump $f"
	done
	[ -n "$ok" ] || { echo "pgo-train: no readable dump for run $n in $tmp" >&2; exit 1; }
	picked+=("$ok")
done
llvm-profdata merge -o "$tmp/out.profdata" "${picked[@]}"
echo "$levels" > "$tmp/out.levels"
mv "$tmp/out.levels" "$out.levels"
mv "$tmp/out.profdata" "$out"
log "profile $(sha256sum "$out" | cut -c1-12) from ${#picked[@]} run(s) ($levels); total $((SECONDS - t0))s"
update_current
echo "$out"
