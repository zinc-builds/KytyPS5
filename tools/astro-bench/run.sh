#!/bin/bash
# Usage: run.sh <logname> [extra kyty_emulator flags...] — launches Astro Bot for benchmarking.
# Windowed 1920x1080, muted (SDL dummy audio), real DualSense hidden so vpad.py is player 1.
# KYTY_INSTALL: emulator install dir (default: this checkout's _Build/linux/install)
# KYTY_GAME:    game dump (default: ~/Games/PS5/PPSA21567.zar)
log=/tmp/bench-$1.log; shift
. "$(dirname "$0")/benchlock.sh"
benchlock_acquire x "run.sh $(basename "$log")" || exit 1
repo=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
cd "${KYTY_INSTALL:-$repo/_Build/linux/install}" || exit 1
export KYTY_PERF_LOG=1 SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1
export SDL_GAMECONTROLLER_IGNORE_DEVICES=0x054c/0x0ce6
export SDL_AUDIO_DRIVER=dummy
./kyty_emulator --game "${KYTY_GAME:-$HOME/Games/PS5/PPSA21567.zar}" --shader-cache --skip-notice-screen \
  --perf-overlay --present-mode Mailbox --vblank-frequency 60 \
  --screen-width 1920 --screen-height 1080 "$@" > "$log" 2>&1
st=$?; echo "EXIT STATUS: $st" >> "$log"
