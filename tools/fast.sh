#!/bin/bash
# fast.sh: build and launch the fast configuration, the one every number in
# docs/mt-rewrite/FINDINGS.md is measured with (docs/mt-rewrite/README.md, "Playing with the fast build").
#
#   tools/fast.sh build            PGO + ThinLTO build of kyty_emulator and the launcher into
#                                  _Build/linux-fast, installed to _Build/linux-fast/install
#   tools/fast.sh run <game> [flags...]
#                                  launch <game> (a .zar or a game folder) from that install with
#                                  the measured settings; extra kyty_emulator flags go last and
#                                  win (e.g. --fullscreen, --frame-skip auto)
#   tools/fast.sh flags            print the settings `run` uses
#
# Profile: KYTY_PGO_PROFILE, else ~/.cache/kyty-pgo/current.profdata (trained on Sky Garden by
# tools/astro-bench/pgo-train.sh; a profile from an older commit still works, the code changed
# since then is compiled without counts). Without a profile the build is ThinLTO only (~+4.7%
# instead of ~+11% in Sky Garden) and says so.
# JOBS: build parallelism (default 8; the machine is shared with benchmarks).
set -eo pipefail
repo=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
bdir=$repo/_Build/linux-fast
inst=$bdir/install

# The settings of the benchmark (tools/astro-bench/run.sh) that matter for speed, minus its
# bench-only parts (muted audio, a fixed 1920x1080 window, the perf log):
#   --amd-cpu        rewrite the AMD-only instructions the host lacks (SSE4a EXTRQ/INSERTQ and
#                    friends) to native code at load, and VRSQRTPS as the bench does. Without any
#                    patching (--no-amd-cpu), an Intel host emulates every EXTRQ in a SIGILL handler
#                    (~1.7M/s in Sky Garden, ~10 FPS instead of ~50).
#   --shader-cache   keep compiled shaders and pipelines between boots (no compile stutter after
#                    the first run of a level).
#   --present-mode Mailbox --vblank-frequency 60
#                    what the numbers are measured with (Mailbox is also the default).
# The patcher mode, in one place: --amd-cpu (what every number of record uses), or "" for the
# default auto, which is the same on a non-AMD host and patches nothing an AMD host has.
FAST_AMD_CPU=--amd-cpu
FAST_FLAGS=($FAST_AMD_CPU --shader-cache --present-mode Mailbox --vblank-frequency 60)

cmd=${1:-}
shift || true
case "$cmd" in
build)
	prof=${KYTY_PGO_PROFILE:-$HOME/.cache/kyty-pgo/current.profdata}
	if prof=$(readlink -e "$prof" 2>/dev/null) && [ -f "$prof" ]; then
		mode=(-DKYTY_LTO=thin -DKYTY_PGO=use "-DKYTY_PGO_PROFILE=$prof")
		echo "fast.sh: PGO + ThinLTO with $prof ($(sha256sum "$prof" | cut -c1-12))" >&2
	else
		mode=(-DKYTY_LTO=thin -DKYTY_PGO=OFF -DKYTY_PGO_PROFILE=)
		echo "fast.sh: no PGO profile (KYTY_PGO_PROFILE or ~/.cache/kyty-pgo/current.profdata);" \
			"building ThinLTO only. Train one with tools/astro-bench/pgo-train.sh." >&2
	fi
	[ -f "$repo/3rdparty/ffmpeg-core/CMakeLists.txt" ] || git -C "$repo" submodule update --init --recursive
	# On the project's shared machine, builds take the bench lock's shared side.
	lock=()
	[ -x "$repo/tools/astro-bench/benchlock.sh" ] && [ -z "$KYTY_BENCH_LOCKED" ] && lock=("$repo/tools/astro-bench/benchlock.sh" build)
	"${lock[@]}" cmake -S "$repo" -B "$bdir" -G Ninja -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ "${mode[@]}"
	"${lock[@]}" cmake --build "$bdir" --target kyty_emulator launcher -j "${JOBS:-8}"
	# The install's last step (the Qt launcher's image-plugin dependencies) can fail on some
	# systems; both binaries are installed by then.
	cmake --install "$bdir" --prefix "$inst" >/dev/null || true
	[ -x "$inst/kyty_emulator" ] || { echo "fast.sh: install failed ($inst)" >&2; exit 1; }
	echo "fast.sh: installed $inst (run: tools/fast.sh run <game>; or $inst/kyty_launcher)" >&2
	;;
run)
	game=${1:-}
	[ -n "$game" ] || { sed -n '2,20p' "$0" >&2; exit 2; }
	shift
	game=$(readlink -f "$game")
	[ -x "$inst/kyty_emulator" ] || { echo "fast.sh: build first (tools/fast.sh build)" >&2; exit 1; }
	cd "$inst"
	exec ./kyty_emulator --game "$game" "${FAST_FLAGS[@]}" "$@"
	;;
flags) echo "${FAST_FLAGS[*]}" ;;
*) sed -n '2,20p' "$0" >&2; exit 2 ;;
esac
