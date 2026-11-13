#!/bin/bash
# Usage: run.sh <logname> [extra kyty_emulator flags...] — launches Astro Bot for benchmarking.
# Windowed 1920x1080, muted (SDL dummy audio), real DualSense hidden so vpad.py is player 1.
# KYTY_INSTALL: emulator install dir (default: this checkout's _Build/linux/install)
# KYTY_GAME:    game dump (default: ~/Games/PS5/PPSA21567.zar)
log=/tmp/bench-$1.log; shift
# Set but empty usually means a failed `ref=$(refbuild.sh …)`: never fall back to this checkout.
if [ -n "${KYTY_INSTALL+x}" ] && [ ! -x "$KYTY_INSTALL/kyty_emulator" ]; then
	echo "run.sh: KYTY_INSTALL='$KYTY_INSTALL' has no kyty_emulator (did refbuild.sh fail?)" >&2
	echo "EXIT STATUS: bad KYTY_INSTALL" > "$log"
	exit 1
fi
. "$(dirname "$0")/benchlock.sh"
benchlock_acquire x "run.sh $(basename "$log")" || exit $?
repo=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
# Whole seconds: cmake --install copies the build's mtime but drops the fraction.
built=$(stat -c %Y "$repo/_Build/linux/kyty_emulator" 2>/dev/null || echo 0)
installed=$(stat -c %Y "$repo/_Build/linux/install/kyty_emulator" 2>/dev/null || echo 0)
if [ -z "$KYTY_INSTALL" ] && [ "$built" -gt "$installed" ]; then
	echo "run.sh: _Build/linux/kyty_emulator is newer than the installed one; run cmake --install first" >&2
	echo "EXIT STATUS: stale install" > "$log"
	exit 1
fi
cd "${KYTY_INSTALL:-$repo/_Build/linux/install}" || exit 1
# The bench's shader store: one directory shared by every bench run (all tracks and installs, one
# store file per codegen hash, device and switches), apart from the store the user plays from
# (~/.cache/kyty-shaders, written only by builds they ran). Set KYTY_SHADER_STORE_DIR to isolate a
# run, e.g. KYTY_SHADER_STORE_DIR=$KYTY_INSTALL/_PipelineCache for a per-install store.
export KYTY_SHADER_STORE_DIR=${KYTY_SHADER_STORE_DIR:-$HOME/.cache/kyty-shaders-bench}
export KYTY_PERF_LOG=1 SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1
export SDL_GAMECONTROLLER_IGNORE_DEVICES=0x054c/0x0ce6
export SDL_AUDIO_DRIVER=dummy
./kyty_emulator --game "${KYTY_GAME:-$HOME/Games/PS5/PPSA21567.zar}" --shader-cache --skip-notice-screen \
  --perf-overlay --present-mode Mailbox --vblank-frequency 60 \
  --screen-width 1920 --screen-height 1080 "$@" > "$log" 2>&1
st=$?; echo "EXIT STATUS: $st" >> "$log"
