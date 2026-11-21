#!/usr/bin/env python3
"""Pixel-for-pixel regression tests for voxel image construction and proofs.

Extract the production functions directly from source so this synthetic suite
does not need a bootstrapped pokeemerald ROM, graphics, or devkitARM.
"""
import ast
import math
import random
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1] / "scripts"


def function(filename, name, namespace):
    path = ROOT / filename
    tree = ast.parse(path.read_text(encoding="utf-8"), str(path))
    node = next(x for x in tree.body
                if isinstance(x, ast.FunctionDef) and x.name == name)
    exec(compile(ast.Module(body=[node], type_ignores=[]), str(path), "exec"),
         namespace)
    return namespace[name]


def class_methods(filename, class_name, methods, namespace):
    path = ROOT / filename
    tree = ast.parse(path.read_text(encoding="utf-8"), str(path))
    node = next(x for x in tree.body
                if isinstance(x, ast.ClassDef) and x.name == class_name)
    body = [x for x in node.body
            if isinstance(x, ast.FunctionDef) and x.name in methods]
    for fn in body:
        exec(compile(ast.Module(body=[fn], type_ignores=[]), str(path), "exec"),
             namespace)
    return [namespace[x] for x in methods]


def old_cell_image(self, m, layers):
    image = Image.new("RGBA", (16, 16), (0, 0, 0, 255))
    px = image.load()
    low, high = self.subtiles(m, 0), self.subtiles(m, 1)
    for q in range(4):
        for k in range(64):
            x = (q & 1) * 8 + k % 8
            y = (q >> 1) * 8 + k // 8
            rgb1, i1 = high[q][k]
            px[x, y] = (rgb1 if i1 and 1 in layers else low[q][k][0]) + (255,)
    return image


def old_building_art(self, x0, y0, w, h, ground, cells=None, ground_px=None,
                     upper=False, swap=None):
    image = Image.new("RGBA", (w * 16, h * 16), (0, 0, 0, 0))
    px = image.load()
    for cy in range(h):
        for cx in range(w):
            if cells is not None and (cx, cy) not in cells:
                continue
            m = self.metatile(x0 + cx, y0 + cy)
            if swap:
                m = swap.get(m, m)
            low, high = self.subtiles(m, 0), self.subtiles(m, 1)
            for q in range(4):
                is_ground = tuple(rgb for rgb, _ in low[q]) in ground
                for k in range(64):
                    lx, ly = (q & 1) * 8 + k % 8, (q >> 1) * 8 + k // 8
                    x, y = cx * 16 + lx, cy * 16 + ly
                    rgb1, i1 = high[q][k]
                    rgb0, _ = low[q][k]
                    if i1:
                        px[x, y] = rgb1 + (255,)
                    elif upper:
                        continue
                    elif not is_ground:
                        if ground_px and any(g[ly][lx] == rgb0 for g in ground_px):
                            continue
                        px[x, y] = rgb0 + (255,)
    return image


class FakeArt:
    def __init__(self, rng):
        palette = [(0, 0, 0), (23, 44, 65), (88, 99, 121),
                   (124, 16, 98), (255, 255, 255)]
        self.parts = {}
        for m in range(4):
            for layer in (0, 1):
                self.parts[m, layer] = [
                    [(palette[rng.randrange(len(palette))],
                      rng.choice([0, 0, 0, 1, 2, 3, 8, 15]))
                     for _ in range(64)] for _ in range(4)
                ]

    def metatile(self, x, y):
        return (x + y * 7) % 4

    def subtiles(self, m, layer):
        return self.parts[m, layer]


class OldRaster:
    def __init__(self, w, h, bg=(40, 44, 60)):
        self.w, self.h = w, h
        self.color = [bg] * (w * h)
        self.depth = [-1e30] * (w * h)
        self.owner = [None] * (w * h)

    def draw(self, verts, tex, shade, owner=None):
        (x0, y0, d0, w0, u0, v0), (x1, y1, d1, w1, u1, v1), (x2, y2, d2, w2, u2, v2) = verts
        area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
        if abs(area) < 1e-9:
            return
        minx = max(0, int(math.floor(min(x0, x1, x2))))
        maxx = min(self.w - 1, int(math.ceil(max(x0, x1, x2))))
        miny = max(0, int(math.floor(min(y0, y1, y2))))
        maxy = min(self.h - 1, int(math.ceil(max(y0, y1, y2))))
        tw, th = tex.size
        tpx = tex.load()
        inv = 1.0 / area
        for py in range(miny, maxy + 1):
            cy = py + 0.5
            for px_ in range(minx, maxx + 1):
                cx = px_ + 0.5
                a = ((x1 - cx) * (y2 - cy) - (x2 - cx) * (y1 - cy)) * inv
                b = ((x2 - cx) * (y0 - cy) - (x0 - cx) * (y2 - cy)) * inv
                c = 1.0 - a - b
                if a < -1e-7 or b < -1e-7 or c < -1e-7:
                    continue
                d = a * d0 + b * d1 + c * d2
                idx = py * self.w + px_
                if d <= self.depth[idx]:
                    continue
                iw = a * w0 + b * w1 + c * w2
                u = (a * u0 + b * u1 + c * u2) / iw
                v = (a * v0 + b * v1 + c * v2) / iw
                tu = int(math.floor(u))
                tv = int(math.floor(v))
                if tu < 0 or tv < 0 or tu >= tw or tv >= th:
                    continue
                r, g, bb, al = tpx[tu, tv]
                if al < 128:
                    continue
                self.depth[idx] = d
                self.owner[idx] = owner
                self.color[idx] = (int(r * shade), int(g * shade), int(bb * shade))


def old_differences(ras, apx, regions, M, dpx=None):
    wrong = missing = extra = 0
    for region in regions:
        rx0, ry0, rx1, ry1 = region[:4]
        behind = len(region) > 4 and region[4]
        for y in range(ry0, ry1):
            for x in range(rx0, rx1):
                r, g, b, a = apx[x, y]
                idx = (y + M) * ras.w + x + M
                got = ras.owner[idx]
                col = ras.color[idx]
                if a >= 128:
                    if got is None:
                        missing += 1
                        if dpx is not None:
                            dpx[x + M, y + M] = (255, 0, 255)
                    elif col != (r, g, b):
                        wrong += 1
                        if dpx is not None:
                            dpx[x + M, y + M] = (255, 0, 0)
                    elif dpx is not None:
                        dpx[x + M, y + M] = (r, g, b)
                elif got is not None and not behind:
                    extra += 1
                    if dpx is not None:
                        dpx[x + M, y + M] = (0, 255, 255)
    return wrong, missing, extra


def old_under(owners, ground, opened, art, y, x):
    found = []
    for step in (-1, 1):
        xx = x
        while 0 <= xx < len(owners) and owners[xx] is not None:
            xx += step
        if not (0 <= xx < len(owners)) or ground[xx] or opened[xx]:
            return None
        found.append(art[xx, y])
    return found[0] if found[0] == found[1] else None


def main():
    rng = random.Random(3032026)
    namespace = {"Image": Image, "math": math}
    art_fn, cell_fn = class_methods(
        "voxel_building.py", "LayoutArt",
        ["building_art", "_cell_image"], namespace)
    fake = FakeArt(rng)
    for m in range(4):
        for layers in ((0,), (0, 1), (1,), ()):
            got = cell_fn(fake, m, layers)
            expected = old_cell_image(fake, m, layers)
            assert got.mode == expected.mode and got.tobytes() == expected.tobytes(), (
                "cell image", m, layers)
    grounds = {tuple(rgb for rgb, _ in fake.subtiles(0, 0)[0])}
    grids = [
        [[rng.choice([(0, 0, 0), (23, 44, 65), (88, 99, 121), (255, 255, 255)])
          for x in range(16)] for y in range(16)]
        for _ in range(3)]
    for _ in range(100):
        w, h = rng.randrange(1, 5), rng.randrange(1, 5)
        cells = (None if rng.random() < .33 else
                 {(x, y) for x in range(w) for y in range(h) if rng.random() < .6})
        ground_px = rng.choice((None, [], grids[:1], grids[:2], grids))
        swap = rng.choice((None, {1: 2, 3: 0}))
        opts = dict(cells=cells, ground_px=ground_px, upper=bool(rng.getrandbits(1)),
                    swap=swap)
        got = art_fn(fake, 0, 0, w, h, grounds, **opts)
        expected = old_building_art(fake, 0, 0, w, h, grounds, **opts)
        assert got.tobytes() == expected.tobytes(), ("building art", w, h, opts)

    compare = function("voxel_building.py", "_ortho_differences", namespace)
    path = ROOT / "voxel_building.py"
    tree = ast.parse(path.read_text(encoding="utf-8"), str(path))
    cls = next(x for x in tree.body if isinstance(x, ast.ClassDef) and x.name == "Raster")
    exec(compile(ast.Module(body=[cls], type_ignores=[]), str(path), "exec"), namespace)
    Raster = namespace["Raster"]
    for case in range(15):
        w, h, margin = 27, 23, 4
        target = Raster(w + 2 * margin, h + margin)
        expected = OldRaster(w + 2 * margin, h + margin)
        tex = Image.frombytes("RGBA", (20, 20), rng.randbytes(1600))
        for tri in range(45):
            verts = [(
                rng.uniform(-10, 40), rng.uniform(-10, 30),
                rng.uniform(-2, 12), 1.0,
                rng.uniform(0, 19), rng.uniform(0, 19)) for _ in range(3)]
            if tri == 0:
                verts[1] = verts[0]  # degenerate triangle
            shade = rng.choice([0.0, .66, .72, .9, 1.0, 1.2])
            target.draw(verts, tex, shade, f"triangle-{tri}")
            expected.draw(verts, tex, shade, f"triangle-{tri}")
        assert target.color == expected.color, ("raster colors", case)
        assert target.depth == expected.depth, ("raster depth", case)
        assert target.owner == expected.owner, ("raster owners", case)

        art = Image.frombytes("RGBA", (w, h), rng.randbytes(w * h * 4))
        regions = [(0, 0, w, h), (2, 2, w - 3, h - 3, True)]
        diff1 = Image.new("RGB", (target.w, target.h), (4, 5, 6))
        diff2 = diff1.copy()
        got = compare(target, art.load(), regions, margin, diff1.load())
        want = old_differences(expected, art.load(), regions, margin, diff2.load())
        assert got == want and diff1.tobytes() == diff2.tobytes(), (
            "proof comparison", case)

    under = function("gen_voxel_buildings.py", "_row_under_colours", namespace)
    width, height = 91, 6
    image = Image.frombytes("RGBA", (width, height), rng.randbytes(width * height * 4))
    art = image.load()
    for y in range(height):
        owners = [rng.choice([None, None, 0, 1, -1]) for _ in range(width)]
        ground = [rng.random() < .1 for _ in range(width)]
        opened = [rng.random() < .07 for _ in range(width)]
        got = under(owners, ground, opened, art, y)
        want = [old_under(owners, ground, opened, art, y, x)
                for x in range(width)]
        assert got == want, ("under-floor mark", y)

    print("PASS image composition, raster depth/pixels, exact/behind comparisons, and cached floor marks")


if __name__ == "__main__":
    main()
