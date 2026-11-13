#!/bin/bash
# build.sh [target]: build this checkout in KYTY_BUILD_MODE, install it into its own install dir
# and print that dir. The other side of an A/B against `refbuild.sh <merge-base>` in the same mode:
#   export KYTY_BUILD_MODE=pgo KYTY_PGO_PROFILE=$(readlink -e ~/.cache/kyty-pgo/current.profdata)
#   ref=$(tools/astro-bench/refbuild.sh "$(git merge-base HEAD origin/dev)")
#   new=$(tools/astro-bench/build.sh)
#   printf '%s\n' "ref1 KYTY_INSTALL=$ref -- --amd-cpu" "new1 KYTY_INSTALL=$new -- --amd-cpu" \
#     "ref2 KYTY_INSTALL=$ref -- --amd-cpu" "new2 KYTY_INSTALL=$new -- --amd-cpu" | tools/astro-bench/batch.sh
# Build dirs: plain uses _Build/linux (install: _Build/linux/install), the others _Build/linux-<mode>
# (install: _Build/linux-<mode>/install), so switching modes never rebuilds the plain tree. A new
# profile rebuilds the pgo tree (every compile command names the profile by its hash).
# The build takes the shared bench lock, -j 8. target: default kyty_emulator; a test target is
# built but not installed. KYTY_CMAKE_ARGS: extra cmake arguments (space-separated).
# Build output goes to stderr; only the install dir is printed on stdout.
set -eo pipefail
repo=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
. "$repo/tools/astro-bench/buildmode.sh"
kyty_build_mode "$repo"
target=${1:-kyty_emulator}
bdir=$repo/_Build/linux${mode_tag:+-$mode}
[ "$KYTY_BENCH_LOCKED" = x ] && { echo "build.sh: not inside an exclusive hold (batch.sh); build first" >&2; exit 1; }
. "$repo/tools/astro-bench/benchlock.sh"
benchlock_acquire s "build.sh $mode $repo" >&2 || exit $?
t0=$SECONDS
echo "build.sh: $(git -C "$repo" rev-parse --short HEAD) $mode_desc -> $bdir" >&2
[ -f "$repo/3rdparty/ffmpeg-core/CMakeLists.txt" ] || git -C "$repo/3rdparty/ffmpeg-core" checkout -qf HEAD >&2
# Reuse the FFmpeg package refbuild.sh caches, so a new build dir doesn't download it again.
ffopt=()
fsha=$(git -C "$repo/3rdparty/ffmpeg-core" rev-parse --short=12 HEAD)
ff=${KYTY_BUILDS:-$HOME/.cache/kyty-builds}/ffmpeg-$fsha
[ ! -f "$bdir/CMakeCache.txt" ] && [ -f "$ff/include/libavcodec/avcodec.h" ] && ffopt=(-DFFMPEG_PREBUILT_DIR="$ff")
cmake -S "$repo" -B "$bdir" -G Ninja -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ "${ffopt[@]}" "${mode_cmake[@]}" $KYTY_CMAKE_ARGS >&2
cmake --build "$bdir" --target "$target" -j 8 >&2
echo "build.sh: configure + build $((SECONDS - t0))s" >&2
[ "$target" = kyty_emulator ] || { echo "$bdir"; exit 0; }
# The install's last step (the Qt launcher's plugin dependencies) fails; kyty_emulator is in by then.
cmake --install "$bdir" --prefix "$bdir/install" >&2 || true
mkdir -p "$bdir/install" && cp -p "$bdir/kyty_emulator" "$bdir/install/"
[ -x "$bdir/install/kyty_emulator" ] || { echo "build.sh: install failed" >&2; exit 1; }
kyty_seed_install "$bdir/install"
echo "commit=$(git -C "$repo" rev-parse HEAD)$(git -C "$repo" diff --quiet HEAD -- src || echo +dirty) $mode_desc" > "$bdir/BUILD_MODE"
echo "$bdir/install"
