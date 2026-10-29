#!/usr/bin/env python3
"""New art around the intro's leaves scene, drawn for the wider screen.

The Game Freak logo appears over a still scene of four layers - big leaves
in front, a row of plants, grass and bushes, a sky with mountains - panning
up. On the 3DS the scene sits 1:1 in the middle of the top screen, and the
56 pixels on either side must hold something. Repeating the picture's edge
tiles out there reads as rows of copies, and mirroring it shows everything
twice; this draws those margins anew, for each layer:

* whatever the edge of the picture cuts - a leaf, a blade, a stalk - is
  carried on past it from the edge's own pixels: each shape's top and
  bottom follow the slope they arrive with and close to a point, so the
  seam is pixel for pixel continuous (continue_edge);
* behind that, new art in the scene's own palette ramps: grass blades, bush
  leaves in clumps, plants with raised, spreading and hanging leaves, the
  ground and the pit closing into their ellipse, hills in the distance;
* flat rows (sky, dithers) carry on their own texture;
* the first eight columns of the plants layer, which end the GBA screen
  with a darker strip where the pit is, are drawn anew from there down, so
  the wider view does not show them as a cut.

Above and below the picture the layers' own rows show (the compositor draws
them), which is what the GBA scrolls in; above the sky's first row, that
row's colour carries on.

It reads the scene from the decomp tree (graphics/intro/scene_1: the tiles,
the palette, the four tilemaps). The builder rebuilds those files from the
player's ROM and runs this same script, so the result - which carries the
edge's own pixels on - never ships: stage/leaves.bin is game data, checked
by CRC like the voxel data.

Output (little endian): b"EM3DLVS1", then for each of the four layers:
    u8 sky        colour (bank << 4 | index) above the map's first row, or 0xFF
    64 x 256      the left strip, screen x -56..7 (the last 8 over the picture)
    56 x 256      the right strip, screen x 240..295
one byte per pixel, bank << 4 | index, 0xFF where there is nothing (in the
left strip's last 8 columns: where the picture shows as it is). Rows are the
map's rows 0..255 (the art's height; the rest of each 512-row map is empty).
"""

import argparse
import math
import random
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCENE = ROOT / "graphics" / "intro" / "scene_1"
MAGIC = b"EM3DLVS1"
W = 56
H = 256
PATCH = 8


def load_scene():
    tiles = (SCENE / "bg.4bpp").read_bytes()
    maps = []
    for i in range(4):
        d = (SCENE / ("bg%d_map.bin" % i)).read_bytes()
        e = list(struct.unpack("<%dH" % (len(d) // 2), d))
        maps.append(e + [0] * (32 * 32 - len(e)))
    return tiles, maps


TILES, MAPS = None, None
PICS = {}
# With PROVENANCE (tools/voxel_precompile.py), every pixel read off the
# picture is a Pixel: the same (bank, index), and where in the tiles it was
# read (tile, pixel). New art is plain tuples. The release stores the
# picture's pixels as those places, not as colours.
PROVENANCE = False


class Pixel(tuple):
    """(bank, index), carrying the tile and pixel (row * 8 + column) it was read from."""

    def __new__(cls, bank, index, tile, pixel):
        self = tuple.__new__(cls, (bank, index))
        self.tile_pixel = (tile, pixel)
        return self


def pic(L):
    """The layer's own pixels, 256 rows of 240, (bank, index) or None."""
    global TILES, MAPS
    if TILES is None:
        TILES, MAPS = load_scene()
    if L not in PICS:
        out = [[None] * 240 for _ in range(H)]
        for r in range(32):
            for c in range(30):
                e = MAPS[L][r * 32 + c]
                base = (e & 1023) * 32
                bank = e >> 12
                for y in range(8):
                    for x in range(8):
                        b = TILES[base + y * 4 + x // 2] if base + 32 <= len(TILES) else 0
                        v = (b >> ((x & 1) * 4)) & 15
                        sx = 7 - x if e & 0x400 else x
                        sy = 7 - y if e & 0x800 else y
                        if v and PROVENANCE:
                            out[r * 8 + sy][c * 8 + sx] = Pixel(bank, v, e & 1023, y * 8 + x)
                        elif v:
                            out[r * 8 + sy][c * 8 + sx] = (bank, v)
        PICS[L] = out
    return PICS[L]


class Canvas:
    """One side's margin, x = 0 next to the picture, growing away from it."""

    def __init__(self):
        self.p = [[None] * W for _ in range(H)]

    def set(self, x, y, c):
        if 0 <= x < W and 0 <= y < H:
            self.p[y][x] = c

    def get(self, x, y):
        if 0 <= x < W and 0 <= y < H:
            return self.p[y][x]
        return None


class Margins:
    def __init__(self, L):
        self.L = L
        self.c = {"left": Canvas(), "right": Canvas()}
        # the colour above the map's first row, across the whole screen
        self.sky = None
        # picture pixels replaced by new art (an edge strip the wider view
        # would otherwise show as a cut), and where that may happen
        self.patch = {}
        self.patchable = lambda x, y: False

    def put(self, x, y, color):
        if not (0 <= y < H):
            return
        if 0 <= x < 240:
            if self.patchable(x, y):
                self.patch[(x, y)] = color
            return
        if x < 0:
            self.c["left"].set(-1 - x, y, color)
        else:
            self.c["right"].set(x - 240, y, color)

    def at(self, x, y):
        if 0 <= x < 240:
            if (x, y) in self.patch:
                return self.patch[(x, y)]
            return pic(self.L)[y][x] if 0 <= y < H else None
        if x < 0:
            return self.c["left"].get(-1 - x, y)
        return self.c["right"].get(x - 240, y)

    def xs(self, side):
        return range(-W, 0) if side == "left" else range(240, 240 + W)


def row_period(row, side, span=32):
    """The shortest period the edge's last span pixels repeat with."""
    xs = range(0, span) if side == "left" else range(240 - span, 240)
    vals = [row[x] for x in xs]
    for p in (1, 2, 4, 8, 16):
        if all(vals[i] == vals[i + p] for i in range(len(vals) - p)):
            return p
    return None


def continue_texture(m, y0, y1, sides=("left", "right"), span=32):
    """Flat rows (a colour, a dither): the same texture carried on."""
    P = pic(m.L)
    for side in sides:
        for y in range(y0, y1):
            p = row_period(P[y], side, span) or 2
            for x in m.xs(side):
                if side == "right":
                    src = 240 - p + ((x - 240) % p)
                else:
                    src = (x % p + p) % p
                m.put(x, y, P[y][src])


# ---------------------------------------------------------- continuation --
def spans_at(P, x, y0, y1, gap):
    """Runs of object pixels in column x: not transparent, not gap(y, c)."""
    out, start = [], None
    for y in range(y0, y1 + 1):
        c = P[y][x] if y < y1 else None
        solid = c is not None and not gap(y, c)
        if solid and start is None:
            start = y
        elif not solid and start is not None:
            out.append((start, y - 1))
            start = None
    return out


def continue_edge(m, side, y0=0, y1=256, gap=lambda y, c: False, long_span=48, look=5, reach=None, edge=None):
    """Carries every shape the edge cuts on past it, pixel for pixel: at the
    seam the new column is the picture's edge column; outwards each shape's
    top and bottom follow the slope they arrive with and bend together to a
    point, the colours between them the edge's own rows squeezed to fit, so
    an outline stays an outline and a midrib a midrib."""
    P = pic(m.L)
    ex = edge if edge is not None else (0 if side == "left" else 239)
    ix = ex + look if side == "left" else ex - look
    out = -1 if side == "left" else 1
    inner = spans_at(P, ix, y0, y1, gap)
    for top0, bot0 in spans_at(P, ex, y0, y1, gap):
        h = bot0 - top0 + 1
        # the same shape one step inwards, to know where its edges head
        mid = (top0 + bot0) / 2
        best = None
        for a, b in inner:
            if a <= bot0 and b >= top0:
                d = abs((a + b) / 2 - mid)
                if best is None or d < best[0]:
                    best = (d, a, b)
        st = sb = 0.0
        if best:
            st = max(-1.2, min(1.2, (top0 - best[1]) / look))
            sb = max(-1.2, min(1.2, (bot0 - best[2]) / look))
        L = reach if reach is not None else max(3.0, min(40.0, h * 1.3))
        if h >= long_span:
            L = 2.0
        d = 0
        while d < L:
            t = (d / L) ** 2
            ct = top0 + st * d
            cb = bot0 + sb * d
            cm = (ct + cb) / 2
            top = ct + (cm - ct) * t
            bot = cb + (cm - cb) * t
            if bot < top:
                break
            yt, yb = int(round(top)), int(round(bot))
            for y in range(yt, yb + 1):
                if yb > yt:
                    src = top0 + (y - yt) * (bot0 - top0) / (yb - yt)
                else:
                    src = (top0 + bot0) / 2
                c = P[int(round(src))][ex]
                if c is not None and not gap(int(round(src)), c):
                    m.put(ex + out * (d + 1), y, c)
            d += 1


# ---------------------------------------------------------------- layer 3 --
def mountain(m, profile, x0, x1, base, body=9, lit=11, rim=10, lit_side="right", lit_depth=9):
    """A mountain whose ridge is profile(x) (top y), filled to base.
    The slope facing lit_side gets a lit band under the ridge, dithered."""
    for x in range(x0, x1):
        top = int(round(profile(x)))
        if top >= base:
            continue
        slope = profile(x + 1) - profile(x - 1)
        facing = (slope > 0) if lit_side == "right" else (slope < 0)
        for y in range(top, base):
            c = (5, body)
            d = y - top
            if facing and abs(slope) > 0.3:
                if d < 1:
                    c = (5, rim)
                elif d < lit_depth - 2:
                    c = (5, lit)
                elif d < lit_depth and (x + y) & 1:
                    c = (5, lit)
            m.put(x, y, c)


def cloud(m, cx, cy, w, h, color=11, shade=10):
    for y in range(cy - h, cy + h + 1):
        for x in range(cx - w, cx + w + 1):
            dx = (x - cx) / w
            dy = (y - cy) / h
            bump = 0.25 * math.sin(x * 0.9) if y < cy else 0
            if dx * dx + dy * dy <= 1 + bump:
                m.put(x, y, (5, shade if y > cy + h // 3 else color))


def layer3():
    m = Margins(3)
    continue_texture(m, 0, 256)
    def ridge(points):
        """Piecewise linear ridge through (x, y) points, rounded down the
        way the art's slopes step."""
        def f(x):
            for (xa, ya), (xb, yb) in zip(points, points[1:]):
                if xa <= x <= xb:
                    return ya + (yb - ya) * (x - xa) / (xb - xa)
            return 99
        return f
    # Left: the valley at the picture's edge, then a far, lower hill.
    mountain(m, ridge([(-W - 1, 49), (-44, 43), (-38, 44), (-30, 47), (-14, 53), (0, 57)]), -W, 0, 57)
    # Right: the mountain at the edge comes down to the valley, and a far
    # hill rises beyond it.
    mountain(m, ridge([(239, 53), (252, 57), (262, 52), (276, 45), (281, 44), (292, 48), (240 + W, 51)]),
             240, 240 + W, 57)
    # the sky above the map: its first row's colour, carried up
    m.sky = pic(3)[0][0]
    cloud(m, 262, 36, 7, 2)
    cloud(m, -30, 30, 6, 2)
    return m


# ------------------------------------------------------------ primitives --
def leaf(m, cx, cy, length, width, angle, light, dark, bank, shine=None, bend=0.0, edge=None, fullness=0.75):
    """A pointed leaf: its upper half light, its lower half dark, split by
    the midrib; optionally a shine pixel near the base and a dark rim."""
    ca, sa = math.cos(angle), math.sin(angle)
    r = int(length) + 2
    for y in range(int(cy) - r, int(cy) + r + 1):
        for x in range(int(cx) - r, int(cx) + r + 1):
            dx, dy = x - cx, y - cy
            u = (dx * ca + dy * sa) / (length / 2)
            v = (-dx * sa + dy * ca)
            v -= bend * (u * u) * width
            if not -1 <= u <= 1:
                continue
            half = width / 2 * (1 - u * u) ** fullness
            if abs(v) > half:
                continue
            c = light if v < 0 else dark
            if edge is not None and abs(v) > half - 0.9 and v >= 0:
                c = edge
            m.put(x, y, (bank, c))
    if shine is not None:
        m.put(int(round(cx - ca * length * 0.2)), int(round(cy - sa * length * 0.2 - width * 0.15)), (bank, shine))


def rng_for(L, side):
    return random.Random(1000 * L + (0 if side == "left" else 1))


# ---------------------------------------------------------------- layer 2 --
def blades(m, xs, rnd, bank, body, lit, base, tip_lo, tip_hi, gap=(2, 4), base_at=None):
    """Grass as the art draws it: a crowd of pointed blades, each a narrow
    triangle leaning a little, packed so their roots merge; the edge of a
    blade that faces the light is lighter."""
    x = xs[0] - 6
    while x <= xs[-1] + 6:
        h = rnd.randint(tip_lo, tip_hi)
        w = rnd.randint(3, 6)
        lean = rnd.uniform(-0.35, 0.35)
        if base_at is not None:
            base = int(round(base_at(x)))
        top = base - h
        for y in range(top, base + 1):
            t = (base - y) / max(1, h)
            half = w * (1 - t) / 2
            cx = x + lean * (base - y)
            x0, x1 = int(math.floor(cx - half)), int(math.ceil(cx + half))
            for xx in range(x0, max(x0 + 1, x1)):
                m.put(xx, y, (bank, body))
            if y < base - 1 and rnd.random() < 0.7:
                m.put(x0, y, (bank, lit))
        x += rnd.randint(*gap)


def layer2():
    m = Margins(2)
    for side in ("left", "right"):
        rnd = rng_for(2, side)
        xs = list(m.xs(side))
        near = side == "right"
        tip_lo, tip_hi = (6, 20) if near else (3, 11)
        # the bushes' crowns: a lumpy line, not a straight one
        crown = {}
        phase = rnd.uniform(0, 6.3)
        for x in xs:
            crown[x] = 71 + 3.5 * math.sin(x / 7.0 + phase) + 2.0 * math.sin(x / 3.1 + 2 * phase) + rnd.uniform(-1, 1)
            for y in range(48, 256):
                m.put(x, y, (2, 8 if y < 62 else 6 if y < crown[x] else 2 if y < 134 else 12))
        blades(m, xs, rnd, 2, 8, 13, 48, tip_lo, tip_hi)
        # the grass's shadow rising in spikes from the bushes
        x = xs[0] - 2
        while x <= xs[-1] + 2:
            h = rnd.choice((0, 2, 4, 6, 8, 10))
            for d in range(h):
                for k in range(2 if d < h - 2 else 1):
                    m.put(x + k, 61 - d, (2, 6))
            x += rnd.randint(3, 7)
        # bush tops poking into it, then the bush body texture
        for i in range(len(xs) // 3):
            cx, cy = rnd.uniform(xs[0], xs[-1]), rnd.uniform(58, 70)
            leaf(m, cx, cy, rnd.uniform(3, 6), 2.2, rnd.uniform(-2.8, -0.3), 9, 9, 2)
        for i in range(int(len(xs) * 1.7)):
            cx, cy = rnd.uniform(xs[0] - 3, xs[-1] + 3), rnd.uniform(68, 104)
            ang = rnd.choice((rnd.uniform(-2.7, -1.9), rnd.uniform(-1.2, -0.4), rnd.uniform(0.3, 0.9)))
            leaf(m, cx, cy, rnd.uniform(5, 8), rnd.uniform(2.5, 3.5), ang, rnd.choice((9, 7, 7)), 5, 2)
            if rnd.random() < 0.18:
                m.put(int(cx) + 1, int(cy), (2, 13))
        # deeper in: dark hollows between the bushes
        for i in range(len(xs) // 8):
            cx, cy = rnd.uniform(xs[0], xs[-1]), rnd.uniform(96, 110)
            leaf(m, cx, cy, rnd.uniform(10, 16), rnd.uniform(4, 6), rnd.uniform(-0.3, 0.3), 1, 1, 2)
        # the front: rounded clumps of big leaves fanning out from a root,
        # the upper ones catching the light, the lower ones in shade
        x = xs[0] - 10 + rnd.randint(0, 12)
        while x < xs[-1] + 16:
            root = rnd.randint(124, 130)
            # back row up and out, then the front row down and out
            fan = [(-1.95, 7, 5, 3, 18, 0, -8), (-1.2, 7, 5, 3, 18, 0, -8),
                   (-2.65, 9, 7, 3, 24, -4, -2), (-0.5, 9, 7, 3, 24, 4, -2),
                   (2.2, 6, 4, 2, 18, -3, 4), (0.95, 6, 4, 2, 18, 3, 4),
                   (2.85, 7, 5, 2, 24, -6, 6), (0.3, 7, 5, 2, 24, 6, 6)]
            for ang, lt, dk, rim, ln, ox, oy in fan:
                ang += rnd.uniform(-0.15, 0.15)
                ln = ln * rnd.uniform(0.85, 1.1)
                cx = x + ox + math.cos(ang) * ln * 0.5
                cy = root + oy + math.sin(ang) * ln * 0.4
                leaf(m, cx, cy, ln, ln * 0.52, ang, lt, dk, 2, edge=rim,
                     shine=14 if rnd.random() < 0.5 else (13 if rnd.random() < 0.3 else None), bend=0.25)
            x += rnd.randint(38, 46)
        # what the edge cuts - bush leaves, grass - carried on over it all
        continue_edge(m, side, 50, 150, gap=lambda y, c: c in ((2, 2), (2, 1), (2, 12), (2, 6), (2, 8)))
    return m


# ---------------------------------------------------------------- layer 1 --
def tier(y):
    """The plants' colours by depth: bright on top, darker further down."""
    if y < 34:
        return 0, 4, 3, 1
    if y < 56:
        return 2, 4, 3, 1
    return 3, 4, 3, 1


def shaped_leaf(m, cx, cy, length, width, angle, bank, fill, outline, shade, heart=False, dew=None):
    """A leaf with the art's modelling: a 1-pixel outline, the fill, and
    the half away from the light in shade. A heart leaf is widest near its
    stalk and ends in a point; dew is a bank for the drop at its centre."""
    ca, sa = math.cos(angle), math.sin(angle)
    r = int(length) + 2
    inside = {}
    for y in range(int(cy) - r, int(cy) + r + 1):
        for x in range(int(cx) - r, int(cx) + r + 1):
            dx, dy = x - cx, y - cy
            u = (dx * ca + dy * sa) / (length / 2)
            v = -dx * sa + dy * ca
            if not -1 <= u <= 1:
                continue
            if heart:
                half = width * 0.58 * (1 + u) ** 0.35 * (1 - u) ** 0.7
            else:
                half = width / 2 * (1 - u * u) ** 0.6
            if abs(v) <= half:
                inside[(x, y)] = (v, half)
    for (x, y), (v, half) in inside.items():
        border = any((x + dx, y + dy) not in inside for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)))
        if border:
            c = outline if v < 0 else shade
        elif v > half - 2.2 or (v > 0 and abs(v) < 0.6):
            # the shaded rim and the midrib's shadow
            c = outline
        else:
            c = fill
        m.put(x, y, (bank, c))
    if dew is not None:
        px, py = int(round(cx)), int(round(cy))
        for dx, c in ((-2, 1), (-1, 14), (0, 4), (1, 14), (2, 1)):
            m.put(px + dx, py, (dew, c))
        m.put(px - 1, py + 1, (dew, 1))
        m.put(px, py + 1, (dew, 13))
        m.put(px + 1, py + 1, (dew, 1))


def plant(m, rnd, x0, top, bottom=80):
    """A plant as the art draws them: a thin dark stalk; at the top two
    leaves raised to a point, under them a pair spreading out, then broad
    heart leaves hanging down, the first of them with a drop of dew; dark
    leaves crowd behind."""
    phase = rnd.uniform(0, 6.3)
    stem = lambda y: x0 + 1.5 * math.sin(y / 10.0 + phase)
    # dark leaves behind the plant
    for i in range(9):
        yy = rnd.uniform(top + 6, bottom - 6)
        ang = rnd.choice((-0.4, 0.4, math.pi - 0.4, math.pi + 0.4)) + rnd.uniform(-0.3, 0.3)
        bank = 0 if yy < 34 else 2
        shaped_leaf(m, stem(yy) + math.cos(ang) * 8, yy + math.sin(ang) * 5, rnd.uniform(10, 14), 6, ang,
                    bank, 1, 1, 1)
    for y in range(top + 4, bottom):
        m.put(int(round(stem(y))), y, (tier(y)[0], 1))
    y = top
    # raised pair
    for s in (-1, 1):
        ang = -math.pi / 2 + s * rnd.uniform(0.55, 0.8)
        ln = rnd.uniform(14, 17)
        shaped_leaf(m, stem(y + 8) + math.cos(ang) * ln * 0.45, y + 8 + math.sin(ang) * ln * 0.45, ln, 7.5,
                    ang, 0, 4, 3, 1)
    y += 14
    # spreading pair
    for s in (-1, 1):
        ang = (-0.25 if s > 0 else math.pi + 0.25) + rnd.uniform(-0.15, 0.15)
        ln = rnd.uniform(14, 18)
        bank = tier(y)[0]
        shaped_leaf(m, stem(y) + math.cos(ang) * ln * 0.5, y + math.sin(ang) * ln * 0.3, ln, 8, ang,
                    bank, 4, 3, 1)
    y += 14
    # hanging hearts, the first with dew
    first = True
    while y < bottom - 6:
        for s in (-1, 1):
            if rnd.random() < 0.25 and not first:
                continue
            ang = (0.55 if s > 0 else math.pi - 0.55) + rnd.uniform(-0.15, 0.15)
            ln = rnd.uniform(15, 18)
            bank = 2 if y < 56 else 3
            shaped_leaf(m, stem(y) + math.cos(ang) * ln * 0.45, y + math.sin(ang) * ln * 0.4, ln, 11, ang,
                        bank, 4, 3, 1, heart=True, dew=3 if first and s > 0 else None)
        first = False
        y += rnd.randint(13, 16)


def layer1():
    m = Margins(1)
    for side in ("left", "right"):
        rnd = rng_for(1, side)
        xs = list(m.xs(side))
        dist = (lambda x: -1 - x) if side == "left" else (lambda x: x - 240)
        pit = (3, 10)
        quiet = 0
        if side == "left":
            # The art's first 8 columns are a darker strip from the
            # undergrowth down (the GBA screen's edge); in the wider view it
            # reads as a cut, so from there down it is drawn anew, carried on
            # from column 8.
            m.patchable = lambda x, y: x < 8 and y >= 72
            xs = list(range(-W, 8))
            dist = lambda x: 7 - x
            fringe = lambda x: 116 + 0.02 * dist(x)
            pit_top = lambda x: 133 + 0.3 * dist(x) + 0.004 * dist(x) ** 2
        else:
            fringe = lambda x: 112 - 0.05 * dist(x)
            pit_top = lambda x: 138 + 0.4 * dist(x)
        for x in xs:
            for y in range(73 if x >= 0 else 84, 256):
                c = (3, 12) if y < fringe(x) else (3, 11) if y < pit_top(x) else pit
                m.put(x, y, c)
            # the fringe's edge against the pit, broken as in the art
            yb = int(pit_top(x))
            for y in range(yb - 3, yb):
                if rnd.random() < 0.3:
                    m.put(x, y, pit)
        blades(m, [x for x in xs if dist(x) >= quiet], rnd, 3, 11, 11, 0, 2, 7, gap=(2, 3),
               base_at=lambda x: fringe(x) + 3)
        # dark back leaves between the plants, then the plants themselves
        for i in range(len(xs) // 3):
            cx, cy = rnd.uniform(xs[0] - 4, xs[-1] + 4), rnd.uniform(58, 86)
            leaf(m, cx, cy, rnd.uniform(12, 18), rnd.uniform(7, 10), rnd.uniform(-0.5, 0.5) + rnd.choice((0, math.pi)),
                 12, 12, 3)
        for i in range(len(xs) // 6):
            cx, cy = rnd.uniform(xs[0], xs[-1]), rnd.uniform(14, 50)
            leaf(m, cx, cy, rnd.uniform(10, 14), rnd.uniform(5, 7), rnd.uniform(-0.6, 0.6) + rnd.choice((0, math.pi)),
                 1, 1, 0 if cy < 34 else 2)
        centre = xs[0] + rnd.uniform(14, 22)
        while centre < xs[-1] + 12:
            plant(m, rnd, centre, rnd.randint(6, 18))
            centre += rnd.uniform(36, 42)
        # the undergrowth under them
        for i in range(int(len(xs) * 0.7)):
            cx, cy = rnd.uniform(xs[0] - 3, xs[-1] + 3), rnd.uniform(74, 106)
            if dist(cx) < quiet + 6:
                continue
            ang = rnd.choice((rnd.uniform(-2.8, -2.2), rnd.uniform(-0.9, -0.3)))
            leaf(m, cx, cy, rnd.uniform(9, 14), rnd.uniform(4, 6), ang, 4, 3, 3, edge=1)
        # and whatever the edge cuts, carried on
        gap = lambda y, c: y >= 74 and c in ((3, 12), (2, 12))
        dark = ((0, 1), (2, 1), (3, 1), (3, 12))
        if side == "left":
            continue_edge(m, side, 0, 72, gap=lambda y, c: c in dark)
            continue_edge(m, side, 72, 112, gap=gap, edge=8)
        else:
            continue_edge(m, side, 0, 72, gap=lambda y, c: c in dark)
            continue_edge(m, side, 72, 110, gap=gap)
    return m


# ---------------------------------------------------------------- layer 0 --
def layer0():
    m = Margins(0)
    for side in ("left", "right"):
        rnd = rng_for(0, side)
        xs = list(m.xs(side))
        # the dark grass in front, rising towards the sides as the mound
        # it grows on does at the picture's edges
        def ground(x, side=side):
            d = -1 - x if side == "left" else x - 240
            return 185 - 0.35 * d - 0.003 * d * d
        for x in xs:
            for y in range(int(round(ground(x))), 256):
                m.put(x, y, (3, 11))
        blades(m, xs, rnd, 3, 11, 11, 0, 3, 11, gap=(2, 4), base_at=ground)
        # the big leaves the edge cuts: carried on to their points
        continue_edge(m, side, 0, 110)
    return m


def generate():
    """{(layer, part): data} for part in left, right (Canvas), sky, patch."""
    out = {}
    for L, fn in ((3, layer3), (2, layer2), (1, layer1), (0, layer0)):
        m = fn()
        out[(L, "left")] = m.c["left"]
        out[(L, "right")] = m.c["right"]
        out[(L, "sky")] = m.sky
        out[(L, "patch")] = m.patch
    return out


def encode(parts) -> bytes:
    def byte(c):
        return 0xFF if c is None else (c[0] << 4) | c[1]

    blob = bytearray(MAGIC)
    for L in range(4):
        blob.append(byte(parts[(L, "sky")]))
        left, right, patch = parts[(L, "left")], parts[(L, "right")], parts[(L, "patch")]
        for y in range(H):
            for x in range(-W, PATCH):
                blob.append(byte(left.get(-1 - x, y) if x < 0 else patch.get((x, y))))
        for y in range(H):
            for x in range(W):
                blob.append(byte(right.get(x, y)))
    return bytes(blob)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--output", type=Path, default=ROOT / "3ds_port" / "romfs" / "stage" / "leaves.bin")
    args = ap.parse_args()
    data = encode(generate())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(data)
    print("gen_intro_margins: %d bytes -> %s" % (len(data), args.output))


if __name__ == "__main__":
    main()
