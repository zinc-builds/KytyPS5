#!/usr/bin/env python3
"""Virtual Xbox 360 pad for driving emulators from scripts.

Runs as a daemon reading commands from a FIFO (default /tmp/vpad.fifo), one per line:
  press <btn> [secs]          tap a button (default 0.15 s)
  hold <btn> <secs>           hold a button
  down <btn> / up <btn>       press / release without timing
  stick <L|R> <x> <y> [secs]  deflect a stick (-1..1, y up = +1); with secs, recentre afterwards
  trig <L|R> <0..1> [secs]    pull a trigger
  dpad <up|down|left|right> [secs]
  sleep <secs>
  center                      recentre both sticks, release everything
  quit
Buttons (Xbox names → DualSense): a=cross b=circle x=square y=triangle lb rb back(=create)
start(=options) guide(=PS) ls rs.
"""
import os
import sys
import time

from evdev import AbsInfo, UInput, ecodes as e

BTN = {
    "a": e.BTN_A, "b": e.BTN_B, "x": e.BTN_X, "y": e.BTN_Y,
    "lb": e.BTN_TL, "rb": e.BTN_TR, "back": e.BTN_SELECT, "start": e.BTN_START,
    "guide": e.BTN_MODE, "ls": e.BTN_THUMBL, "rs": e.BTN_THUMBR,
}
STICK = AbsInfo(0, -32768, 32767, 16, 128, 0)
TRIG = AbsInfo(0, 0, 255, 0, 0, 0)
HAT = AbsInfo(0, -1, 1, 0, 0, 0)
CAPS = {
    e.EV_KEY: list(BTN.values()),
    e.EV_ABS: [
        (e.ABS_X, STICK), (e.ABS_Y, STICK), (e.ABS_RX, STICK), (e.ABS_RY, STICK),
        (e.ABS_Z, TRIG), (e.ABS_RZ, TRIG), (e.ABS_HAT0X, HAT), (e.ABS_HAT0Y, HAT),
    ],
}


def axis(v: float) -> int:
    return max(-32768, min(32767, int(v * 32767)))


class Pad:
    def __init__(self) -> None:
        self.ui = UInput(CAPS, name="Microsoft X-Box 360 pad", vendor=0x045E, product=0x028E,
                         version=0x110, bustype=e.BUS_USB)

    def key(self, name: str, down: bool) -> None:
        self.ui.write(e.EV_KEY, BTN[name], 1 if down else 0)
        self.ui.syn()

    def abs(self, code: int, value: int) -> None:
        self.ui.write(e.EV_ABS, code, value)
        self.ui.syn()

    def stick(self, side: str, x: float, y: float) -> None:
        cx, cy = (e.ABS_X, e.ABS_Y) if side.upper() == "L" else (e.ABS_RX, e.ABS_RY)
        self.ui.write(e.EV_ABS, cx, axis(x))
        self.ui.write(e.EV_ABS, cy, axis(-y))  # evdev Y grows downwards
        self.ui.syn()

    def center(self) -> None:
        for code in (e.ABS_X, e.ABS_Y, e.ABS_RX, e.ABS_RY, e.ABS_Z, e.ABS_RZ, e.ABS_HAT0X,
                     e.ABS_HAT0Y):
            self.ui.write(e.EV_ABS, code, 0)
        for code in BTN.values():
            self.ui.write(e.EV_KEY, code, 0)
        self.ui.syn()

    def run(self, line: str) -> bool:
        parts = line.split()
        if not parts:
            return True
        cmd, args = parts[0].lower(), parts[1:]
        if cmd == "quit":
            return False
        if cmd == "press":
            self.key(args[0], True)
            time.sleep(float(args[1]) if len(args) > 1 else 0.15)
            self.key(args[0], False)
        elif cmd == "hold":
            self.key(args[0], True)
            time.sleep(float(args[1]))
            self.key(args[0], False)
        elif cmd == "down":
            self.key(args[0], True)
        elif cmd == "up":
            self.key(args[0], False)
        elif cmd == "stick":
            self.stick(args[0], float(args[1]), float(args[2]))
            if len(args) > 3:
                time.sleep(float(args[3]))
                self.stick(args[0], 0.0, 0.0)
        elif cmd == "trig":
            code = e.ABS_Z if args[0].upper() == "L" else e.ABS_RZ
            self.abs(code, int(float(args[1]) * 255))
            if len(args) > 2:
                time.sleep(float(args[2]))
                self.abs(code, 0)
        elif cmd == "dpad":
            code, val = {"up": (e.ABS_HAT0Y, -1), "down": (e.ABS_HAT0Y, 1),
                         "left": (e.ABS_HAT0X, -1), "right": (e.ABS_HAT0X, 1)}[args[0]]
            self.abs(code, val)
            time.sleep(float(args[1]) if len(args) > 1 else 0.15)
            self.abs(code, 0)
        elif cmd == "sleep":
            time.sleep(float(args[0]))
        elif cmd == "center":
            self.center()
        else:
            print(f"vpad: unknown command: {line}", file=sys.stderr, flush=True)
        return True


def main() -> None:
    fifo = sys.argv[1] if len(sys.argv) > 1 else "/tmp/vpad.fifo"
    if not os.path.exists(fifo):
        os.mkfifo(fifo)
    pad = Pad()
    print(f"vpad: ready on {fifo}", flush=True)
    try:
        while True:
            with open(fifo, encoding="utf-8") as f:
                for line in f:
                    try:
                        if not pad.run(line.strip()):
                            return
                    except (KeyError, IndexError, ValueError) as err:
                        print(f"vpad: bad command {line.strip()!r}: {err}", file=sys.stderr,
                              flush=True)
    finally:
        pad.center()
        pad.ui.close()


if __name__ == "__main__":
    main()
