#!/bin/bash
# Copy the Astro Bot save (parked next to Sky Garden on the galaxy map) and the warm shader cache
# from an existing install into this checkout's install dir, so garden.sh works right after a build.
# Usage: setup-install.sh [source install dir] (default: ~/AI/emu/KytyPS5/_Build/linux/install)
src=${1:-$HOME/AI/emu/KytyPS5/_Build/linux/install}
repo=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
dst=$repo/_Build/linux/install
[ -d "$dst" ] || { echo "build and install first: $dst missing" >&2; exit 1; }
for d in _SaveData _PipelineCache; do
	[ -d "$src/$d" ] && cp -a "$src/$d" "$dst/" && echo "copied $d"
done
