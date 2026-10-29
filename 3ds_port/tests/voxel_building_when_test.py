"""The placements a script switches, end to end (VXB9's conditions): a
buildings.bin of one model placed twice, the second placement standing only
while the cell at its corner holds metatile 0x222 - a Mauville gym beam, lit
(MAUVILLE_GYM_RED_H) - loaded by the console's own code
(voxel_building.c, through tests/voxel_building_when_test.c). The first
placement's cell is the model's whatever the switched cell holds; the
second's only while it holds 0x222, the map's own otherwise. The same file
as VXB8, with no conditions, stands both always.

    python tests/voxel_building_when_test.py [--cc gcc]
"""
import argparse
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ["src/voxel/voxel_building.c", "src/voxel/voxel_grade.c", "src/3ds_video_decode.c"]


def buildings(magic):
    """One 8x8 page, one model a cell high 16 px, placed at (2, 3) and
    (5, 3) of layout 1; with VXB9, the second only while its cell (0, 0)
    holds 0x222."""
    vertices = [0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 0.0, 8.0, 0.0, 1.0,
                1.0, 1.0, 1.0, 8.0, 8.0, 1.0]
    conditions = [(1, 0x222, 0, 0)] if magic == b"VXB9" else []
    head = magic + struct.pack("<HHHHHHIHH", 1, 1, 1, 2, 1, 0, 3, 0, 0)
    if magic == b"VXB9":
        head += struct.pack("<HH", len(conditions), 0)
    body = bytearray()
    body += struct.pack("<BBHIII", 1, 1, 0x20A, 0, 3, 0)          # the model
    body += struct.pack("<HHhh", 0, 0, 0, 0)                      # on page 0
    for x in (2, 5):
        body += struct.pack("<HHHHHHI", 1, 0, x, 3, 0x20A, 0, 0)
    body += bytes([16, 0])                                        # its cell, 16 px; pad
    body += struct.pack("<H", 0xFFFF)                             # a box
    body += bytes([0, 0])                                         # quarters; pad
    for c in conditions:
        body += struct.pack("<HHBB", *c)
    fixed = len(head) + 8 + len(body)
    body += bytes((-fixed) % 4)
    body += struct.pack("<%df" % len(vertices), *vertices)
    offset = len(head) + 8 + len(body)
    return head + struct.pack("<HHI", 8, 8, offset) + bytes(body) + bytes(8 * 8 * 2)


def run(cc, tmp, magic):
    path = tmp / ("buildings_%s.bin" % magic.decode())
    path.write_bytes(buildings(magic))
    exe = tmp / ("when_test_%s.exe" % magic.decode())
    subprocess.run([cc, "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror", "-Iinclude",
                    "-Isrc/voxel", "-iquote", "../include", "-D_GNU_SOURCE",
                    "-DPORT_LOG(...)=((void)0)", "-DVOXEL_HOST_FILES",
                    '-DVOXEL_BUILDINGS_PATH="%s"' % path.as_posix(),
                    "tests/voxel_building_when_test.c"] + SOURCES + ["-lm", "-o", str(exe)],
                   cwd=ROOT, check=True)
    return subprocess.run([str(exe)], cwd=ROOT, capture_output=True, text=True).stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", default="cc")
    args = ap.parse_args()
    want = {
        b"VXB9": ["0x20A 12,13:model@1.00 15,13:map",
                  "0x222 12,13:model@1.00 15,13:model@1.00",
                  "0x232 12,13:model@1.00 15,13:map"],
        b"VXB8": ["0x20A 12,13:model@1.00 15,13:model@1.00",
                  "0x222 12,13:model@1.00 15,13:model@1.00",
                  "0x232 12,13:model@1.00 15,13:model@1.00"],
    }
    failed = False
    with tempfile.TemporaryDirectory() as tmp:
        for magic, lines in want.items():
            got = run(args.cc, Path(tmp), magic).split("\n")[:len(lines)]
            ok = got == lines
            failed |= not ok
            print("%s %s: %s" % ("PASS" if ok else "FAIL", magic.decode(),
                                 "the switched placement stands with its metatile alone"
                                 if magic == b"VXB9" else "no conditions, both always"))
            if not ok:
                print("    got  %s\n    want %s" % (got, lines))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
