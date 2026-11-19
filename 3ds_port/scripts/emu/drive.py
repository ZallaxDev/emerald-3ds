#!/usr/bin/env python3
"""Press the 3DS buttons in a running Azahar, by a sequence.

    drive.py "start,wait4000,a,wait6000,a,b*3,up+2600" [--hold 130] [--gap 180]

Tokens: a b start select l r up down left right; `up*3` repeats, `up+2600`
holds 2600 ms, `wait1500` waits. The keys are the ones in Azahar's
qt-config.ini, sent to the window with Hyprland's send_key_state, which does not
need the window to have focus.
"""
import argparse
import re
import time

import azahar

ap = argparse.ArgumentParser()
ap.add_argument("seq")
ap.add_argument("--hold", type=int, default=130)
ap.add_argument("--gap", type=int, default=180)
a = ap.parse_args()

win = azahar.window()
keys = azahar.controls()
for tok in a.seq.split(","):
    t = tok.strip().lower()
    if not t:
        continue
    m = re.fullmatch(r"wait(\d+)", t)
    if m:
        time.sleep(int(m.group(1)) / 1000)
        continue
    rep, hold = 1, a.hold
    m = re.fullmatch(r"(\w+)\*(\d+)", t)
    if m:
        t, rep = m.group(1), int(m.group(2))
    m = re.fullmatch(r"(\w+)\+(\d+)", t)
    if m:
        t, hold = m.group(1), int(m.group(2))
    if t not in keys:
        print("unknown '%s'" % t)
        continue
    for _ in range(rep):
        azahar.key(win, keys[t], "down")
        time.sleep(hold / 1000)
        azahar.key(win, keys[t], "up")
        time.sleep(a.gap / 1000)
print("ok: " + a.seq)
