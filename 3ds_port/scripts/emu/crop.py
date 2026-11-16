#!/usr/bin/env python3
"""Crop and scale a capture so geometry can be judged by eye.

    crop.py IN OUT [--scale 2] [--rect X,Y,W,H]

Without --rect it cuts the 400x240 top screen out of a grab.py shot, assuming
the stacked layout of touch.py (--top/--bottom as there)."""
import argparse
import subprocess

import azahar

ap = argparse.ArgumentParser()
ap.add_argument("inp")
ap.add_argument("out")
ap.add_argument("--scale", type=int, default=2)
ap.add_argument("--rect")
ap.add_argument("--top", type=int, default=0)
ap.add_argument("--bottom", type=int, default=0)
a = ap.parse_args()

if a.rect:
    x, y, w, h = map(int, a.rect.split(","))
else:
    win = azahar.window()
    s, gx, gy = azahar.screens(win, a.top, a.bottom)
    x, y = int(gx - win["at"][0]), int(gy - win["at"][1])
    w, h = int(400 * s), int(240 * s)
subprocess.run(["magick", a.inp, "-crop", "%dx%d+%d+%d" % (w, h, x, y), "+repage",
                "-filter", "point", "-resize", "%d%%" % (a.scale * 100), a.out], check=True)
print("cropped -> " + a.out)
