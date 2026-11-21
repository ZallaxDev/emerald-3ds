#!/usr/bin/env python3
"""Check Pillow optimizations against the literal old per-pixel algorithms.

No ROM or devkitARM needed. Test the actual production helper and riser_mask
function, including threshold boundaries, RGB/RGBA inputs and stair patterns.
"""
import ast
import random
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from voxel_image import image_pixels, opaque_bottom, rgb_palette  # noqa: E402


def old_bottom(art):
    return max(y for y in range(art.height) for x in range(art.width)
               if art.getpixel((x, y))[3] >= 128) + 1


def old_palette(img):
    return {img.getpixel((x, y))[:3] for y in range(16) for x in range(16)}


def old_riser(img):
    cols = range(3, 16 - 3)
    lum = [[0.3 * img.getpixel((c, r))[0] + 0.59 * img.getpixel((c, r))[1]
            + 0.11 * img.getpixel((c, r))[2] for c in cols] for r in range(16)]
    mean = [sum(row) / len(row) for row in lum]
    spread = max((sum((v - mean[r]) ** 2 for v in lum[r]) / len(lum[r])) ** 0.5 for r in range(16))
    agree = max(abs(mean[r] - mean[r + 8]) for r in range(8))
    fold = [(mean[r] + mean[r + 8]) / 2 for r in range(8)]
    start = min(range(8), key=lambda s: (sum(fold[(s + k) % 8] for k in range(3)), s))
    risers = [(start + k) % 8 for k in range(3)]
    gap = min(fold[r] for r in range(8) if r not in risers) - max(fold[r] for r in risers)
    mask = sum(1 << (r + 8 * p) for r in risers for p in (0, 1))
    clean = spread <= 20 and agree <= 16 and gap >= 24
    return (mask if clean else 0), (round(spread, 1), round(agree, 1), round(gap, 1))


def production_riser():
    source = ROOT / "scripts/gen_voxel_relief.py"
    tree = ast.parse(source.read_text(encoding="utf-8"), str(source))
    node = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == "riser_mask")
    namespace = {"image_pixels": image_pixels, "STEP_EDGE": 3,
                 "STEP_UNIFORM": 20, "STEP_AGREE": 16, "STEP_GAP": 24}
    exec(compile(ast.Module(body=[node], type_ignores=[]), str(source), "exec"), namespace)
    return namespace["riser_mask"]


def main():
    rng = random.Random(0xE3D5)
    riser = production_riser()
    for w, h in [(1, 1), (16, 16), (37, 19), (64, 64)]:
        for kind in ("empty", "below", "edge", "random", "opaque"):
            img = Image.new("RGBA", (w, h))
            if kind == "below":
                img.putpixel((w - 1, h - 1), (4, 5, 6, 127))
            elif kind == "edge":
                img.putpixel((w - 1, h - 1), (4, 5, 6, 128))
            elif kind == "opaque":
                img.putpixel((0, h - 1), (4, 5, 6, 255))
            elif kind == "random":
                img = Image.frombytes("RGBA", (w, h), rng.randbytes(w * h * 4))
            try:
                want = old_bottom(img)
            except ValueError:
                try:
                    opaque_bottom(img)
                except ValueError:
                    pass
                else:
                    raise AssertionError(f"Expected empty bounding box: {w}x{h}, {kind}")
            else:
                assert opaque_bottom(img) == want, (w, h, kind)
    for mode in ("RGB", "RGBA"):
        channels = len(mode)
        for i in range(150):
            # Include a restricted-palette tile (common in Emerald) as well
            # as noisy art; RGBA includes transparent colours intentionally.
            if i % 2:
                colours = [tuple(rng.randrange(256) for _ in range(channels))
                           for _ in range(rng.randint(1, 12))]
                img = Image.new(mode, (16, 16))
                img.putdata([colours[rng.randrange(len(colours))] for _ in range(256)])
            else:
                img = Image.frombytes(mode, (16, 16), rng.randbytes(256 * channels))
            assert rgb_palette(img) == old_palette(img), (mode, i)
            if mode == "RGB":
                assert riser(img) == old_riser(img), i
    # Sharp, repeated stair stripes (including wraparound) detect changes
    # to row indexing, luminance order, or the mask's bit orientation.
    for start in range(8):
        img = Image.new("RGB", (16, 16))
        img.putdata([(12, 21, 34) if (y - start) % 8 < 3 else (160, 173, 185)
                     for y in range(16) for _ in range(16)])
        assert riser(img) == old_riser(img), start
    print("PASS voxel Pillow alpha, palette and riser equivalence")


if __name__ == "__main__":
    main()
