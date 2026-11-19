#!/usr/bin/env python3
"""Capture Azahar's window to a PNG:  grab.py [out.png]

grim copies the screen under the window's rectangle, so anything on top of the
emulator ends up in the shot; the window is raised first."""
import subprocess
import sys
import time

import azahar

out = sys.argv[1] if len(sys.argv) > 1 else "shot.png"
win = azahar.window()
azahar.hypr("dispatch", 'hl.dsp.focus({window="%s"})' % azahar.sel(win))
time.sleep(0.5)
win = azahar.window()
(x, y), (w, h) = win["at"], win["size"]
if w <= 0 or h <= 0:
    azahar.die("bad rect")
subprocess.run(["grim", "-g", "%d,%d %dx%d" % (x, y, w, h), out], check=True)
print("saved %s  %dx%d" % (out, w, h))
