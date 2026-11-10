#!/usr/bin/env python3
"""What each voxel optimisation is worth, from a run with settings.txt voxel_ab=N.

  voxel_ab_report.py port.log perf.bin [--skip 30]

With voxel_ab=N the voxel world goes through five configurations, N frames
each, every one adding one switch to the last (ctr_voxel.c, CtrVoxel_Update):
none, the scissor, the plain vertex program, the cards' memo, the fused blur.
port.log has a "VOXEL A/B config" line at each change with the game's frame,
which is perf.bin's tick. This puts every voxel picture of perf.bin under the
configuration it was drawn with, leaves out the first --skip frames after each
change, and prints the configurations side by side: the CPU's times, when the
GPU's queue was drained (gpuDone, the figure that decides a refresh on the
console) and the refreshes repeated.

Stand still, or ride the same loop, for the whole run: the configurations are
compared over different frames, so the scene has to be the same in all.
"""
import argparse
import csv
import os
import re
import statistics
import subprocess
import sys
import tempfile

LINE = re.compile(r"VOXEL A/B config (\d+) \(scissor (\d) plain (\d) memo (\d) fused (\d)\) from game frame (\d+)")
NAMES = ["none", "+scissor", "+plain vs", "+card memo", "+fused blur"]
# settings.txt voxel_cut=N: the whole picture, then less one part each time
CUT = re.compile(r"VOXEL CUT config (\d+) \(bits ([0-9a-f]+)\) from game frame (\d+)")
CUT_NAMES = ["whole", "-ground", "-trees", "-buildings", "-half fill", "-diorama", "-bloom", "-overlay", "-all"]
# settings.txt voxel_parts_ab=N: the ground whole, then only its parts in the picture
PARTS = re.compile(r"VOXEL PARTS config (\d+) from game frame (\d+)")
PARTS_NAMES = ["whole chunks", "parts in view"]
TIMES = ["ready", "game", "voxel", "vBillboards", "draw", "vDraw", "vChunks", "gpuStart", "gpu", "gpuDone", "vertices"]


def number(row, key):
    try:
        return float(row[key])
    except (KeyError, ValueError):
        return float("nan")


def percentile(values, p):
    values = sorted(values)
    return values[min(len(values) - 1, int(len(values) * p))] if values else float("nan")


def main():
    a = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    a.add_argument("log")
    a.add_argument("perf")
    a.add_argument("--skip", type=int, default=30)
    args = a.parse_args()

    changes = []
    names = NAMES
    with open(args.log, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = LINE.search(line)
            if m:
                changes.append((int(m.group(6)), int(m.group(1))))
            m = CUT.search(line)
            if m:
                changes.append((int(m.group(3)), int(m.group(1))))
                names = CUT_NAMES
            m = PARTS.search(line)
            if m:
                changes.append((int(m.group(2)), int(m.group(1))))
                names = PARTS_NAMES
    if not changes:
        sys.exit("no 'VOXEL A/B config', 'VOXEL CUT config' or 'VOXEL PARTS config' lines: was voxel_ab, voxel_cut or voxel_parts_ab set?")

    report = os.path.join(os.path.dirname(os.path.abspath(__file__)), "perf_report.py")
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "perf.csv")
        subprocess.run([sys.executable, report, "csv", args.perf, out], check=True, stdout=subprocess.DEVNULL)
        with open(out, newline="") as f:
            rows = list(csv.DictReader(f))

    by_config = {}
    at = 0
    for row in rows:
        tick = int(row["tick"])
        while at + 1 < len(changes) and changes[at + 1][0] <= tick:
            at += 1
        start, config = changes[at]
        if tick < start + args.skip or row.get("mode") != "voxel" or not row.get("route", "").startswith("voxel"):
            continue
        by_config.setdefault(config, []).append(row)

    configs = sorted(by_config)
    print("%-12s" % "" + "".join("%14s" % names[c] for c in configs))
    print("%-12s" % "pictures" + "".join("%14d" % len(by_config[c]) for c in configs))
    for key in TIMES:
        for label, pick in (("mean", None), ("p95", 0.95)):
            cells = []
            for c in configs:
                values = [v for v in (number(r, key) for r in by_config[c]) if v == v]
                if key == "gpuDone":
                    values = [v for v in values if v > 0]
                cells.append(statistics.mean(values) if values and pick is None
                             else percentile(values, pick) if values else float("nan"))
            print("%-12s" % ("%s %s" % (key, label)) + "".join("%14.2f" % v for v in cells))
    for key in ("lost", "shown"):
        cells = []
        for c in configs:
            values = [v for v in (number(r, key) for r in by_config[c]) if v == v and v < 255]
            cells.append(sum(values))
        print("%-12s" % ("%s sum" % key) + "".join("%14.0f" % v for v in cells))
    stereo = {r.get("route") for c in configs for r in by_config[c]}
    print("routes: " + ", ".join(sorted(stereo)))


if __name__ == "__main__":
    main()
