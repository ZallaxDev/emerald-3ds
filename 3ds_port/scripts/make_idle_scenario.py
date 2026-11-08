#!/usr/bin/env python3
"""Makes a scenario with no input of its own, from a recorded one.

  make_idle_scenario.py SRC DST [--frames N] [--set key=value ...]

SRC is a recorded scenario's folder (scenarios/NAME: save.sav, meta.txt,
input.bin); DST gets its save and its clock, its settings with --set over them,
no checks (there is no recording to compare with) and an input.bin of N frames
with nothing pressed. Played with a test build that drives the game itself
(3ds_cycle_test.c, CTR_BATTLE_TEST: A through the title and in battle), the run
is the same every time: the clock, and so the random numbers, are the
recording's. The double battle of the engine measurements.
"""
import argparse
import os
import shutil
import struct
import sys

INPUT_MAGIC = 0x4E493345  # "E3IN"
SAMPLE_SIZE = 12


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("src")
    p.add_argument("dst")
    p.add_argument("--frames", type=int, default=8000)
    p.add_argument("--set", action="append", default=[], metavar="KEY=VALUE")
    a = p.parse_args()
    meta = os.path.join(a.src, "meta.txt")
    save = os.path.join(a.src, "save.sav")
    if not os.path.isfile(meta) or not os.path.isfile(save):
        sys.exit(f"{a.src}: not a recorded scenario (meta.txt, save.sav)")
    overrides = dict(kv.split("=", 1) for kv in a.set)
    lines = []
    with open(meta, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith(("check ", "frames ")):
                continue
            key = line.split("=", 1)[0] if "=" in line else None
            if key in overrides:
                line = f"{key}={overrides.pop(key)}"
            lines.append(line)
    lines += [f"{k}={v}" for k, v in overrides.items()]
    lines.append(f"frames {a.frames}")
    os.makedirs(a.dst, exist_ok=True)
    shutil.copyfile(save, os.path.join(a.dst, "save.sav"))
    with open(os.path.join(a.dst, "meta.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    with open(os.path.join(a.dst, "input.bin"), "wb") as f:
        f.write(struct.pack("<4I", INPUT_MAGIC, 1, SAMPLE_SIZE, 0))
        f.write(bytes(SAMPLE_SIZE * a.frames))
    print(f"{a.dst}: {a.frames} frames, settings from {a.src}" + (f", {', '.join(a.set)}" if a.set else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
