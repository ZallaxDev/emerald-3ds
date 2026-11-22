#!/usr/bin/env python3
"""Compare original per-pixel Pillow paths against equivalent bulk operations.

Diagnostic only: speedups are platform, Python and Pillow dependent.
Run: python tools/benchmark_pillow_hotspots.py
"""
import argparse
import random
import sys
import timeit
from pathlib import Path

from PIL import Image, __version__ as pillow_version

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "3ds_port/scripts"))
from voxel_image import image_pixels, opaque_bottom, rgb_palette  # noqa: E402


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repeats", type=int, default=5)
    args = parser.parse_args()
    rng = random.Random(300)
    alpha = Image.frombytes("RGBA", (64, 64), rng.randbytes(64 * 64 * 4))
    palette = Image.new("RGBA", (16, 16))
    colors = [(rng.randrange(256), rng.randrange(256), rng.randrange(256), rng.randrange(256))
              for _ in range(12)]
    palette.putdata([colors[rng.randrange(12)] for _ in range(256)])

    def old_bottom():
        return max(y for y in range(alpha.height) for x in range(alpha.width)
                   if alpha.getpixel((x, y))[3] >= 128) + 1

    def old_palette():
        return {palette.getpixel((x, y))[:3] for y in range(16) for x in range(16)}

    riser = Image.frombytes("RGB", (16, 16), rng.randbytes(256 * 3))
    columns = range(3, 13)

    def old_luminance():
        return [[0.3 * riser.getpixel((x, y))[0] + 0.59 * riser.getpixel((x, y))[1]
                 + 0.11 * riser.getpixel((x, y))[2] for x in columns] for y in range(16)]

    def new_luminance():
        pixels = image_pixels(riser)
        return [[0.3 * pixels[y * 16 + x][0] + 0.59 * pixels[y * 16 + x][1]
                 + 0.11 * pixels[y * 16 + x][2] for x in columns] for y in range(16)]

    print(f"Python {sys.version.split()[0]}, Pillow {pillow_version}")
    for name, old, new, n in [
        ("opaque-bottom 64x64", old_bottom, lambda: opaque_bottom(alpha), 100),
        ("RGB palette 16x16", old_palette, lambda: rgb_palette(palette), 1000),
        ("stair luminance 16x16", old_luminance, new_luminance, 1000),
    ]:
        assert old() == new()
        t_old = min(timeit.repeat(old, number=n, repeat=args.repeats))
        t_new = min(timeit.repeat(new, number=n, repeat=args.repeats))
        print(f"{name}: old {t_old:.4f}s / new {t_new:.4f}s ({t_old / t_new:.2f}x)")


if __name__ == "__main__":
    main()
