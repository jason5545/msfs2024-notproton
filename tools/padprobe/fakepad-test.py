#!/usr/bin/env python3
"""Checks the whole SDL -> winebus -> XInput path with the virtual gamepad of Wine patch 0059.

    tools/padprobe/fakepad-test.py [runtime dir] [prefix dir] [out dir]

Defaults: the fork's out/runtime/wine-11.18-fex, the throwaway prefix hadron-test/pfx-sdl, and
records/data/hadron/fakepad-<time>. Runs padprobe.exe under tools/hadron/run.sh in the background
(HADRON_MAC_BACKGROUND=1) with SQUARK_FAKEPAD set, moves the pad in physical terms with fakepad.py
and compares what XInput reports with what Windows gives for the same hand movement (XInput: thumb
Y positive up, X positive right, triggers 0 to 255). Never touches a game's prefix.
"""
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
SRC = os.path.expanduser("~/GitHub/msfs-src")
RT = sys.argv[1] if len(sys.argv) > 1 else f"{SRC}/forks/Hadron-steamac/out/runtime/wine-11.18-fex"
PFX = sys.argv[2] if len(sys.argv) > 2 else f"{SRC}/hadron-test/pfx-sdl"
OUT = sys.argv[3] if len(sys.argv) > 3 else f"{REPO}/records/data/hadron/fakepad-{time.strftime('%m%d-%H%M')}"

# XInput button bits
BIT = {"up": 0x0001, "down": 0x0002, "left": 0x0004, "right": 0x0008, "start": 0x0010,
       "back": 0x0020, "ls": 0x0040, "rs": 0x0080, "lb": 0x0100, "rb": 0x0200,
       "a": 0x1000, "b": 0x2000, "x": 0x4000, "y": 0x8000}

# (label, fakepad steps, expected XInput fields; a field left out must be at rest)
CASES = [
    ("rest", ["reset"], {}),
    ("left stick up (pushed away from you)", ["stick L 0 1"], {"ly": "+"}),
    ("left stick down", ["stick L 0 -1"], {"ly": "-"}),
    ("left stick right", ["stick L 1 0"], {"lx": "+"}),
    ("left stick left", ["stick L -1 0"], {"lx": "-"}),
    ("right stick up", ["stick R 0 1"], {"ry": "+"}),
    ("right stick right", ["stick R 1 0"], {"rx": "+"}),
    ("left trigger full", ["trigger L 1"], {"LT": 255}),
    ("left trigger half", ["trigger L 0.5"], {"LT": "mid"}),
    ("right trigger full", ["trigger R 1"], {"RT": 255}),
] + [(f"button {b}", [f"hold {b}"], {"buttons": BIT[b]}) for b in BIT]

LINE = re.compile(r"xinput slot 0: (\w+) buttons=([0-9a-f]+) LT=(\d+) RT=(\d+) "
                  r"lx=(-?\d+) ly=(-?\d+) rx=(-?\d+) ry=(-?\d+)")


def last_state(path):
    state = None
    with open(path, errors="replace") as f:
        for line in f:
            m = LINE.search(line)
            if m:
                state = {"conn": m[1], "buttons": int(m[2], 16), "LT": int(m[3]), "RT": int(m[4]),
                         "lx": int(m[5]), "ly": int(m[6]), "rx": int(m[7]), "ry": int(m[8])}
    return state


def check(state, want):
    bad = []
    for k in ("buttons", "LT", "RT", "lx", "ly", "rx", "ry"):
        v, w = state[k], want.get(k)
        if w is None:
            ok = v == 0 if k in ("buttons", "LT", "RT") else abs(v) < 1000
        elif w == "+":
            ok = v > 30000
        elif w == "-":
            ok = v < -30000
        elif w == "mid":
            ok = 100 <= v <= 155
        else:
            ok = v == w
        if not ok:
            bad.append(f"{k}={v}")
    return bad


def main():
    os.makedirs(OUT, exist_ok=True)
    fifo = os.path.join(OUT, "pad.fifo")
    if os.path.exists(fifo):
        os.unlink(fifo)
    env = dict(os.environ, SQUARK_FAKEPAD=fifo, HADRON_MAC_BACKGROUND="1", TMO="90",
               WINEDEBUG=os.environ.get("WINEDEBUG", "warn+hid"))
    secs = 60
    run = subprocess.Popen([f"{REPO}/tools/hadron/run.sh", RT, PFX, OUT,
                            f"{HERE}/padprobe.exe", str(secs)], env=env)
    stdout, stderr = os.path.join(OUT, "stdout.txt"), os.path.join(OUT, "stderr.txt")
    deadline = time.time() + 60
    while time.time() < deadline:
        s = last_state(stdout) if os.path.exists(stdout) else None
        if s and s["conn"] == "connected" and os.path.exists(fifo):
            break
        if run.poll() is not None:
            break
        time.sleep(0.5)
    else:
        s = None
    if not s or s["conn"] != "connected":
        print("FAIL: XInput slot 0 never connected; see", OUT)
        run.wait()
        return 1

    results = []
    for label, steps, want in CASES:
        subprocess.run([sys.executable, "-I", f"{HERE}/fakepad.py", fifo, "reset", *steps], check=True)
        time.sleep(0.4)
        state = last_state(stdout)
        bad = check(state, want)
        results.append((label, bad, state))
        print(f"{'ok  ' if not bad else 'FAIL'} {label}: "
              f"buttons={state['buttons']:04x} LT={state['LT']} RT={state['RT']} "
              f"lx={state['lx']} ly={state['ly']} rx={state['rx']} ry={state['ry']}"
              + (f"   wrong: {', '.join(bad)}" if bad else ""))
    subprocess.run([sys.executable, "-I", f"{HERE}/fakepad.py", fifo, "reset"], check=True)
    run.terminate()
    run.wait()
    failed = sum(1 for _, bad, _ in results if bad)
    with open(os.path.join(OUT, "result.txt"), "w") as f:
        for label, bad, state in results:
            f.write(f"{'ok' if not bad else 'FAIL'}\t{label}\t{state}\t{' '.join(bad)}\n")
    print(f"{len(results) - failed}/{len(results)} ok; raw output in {OUT}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
