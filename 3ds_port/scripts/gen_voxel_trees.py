#!/usr/bin/env python3
"""Pack the voxel tree artwork as a small, GPU-ready RGBA5551 texture."""

import argparse
from pathlib import Path
import struct

from PIL import Image

DIM = 128
SOURCES = (("tree_crown.png", (32, 36), (0, 0)),
           ("tree_trunk.png", (32, 32), (32, 0)),
           ("tree_small_crown.png", (16, 32), (32, 32)),
           ("tree_small_trunk.png", (16, 16), (48, 32)),
           ("tree_2_small_crown.png", (16, 32), (64, 0)),
           ("tree_2_small_trunk.png", (16, 16), (80, 0)),
           ("tree_small_trunk_jungle.png", (16, 16), (48, 48)),
           ("tree_small_trunk_grass_road.png", (16, 16), (80, 16)),
           ("tree_small_trunk_volcan.png", (16, 16), (64, 32)),
           ("tree_crown_base_floor.png", (32, 32), (96, 0)),
           ("tree_crown_base.png", (32, 32), (0, 64)),
           ("tree_crown_base_open.png", (32, 32), (32, 64)),
           ("tree_crown_mossdeep.png", (32, 32), (64, 64)),
           ("tree_north_crown.png", (32, 32), (96, 64)),
           ("tree_base_mossdeep.png", (32, 16), (64, 96)),
           ("tree_base_water_mossdeep.png", (32, 16), (96, 96)),
           ("tree_north_base_rock.png", (32, 16), (0, 96)),
           ("tree_north_base_ash.png", (32, 16), (32, 96)),
           ("tall_grass.png", (16, 32), (80, 32)),
           ("tall_grass_floor.png", (16, 16), (96, 32)),
           ("tree_small_trunk_access.png", (16, 16), (112, 32)))


def texel_offset(x, y):
    """8x8 Morton tiles, matching CtrVideo_Texel (top row is v=1)."""
    morton = sum(((x >> bit) & 1) << (2 * bit) |
                 ((y >> bit) & 1) << (2 * bit + 1) for bit in range(3))
    return ((y // 8) * (DIM // 8) + x // 8) * 64 + morton


def pack(assets):
    pixels = bytearray(DIM * DIM * 2)
    for name, size, (ox, oy) in SOURCES:
        with Image.open(assets / name) as source:
            if source.size != size:
                raise ValueError(f"{name}: expected {size}, got {source.size}")
            image = source.convert("RGBA")
        for y in range(size[1]):
            for x in range(size[0]):
                r, g, b, a = image.getpixel((x, y))
                value = ((r >> 3) << 11 | (g >> 3) << 6 |
                         (b >> 3) << 1 | int(a >= 128))
                struct.pack_into("<H", pixels, 2 * texel_offset(ox + x, oy + y), value)
    return pixels


def main():
    port = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assets", type=Path, default=port / "assets/voxel/trees")
    parser.add_argument("--output", type=Path, default=port / "romfs/voxel/trees.rgba5551")
    args = parser.parse_args()
    pixels = pack(args.assets)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(pixels)
    print(f"voxel trees: {DIM}x{DIM}, {len(pixels)} bytes -> {args.output}")


if __name__ == "__main__":
    main()
