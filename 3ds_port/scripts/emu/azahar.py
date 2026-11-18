"""Shared pieces of the Azahar test loop on Linux (Hyprland / Wayland).

Windows drove the emulator by posting window messages; here the compositor does
it. Hyprland can hand a key press to a window that has no focus
(`send_key_state`), so keys need no focus and no foreign tool. Taps need a
pointer, which only exists as real input: ydotool (user service `ydotool`,
/dev/uinput) after moving Hyprland's cursor. Captures are grim over the
window's rectangle.

Needs: hyprctl (Hyprland >= 0.55, Lua config), grim, ydotool + `ydotool.service`
running, ImageMagick (crop.py), Python 3.
"""
import configparser
import json
import os
import re
import subprocess
import sys
import time

CONFIG = os.path.expanduser("~/.config/azahar-emu/qt-config.ini")
SAVE_DIR = os.path.expanduser("~/.local/share/azahar-emu/sdmc/3ds/pokeemerald")
APPIMAGE = os.path.expanduser("~/.local/opt/azahar/azahar.AppImage")

# Qt key code -> X keysym, for the printable ones; the rest by name.
SPECIAL = {0x01000004: "Return", 0x01000005: "Return", 0x01000003: "BackSpace",
           0x20: "space", 0x01000012: "Left", 0x01000013: "Up",
           0x01000014: "Right", 0x01000015: "Down"}
DEFAULT_KEYS = {"a": "a", "b": "s", "start": "m", "select": "n", "l": "q", "r": "w",
                "up": "t", "down": "g", "left": "f", "right": "h"}


def die(msg):
    print(msg)
    sys.exit(1)


def hypr(*args):
    r = subprocess.run(["hyprctl", *args], capture_output=True, text=True)
    return r.stdout + r.stderr


def window():
    """Azahar's main window as Hyprland reports it: address, at, size."""
    try:
        clients = json.loads(subprocess.check_output(["hyprctl", "-j", "clients"], text=True))
    except Exception as e:  # not running under Hyprland
        die("no hyprctl: %s" % e)
    for c in clients:
        name = (c.get("class", "") + c.get("initialClass", "")).lower()
        if "azahar" in name and c.get("mapped", True) and c["size"][0] > 200:
            return c
    die("no azahar window")


def sel(win):
    return "address:" + win["address"]


def controls():
    """button -> keysym, from the emulator's own qt-config.ini ([Controls])."""
    keys = dict(DEFAULT_KEYS)
    if not os.path.exists(CONFIG):
        return keys
    for line in open(CONFIG, encoding="utf-8", errors="replace"):
        m = re.match(r'profiles\\1\\button_(\w+)="code:(\d+),engine:keyboard"', line.strip())
        if not m or m.group(1) not in keys:
            continue
        code = int(m.group(2))
        if code in SPECIAL:
            keys[m.group(1)] = SPECIAL[code]
        elif 0x20 < code < 0x7F:
            keys[m.group(1)] = chr(code).lower()
    return keys


def key(win, sym, state):
    out = hypr("dispatch", 'hl.dsp.send_key_state({mods="",key="%s",state="%s",window="%s"})'
               % (sym, state, sel(win)))
    if "error" in out.lower() or "not found" in out.lower():
        die("send_key_state failed: " + out.strip())


def screens(win, top=0, bottom=0):
    """Both screens stacked, one scale, centred (the layout the scripts assume):
    returns (scale, x0, y0) with (x0, y0) the top-left of the 400x240 top screen
    in global coordinates. `top`/`bottom` are the pixels the menu and status
    bars take from the window (calibrate once: see README)."""
    x, y = win["at"]
    w, h = win["size"]
    h -= top + bottom
    scale = min(w / 400.0, h / 480.0)
    return scale, x + (w - 400 * scale) / 2, y + top + (h - 480 * scale) / 2
