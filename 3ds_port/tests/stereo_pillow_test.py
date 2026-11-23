#!/usr/bin/env python3
"""Check the Pillow 13 pixel-access migration against synthetic sprite art.

No copyrighted Pokémon artwork or ROM is used. These are precise pixel
fixtures for the code the stereo depth generator reads before packing.
"""
import importlib.util
import sys
import tempfile
from pathlib import Path

from PIL import Image

SCRIPT_DIR = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPT_DIR))
SPEC = importlib.util.spec_from_file_location("stereo", SCRIPT_DIR / "gen_stereo_mons.py")
stereo = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(stereo)


def test_gif_frame0(tmp):
    image = Image.new("RGBA", (2, 2), (0, 0, 0, 0))
    image.putpixel((0, 0), (255, 0, 0, 255))
    image.putpixel((1, 1), (8, 16, 24, 255))
    path = tmp / "lab.png"
    image.save(path)
    size, colors = stereo.gif_frame0(path)
    assert size == (2, 2)
    assert colors == [(31, 0, 0), None, None, (1, 2, 3)]


def test_depth_map(tmp):
    image = Image.new("L", (2, 2), 0)
    for point, level in [((1, 0), 64), ((0, 1), 128), ((1, 1), 255)]:
        image.putpixel(point, level)
    path = tmp / "depth.png"
    image.save(path)
    planes = stereo.key_planes(path, 2, 2, [(31, 0, 0)] * 4, 4)
    assert len(planes) == 4
    assert planes[0] == 0 and planes[3] == 3, planes
    assert planes == sorted(planes), planes


def test_two_palette_frames(tmp):
    image = Image.new("P", (64, 128), 0)
    colors = [0, 0, 0, 255, 0, 0, 0, 255, 0]
    image.putpalette(colors + [0] * (768 - len(colors)))
    image.putpixel((5, 6), 1)
    image.putpixel((20, 80), 2)
    path = tmp / "anim_front.png"
    image.save(path)
    frames = stereo.decomp_frames(path)
    assert frames is not None and len(frames) == 2
    assert frames[0][6 * 64 + 5] == (31, 0, 0)
    assert frames[1][16 * 64 + 20] == (0, 31, 0)
    assert frames[0][16 * 64 + 20] is None
    assert frames[1][6 * 64 + 5] is None
    assert sum(p is not None for f in frames for p in f) == 2


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as temp:
        directory = Path(temp)
        test_gif_frame0(directory)
        test_depth_map(directory)
        test_two_palette_frames(directory)
    print("PASS Pillow pixel access: transparent colors, 4-plane depth and 2-frame palette")
