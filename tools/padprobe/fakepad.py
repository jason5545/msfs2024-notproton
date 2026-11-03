#!/usr/bin/env python3
"""Drive the virtual gamepad of Wine patch 0059 (SQUARK_FAKEPAD) through its FIFO.

    tools/padprobe/fakepad.py <fifo> <step> [<step> ...]

Steps, in physical terms (what a hand does to a real pad):

    stick L|R <x> <y>    stick position, -1 to 1; y=1 is pushed up (away from you), x=1 right
    trigger L|R <v>      0 released to 1 fully pressed
    hold <button>        a b x y back guide start ls rs lb rb up down left right
    release <button>
    press <button> [ms]  hold, wait (default 100 ms), release
    wait <ms>
    reset                sticks centered, triggers and buttons released
    raw "<line>"         a line as the patch reads it (raw SDL values)

The physical-to-SDL conversion follows SDL's game controller convention: stick Y is negative when
pushed up, triggers run from -32768 (released) to 32767 on the virtual axis. Example:

    fakepad.py /tmp/pad.fifo "stick L 0 1" "wait 500" "press a" reset
"""
import os
import shlex
import sys
import time

BUTTONS = {"a", "b", "x", "y", "back", "guide", "start", "ls", "rs", "lb", "rb",
           "up", "down", "left", "right"}


def clamp(v):
    return max(-32768, min(32767, int(round(v))))


def lines_for(step):
    """Returns (lines to send, seconds to wait afterwards) for one step."""
    w = shlex.split(step)
    if not w:
        return [], 0
    op = w[0].lower()
    if op == "stick" and len(w) == 4 and w[1].upper() in ("L", "R"):
        s = w[1].lower()
        x, y = float(w[2]), float(w[3])
        return [f"{s}x {clamp(x * 32767)}", f"{s}y {clamp(-y * 32768)}"], 0
    if op == "trigger" and len(w) == 3 and w[1].upper() in ("L", "R"):
        v = min(1.0, max(0.0, float(w[2])))
        return [f"{w[1].lower()}t {clamp(-32768 + v * 65535)}"], 0
    if op in ("hold", "release") and len(w) == 2 and w[1].lower() in BUTTONS:
        return [f"{w[1].lower()} {1 if op == 'hold' else 0}"], 0
    if op == "press" and len(w) in (2, 3) and w[1].lower() in BUTTONS:
        return None, (w[1].lower(), int(w[2]) / 1000 if len(w) == 3 else 0.1)
    if op == "wait" and len(w) == 2:
        return [], int(w[1]) / 1000
    if op == "reset" and len(w) == 1:
        return ["reset"], 0
    if op == "raw" and len(w) == 2:
        return [w[1]], 0
    sys.exit(f"fakepad: cannot read step {step!r}")


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    fifo, steps = sys.argv[1], sys.argv[2:]
    plan = [lines_for(s) for s in steps]  # check every step before sending any
    # O_RDWR does not block when winebus has not opened the FIFO yet; O_WRONLY would.
    fd = os.open(fifo, os.O_RDWR)
    try:
        for lines, extra in plan:
            if lines is None:
                button, hold = extra
                os.write(fd, f"{button} 1\n".encode())
                time.sleep(hold)
                os.write(fd, f"{button} 0\n".encode())
                continue
            if lines:
                os.write(fd, ("\n".join(lines) + "\n").encode())
            if extra:
                time.sleep(extra)
    finally:
        os.close(fd)


if __name__ == "__main__":
    main()
