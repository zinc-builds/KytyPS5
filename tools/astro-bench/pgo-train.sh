#!/bin/bash
# pgo-train.sh [commit]: train a PGO profile for <commit> (default origin/dev) on the Sky Garden
# bench and print its path, ~/.cache/kyty-pgo/<sha>.profdata. A trained commit is reused.
#   1. builds the instrumented emulator (KYTY_BUILD_MODE=pgogen refbuild.sh, shared build cache),
#   2. under one exclusive bench-lock hold: a short warm-up run (shader cache), then the measured
#      garden.sh run with KYTY_PGO_DUMP (the emulator writes its counters every 10 s),
#   3. merges the newest whole dump into <sha>.profdata.
# It moves current.profdata (a link) to the new profile when <commit> is on origin/dev and is not
# older than the commit current.profdata was trained on; KYTY_PGO_CURRENT=0 never moves it.
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
dir=$KYTY_PGO_DIR
out=$dir/$sha.profdata
mkdir -p "$dir"
log() { echo "pgo-train: $*" >&2; }

# current.profdata -> <sha>.profdata: move it forward along origin/dev only.
update_current() {
	[ "${KYTY_PGO_CURRENT:-1}" = 0 ] && return 0
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

if [ -f "$out" ]; then update_current; echo "$out"; exit 0; fi
[ -n "$KYTY_BENCH_LOCKED" ] && { echo "pgo-train: call it outside any bench-lock hold (it builds, then runs)" >&2; exit 1; }
exec 7>"$dir/$sha.lock"
flock 7 # one trainer per commit; later callers wait and reuse its profile
if [ -f "$out" ]; then update_current; echo "$out"; exit 0; fi

t0=$SECONDS
inst=$(KYTY_BUILD_MODE=pgogen "$bench/refbuild.sh" "$sha")
t_build=$((SECONDS - t0))
grep -q KYTY_PGO_DUMP "$inst/kyty_emulator" || { echo "pgo-train: $sha predates KYTY_PGO_DUMP; cannot train it" >&2; exit 1; }
log "instrumented build ${t_build}s (lock wait included): $inst"

tmp=$dir/$sha.tmp.$$
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
GARDEN_FRAMES=0-150 GARDEN_WINDOW='' "$bench/garden.sh" "pgo-warm-$label" --amd-cpu > "/tmp/garden-pgo-warm-$label.out" 2>&1 ||
	log "warm-up run failed (/tmp/garden-pgo-warm-$label.out); training anyway"
sleep 3
KYTY_PGO_DUMP=$tmp/train "$bench/garden.sh" "pgo-train-$label" --amd-cpu > "/tmp/garden-pgo-train-$label.out" 2>&1 || {
	echo "pgo-train: training run failed: $(tail -3 "/tmp/garden-pgo-train-$label.out")" >&2
	exit 1
}
exec 9>&-
unset KYTY_BENCH_LOCKED
t_run=$((SECONDS - t1))
log "runs ${t_run}s: $(head -1 "/tmp/garden-pgo-train-$label.out")"

# The two dump files alternate; a kill during a write truncates one. Take the newest that merges.
merged=
for f in $(ls -t "$tmp"/train.*.profraw 2>/dev/null); do
	if llvm-profdata merge -o "$tmp/out.profdata" "$f" 2>/dev/null; then merged=$f; break; fi
	log "skipping unreadable dump $f"
done
[ -n "$merged" ] || { echo "pgo-train: no readable dump in $tmp" >&2; exit 1; }
mv "$tmp/out.profdata" "$out"
log "profile $(sha256sum "$out" | cut -c1-12) from $(basename "$merged"); total $((SECONDS - t0))s"
update_current
echo "$out"
