#!/usr/bin/env python3
"""Compares two frames.raw dumps (FRAME_DUMP=1 builds, 3ds_video.c) frame by frame.

  frames_compare.py A.raw B.raw [--png DIR] [--all]

A dump is a run of records: a u32 frame number, then the top screen's left and
right buffers as the GPU left them (240x400 RGBA8 each, in 8x8 tiles, zeros for a
right eye that was not drawn). Records are paired by frame number. For each pair
it prints whether the pictures are identical byte for byte, and otherwise how many
pixels differ and by how much. With --png, the frames that differ (or every frame,
with --all) are written as A | B | difference, each eye on its own row.
"""
import argparse
import struct
import sys

W, H = 240, 400            # the buffer as the GPU holds it (the screen turned on its side)
EYE = W * H * 4
RECORD = 4 + 2 * EYE


def read(path):
    frames = {}
    with open(path, "rb") as f:
        data = f.read()
    for offset in range(0, len(data) - RECORD + 1, RECORD):
        (number,) = struct.unpack_from("<I", data, offset)
        frames[number] = (data[offset + 4:offset + 4 + EYE], data[offset + 4 + EYE:offset + RECORD])
    return frames


def untile(buf):
    """The 8x8-tiled, Z-ordered buffer as rows of 400x240 RGB pixels, as the screen shows it."""
    out = bytearray(400 * 240 * 3)
    for ty in range(H // 8):
        for tx in range(W // 8):
            base = (ty * (W // 8) + tx) * 64 * 4
            for i in range(64):
                x = (i & 1) | ((i >> 1) & 2) | ((i >> 2) & 4)
                y = ((i >> 1) & 1) | ((i >> 2) & 2) | ((i >> 3) & 4)
                bx, by = tx * 8 + x, ty * 8 + y
                a, b, g, r = buf[base + i * 4:base + i * 4 + 4]
                # The GPU's buffer is the screen turned: its x runs down the
                # screen from the bottom, its y across it.
                sx, sy = by, W - 1 - bx
                o = (sy * 400 + sx) * 3
                out[o:o + 3] = bytes((r, g, b))
    return out


def save_png(path, rows):
    from PIL import Image  # only needed for --png
    width = 400 * 3
    img = Image.new("RGB", (width, 240 * len(rows)))
    for row, (a, b) in enumerate(rows):
        pa, pb = untile(a), untile(b)
        diff = bytes(min(255, abs(x - y) * 8) for x, y in zip(pa, pb))
        for col, pix in enumerate((pa, pb, diff)):
            img.paste(Image.frombytes("RGB", (400, 240), bytes(pix)), (col * 400, row * 240))
    img.save(path)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("a")
    p.add_argument("b")
    p.add_argument("--png", help="folder for the pictures of the frames that differ")
    p.add_argument("--all", action="store_true", help="with --png, every frame")
    args = p.parse_args()
    fa, fb = read(args.a), read(args.b)
    common = sorted(set(fa) & set(fb))
    same = 0
    for n in common:
        eyes = []
        for eye in (0, 1):
            a, b = fa[n][eye], fb[n][eye]
            if a == b:
                eyes.append("same")
                continue
            px = worst = 0
            for i in range(0, EYE, 4):
                if a[i:i + 4] != b[i:i + 4]:
                    px += 1
                    worst = max(worst, max(abs(x - y) for x, y in zip(a[i:i + 4], b[i:i + 4])))
            eyes.append(f"{px} px differ (worst channel {worst})")
        identical = eyes == ["same", "same"]
        same += identical
        print(f"frame {n}: left {eyes[0]}; right {eyes[1]}")
        if args.png and (args.all or not identical):
            import os
            os.makedirs(args.png, exist_ok=True)
            save_png(os.path.join(args.png, f"frame_{n:06d}.png"), [(fa[n][0], fb[n][0]), (fa[n][1], fb[n][1])])
    only = sorted(set(fa) ^ set(fb))
    print(f"\n{same} of {len(common)} frames identical"
          + (f"; {len(only)} frames in one dump only: {only[:10]}{'...' if len(only) > 10 else ''}" if only else ""))
    return 0 if same == len(common) and common else 1


if __name__ == "__main__":
    sys.exit(main())
