#!/usr/bin/env python3
"""Tap the bottom screen at (X, Y) in 3DS pixels (0..319, 0..239).

    touch.py X Y [--hold 120] [--top 0] [--bottom 0]

Moves Hyprland's cursor over the bottom screen of Azahar's window (two screens
stacked, one scale, centred) and clicks with ydotool (user service
`ydotool`). The pointer is real, so the cursor ends up there. --top/--bottom
are the pixels of menu and status bar the window loses (calibrate once)."""
import argparse
import subprocess
import time

import azahar

ap = argparse.ArgumentParser()
ap.add_argument("x", type=int, nargs="?", default=160)
ap.add_argument("y", type=int, nargs="?", default=120)
ap.add_argument("--hold", type=int, default=120)
ap.add_argument("--top", type=int, default=0)
ap.add_argument("--bottom", type=int, default=0)
a = ap.parse_args()

win = azahar.window()
scale, x0, y0 = azahar.screens(win, a.top, a.bottom)
cx = int(x0 + (400 - 320) / 2 * scale + (a.x + 0.5) * scale)
cy = int(y0 + 240 * scale + (a.y + 0.5) * scale)
azahar.hypr("dispatch", 'hl.dsp.cursor.move({x=%d,y=%d})' % (cx, cy))
time.sleep(0.1)
subprocess.run(["ydotool", "click", "0x40"], check=True)      # left down
time.sleep(a.hold / 1000)
subprocess.run(["ydotool", "click", "0x80"], check=True)      # left up
print("tap %d,%d -> screen %d,%d (scale %.3f)" % (a.x, a.y, cx, cy, scale))
