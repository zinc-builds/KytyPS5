#!/bin/bash
# refbuild.sh <commit>: print the install dir of a Release build of <commit>, building it once.
# Builds are shared by every checkout (~/.cache/kyty-builds/<sha>/install), so the reference
# build for an A/B is made once, not once per agent. Run it before batch.sh, not inside it:
#   ref=$(tools/astro-bench/refbuild.sh "$(git merge-base HEAD origin/dev)")
#   printf '%s\n' "ref1 KYTY_INSTALL=$ref -- --amd-cpu" "new1 -- --amd-cpu" ... | tools/astro-bench/batch.sh
# KYTY_BUILDS: cache root (default ~/.cache/kyty-builds).
# KYTY_SETUP_FROM: install dir to copy the save and shader cache from (as setup-install.sh does).
# Build output goes to stderr; only the install dir is printed on stdout.
set -eo pipefail
[ -n "$1" ] || { sed -n '2,10p' "$0" >&2; exit 2; }
repo=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
sha=$(git -C "$repo" rev-parse --verify --quiet "$1^{commit}") || { echo "refbuild: unknown commit $1" >&2; exit 1; }
root=${KYTY_BUILDS:-$HOME/.cache/kyty-builds}
dst=$root/$sha
done_marker=$dst/.complete
[ -f "$done_marker" ] && { echo "$dst/install"; exit 0; }

mkdir -p "$root"
exec 7>"$root/$sha.lock"
flock 7 # one builder per commit; later callers wait and reuse its result
[ -f "$done_marker" ] && { echo "$dst/install"; exit 0; }

src=$root/$sha.src
rm -rf "$src" "$dst"
git -C "$repo" worktree prune
git -C "$repo" worktree add -q --detach "$src" "$sha" >&2
trap 'git -C "$repo" worktree remove --force "$src" >/dev/null 2>&1 || true' EXIT
git -C "$src" submodule update --init --recursive -q >&2
# A shallow submodule can come back with an empty working tree.
[ -f "$src/3rdparty/ffmpeg-core/CMakeLists.txt" ] || git -C "$src/3rdparty/ffmpeg-core" checkout -qf HEAD >&2

cmake -S "$src" -B "$src/_Build/linux" -G Ninja -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ >&2
"$repo/tools/astro-bench/benchlock.sh" build cmake --build "$src/_Build/linux" --target kyty_emulator -j 8 >&2
# The install's last step (the Qt launcher's plugin dependencies) fails; kyty_emulator is in by then.
cmake --install "$src/_Build/linux" --prefix "$dst/install" >&2 || true
[ -x "$dst/install/kyty_emulator" ] || { echo "refbuild: install of $sha failed" >&2; exit 1; }
cp "$src/_Build/linux/kyty_emulator_clang_lld.map" "$dst/"
from=${KYTY_SETUP_FROM:-$HOME/AI/emu/KytyPS5/_Build/linux/install}
for d in _SaveData _PipelineCache; do
	[ -d "$from/$d" ] && cp -a "$from/$d" "$dst/install/"
done
touch "$done_marker"
echo "$dst/install"
