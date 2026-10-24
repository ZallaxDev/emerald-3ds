#!/usr/bin/env python3
"""Terrain relief read off the drawing: cliffs, their stairs, rocks at sea.

The premise is the one voxel_building.py builds on: the GBA draws the world
in a 45-degree oblique projection, a point (X, Y, Z) of it landing on the art
at u = X, v = Z - Y. Turned round, a height h(u, v) for every point of the
art places that point at

    (u, h, v + h)

and for ANY choice of h the GBA view of the result is the drawing itself,
pixel for pixel. So terrain is not built of boxes: the art is laid as a sheet
and lifted, and the only question is how high each point of it is.

How high
--------
The cartridge does not say - a plateau and the beach below it share an
elevation - but the drawing does, because in an oblique view a cliff is only
ever seen from the front or from the side:

  * land splits into regions at the rock bands (roles `cliff` and `shelf`)
    and at the stairs through them;
  * a band with land to its north and land to its south, of different
    regions, is a south-facing face: the north region stands one level above
    the south one. Contradictions (a fence line between one wood) are simply
    not constraints;
  * water stands at the lowest land it touches, and a level below any land
    it only reaches across a rock band (a sea cliff facing west or east).

One level is one tile - the rock band is drawn one cell tall. Heights live on
a lattice every 4 pixels. Land and water fix it; the rock and stair cells in
between are solved harmonically, which makes a south face exactly vertical (h
falls one pixel per row, so v + h is constant down it), a west or east face a
45-degree slope (the diagonal striations the artist drew) and every corner of
the staircase a smooth turn - never a square block. A rock mass with no higher
land beside it (a rock at sea) is raised as a mound by its distance from the
water.

Ledges
------
Every outdoor map's ledges are lifted the same way, on maps whose cliffs
are not solved as well: the brown lip, told from the ground round it by its
colours, rises
from its foot on the jump side to LIP pixels at its top edge, and the ground
behind rises to meet it, a low berm facing where the jump goes. Corner cells
jump two ways and take the nearer foot; the junction pieces where a ledge
steps are ledge too; a lip comes down to the ground where the ledge ends.

    python gen_voxel_relief.py [--preview DIR] [--proof DIR] [--layouts A,B] [--output FILE]
"""

import argparse
import collections
import itertools
import json
import math
import operator
import os
import shutil
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
# The commonest item of a list, ties broken as 64-bit CPython breaks them
# for max(set(seq), key=seq.count): by the order of the set's hash table.
# That order follows hash(), which is 32-bit on WebAssembly (the web
# builder's Pyodide), so the table is rebuilt here with 64-bit hashes and
# the same output comes out of every interpreter.
_M64 = (1 << 64) - 1
_P61 = (1 << 61) - 1


def _hash64(o):
    """hash(o) as 64-bit CPython computes it, for ints, floats and tuples of them."""
    if isinstance(o, tuple):
        acc = 2870177450012600261
        for item in o:
            acc = (acc + (_hash64(item) & _M64) * 14029467366897019727) & _M64
            acc = ((acc << 31) | (acc >> 33)) & _M64
            acc = (acc * 11400714785074694791) & _M64
        acc = (acc + (len(o) ^ (2870177450012600261 ^ 3527539))) & _M64
        if acc == _M64:
            return 1546275796
        return acc - (1 << 64) if acc >> 63 else acc
    if isinstance(o, float):
        if o != o or o in (float("inf"), float("-inf")):
            raise TypeError("no portable hash for %r" % o)
        m, n = o.as_integer_ratio()
    elif isinstance(o, int):
        m, n = int(o), 1
    else:
        raise TypeError("no portable hash for %r" % type(o).__name__)
    h = (abs(m) % _P61) * pow(n, _P61 - 2, _P61) % _P61
    if m < 0:
        h = -h
    return -2 if h == -1 else h


def _insert_clean(table, mask, key, h):
    perturb = h & _M64
    i = perturb & mask
    while True:
        if table[i] is None:
            table[i] = (h, key)
            return
        if i + 9 <= mask:
            for j in range(i + 1, i + 10):
                if table[j] is None:
                    table[j] = (h, key)
                    return
        perturb >>= 5
        i = (i * 5 + 1 + perturb) & mask


def set_order(seq):
    """list(set(seq)) as 64-bit CPython orders it."""
    mask, table, used = 7, [None] * 8, 0
    for key in seq:
        h = _hash64(key)
        perturb = h & _M64
        i = perturb & mask
        slot = None
        while slot is None:
            probes = 9 if i + 9 <= mask else 0
            j = i
            while True:
                e = table[j]
                if e is None:
                    slot = j
                    break
                if e[0] == h and e[1] == key:
                    slot = -1
                    break
                if probes == 0:
                    break
                probes -= 1
                j += 1
            if slot is None:
                perturb >>= 5
                i = (i * 5 + 1 + perturb) & mask
        if slot < 0:
            continue
        table[slot] = (h, key)
        used += 1
        if used * 5 >= mask * 3:
            size = 8
            minused = used * 2 if used > 50000 else used * 4
            while size <= minused:
                size <<= 1
            old, table, mask = table, [None] * size, size - 1
            for e in old:
                if e is not None:
                    _insert_clean(table, mask, e[1], e[0])
    return [e[1] for e in table if e is not None]


def commonest(seq):
    """max(set(seq), key=seq.count) with 64-bit CPython's tie-break, anywhere."""
    counts = collections.Counter(seq)
    return max(set_order(seq), key=counts.__getitem__)


import voxel_building as vb  # noqa: E402
import voxel_cells as vc  # noqa: E402
import voxel_props  # noqa: E402
import voxel_relief_fixes  # noqa: E402

PORT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

LEVEL = 16          # pixels per level: the rock band is drawn one cell tall
STEP = 4            # lattice spacing in pixels
PER_CELL = 16 // STEP
MOUND = 8           # mound rise per lattice step inland from the water
MOUND_MAX = 24

# Maps the relief is solved for. Others keep the region extrusion until each
# has been looked at: a height model is a claim about a whole map.
ENABLED = ["LAYOUT_ROUTE104", "LAYOUT_RUSTBORO_CITY"]

RELIEF_ROLES = {"cliff", "shelf", "stair"}

# Ledges. The cartridge names each ledge cell's jump by its behaviour; the
# drawing gives its lip, the half of the cell on the jump side painted in the
# colours no ground round it has.
JUMPS = {0x38: ((1, 0),), 0x39: ((-1, 0),), 0x3A: ((0, -1),), 0x3B: ((0, 1),),
         0x3C: ((1, 0), (0, -1)), 0x3D: ((-1, 0), (0, -1)),
         0x3E: ((1, 0), (0, 1)), 0x3F: ((-1, 0), (0, 1))}
LIP = 6             # height of the lip's top edge, pixels
LIP_WIDTH = 8       # the lip is drawn half a cell deep: its foot to its top
LIP_BACK = 8        # the ground behind the lip rises to it over this many pixels


def solve(layout):
    """Per-cell level (None for relief cells) and the lattice heights."""
    W, H = layout.w, layout.h
    role = [[layout.role_at(x, y) for x in range(W)] for y in range(H)]
    kind = [["relief" if role[y][x] in RELIEF_ROLES else
             "water" if role[y][x] == "water" else "land" for x in range(W)] for y in range(H)]

    # land regions
    region = [[-1] * W for _ in range(H)]
    count = 0
    for y in range(H):
        for x in range(W):
            if kind[y][x] != "land" or region[y][x] >= 0:
                continue
            stack = [(x, y)]
            region[y][x] = count
            while stack:
                cx, cy = stack.pop()
                for nx, ny in ((cx + 1, cy), (cx - 1, cy), (cx, cy + 1), (cx, cy - 1)):
                    if 0 <= nx < W and 0 <= ny < H and kind[ny][nx] == "land" and region[ny][nx] < 0:
                        region[ny][nx] = count
                        stack.append((nx, ny))
            count += 1

    # south faces: land above a rock run and land below it
    edges = {}
    for x in range(W):
        y = 0
        while y < H:
            if kind[y][x] != "relief" or role[y][x] == "stair":
                y += 1
                continue
            y0 = y
            while y < H and kind[y][x] == "relief" and role[y][x] != "stair":
                y += 1
            if y0 > 0 and y < H and kind[y0 - 1][x] == "land" and kind[y][x] == "land":
                up, down = region[y0 - 1][x], region[y][x]
                if up != down:
                    edges[(up, down)] = edges.get((up, down), 0) + 1

    # levels: breadth first over "up is down + 1", first answer kept
    level = [None] * count
    adj = [[] for _ in range(count)]
    for (up, down), n in edges.items():
        adj[up].append((down, -1))
        adj[down].append((up, +1))
    for start in range(count):
        if level[start] is not None:
            continue
        level[start] = 0
        comp, queue = [start], [start]
        while queue:
            r = queue.pop()
            for (o, d) in adj[r]:
                if level[o] is None:
                    level[o] = level[r] + d
                    comp.append(o)
                    queue.append(o)
        low = min(level[r] for r in comp)
        for r in comp:
            level[r] -= low

    cell = [[None] * W for _ in range(H)]
    for y in range(H):
        for x in range(W):
            if kind[y][x] == "land":
                cell[y][x] = level[region[y][x]]

    # Level 0 is the land the map meets its neighbours with: a town beside
    # this route is drawn flat, so the seam must be too. Whatever lies lower
    # (a beach under its cliff) goes below the ground plane.
    edge = []
    for side in getattr(layout, "connected_sides", ()):
        cells = {"up": [(x, 0) for x in range(W)], "down": [(x, H - 1) for x in range(W)],
                 "left": [(0, y) for y in range(H)], "right": [(W - 1, y) for y in range(H)]}[side]
        edge += [cell[y][x] for (x, y) in cells if kind[y][x] == "land"]
    if edge:
        ref = commonest(edge)
        for y in range(H):
            for x in range(W):
                if cell[y][x] is not None:
                    cell[y][x] -= ref

    # water: the lowest land it touches, body by body
    seen = set()
    for y in range(H):
        for x in range(W):
            if kind[y][x] != "water" or (x, y) in seen:
                continue
            body, stack, touch = [], [(x, y)], []
            seen.add((x, y))
            while stack:
                cx, cy = stack.pop()
                body.append((cx, cy))
                for nx, ny in ((cx + 1, cy), (cx - 1, cy), (cx, cy + 1), (cx, cy - 1)):
                    if not (0 <= nx < W and 0 <= ny < H):
                        continue
                    if kind[ny][nx] == "water" and (nx, ny) not in seen:
                        seen.add((nx, ny))
                        stack.append((nx, ny))
                    elif kind[ny][nx] == "land":
                        touch.append(cell[ny][nx])
                    elif kind[ny][nx] == "relief" and role[ny][nx] != "stair":
                        # across a rock band the land stands above the water
                        dx, dy = nx - cx, ny - cy
                        ax, ay = nx, ny
                        for _ in range(3):
                            ax, ay = ax + dx, ay + dy
                            if not (0 <= ax < W and 0 <= ay < H) or kind[ay][ax] == "water":
                                break
                            if kind[ay][ax] == "land":
                                touch.append(cell[ay][ax] - 1)
                                break
            lv = min(touch) if touch else 0
            for (cx, cy) in body:
                cell[cy][cx] = lv

    # Rock masses with one level all round are not terrain steps. Touching
    # water they are rocks at sea, raised as mounds below; on land they are a
    # fence line or a patch of soil the role table calls rock, and they stay
    # level with the ground round them for the structure pass to stand up.
    mounds = []
    mass_seen = set()
    for y in range(H):
        for x in range(W):
            if kind[y][x] != "relief" or role[y][x] == "stair" or (x, y) in mass_seen:
                continue
            mass, stack, around, wet = [], [(x, y)], set(), False
            mass_seen.add((x, y))
            while stack:
                cx, cy = stack.pop()
                mass.append((cx, cy))
                for nx, ny in ((cx + 1, cy), (cx - 1, cy), (cx, cy + 1), (cx, cy - 1)):
                    if not (0 <= nx < W and 0 <= ny < H):
                        continue
                    if kind[ny][nx] == "relief" and role[ny][nx] != "stair":
                        if (nx, ny) not in mass_seen:
                            mass_seen.add((nx, ny))
                            stack.append((nx, ny))
                    elif cell[ny][nx] is not None:
                        around.add(cell[ny][nx])
                        wet = wet or kind[ny][nx] == "water"
            if len(around) > 1:
                continue
            if wet and not awash(layout, mass, kind):
                mounds.append(mass)
            else:
                lv = around.pop() if around else 0
                for (cx, cy) in mass:
                    cell[cy][cx] = lv

    # lattice: fixed where any non-relief cell touches, the highest of them
    LW, LH = W * PER_CELL + 1, H * PER_CELL + 1
    h = [[0.0] * LW for _ in range(LH)]
    fixed = [[False] * LW for _ in range(LH)]
    for j in range(LH):
        for i in range(LW):
            touching = []
            xs = (i // PER_CELL - 1, i // PER_CELL) if i % PER_CELL == 0 else (i // PER_CELL,)
            ys = (j // PER_CELL - 1, j // PER_CELL) if j % PER_CELL == 0 else (j // PER_CELL,)
            for cx in xs:
                for cy in ys:
                    if 0 <= cx < W and 0 <= cy < H and cell[cy][cx] is not None:
                        touching.append(cell[cy][cx])
            if touching:
                h[j][i] = max(touching) * LEVEL
                fixed[j][i] = True
    # harmonic fill of the relief (Gauss-Seidel, warm-started at the mean)
    free = [(i, j) for j in range(LH) for i in range(LW) if not fixed[j][i]]
    for _ in range(400):
        delta = 0.0
        for (i, j) in free:
            s, n = 0.0, 0
            for (a, b) in ((i + 1, j), (i - 1, j), (i, j + 1), (i, j - 1)):
                if 0 <= a < LW and 0 <= b < LH:
                    s += h[b][a]
                    n += 1
            v = s / n
            delta = max(delta, abs(v - h[j][i]))
            h[j][i] = v
        if delta < 0.01:
            break

    # mounds: rocks at sea, raised by their distance from the water
    for mass in mounds:
        inside = set()
        for (cx, cy) in mass:
            for j in range(cy * PER_CELL, cy * PER_CELL + PER_CELL + 1):
                for i in range(cx * PER_CELL, cx * PER_CELL + PER_CELL + 1):
                    if not fixed[j][i]:
                        inside.add((i, j))
        # distance (in lattice steps) to the nearest fixed point
        dist = {}
        frontier = [p for p in inside if any(
            0 <= a < LW and 0 <= b < LH and fixed[b][a]
            for (a, b) in ((p[0] + 1, p[1]), (p[0] - 1, p[1]), (p[0], p[1] + 1), (p[0], p[1] - 1)))]
        for p in frontier:
            dist[p] = 1
        k = 0
        while k < len(frontier):
            p = frontier[k]
            k += 1
            for q in ((p[0] + 1, p[1]), (p[0] - 1, p[1]), (p[0], p[1] + 1), (p[0], p[1] - 1)):
                if q in inside and q not in dist:
                    dist[q] = dist[p] + 1
                    frontier.append(q)
        for p, d in dist.items():
            h[p[1]][p[0]] += min(MOUND_MAX, d * MOUND)
    return role, cell, h


_ART = {}

# Secondary tilesets whose mountains are the General tileset's rock drawn
# again in their own colours (Jagged Pass, Mt Chimney: Lavaridge's red rock;
# Route 115: Fallarbor's beige rock).
# The analysis reads them as the General rock - each of their tiles that is
# a General rock tile recoloured stands for it, every other tile is seen in
# the General colours - and the export writes their own ids back, so the
# console draws their own drawing on the relief read off its twin.
ALIAS_TILESETS = {"gTileset_Lavaridge", "gTileset_Fallarbor"}
# the maps read so, each checked by the line check (voxel_relief_lines.py);
# Route 112's red mountain, trees on every terrace, is not yet
ALIAS_LAYOUTS = {"LAYOUT_JAGGED_PASS", "LAYOUT_MT_CHIMNEY", "LAYOUT_LAVARIDGE_TOWN"}
# Maps whose recoloured rock only the terraces read (voxel_terraces.py,
# terrace_art): the world is placed as it was before (read by the world's
# solve, Route 115's mountain moved Route 114 48 px, Fortree 64 px and left a
# step at the seam with Rustboro), and the terraces stand on it. Route 114's
# south is the same beige mountain as Route 115's (voxel_terraces.JOIN).
ALIAS_TERRACES = {"LAYOUT_ROUTE115", "LAYOUT_ROUTE114", "LAYOUT_ROUTE112"}
ALIAS_REFERENCE = "LAYOUT_ROUTE116"     # a layout over the General tileset
_ALIAS = {}
_TERRACE_ART = {}


def _drawing(img):
    """A tile's drawing without its palette: colours numbered as they come."""
    lab = {}
    return tuple(lab.setdefault(p, len(lab)) for p in img.convert("RGB").getdata())


def alias_of(lid, layouts=ALIAS_LAYOUTS):
    """({own id: General id}, {General id: own id}, {own colour: General
    colour}) of a layout drawn with an ALIAS_TILESETS tileset, else None.
    `layouts`: the maps read so (the world's solve: ALIAS_LAYOUTS)."""
    if lid not in layouts:
        return None
    if lid in _ALIAS:
        return _ALIAS[lid]
    _ALIAS[lid] = None
    e = next((e for e in json.load(open(os.path.join(vb.ROOT, "data", "layouts", "layouts.json"),
                                        encoding="utf-8"))["layouts"] if e.get("id") == lid), None)
    if e is None or e.get("secondary_tileset") not in ALIAS_TILESETS:
        return None
    art, ref = vb.LayoutArt(lid), vb.LayoutArt(ALIAS_REFERENCE)
    roles = ROCK_TILES | SIDE_WEST | SIDE_EAST | SEA_CAPS | BOULDER
    known = {}
    for m in sorted(roles):
        if m < vb.NUM_PRIMARY:
            known.setdefault(_drawing(ref.cell_image(m)), m)
    used = collections.Counter(art.metatile(x, y) for y in range(art.h) for x in range(art.w))
    alias, votes = {}, collections.defaultdict(collections.Counter)
    for m, n in used.items():
        if m < vb.NUM_PRIMARY:
            continue
        g = known.get(_drawing(art.cell_image(m)))
        if g is None:
            continue
        alias[m] = g
        for a, b in zip(art.cell_image(m).convert("RGB").getdata(), ref.cell_image(g).convert("RGB").getdata()):
            votes[a][b] += n
    # a boulder's head is drawn as a band's head turning the corner under a
    # rim is (093 is 06b's drawing): it is the boulder only where the map
    # draws it over the boulder's foot (Route 115's mounds); elsewhere it is
    # left to the analysis, which reads it as the corner it is (23b/23d)
    for m, g in list(alias.items()):
        if g not in BOULDER_HEADS:
            continue
        over = sum(1 for y in range(art.h - 1) for x in range(art.w)
                   if art.metatile(x, y) == m and alias.get(art.metatile(x, y + 1)) == BOULDER_HEADS[g])
        if 2 * over < used[m]:
            del alias[m]
    if not alias:
        return None
    colour = {a: c.most_common(1)[0][0] for a, c in votes.items()}
    back = {}
    for m, g in sorted(alias.items(), key=lambda kv: -used[kv[0]]):
        back.setdefault(g, m)
    _ALIAS[lid] = (alias, back, colour)
    return _ALIAS[lid]


class AliasArt(vb.LayoutArt):
    """A layout's drawing as the analysis reads it (alias_of)."""

    def __init__(self, layout_id, alias):
        super().__init__(layout_id)
        self.alias, self.back, self.colour = alias
        self._images = {}

    def metatile(self, x, y):
        m = super().metatile(x, y)
        return self.alias.get(m, m)

    def own_metatile(self, x, y):
        return super().metatile(x, y)

    def cell_image(self, m, layers=(0, 1)):
        key = (m, layers)
        if key not in self._images:
            img = super().cell_image(m, layers)
            if m >= vb.NUM_PRIMARY:
                img = img.copy()
                px = img.load()
                for j in range(16):
                    for i in range(16):
                        c = px[i, j]
                        n = self.colour.get(c[:3])
                        if n is not None:
                            px[i, j] = n + c[3:]
            self._images[key] = img
        return self._images[key]


def reading(art):
    """What a cache of a tile read off `art` must be keyed by besides its
    tilesets: an alias reading recolours its secondary tileset's tiles, so
    another map with the same tilesets read plainly sees other pixels (Route
    113 and Route 114 share Fallarbor's; keyed by the tilesets alone, the
    first map read decided the other's tiles, and the terraces of both groups
    changed with the order the groups were solved in)."""
    return art.id if isinstance(art, AliasArt) else None


def layout_art(lid):
    """The layout's drawing for the analysis (cached)."""
    if lid not in _ART:
        a = alias_of(lid)
        _ART[lid] = AliasArt(lid, a) if a else vb.LayoutArt(lid)
    return _ART[lid]


def own_id(lid, m, layouts=ALIAS_LAYOUTS):
    """The metatile the console has for an id the analysis read (alias_of)."""
    a = alias_of(lid, layouts)
    return a[1].get(m, m) if a else m


def terrace_art(lid):
    """The layout's drawing as voxel_terraces reads it: layout_art, or the
    alias reading of an ALIAS_TERRACES map (the world's solve keeps its own)."""
    if lid not in ALIAS_TERRACES:
        return layout_art(lid)
    if lid not in _TERRACE_ART:
        a = alias_of(lid, ALIAS_TERRACES)
        _TERRACE_ART[lid] = AliasArt(lid, a) if a else vb.LayoutArt(lid)
    return _TERRACE_ART[lid]


_OWN_ART = {}


def own_art(lid):
    """The layout's own drawing, its own ids and colours (cached)."""
    if lid not in _OWN_ART:
        _OWN_ART[lid] = vb.LayoutArt(lid)
    return _OWN_ART[lid]


def terrace_own_id(lid, m):
    """own_id for the terraces' reading (terrace_art)."""
    return own_id(lid, m, ALIAS_LAYOUTS | ALIAS_TERRACES)


# ── mountains read off the drawing ──────────────────────────────────────────
#
# The mountains of the General tileset (Route 116, Verdanturf, ...) are drawn
# terrace upon terrace, and their tops, faces and rims are told apart by their
# colours, not by the cell they are in: most of their metatiles are part top,
# part face. So these maps are read pixel by pixel.
#
#   * a pixel is top, face, rim (the pale edge a terrace is drawn with where it
#     rises from the one north of it: its own face, turned north, is hidden),
#     ground (anything else: grass, trees, houses), or free (stairs);
#   * tops and grounds are regions, 4-connected, each of its own kind;
#   * down a column, a face between two regions is a south face: seen straight
#     on at 45 degrees it is drawn as tall as it is, so the region above stands
#     exactly as many pixels higher as the face is long. A rim is a step of
#     RIM_RISE up to the south;
#   * the levels are the least-squares answer to all of that, weighed by how
#     many columns say it, the ground at the maps' open edges held at 0. Runs
#     that disagree with the answer (a west or east face read down its length)
#     are dropped and it is solved again;
#   * levels are whole levels (every face is drawn one level tall);
#   * the shape is laid cell by cell, since the art is drawn in whole cells:
#     a cell mostly top or mostly ground is a footprint at its region's level
#     (a ledge a cell wide between bands counts its level across them); the
#     rest - faces, bands, corners, stairs - is rock, one level per cell,
#     hung from the cell it is drawn below (see the tile pass). A tile
#     repeated is the same plane all along, so every edge is a straight line
#     where the drawing's is, parallel to the next; the rim's drawn wobble
#     stays in the texture, and grass is never bent. `--proof` draws it.
#
# The tile pattern. Every tile of rock is one level and one of four shapes,
# Route 116's, and each hangs from the edge it shares with what it is drawn
# below - the lattice points of that edge, as they stand, not the
# neighbour's level:
#
#   face    from its north edge: vertical, a pixel down a pixel
#   band    from its high side's edge: a 45-degree slope across its row
#   corner  from every edge round it: a quarter cone under a terrace's
#           corner, an inner corner under two edges, a band's end wall
#           under a band's profile (Route 105's ridge ends) - and never more
#           than a level a tile: a corner over land further down ends in a
#           wall at its side, which the camera sees edge on
#   top     a footprint, flat at its level
#
# Levels are counted the same way where regions cannot say: a crest (two
# bands back to back) and a ridge's top a cell or two wide stand a level
# over their foot for every band down to it (Route 105's ridge on a ridge,
# two bands a side); the ends of such a ridge, drawn as a boulder's halves,
# are rock. voxel_relief_check.py checks the result against what the
# projection allows, cell by cell, with Route 116 as the reference, and
# shows any cell through the game's camera.
#
# Maps drawn across a seam are solved on one canvas, placed as they connect,
# so the mountain is one mountain on both sides of it.

ROCK_TOP = {(0xde, 0xb4, 0xa4), (0xbd, 0x94, 0x8b)}
ROCK_RIM = {(0xee, 0xd5, 0xcd)}
ROCK_FACE = {(0x83, 0x5a, 0x5a), (0x62, 0x41, 0x52), (0x41, 0x31, 0x41)}
ROCK_FLECK = {(0x9c, 0x73, 0x73)}   # in both: the tops' speckle, the faces' shading
ROCK_ALL = ROCK_TOP | ROCK_RIM | ROCK_FACE | ROCK_FLECK
RIM_RISE = 16
# cut record flag (voxel_relief.h VOXEL_RELIEF_GOES_ON >> 8): the rock north
# of this cell goes on under it
GOES_ON = 2
# Patches of bare soil on the grass: drawn in the rock faces' colours, flat.
DIRT = {0x113, 0x114, 0x115, 0x14b, 0x14c, 0x14d}
# A boulder drawn two cells tall sits on its terrace: all of it is top.
BOULDER = {0x93, 0x94, 0x9b, 0x9c}
BOULDER_HEADS = {0x93: 0x9b, 0x94: 0x9c}    # its upper half: the lower half under it
# West and east faces are drawn as bands down the side of the higher terrace,
# one band a level: the face on the left of the cell and the top on its right
# faces west, and the other way round east. Nothing in a 45-degree drawing
# says how tall they are, so each is a level, as its south faces are.
SIDE_WEST = {0x070, 0x073}
SIDE_EAST = {0x072, 0x075, 0x0a2}
# A ridge at sea (Route 105's) is drawn of these: its north end rounded by
# the caps, drawn over the water behind it (most of their pixels are sea, but
# they are the ridge's end, a corner of rock hung from its top). Where one
# stretch of it runs on beside the next, 074 is the corner it is inland - the
# top of a band beside the end of the band before it - and is one.
SEA_CAPS = {0x172, 0x174}
SIDE_RISE = 16
RIDGE_TOP = 2       # cells: a top this narrow between bands is a ridge's, counted across them
# South faces as drawn down a column, one level each: the faces, the cave
# mouths and doors in them, their feet over grass or trees. A cell of rock
# that is neither one of these nor a band is a corner.
FACE_SOUTH = {0x07c, 0x0a9, 0x09f, 0x0a7, 0x091, 0x079, 0x1b0, 0x0af, 0x0cf,
              0x1f0, 0x1f1, 0x33b, 0x33c}
FOOTPRINT = 0.6    # share of a cell drawn as top (or as ground) that makes it a footprint
REGION_MIN = 200    # smaller patches of top (a boulder, a speck) are not terraces
THIN = 7            # nor is a strip of top thinner than this down its columns, on average
MAJORITY = 2        # half-width of the window a rock pixel's kind is voted in

GROUND, TOP, FACE, FLECK, RIM, VOID, FREE = range(7)
# Rock is never walked on, and never a sign or water: a cell the player can
# walk (a bridge, a pier, a path of soil), a signpost or water is ground,
# flat at the level of the ground it joins, whatever colours it is drawn in -
# the wood of a bridge is the rock's brown. Not the stairs, nor a cave's
# mouth; but a bridge's planks read as treads, so a "stair" the cartridge
# calls a bridge, a log or a door is flat too. Not a cell the rock is drawn
# over, in the upper layer (a sea stack's crown over the water behind it).
FLAT_ROLES = {"floor", "signpost", "water"}
WATERFALL = vc.MB["MB_WATERFALL"]
# Water drawn as rock, the shoulder of a stack or a hill in the sea with the
# water surfed behind it, is the rock: this share of its pixels in the rock's
# colours.
ROCKY_WATER = 0.5
_ROCKY_WATER = {}
SHORE_STRIP = 8     # cells: a patch of shore this small, reached from nowhere, is the water's
FLAT_BEHAVIOURS = {vc.MB[k] for k in vc.MB if "BRIDGE" in k or "_LOG_" in k or k.endswith("_DOOR")
                   or k == "MB_NO_RUNNING"} - {vc.MB.get("MB_REFLECTION_UNDER_BRIDGE")}
# A pier: planks the player walks, drawn in the upper layer over the water
# beside them (Mr Briney's, off Route 104's cliff). It stands on the ground it
# runs out from; its pixels do not join the water's, or the sea below a cliff
# and the land on top of it would be one terrace.
_PIER = {}      # group -> canvas cells of pier
SANDS = {vc.MB[k] for k in vc.MB if "SAND" in k}

# Every map whose mountains are drawn with the General tileset's rock is read
# off its drawing: a layout on it with at least DRAWN_MIN cells of face or
# band, outdoors. Maps whose rock runs across a seam are solved together
# (a neighbour joins with any rock at all at the seam), on one
# canvas placed as the maps connect, so a mountain is one mountain on both
# sides of it; a seam with no rock at it keeps the maps apart.
DRAWN_MIN = 5
DRAWN_SEAM = 2          # cells either side of a seam that count as "at" it
ROCK_TILES = ({0x070, 0x072, 0x073, 0x074, 0x075, 0x07b, 0x07c, 0x07d, 0x089, 0x0a9}
              | FACE_SOUTH | {0x070, 0x073} | {0x072, 0x075, 0x0a2})


def find_drawn():
    root = vb.ROOT
    layouts = {e["id"]: e for e in json.load(open(os.path.join(root, "data", "layouts", "layouts.json"),
                                                  encoding="utf-8"))["layouts"] if e.get("id")}
    blocks, seeds = {}, set()
    outdoor = vc.MapEvents().outdoor
    alternates = vc.alternate_layouts()
    for lid, e in layouts.items():
        if lid in alternates:
            continue
        if e.get("primary_tileset") != "gTileset_General":
            continue
        path = os.path.join(root, e["blockdata_filepath"])
        if not os.path.exists(path):
            continue
        raw = open(path, "rb").read()
        ms = [v & 0x3FF for v in struct.unpack("<%dH" % (len(raw) // 2), raw)]
        a = alias_of(lid)
        if a:
            ms = [a[0].get(m, m) for m in ms]
        n = sum(1 for m in ms if m in ROCK_TILES)
        if n and lid in outdoor:
            blocks[lid] = ms
            if n >= DRAWN_MIN:
                seeds.add(lid)
    def rock_near(lid, x0, y0, x1, y1):
        e, ms = layouts[lid], blocks[lid]
        w, h = e["width"], e["height"]
        return any(ms[y * w + x] in ROCK_TILES for y in range(max(0, y0), min(h, y1))
                   for x in range(max(0, x0), min(w, x1)))
    # map connections, as layouts: (a, b, dx, dy) places b at a's origin + (dx, dy)
    links = {}
    maps_dir = os.path.join(root, "data", "maps")
    for name in sorted(os.listdir(maps_dir)):
        path = os.path.join(maps_dir, name, "map.json")
        if not os.path.exists(path):
            continue
        m = json.load(open(path, encoding="utf-8"))
        a = m.get("layout")
        if a not in blocks:
            continue
        for c in m.get("connections") or []:
            other = os.path.join(maps_dir, "".join(p.capitalize() for p in c["map"][4:].split("_")), "map.json")
            b = None
            for cand in (other,):
                if os.path.exists(cand):
                    b = json.load(open(cand, encoding="utf-8")).get("layout")
            if b is None:
                for n2 in os.listdir(maps_dir):
                    p2 = os.path.join(maps_dir, n2, "map.json")
                    if os.path.exists(p2):
                        j = json.load(open(p2, encoding="utf-8"))
                        if j.get("id") == c["map"]:
                            b = j.get("layout")
                            break
            if b not in blocks or b == a:
                continue
            A, B, off, d = layouts[a], layouts[b], c.get("offset", 0), c.get("direction")
            if d == "down":
                dx, dy = off, A["height"]
                seam = (rock_near(a, off, A["height"] - DRAWN_SEAM, off + B["width"], A["height"])
                        and rock_near(b, -off, 0, -off + A["width"], DRAWN_SEAM))
            elif d == "up":
                dx, dy = off, -B["height"]
                seam = (rock_near(a, off, 0, off + B["width"], DRAWN_SEAM)
                        and rock_near(b, -off, B["height"] - DRAWN_SEAM, -off + A["width"], B["height"]))
            elif d == "right":
                dx, dy = A["width"], off
                seam = (rock_near(a, A["width"] - DRAWN_SEAM, off, A["width"], off + B["height"])
                        and rock_near(b, 0, -off, DRAWN_SEAM, -off + A["height"]))
            elif d == "left":
                dx, dy = -B["width"], off
                seam = (rock_near(a, 0, off, DRAWN_SEAM, off + B["height"])
                        and rock_near(b, B["width"] - DRAWN_SEAM, -off, B["width"], -off + A["height"]))
            else:
                continue
            if seam:
                links.setdefault(a, []).append((b, dx, dy))
                links.setdefault(b, []).append((a, -dx, -dy))
    groups, seen = {}, set()
    for lid in sorted(seeds):
        if lid in seen:
            continue
        pos, queue = {lid: (0, 0)}, [lid]
        while queue:
            a = queue.pop(0)
            for (b, dx, dy) in links.get(a, []):
                if b not in pos:
                    pos[b] = (pos[a][0] + dx, pos[a][1] + dy)
                    queue.append(b)
        seen.update(pos)
        mx, my = min(p[0] for p in pos.values()), min(p[1] for p in pos.values())
        groups[lid[7:].lower()] = {l: (x - mx, y - my) for l, (x, y) in pos.items()}
    # a layout a script swaps in is solved with its map's neighbours, in its
    # map's place, as a group of its own
    originals = list(groups.items())
    for alt, base in sorted(alternates.items()):
        for name, members in originals:
            if base in members and alt in outdoor:
                groups[alt[7:].lower()] = {(alt if l == base else l): at for l, at in members.items()}
    return groups


DRAWN = find_drawn()


_DRAWN = {}
_SHIFT = {}     # layout -> the depth lattice of a drawn map (see solve_drawn)
_DRAWN_SIDE = {}  # group -> per-cell band side, for camera proofs
_ROCK = {}      # group -> (rock tops, rock tile kinds, soil, metatile lookup), for proofs
_CELLS = {}     # group -> footprint level per cell (None: rock between), for proofs


def drawn_group(layout_id, checked=True):
    """The group a layout is drawn in, or None. With `checked`, only a group
    whose solve passed drawn_ok: the rest keep level ground and their
    ledges until they are looked at."""
    for name, members in DRAWN.items():
        if layout_id in members:
            return name if not checked or drawn_ok(name) else None
    return None


# A group's solve is trusted when its ground stays within reach of itself:
# at most this share of its grass, sand and soil cells stands more than
# MASSIF off the level most of it stands at. Terraces a few levels apart are
# what a mountain is; where the rock is drawn as a fill (a massif of one band
# tile repeated, as round Ever Grande) counting a level a band lifts whole
# towns hundreds of pixels up, and the map is left level instead.
GROUND_SPREAD = 0.05
MASSIF = 160    # pixels: ground this far from most of the group's is a massif counted a level a band
_QUALITY = {}
_PREP = {}      # group -> drawn_prepare's reading of it
_BASE = {}      # layout -> the level its ground stands at, pixels (world_levels, solve_drawn)
_WORLD = {}


# Groups that pass but were seen wrong in their camera proofs, and why.
DRAWN_EXCLUDED = {
    "route122": "Mt Pyre's island is a massif of one band tile: counted a level a band it spikes",
    "ever_grande_city": "the cliff and the fall under the League, ten cells of band tiles tall: "
                        "a level a cell, slabs with gaps between them and the fall a stack of walls",
}


def drawn_ok(name):
    if name in DRAWN_EXCLUDED:
        return False
    return name in world_levels()["regions"]


# ── the world's levels ───────────────────────────────────────────────────
#
# A map is lifted as a whole where its ground has to be: Fortree stands on
# the plateau Route 119's cliffs climb to, not at the level of the sea Route
# 118 runs down to. Every drawn group's terraces are solved against each
# other first, in blocks the drawing holds together (terraces joined by a
# face, a band, a walk); then the blocks and the maps that are not drawn -
# a town, a route of grass - are placed so that the ground either side of
# every seam meets. Where the world does not close up (the seams round a
# loop of routes asking for more than its cliffs give), the seams that
# disagree most are the ones given up: a step at one seam, not a cliff
# laid flat.

SEAM_WEIGHT = 16     # per cell of seam, as a column of face pixels counts
# Where a loop of maps does not close, the step it leaves goes to the seam
# with the least say, and nothing in the drawing says which one should be:
# these are the ones chosen, out in the open sea, and why. A seam here keeps
# STEP_SEAM_WEIGHT a cell, so the loop's level is given up there and nowhere
# else (no other loop runs through it).
STEP_SEAMS = {
    ("LAYOUT_ROUTE107", "LAYOUT_ROUTE108"):
        "the sea round Route 104's beach stands a level under the sea round "
        "Slateport; on Route 106's seam with Dewford the step cut the ridge, "
        "the wood and the beach they share",
}
STEP_SEAM_WEIGHT = 1
# The level a step seam gives up, laid over a whole map of open sea as a
# slope instead of standing at the seam as a wall of water a level tall (seen
# from Route 107 and from Route 108 alike, and walked through on the crossing).
# The map keeps its base, where the world placed it; its sea leans from the
# level its neighbour on one side has at the seam to the level of the one on
# the other side - a level over its sixty cells, a pixel every four, so every
# seam it has meets and nothing between Dewford and Slateport steps. Its
# heights have no depth (level, in relief.bin): they lift it straight up, as
# the bases either side of it do. As far south as high, its west edge stood a
# row north of Route 107's and the player jumped a row on the crossing.
SEA_RAMPS = {
    "LAYOUT_ROUTE108": "open sea between the Dewford side (a level under) and "
                       "Slateport's (Route 107 / Route 108, the step seam above)",
}
# Groups the world places by voxel_terraces.py's count of their levels
# (region_ties) instead of this file's solve, and why. Only these: every
# group read anew moves the world (all of them at once put steps at
# Rustboro's seams with Routes 115 and 116).
COUNTED_GROUPS = {
    "route111": "the harmonic solve put three levels between the hollow at "
                "Mauville and the plateau its two flights of stairs climb to: "
                "pinned at Fallarbor and Lavaridge, the world left the terraces "
                "a level off Mauville's seam (the step goes to Fallarbor / Route "
                "114's, given up already)",
    "route111_no_mirage_tower": "the same, the layout without the Mirage Tower",
}
HARD_WEIGHT = 1e6    # ground walked from one to the other
LOOSE_WEIGHT = 0.01  # a block nothing places: kept near its group's ground
WORLD_ROOT = "LAYOUT_LITTLEROOT_TOWN"


def map_links():
    """[(a, b, direction, offset)]: every connection between two outdoor
    layouts, as map.json states it (b is `direction` of a, shifted by
    `offset`). A layout a script swaps in has its map's."""
    root = vb.ROOT
    maps_dir = os.path.join(root, "data", "maps")
    layout_of, found = {}, []
    for name in sorted(os.listdir(maps_dir)):
        path = os.path.join(maps_dir, name, "map.json")
        if os.path.exists(path):
            m = json.load(open(path, encoding="utf-8"))
            layout_of[m.get("id")] = m.get("layout")
            found.append(m)
    outdoor = vc.MapEvents().outdoor
    links = set()
    for m in found:
        a = m.get("layout")
        if a not in outdoor:
            continue
        for c in m.get("connections") or []:
            b = layout_of.get(c.get("map"))
            if b in outdoor and b != a and c.get("direction") in ("up", "down", "left", "right"):
                links.add((a, b, c["direction"], c.get("offset", 0)))
    for alt, base in vc.alternate_layouts().items():
        if alt in outdoor:
            for (a, b, d, off) in list(links):
                if a == base:
                    links.add((alt, b, d, off))
                if b == base:
                    links.add((a, alt, d, off))
    return sorted(links)


def _seam_cells(a, b, direction, offset, size):
    """[(edge of a, index along it, edge of b, index along it)] for the cells
    of a seam, `size` a layout's (w, h)."""
    (aw, ah), (bw, bh) = size[a], size[b]
    out = []
    if direction in ("up", "down"):
        for x in range(max(0, offset), min(aw, offset + bw)):
            out.append(("down" if direction == "down" else "up", x,
                        "up" if direction == "down" else "down", x - offset))
    else:
        for y in range(max(0, offset), min(ah, offset + bh)):
            out.append(("right" if direction == "right" else "left", y,
                        "left" if direction == "right" else "right", y - offset))
    return out


def _gauss_seidel(n, pairs, fixed, iterations=4000):
    """Least squares over "h[a] - h[b] = d" (pairs: {(a, b): (d, w)}), the
    nodes in `fixed` held at their values."""
    h = [0.0] * n
    for k, v in fixed.items():
        h[k] = v
    adj = [[] for _ in range(n)]
    for (a, b), (d, w) in pairs.items():
        adj[a].append((b, d, w))
        adj[b].append((a, -d, w))
    for _ in range(iterations):
        delta = 0.0
        for r in range(n):
            if r in fixed or not adj[r]:
                continue
            # Keep the release's left-to-right rounding: Python 3.12+ sum()
            # compensates float additions, which can change seam selection.
            total = weight = 0
            for o, d, w in adj[r]:
                total += w * (h[o] + d)
                weight += w
            v = total / weight
            delta = max(delta, abs(v - h[r]))
            h[r] = v
        if delta < 0.001:
            break
    return h


def _robust(n, samples, fixed, hard=()):
    """Solve samples {(a, b): [d...]} with each pair's median, drop the
    samples more than a half level off the answer and solve again. The
    pairs in `hard` are one level, whatever the rest says."""
    def median(v):
        v = sorted(v)
        return v[len(v) // 2]
    tie = {k: (0.0, HARD_WEIGHT) for k in hard}
    first = dict(tie)
    first.update({k: (median([d for d, _ in v]), sum(w for _, w in v))
                  for k, v in samples.items() if k not in tie})
    first = _gauss_seidel(n, first, fixed)
    kept = dict(tie)
    for (a, b), v in samples.items():
        if (a, b) in tie:
            continue
        good = [(d, w) for d, w in v if abs(first[a] - first[b] - d) <= 8]
        if good:
            kept[(a, b)] = (median([d for d, _ in good]), sum(w for _, w in good))
    return _gauss_seidel(n, kept, fixed), kept


def _give_up_seams(n, samples, fixed):
    """The world's offsets: solved, and while some pair of nodes is joined
    more than a half level off what it asks, the worst of them is given up
    - one seam at a time, so that a map pulled two ways follows one of them
    and the step is at the other, not halfway at both.

    A loop of seams a level short (the sea below Route 104's beach and the
    sea round Slateport) spreads its level over every seam round it, a few
    pixels each, never a half level at any: but the maps stand whole levels
    up, and where two of them round to levels apart the step is there. That
    seam is given up too, and the world solved again without it, so the
    pixels of the loop are not left on every map, tipping terraces drawn
    between two levels far away."""
    def median(v):
        v = sorted(v)
        return v[len(v) // 2]
    pairs = {k: (median([d for d, _ in v]), sum(w for _, w in v)) for k, v in samples.items()}
    while True:
        off = _gauss_seidel(n, pairs, fixed)
        worst, err = None, 8.0
        for (a, b), (d, w) in pairs.items():
            e = abs(off[a] - off[b] - d)
            if w >= SEAM_WEIGHT and e > err:
                worst, err = (a, b), e
        if worst is None:
            whole = [LEVEL * round(v / LEVEL) for v in off]
            err = 0.0
            for (a, b), (d, w) in pairs.items():
                e = abs(off[a] - off[b] - d)
                if w >= SEAM_WEIGHT and abs(whole[a] - whole[b] - d) > 8.0 and e > err:
                    worst, err = (a, b), e
        if worst is None:
            return off
        del pairs[worst]


def _blocks(prep, counted=None):
    """A group's terraces solved against each other: (levels relative to
    their block's first terrace, block of each terrace). A terrace nothing
    joins to another is a block of its own. `counted`: {(a, b): d} drops
    between regions that hold whatever the drawing's runs say (the count of
    voxel_terraces.py, for the groups built that way)."""
    sizes, runs = prep["sizes"], prep["runs"]
    if counted:
        runs = {k: v for k, v in runs.items() if k not in counted and k[::-1] not in counted}
    n = len(sizes)
    adj = [set() for _ in range(n)]
    for (a, b) in list(runs) + list(counted or ()):
        adj[a].add(b)
        adj[b].add(a)
    block, anchors = [-1] * n, {}
    for r in sorted(range(n), key=lambda r: -sizes[r]):
        if block[r] >= 0 or not prep["big"][r]:
            continue
        k = len(anchors)
        anchors[r] = 0.0
        block[r], stack = k, [r]
        while stack:
            c = stack.pop()
            for o in adj[c]:
                if block[o] < 0:
                    block[o] = k
                    stack.append(o)
    samples = {k: [(d, 1) for d in v] for k, v in runs.items()}
    samples.update({k: [(d, HARD_WEIGHT)] for k, d in (counted or {}).items()})
    level, _ = _robust(n, samples, anchors, prep.get("ties", ()))
    return level, block


def world_levels():
    """{"regions": {group: absolute level of each terrace}, "base": {layout:
    level of a map not drawn}} - see above. Groups whose ground comes out
    spread over many levels (a massif of one band tile, counted a level a
    band) are left out and their maps laid level, as maps not drawn."""
    if _WORLD:
        return _WORLD
    size = {e["id"]: (e["width"], e["height"])
            for e in json.load(open(os.path.join(vb.ROOT, "data", "layouts", "layouts.json"),
                                    encoding="utf-8"))["layouts"] if e.get("id")}
    links = map_links()
    candidates = [g for g in DRAWN if g not in DRAWN_EXCLUDED]
    # the groups placed by voxel_terraces.py's count of their levels
    import voxel_terraces
    terraced = {g for g in candidates if g in COUNTED_GROUPS
                and set(DRAWN[g]) & set(voxel_terraces.chosen_layouts())}
    local = {}
    for g in candidates:
        prep = drawn_prepare(g)
        local[g] = _blocks(prep, voxel_terraces.region_ties(g, prep) if g in terraced else None)

    def solve(groups):
        owner = {}
        for g in groups:
            for lid in DRAWN[g]:
                owner.setdefault(lid, g)
        nodes, index = [], {}

        def node(key):
            if key not in index:
                index[key] = len(nodes)
                nodes.append(key)
            return index[key]
        samples = collections.defaultdict(list)
        # every block is kept loosely at its group's biggest block, so that
        # one nothing places does not drift off
        for g in groups:
            level, block = local[g]
            main = node(("b", g, 0))
            for k in set(block) - {-1, 0}:
                samples[(node(("b", g, k)), main)].append((0.0, LOOSE_WEIGHT))
        for (a, b, direction, offset) in links:
            ga, gb = owner.get(a), owner.get(b)
            if ga is not None and ga == gb and b in DRAWN[ga] and a in DRAWN[ga]:
                continue    # a seam inside a group: its canvas has it
            for (ea, ia, eb, ib) in _seam_cells(a, b, direction, offset, size):
                def side(g, lid, edge, i):
                    if g is None:
                        return node(("f", lid)), 0.0
                    r = _PREP[g]["edges"][(lid, edge)][i]
                    if r is None:
                        return None
                    level, block = local[g]
                    return node(("b", g, block[r])), level[r]
                sa, sb = side(ga, a, ea, ia), side(gb, b, eb, ib)
                if sa is None or sb is None:
                    continue
                # off[a] + h[a] = off[b] + h[b]
                w = STEP_SEAM_WEIGHT if (a, b) in STEP_SEAMS or (b, a) in STEP_SEAMS else SEAM_WEIGHT
                samples[(sa[0], sb[0])].append((sb[1] - sa[1], w))
        fixed = {}
        root = index.get(("f", WORLD_ROOT))
        # one node held in every piece of the world: Littleroot where it is
        comp = [-1] * len(nodes)
        adj = collections.defaultdict(set)
        for (a, b) in samples:
            adj[a].add(b)
            adj[b].add(a)
        order = ([root] if root is not None else []) + list(range(len(nodes)))
        for start in order:
            if comp[start] >= 0:
                continue
            comp[start] = start
            fixed[start] = 0.0
            stack = [start]
            while stack:
                c = stack.pop()
                for o in adj[c]:
                    if comp[o] < 0:
                        comp[o] = start
                        stack.append(o)
        off = _give_up_seams(len(nodes), samples, fixed)
        regions = {}
        for g in groups:
            level, block = local[g]
            regions[g] = [LEVEL * round((off[index[("b", g, block[r])]] + level[r]) / LEVEL)
                          if block[r] >= 0 and ("b", g, block[r]) in index else
                          LEVEL * round(off[index[("b", g, 0)]] / LEVEL)
                          for r in range(len(level))]
        base = {key[1]: LEVEL * round(off[i] / LEVEL) for key, i in index.items() if key[0] == "f"}
        broken = collections.Counter()
        for (a, b), v in samples.items():
            for d, w in v:
                if w in (SEAM_WEIGHT, STEP_SEAM_WEIGHT) and abs(off[a] - off[b] - d) > 8:
                    broken[(nodes[a][1], nodes[b][1])] += 1
        return regions, base, broken

    def spread(g, regions):
        prep = _PREP[g]
        got = collections.Counter()
        for row in prep["stats"]:
            for st in row:
                if st is None:
                    continue
                n, top, ground, counts = st
                if ground >= FOOTPRINT * n and counts:
                    got[regions[g][max(counts, key=counts.get)]] += 1
        if not got:
            return 0.0
        mode = got.most_common(1)[0][0]
        return sum(v for k, v in got.items() if abs(k - mode) > MASSIF) / float(sum(got.values()))

    groups = list(candidates)
    while True:
        regions, base, broken = solve(groups)
        for g in groups:
            _QUALITY[g] = spread(g, regions)
        bad = [g for g in groups if _QUALITY[g] > GROUND_SPREAD]
        if not bad:
            break
        groups = [g for g in groups if g not in bad]
    for g in candidates:
        if g not in groups:
            print("level  %-28s ground spread %.1f%%: laid level" % (g, 100 * _QUALITY[g]))
    print("world: %d groups, %d maps placed, %d seam cells given up"
          % (len(groups), len(base), sum(broken.values())))
    for (a, b), n in sorted(broken.items(), key=lambda kv: -kv[1]):
        print("  step at the seam %s / %s: %d cells" % (a, b, n))
    for lid, b in sorted(base.items()):
        if b:
            print("  %-36s stands at %+d px" % (lid, b))
    _WORLD["broken"] = broken
    _WORLD["regions"], _WORLD["base"] = regions, base
    return _WORLD


def sea_ramps(relief, quiet=False):
    """SEA_RAMPS over a parsed relief.bin (voxel_relief_fixes.parse), in
    place: each map's sea leans from the level of its west (north) neighbour
    at their seam to that of its east (south) one, every lattice point by its
    distance across the map, with no depth (level). The levels are read off
    the neighbours as the file has them - after the terraces, whose blocks
    keep the world's seams - the commonest height along the seam, each of its
    cells' edge points over the base. Returns the maps changed."""
    size = {e["id"]: (e["width"], e["height"])
            for e in json.load(open(os.path.join(vb.ROOT, "data", "layouts", "layouts.json"),
                                    encoding="utf-8"))["layouts"] if e.get("id")}
    links = map_links()
    side = PER_CELL + 1
    edge_points = {"left": [j * side for j in range(side)],
                   "right": [j * side + side - 1 for j in range(side)],
                   "up": list(range(side)),
                   "down": [(side - 1) * side + i for i in range(side)]}

    def edge_cell(lid, edge, i):
        w, h = size[lid]
        return {"down": (i, h - 1), "up": (i, 0), "right": (w - 1, i), "left": (0, i)}[edge]

    def level_at(lid, direction):
        """The commonest height of the neighbour `direction` of lid along
        their seam, pixels in the world; None if there is none."""
        found = collections.Counter()
        for (a, b, d, off) in links:
            if a != lid or d != direction:
                continue
            t = relief["layouts"].get(voxel_relief_fixes.layout_index(b))
            base = t["base"] if t else 0
            for (_, _, eb, ib) in _seam_cells(a, b, d, off, size):
                g = t["cells"].get(edge_cell(b, eb, ib)) if t else None
                for k in edge_points[eb]:
                    found[base + (g[k] if g else 0)] += 1
        return found.most_common(1)[0][0] if found else None

    changed = []
    for lid in sorted(SEA_RAMPS):
        index = voxel_relief_fixes.layout_index(lid)
        w, h = size[lid]
        for (lo, hi, across) in (("left", "right", True), ("up", "down", False)):
            start, end = level_at(lid, lo), level_at(lid, hi)
            if start is not None and end is not None and start != end:
                break
        else:
            continue
        t = relief["layouts"].setdefault(index, {"w": w, "h": h, "flags": 0, "base": 0, "belt": 0,
                                                 "cells": {}})
        if t["flags"] & (voxel_relief_fixes.UNIT2 | voxel_relief_fixes.DRAWN):
            raise ValueError("%s: a sea ramp over a drawn map's relief" % lid)
        if t["cells"]:
            # a ledge's berm would lose its depth with the map's
            raise ValueError("%s: a sea ramp over a map with relief of its own" % lid)
        t["level"] = True
        span = float(w if across else h)
        for y in range(h):
            for x in range(w):
                g = [0] * (side * side)
                for j in range(side):
                    for i in range(side):
                        f = ((x + i / float(PER_CELL)) if across else (y + j / float(PER_CELL))) / span
                        v = start + (end - start) * f - t["base"]
                        g[j * side + i] += int(math.floor(v + 0.5))
                if any(g):
                    t["cells"][(x, y)] = g
        changed.append(lid)
        if not quiet:
            print("sea ramp %-30s %+d px at its %s seam to %+d px at its %s one, on a base of %+d px"
                  % (lid, start, lo, end, hi, t["base"]))
    return changed


def apply_sea_ramps(path, quiet=False):
    relief = voxel_relief_fixes.parse(open(path, "rb").read())
    if sea_ramps(relief, quiet):
        open(path, "wb").write(voxel_relief_fixes.pack(relief))


CUT_SINK = 2        # pixels: a cut's foot further under all the ground beside it is raised


def cut_feet(relief, quiet=False):
    """Every cut tile's foot (the ground run on under its rock, seen through
    its background) at most CUT_SINK under the lowest edge point of the
    cells beside it that are no cut tile themselves, over a parsed relief.bin
    (voxel_relief_fixes.parse), in place. cut_cells reads the foot off the
    cell's lattice before the cut levels it: a lone piece of rock on level
    ground (Jagged Pass's shrubs, the stairs up to Lilycove's department
    store, a rock in the sea) dips there, and its ground stood in a pit the
    ground round it never reaches, the void showing at the pit's rim.
    Returns how many feet it raised."""
    side = PER_CELL + 1
    facing = {(0, -1): [(side - 1) * side + i for i in range(side)],    # north: its south edge
              (0, 1): list(range(side)),
              (-1, 0): [j * side + side - 1 for j in range(side)],
              (1, 0): [j * side for j in range(side)]}
    cuts = relief["cuts"]
    raised = 0
    for (lay, x, y), rec in cuts.items():
        t = relief["layouts"].get(lay)
        if rec[0] == 0xFFFF or t is None:
            continue
        lows = []
        for (dx, dy), points in facing.items():
            n = (x + dx, y + dy)
            if not (0 <= n[0] < t["w"] and 0 <= n[1] < t["h"]):
                continue
            other = cuts.get((lay,) + n)
            if other is not None and other[0] != 0xFFFF:
                continue    # (a cut beside it: its own lattice dips the same way)
            g = t["cells"].get(n)
            lows.append(min(g[k] for k in points) if g else 0)
        if lows and rec[1] < min(lows) - CUT_SINK:
            rec[1] = min(lows)
            raised += 1
    if not quiet:
        print("voxel relief: %d cut tiles' feet raised to the ground beside them" % raised)
    return raised


def apply_cut_feet(path, quiet=False):
    relief = voxel_relief_fixes.parse(open(path, "rb").read())
    if cut_feet(relief, quiet):
        open(path, "wb").write(voxel_relief_fixes.pack(relief))


def drawn_canvas(name):
    """The group's canvas: (layouts, width, height, per-pixel kind, per-cell
    hiding - a roof or a tree a face can go down behind -, per-cell side: +1
    a face turned west, -1 east, per-cell flat: ground that is never rock
    whatever its colours - see FLAT_ROLES)."""
    members = DRAWN[name]
    layouts = {lid: open_roles(lid) for lid in members}
    for lid in members:
        if lid not in _ART:
            layout_art(lid)
    CW = max(ox + layouts[l].w for l, (ox, oy) in members.items())
    CH = max(oy + layouts[l].h for l, (ox, oy) in members.items())
    W, H = CW * 16, CH * 16
    kind = [[VOID] * W for _ in range(H)]
    blocked = [[False] * CW for _ in range(CH)]
    side = [[0] * CW for _ in range(CH)]
    flat = [[False] * CW for _ in range(CH)]
    face_low = set()
    tile_kinds = {}     # (tilesets, metatile, layers, stair) -> its 16 rows of kinds
    entries = {e["id"]: e for e in voxel_props._layouts()}
    for lid, (ox, oy) in members.items():
        L, A = layouts[lid], _ART[lid]
        # the cells a modelled object stands on (voxel_props: a rock or a
        # stack in the sea, a boulder on a terrace): the model stands for
        # the object, and the relief reads what is under it, the cell's
        # lower layer - the sea, flat at the water's level, or the ground or
        # the terrace's top it lies on, read as any other
        props = set(voxel_props.cells_in(entries[lid]))
        # in a map read off its drawing only now (alias_of), a ledge and
        # berry soil are ground, whatever rock they are drawn like: they
        # keep the shape they always had (Jagged Pass draws its ledges'
        # lips as pieces of a face)
        keep = set()
        if alias_of(lid):
            berry = vc.behaviours().get("MB_BERRY_TREE_SOIL")
            keep = set(ledge_cells(L)) | {(x, y) for y in range(L.h) for x in range(L.w)
                                          if L.behaviour(x, y) == berry}
        for cy in range(L.h):
            for cx in range(L.w):
                if (cx, cy) in keep:
                    flat[oy + cy][ox + cx] = "floor"
                    for j in range(16):
                        kind[(oy + cy) * 16 + j][(ox + cx) * 16:(ox + cx + 1) * 16] = [GROUND] * 16
                    continue
                if (cx, cy) in props and all(432 <= (e & 0x3FF) < 462
                                             for e in A.entries(A.metatile(cx, cy))[:4]):
                    flat[oy + cy][ox + cx] = "water"
                    for j in range(16):
                        kind[(oy + cy) * 16 + j][(ox + cx) * 16:(ox + cx + 1) * 16] = [GROUND] * 16
                    continue
                # a face goes down behind a roof or a tree, not a signpost
                blocked[oy + cy][ox + cx] = L.blocked(cx, cy) and                     L.role_at(cx, cy) in ("tree", "wall", "prop")
                m = A.metatile(cx, cy)
                drawn_as = drawn_role(A, m)
                if drawn_as[1] and L.role_at(cx, cy) != "tree":
                    drawn_as = (None, False)    # under a roof: the house's, not a face
                side[oy + cy][ox + cx] = 1 if drawn_as[0] == "west" else -1 if drawn_as[0] == "east" else 0
                if drawn_as[0] == "face":
                    face_low.add((ox + cx, oy + cy))    # a south face, as drawn
                role = L.role_at(cx, cy)
                if L.behaviour(cx, cy) == WATERFALL:
                    # a waterfall is a face of water, falling a level
                    flat[oy + cy][ox + cx] = "fall"
                    for j in range(16):
                        kind[(oy + cy) * 16 + j][(ox + cx) * 16:(ox + cx + 1) * 16] = [FACE] * 16
                    continue
                tile_key = (L.secondary, m)     # a secondary tileset's ids are its own
                if role == "water" and tile_key not in _ROCKY_WATER:
                    px = list(A.cell_image(m).convert("RGB").getdata())
                    _ROCKY_WATER[tile_key] = sum(1 for c in px if c in ROCK_ALL) >= ROCKY_WATER * len(px)
                if drawn_as[0] != "face" and m not in FACE_SOUTH and ((role in FLAT_ROLES and (cx, cy) not in L.warps
                                             and not L.covers(m)
                                             and not (role == "water" and _ROCKY_WATER[tile_key])
                                             and m not in SEA_CAPS)
                                            or L.behaviour(cx, cy) in FLAT_BEHAVIOURS):
                    flat[oy + cy][ox + cx] = ("bridge" if L.behaviour(cx, cy) in FLAT_BEHAVIOURS else
                                              role if role in ("water", "signpost") else "floor")
                layers = (0, 1)
                if (cx, cy) in props:
                    layers = (0,)
                if drawn_as == ("face", True):
                    # a south face with a tree's crown drawn over it on the
                    # upper layer (Dewford's trees under Route 106's
                    # mountain): its lower layer is a face of the General
                    # tileset, tile for tile. It is that face; the crown
                    # only stands in front of it.
                    layers = (0,)
                stair = role == "stair" and not flat[oy + cy][ox + cx]
                if m in DIRT:
                    for j in range(16):
                        kind[(oy + cy) * 16 + j][(ox + cx) * 16:(ox + cx + 1) * 16] = [GROUND] * 16
                    continue
                # the same metatile reads the same: its rows, worked out once
                key = (A.primary, A.secondary, m, layers, stair, reading(A))
                rows = tile_kinds.get(key)
                if rows is None:
                    img = A.cell_image(m, layers=layers).load()
                    rows = tile_kinds[key] = [
                        [FREE if stair else TOP if c in ROCK_TOP else
                         FACE if c in ROCK_FACE else FLECK if c in ROCK_FLECK else
                         RIM if c in ROCK_RIM else GROUND
                         for c in (img[i, j][:3] for i in range(16))] for j in range(16)]
                for j in range(16):
                    kind[(oy + cy) * 16 + j][(ox + cx) * 16:(ox + cx + 1) * 16] = rows[j]
    # a bridge's ends, drawn over the player where they cross a rock's lip,
    # are the bridge
    grow = True
    while grow:
        grow = False
        for lid, (ox, oy) in members.items():
            L = layouts[lid]
            for cy in range(L.h):
                for cx in range(L.w):
                    if flat[oy + cy][ox + cx] or L.role_at(cx, cy) not in ("floor", "stair"):
                        continue
                    if any(0 <= cx + dx < L.w and 0 <= cy + dy < L.h
                           and flat[oy + cy + dy][ox + cx + dx] == "bridge"
                           for (dx, dy) in ((1, 0), (-1, 0), (0, 1), (0, -1))):
                        flat[oy + cy][ox + cx] = "bridge"
                        grow = True
    # a pier: walked planks drawn over the player (not the sand a rock's
    # foot is drawn over), a run of them out into the water
    pier = _PIER[name] = set()
    four = ((1, 0), (-1, 0), (0, 1), (0, -1))
    for lid, (ox, oy) in members.items():
        L, A = layouts[lid], _ART[lid]
        planks = {(cx, cy) for cy in range(L.h) for cx in range(L.w)
                  if not flat[oy + cy][ox + cx] and L.role_at(cx, cy) == "floor"
                  and not L.blocked(cx, cy) and L.behaviour(cx, cy) not in SANDS
                  and L.covers(A.metatile(cx, cy))}
        seen = set()
        for start in sorted(planks):
            if start in seen:
                continue
            run, stack = [], [start]
            seen.add(start)
            while stack:
                c = stack.pop()
                run.append(c)
                for (dx, dy) in four:
                    q = (c[0] + dx, c[1] + dy)
                    if q in planks and q not in seen:
                        seen.add(q)
                        stack.append(q)
            # it runs out into the water: water on three sides of it at least
            # (a tree's crown or a rock's top drawn over the shore has it on one)
            sides = {(dx, dy) for (x, y) in run for (dx, dy) in four
                     if 0 <= x + dx < L.w and 0 <= y + dy < L.h
                     and L.role_at(x + dx, y + dy) == "water"}
            if len(sides) >= 3:
                pier.update((ox + x, oy + y) for (x, y) in run)
    # a rock pixel is what most rock round it is: faces are speckled with the
    # tops' colours and the tops with the faces'
    voted = [row[:] for row in kind]
    R = MAJORITY
    # the votes are counted off running sums (top or rim, face) over the canvas
    st = [[0] * (W + 1) for _ in range(H + 1)]
    sf = [[0] * (W + 1) for _ in range(H + 1)]
    for y in range(H):
        row = kind[y]
        t = itertools.accumulate([k == TOP or k == RIM for k in row])
        f = itertools.accumulate([k == FACE for k in row])
        st[y + 1][1:] = map(operator.add, st[y][1:], t)
        sf[y + 1][1:] = map(operator.add, sf[y][1:], f)
    for y in range(H):
        y0, y1 = max(0, y - R), min(H, y + R + 1)
        row = kind[y]
        for x in range(W):
            if row[x] not in (TOP, FACE, FLECK):
                continue
            x0, x1 = max(0, x - R), min(W, x + R + 1)
            t = st[y1][x1] - st[y0][x1] - st[y1][x0] + st[y0][x0]
            f = sf[y1][x1] - sf[y0][x1] - sf[y1][x0] + sf[y0][x0]
            voted[y][x] = TOP if t > f else FACE
    return layouts, CW, CH, voted, blocked, side, flat, face_low


# What a tile of rock is, read off its drawing: a tile whose rock pixels
# are those of one of Route 116's - a south face, a band turned west or east
# - pixel for pixel, whatever is drawn round them (grass there, the sea or
# the sand elsewhere: Route 105's ridges, Route 106's beach), is that tile,
# and its rock takes that tile's shape. A tile with a crown or a roof drawn
# over it on the upper layer is read by its lower layer alone.
ROLE_REFERENCE = dict([(m, "face") for m in FACE_SOUTH if m < 0x200]
                      + [(m, "west") for m in SIDE_WEST] + [(m, "east") for m in SIDE_EAST])
ROLE_MATCH = 0.9    # share of the rock pixels of both that agree
_ROLES_DRAWN = {}


def _rock_pixels(art, m, layers=(0, 1)):
    img = art.cell_image(m, layers).load()
    return {(x, y): img[x, y][:3] for y in range(16) for x in range(16)
            if img[x, y][:3] in ROCK_ALL}


def drawn_role(art, m):
    """(role, lower layer only) of metatile m as drawn: role "face",
    "west", "east" or None (a corner, a top, ground)."""
    key = (art.primary, art.secondary, m, reading(art))
    if key in _ROLES_DRAWN:
        return _ROLES_DRAWN[key]
    out = (None, False)
    if m in FACE_SOUTH or m in SIDE_WEST or m in SIDE_EAST:
        # the reference tiles are what they are
        out = ("face" if m in FACE_SOUTH else "west" if m in SIDE_WEST else "east", False)
    elif art.primary == "gTileset_General":
        if "refs" not in _ROLES_DRAWN:
            _ROLES_DRAWN["refs"] = [(_rock_pixels(art, r), role) for r, role in ROLE_REFERENCE.items()]
        # read by its lower layer only under a cover that is not rock: a
        # crown or a roof, a quarter of the cell at least
        hi = art.cell_image(m).load()
        lo = art.cell_image(m, (0,)).load()
        cover = [(x, y) for y in range(16) for x in range(16) if hi[x, y] != lo[x, y]]
        upper = (len(cover) >= 64
                 and not any(hi[x, y][:3] in ROCK_ALL for (x, y) in cover))
        for layers, low in (((0, 1), False), ((0,), True)):
            if low and not upper:
                break
            a = _rock_pixels(art, m, layers)
            if not a:
                continue
            for b, role in _ROLES_DRAWN["refs"]:
                same = sum(1 for q in a if b.get(q) == a[q])
                if same >= ROLE_MATCH * len(set(a) | set(b)):
                    out = (role, low and m not in ROLE_REFERENCE)
                    break
            if out[0]:
                break
    _ROLES_DRAWN[key] = out
    return out


# Terraces the flood joined. Two terraces a face apart touch where the face
# ends - a band that tapers to nothing between a plateau and the ledge
# round it (Route 106's east end), a ring of grass whose bands end at the
# shore (Mossdeep) - and the flood runs round the end of the face: one
# region above and below it. A face drawn down a column with the same region
# at its top and its foot says the region is two; it is cut where it is
# narrowest between the face's tops and its feet.
# Where the terraces are split so, and their backs run on as below: the maps
# the player reaches first, looked at in the game one by one; the rest keep
# their old reading until they are.
WRAP_GROUPS = tuple(os.environ.get("VOXEL_WRAP_GROUPS", "route104,route105,route106").split(","))
WRAP_DROP = 6       # pixels of face down a column, at least
WRAP_COLUMNS = 3    # columns that say so, at least
WRAP_CUT = 24       # the neck is no wider than this, pixels (wider: one region after all)
WRAP_POCKET = 64    # a side smaller than this is a pocket among the face's pixels
WRAP_REACH = 64     # the cut is sought this far round the face, pixels
WRAP_DEEP = 5       # a rim's tops run this deep each side of it, pixels


def _wrapped(region, kind, side, south, W, H):
    """{region: [(high end, low end)]} of faces with the same region at both
    ends: down a column (a south face: its top and its foot), along a row (a
    band turned west or east: its high edge and its low one), and of rims
    with the same top both sides."""
    out = {}
    for x in range(W):
        y = 0
        while y < H:
            if kind[y][x] != FACE:
                y += 1
                continue
            y0 = y
            while y < H and kind[y][x] == FACE:
                y += 1
            if (y0 and y < H and y - y0 >= WRAP_DROP and region[y][x] >= 0
                    and region[y0 - 1][x] == region[y][x]
                    and sum((x // 16, j // 16) in south for j in range(y0, y)) * 2 > y - y0):
                # (a south face drawn as one: down a band a column crosses
                # its diagonal, and a band's tip is drawn in a tile of its own)
                out.setdefault(region[y][x], []).append(((x, y0 - 1), (x, y)))
    # a rim with top on both sides of it, down a column: the edge of the
    # terrace in front drawn over the one behind, which it stands above
    for x in range(W):
        y = 0
        while y < H:
            if kind[y][x] != RIM:
                y += 1
                continue
            y0 = y
            while y < H and kind[y][x] == RIM:
                y += 1
            # (top a few pixels deep each side: not the pocket inside a scallop)
            r = region[y][x] if y < H else -1
            if (r >= 0 and y0 >= WRAP_DEEP and y + WRAP_DEEP <= H
                    and all(kind[y0 - k][x] == TOP and region[y0 - k][x] == r for k in range(1, WRAP_DEEP + 1))
                    and all(kind[y + k][x] == TOP and region[y + k][x] == r for k in range(WRAP_DEEP))):
                out.setdefault(r, []).append(((x, y), (x, y0 - 1)))
    for y in range(H):
        row = kind[y]
        x = 0
        while x < W:
            if row[x] != FACE:
                x += 1
                continue
            x0 = x
            while x < W and row[x] == FACE:
                x += 1
            if (x0 and x < W and x - x0 >= WRAP_DROP and region[y][x] >= 0
                    and region[y][x0 - 1] == region[y][x]
                    and side[y // 16][x0 // 16] and side[y // 16][(x - 1) // 16]):
                # the high edge first: a band turned west (+1) rises to the east
                hi, lo = ((x, y), (x0 - 1, y)) if side[y // 16][x0 // 16] > 0 else ((x0 - 1, y), (x, y))
                out.setdefault(region[y][x], []).append((hi, lo))
    return out


def _clusters(pairs, near=2):
    """The pairs grouped by where they are: cells within `near` of each other."""
    groups = []
    for pr in sorted(pairs):
        c = (pr[0][0] // 16, pr[0][1] // 16)
        hits = [g for g in groups if any(max(abs(c[0] - d[0]), abs(c[1] - d[1])) <= near for d in g[0])]
        merged = (set([c]), [pr])
        for g in hits:
            merged[0].update(g[0])
            merged[1].extend(g[1])
            groups.remove(g)
        groups.append(merged)
    return [g[1] for g in groups]


def _rim_gaps(kind, W, H, reach=4):
    """Pixels of top or ground between two pixels of rim close by in their
    row or column: the gaps of a scalloped rim, the edge of the terrace in
    front drawn over the one behind. A cut between two terraces runs along it."""
    out = set()
    for y in range(H):
        row = kind[y]
        for x in range(W):
            if row[x] not in (TOP, GROUND):
                continue
            for (dx, dy) in ((1, 0), (0, 1), (1, 1), (1, -1)):
                if (any(0 <= x - k * dx < W and 0 <= y - k * dy < H and kind[y - k * dy][x - k * dx] == RIM
                        for k in range(1, reach + 1))
                        and any(0 <= x + k * dx < W and 0 <= y + k * dy < H
                                and kind[y + k * dy][x + k * dx] == RIM for k in range(1, reach + 1))):
                    out.add((x, y))
                    break
    return out


def _neck(region, r, tops, feet, gaps, W, H, limit):
    """(the pixels of region r on the tops' side of the narrowest cut between
    tops and feet - free through the gaps of a rim -, those on the feet's
    side (only the first WRAP_POCKET of them if the tops' side is a pocket),
    the cut's width), or None if every cut is
    wider than `limit`. The gaps themselves are on neither side. The cut is
    sought near the face (a whole sea is too much to search) and then checked
    on the whole region: no way round it."""
    xs = [p[0] for p in tops | feet]
    ys = [p[1] for p in tops | feet]
    box = (min(xs) - WRAP_REACH, min(ys) - WRAP_REACH, max(xs) + WRAP_REACH, max(ys) + WRAP_REACH)

    def inside(q):
        return box[0] <= q[0] <= box[2] and box[1] <= q[1] <= box[3]

    def steps(p):
        x, y = p
        for q in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if (0 <= q[0] < W and 0 <= q[1] < H and region[q[1]][q[0]] == r
                    and q not in gaps and p not in gaps):
                yield q
    flow = {}

    def reach():
        prev = dict.fromkeys(tops)
        queue = collections.deque(tops)
        while queue:
            p = queue.popleft()
            if p in feet:
                return prev, p
            for q in steps(p):
                if q not in prev and inside(q) and flow.get((p, q), 0) < 1:
                    prev[q] = p
                    queue.append(q)
        return prev, None

    def spread(start, allowed, cap=None):
        seen = set(start)
        queue = collections.deque(start)
        while queue:
            p = queue.popleft()
            for q in steps(p):
                if q not in seen and allowed(p, q):
                    if q in feet and start is tops:
                        return None
                    seen.add(q)
                    queue.append(q)
                    if cap and len(seen) >= cap:
                        return seen
        return seen

    for width in range(limit + 1):
        prev, end = reach()
        if end is not None:
            while prev[end] is not None:
                p = prev[end]
                flow[(p, end)] = flow.get((p, end), 0) + 1
                flow[(end, p)] = flow.get((end, p), 0) - 1
                end = p
            continue
        near = set(prev)
        # the whole region: across the cut only where it runs out of the box
        side = spread(tops, lambda p, q: q in near or not inside(q) or p not in near)
        if side is None:
            return None
        far = spread(feet, lambda p, q: q not in side, WRAP_POCKET if len(side) < WRAP_POCKET else None)
        return side, far, width
    return None


def split_wrapped(region, sizes, kind, side, south, W, H):
    """Cuts the regions faces say are two; returns the regions it cut, each
    a terrace however thin: the drawing gave it a level of its own."""
    made = set()
    if os.environ.get("NO_WRAP"):
        return made
    tried = set()
    gaps = None
    while True:
        todo = []
        for r, pairs in _wrapped(region, kind, side, south, W, H).items():
            for group in _clusters(pairs):
                key = (r, frozenset(group))
                if len(group) >= WRAP_COLUMNS and key not in tried:
                    todo.append((len(group), r, group, key))
        if not todo:
            return made
        if gaps is None:
            gaps = _rim_gaps(kind, W, H)
        done = False
        for _, r, group, key in sorted(todo, reverse=True):
            tried.add(key)
            tops, feet = {p for p, q in group}, {q for p, q in group}
            # an end both above one face and below another (a band's tip
            # in a tile not drawn as a band) says nothing
            tops, feet = tops - feet, feet - tops
            # a cut round a pocket - a few pixels shut in among the face's
            # own - is no terrace: those ends are let go and it is sought again
            cut = width = None
            while tops and feet:
                got = _neck(region, r, tops, feet, gaps, W, H, WRAP_CUT)
                if got is None:
                    break
                cut, far, width = got
                if not width:
                    cut = None
                    break       # a rim's gaps alone part them: nothing to cut
                if len(cut) < WRAP_POCKET:
                    tops -= cut
                elif len(far) < WRAP_POCKET:
                    feet -= far     # a pocket among the face's pixels
                else:
                    break
                cut = None
            if os.environ.get("WRAP_LOG"):
                print("wrap region %d (%d px) at %s: %d ends, %s" % (
                    r, sizes[r], sorted({(x // 16, y // 16) for (x, y) in tops}), len(group),
                    "cut %d wide, %d px off" % (width, len(far)) if cut else "no neck"))
            if cut is None:
                continue
            # the feet's side is the new region: what the gaps shut in stays
            # with the tops' side, the terrace it is drawn in
            n = len(sizes)
            for (x, y) in far:
                region[y][x] = n
            sizes[r] -= len(far)
            sizes.append(len(far))
            made.update((r, n))
            done = True
            break
        if not done:
            return made


def drawn_prepare(name):
    """What a group's drawing says, the pixels read and let go: its regions
    (terraces and ground), the drops between them, and per cell what it is
    drawn as. The levels come from world_levels, which weighs every group
    and every map's seams together."""
    if name in _PREP:
        return _PREP[name]
    cache = os.environ.get("VOXEL_RELIEF_CACHE")    # a developer's, between runs
    if cache:
        import hashlib, inspect, pickle
        here = os.path.dirname(os.path.abspath(__file__))
        # what the reading depends on: the constants, this function, the canvas, the cells
        text = open(os.path.join(here, "gen_voxel_relief.py"), encoding="utf-8").read()
        key = hashlib.sha1((text[:text.index("def find_drawn")]     # the constants
                            + inspect.getsource(drawn_prepare) + inspect.getsource(drawn_canvas)
                            + inspect.getsource(drawn_role) + repr(sorted(ROLE_REFERENCE.items()))
                            + ((inspect.getsource(split_wrapped) + inspect.getsource(_neck)
                                + inspect.getsource(_wrapped) + inspect.getsource(_rim_gaps)
                                + inspect.getsource(_clusters)
                                + repr((WRAP_DROP, WRAP_COLUMNS, WRAP_CUT, WRAP_POCKET, WRAP_REACH, WRAP_DEEP))
                                + os.environ.get("NO_WRAP", "")) if name in WRAP_GROUPS else "")
                            + open(os.path.join(here, "voxel_cells.py"), encoding="utf-8").read()
                            + open(os.path.join(here, "voxel_props.py"), encoding="utf-8").read()
                            ).encode("utf-8")).hexdigest()[:12]
        path = os.path.join(cache, "prep_%s_%s.pickle" % (name, key))
        if os.path.exists(path):
            _PREP[name] = pickle.load(open(path, "rb"))
            for lid in DRAWN[name]:
                if lid not in _ART:
                    layout_art(lid)
            return _PREP[name]
    members = DRAWN[name]
    layouts, CW, CH, kind, blocked, side, flat, face_low = drawn_canvas(name)

    def metatile_at(cx, cy):
        for lid, (ox, oy) in members.items():
            L = layouts[lid]
            if ox <= cx < ox + L.w and oy <= cy < oy + L.h:
                return _ART[lid].metatile(cx - ox, cy - oy)
        return None
    W, H = CW * 16, CH * 16

    # regions
    region = [[-1] * W for _ in range(H)]
    sizes = []
    pier = _PIER[name]

    def apart(a, b, i, j):
        """A pier's planks join only the land they are walked from - not the
        water, the rock or the posts under them - across a cell edge."""
        c, d = (a // 16, b // 16), (i // 16, j // 16)
        if c == d or (c in pier) == (d in pier):
            return False
        other = d if c in pier else c
        return flat[other[1]][other[0]] not in ("floor", "bridge")
    def band_seam(a, b, i, j):
        """The top drawn on a band is its terrace's edge in its own row: it
        does not run on north or south into a cell that is not a band too.
        A ridge built of stretches with one band a side and stretches with
        two (Route 105's) is terraces of their own, not one top that the
        drops down its two flanks disagree about."""
        if b // 16 == j // 16:
            return False
        sa, sb = side[b // 16][a // 16], side[j // 16][i // 16]
        if (sa != 0) == (sb != 0):
            return False
        # only a ridge at sea: the band falls to water (inland, Route 116's
        # terraces meet their bands that way and are one terrace)
        cx, cy = (a // 16, b // 16) if sa else (i // 16, j // 16)
        low = cx - side[cy][cx]
        while 0 <= low < CW and side[cy][low] == side[cy][cx]:
            low -= side[cy][cx]
        return 0 <= low < CW and flat[cy][low] == "water"
    # whether a region runs on across the edge between two cells: apart and
    # band_seam read only the cells (both are False within one cell)
    crossing = {}

    def crosses(a, b, i, j):
        key = (a >> 4, b >> 4, i >> 4, j >> 4)
        v = crossing.get(key)
        if v is None:
            v = crossing[key] = not apart(a, b, i, j) and not band_seam(a, b, i, j)
        return v
    for y in range(H):
        krow, rrow = kind[y], region[y]
        for x in range(W):
            k = krow[x]
            if (k != GROUND and k != TOP) or rrow[x] >= 0:
                continue
            n = len(sizes)
            stack, count = [(x, y)], 0
            rrow[x] = n
            while stack:
                a, b = stack.pop()
                count += 1
                ra, ka = region[b], kind[b]
                # west and east, in the row
                if a + 1 < W and ra[a + 1] < 0 and ka[a + 1] == k and ((a + 1) & 15 or crosses(a, b, a + 1, b)):
                    ra[a + 1] = n
                    stack.append((a + 1, b))
                if a > 0 and ra[a - 1] < 0 and ka[a - 1] == k and (a & 15 or crosses(a, b, a - 1, b)):
                    ra[a - 1] = n
                    stack.append((a - 1, b))
                # south and north
                if b + 1 < H and region[b + 1][a] < 0 and kind[b + 1][a] == k and ((b + 1) & 15 or crosses(a, b, a, b + 1)):
                    region[b + 1][a] = n
                    stack.append((a, b + 1))
                if b > 0 and region[b - 1][a] < 0 and kind[b - 1][a] == k and (b & 15 or crosses(a, b, a, b - 1)):
                    region[b - 1][a] = n
                    stack.append((a, b - 1))
            sizes.append(count)
    cut_apart = set() if name not in WRAP_GROUPS else split_wrapped(region, sizes, kind, side,
                  {(cx, cy) for cy in range(CH) for cx in range(CW)
                              if (cx, cy) in face_low or metatile_at(cx, cy) in FACE_SOUTH}, W, H)
    # a strip of top a few pixels thick - the light lip between two faces
    # stacked one on the other - is part of the face, not a terrace
    columns = [set() for _ in sizes]
    for y in range(H):
        row = region[y]
        for x in range(W):
            if row[x] >= 0:
                columns[row[x]].add(x)
    big = [s >= REGION_MIN and (s >= THIN * len(columns[n]) or n in cut_apart) for n, s in enumerate(sizes)]

    # what the columns say: (upper, lower) -> drops
    runs = {}
    for x in range(W):
        y = 0
        while y < H:
            k = kind[y][x]
            if k not in (FACE, RIM):
                y += 1
                continue
            y0 = y
            while y < H and kind[y][x] == k:
                y += 1
            if y0 == 0 or y == H:
                continue
            a, b = region[y0 - 1][x], region[y][x]
            if a < 0 or b < 0 or a == b or not (big[a] and big[b]):
                continue
            if kind[y][x] == GROUND and blocked[y // 16][x // 16]:
                continue  # its foot is behind a roof or a tree: not all of it shows
            if k == FACE and sum(side[j // 16][x // 16] != 0 for j in range(y0, y)) * 2 > y - y0:
                continue  # down a west or east face, not across a south one
            drop = y - y0 if k == FACE else -RIM_RISE
            runs.setdefault((a, b), []).append(drop)

    # across the rows: the west and east faces
    for y in range(H):
        x = 0
        row = kind[y]
        while x < W:
            if row[x] != FACE:
                x += 1
                continue
            x0 = x
            while x < W and row[x] == FACE:
                x += 1
            if x0 == 0 or x == W:
                continue
            turn = sum(side[y // 16][i // 16] for i in range(x0, x))
            if abs(turn) * 2 <= x - x0:
                continue
            left, right = region[y][x0 - 1], region[y][x]
            if left < 0 or right < 0 or left == right or not (big[left] and big[right]):
                continue
            if turn > 0:
                runs.setdefault((right, left), []).append(SIDE_RISE)
            else:
                runs.setdefault((left, right), []).append(SIDE_RISE)

    # a flight of two bands or more down a row, all turned the same way, is
    # as many levels between the terraces at its two ends - a gorge's walls
    # count the river down, where no single face joins the two
    for y in range(H):
        cy = y // 16
        cx = 0
        while cx < CW:
            s_ = side[cy][cx]
            if not s_:
                cx += 1
                continue
            x0 = cx
            while cx < CW and side[cy][cx] == s_:
                cx += 1
            if cx - x0 < 2 or x0 == 0 or cx == CW:
                continue
            west, east = region[y][x0 * 16 - 8], region[y][cx * 16 + 8]
            if west < 0 or east < 0 or west == east or not (big[west] and big[east]):
                continue
            rise = SIDE_RISE * (cx - x0)
            if s_ > 0:
                runs.setdefault((east, west), []).append(rise)
            else:
                runs.setdefault((west, east), []).append(rise)

    # and so is a column of two south faces or more stacked one on the other
    # (a waterfall among them): the lip between two faces is a strip of top
    # too thin to be a terrace, so no single face joins the two ends
    def south_face(cx, cy):
        return (not side[cy][cx] and flat[cy][cx] in (None, "fall")
                and (metatile_at(cx, cy) in FACE_SOUTH or (cx, cy) in face_low
                     or flat[cy][cx] == "fall"))
    for x in range(8, W, 16):
        cx = x // 16
        cy = 0
        while cy < CH:
            if not south_face(cx, cy):
                cy += 1
                continue
            y0 = cy
            while cy < CH and south_face(cx, cy):
                cy += 1
            if cy - y0 < 2 or y0 == 0 or cy == CH:
                continue
            for i in range(x - 6, x + 7, 3):
                up, down = region[y0 * 16 - 4][i], region[cy * 16 + 4][i]
                if up >= 0 and down >= 0 and up != down and big[up] and big[down]:
                    runs.setdefault((up, down), []).append(LEVEL * (cy - y0))

    # the ground the player walks on without a flight of stairs or a ledge
    # to jump is one level: a bridge's two ends, the ground either side of a
    # gap in a fence. The cartridge says so; the drawing only draws it.
    # So is a body of water, a waterfall apart - its shaded edge along a
    # shore is not a pool of its own -, and the land it laps: a shore is
    # never a step. A bridge goes over the water, not down to it.
    WALKED = 1000
    walk_region = {}
    ties = set()        # never given up: the cartridge says they are one level
    for cy in range(CH):
        for cx in range(CW):
            if flat[cy][cx] not in ("floor", "bridge", "water"):
                continue
            got = collections.Counter(region[cy * 16 + j][cx * 16 + i] for j in range(0, 16, 2)
                                      for i in range(0, 16, 2))
            got = [(n, r) for r, n in got.items() if r >= 0 and big[r]]
            walk_region[(cx, cy)] = max(got)[1] if got else None
    seen_walk = set()
    for start in walk_region:
        if start in seen_walk:
            continue
        comp, stack = [], [start]
        seen_walk.add(start)
        while stack:
            c = stack.pop()
            comp.append(c)
            for d in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                q = (c[0] + d[0], c[1] + d[1])
                if (q in walk_region and q not in seen_walk
                        and {flat[q[1]][q[0]], flat[c[1]][c[0]]} != {"bridge", "water"}):
                    seen_walk.add(q)
                    stack.append(q)
        found = sorted({walk_region[c] for c in comp if walk_region[c] is not None})
        for r in found[1:]:
            runs.setdefault((found[0], r), []).extend([0] * WALKED)
            ties.add((found[0], r))

    # per cell: how much of it is drawn, as top, as ground, and which
    # terraces it is drawn in
    stats = [[None] * CW for _ in range(CH)]
    free_mid = [[False] * CW for _ in range(CH)]
    for cy in range(CH):
        for cx in range(CW):
            counts = {}
            n = top = ground = 0
            for j in range(16):
                krow, rrow = kind[cy * 16 + j], region[cy * 16 + j]
                for i in range(cx * 16, cx * 16 + 16):
                    k = krow[i]
                    if k == VOID:
                        continue
                    n += 1
                    top += k in (TOP, RIM)
                    ground += k == GROUND
                    r = rrow[i]
                    if r >= 0 and big[r]:
                        counts[r] = counts.get(r, 0) + 1
            if n:
                stats[cy][cx] = (n, top, ground, counts)
            free_mid[cy][cx] = kind[cy * 16 + 8][cx * 16 + 8] == FREE
    # what stands at each map's edges, cell by cell: the terrace or ground
    # drawn there, where a seam joins it to the next map
    edges = {}
    for lid, (ox, oy) in members.items():
        L = layouts[lid]
        for edge in ("up", "down", "left", "right"):
            if edge in ("up", "down"):
                y = oy * 16 + 1 if edge == "up" else (oy + L.h) * 16 - 2
                px = [((ox + i) * 16 + 8, y) for i in range(L.w)]
            else:
                x = ox * 16 + 1 if edge == "left" else (ox + L.w) * 16 - 2
                px = [(x, (oy + j) * 16 + 8) for j in range(L.h)]
            edges[(lid, edge)] = [region[y][x] if region[y][x] >= 0 and big[region[y][x]]
                                  and kind[y][x] in (GROUND, TOP) else None for (x, y) in px]
    prep = dict(members=members, layouts=layouts, CW=CW, CH=CH, side=side, flat=flat,
                sizes=sizes, big=big, runs=runs, ties=ties, stats=stats, free_mid=free_mid,
                face_low=face_low, region=region,
                edges=edges)
    _PREP[name] = prep
    if cache:
        os.makedirs(cache, exist_ok=True)
        pickle.dump(prep, open(path, "wb"))
    return prep


def solve_drawn(name):
    """{layout id: lattice} of a group of maps drawn as one."""
    if name in _DRAWN:
        return _DRAWN[name]
    prep = drawn_prepare(name)
    members, layouts, CW, CH = prep["members"], prep["layouts"], prep["CW"], prep["CH"]
    side, flat, sizes, big = prep["side"], prep["flat"], prep["sizes"], prep["big"]
    stats, free_mid = prep["stats"], prep["free_mid"]
    face_low = prep["face_low"]
    level = world_levels()["regions"][name]

    def metatile_at(cx, cy):
        for lid, (ox, oy) in members.items():
            L = layouts[lid]
            if ox <= cx < ox + L.w and oy <= cy < oy + L.h:
                return _ART[lid].metatile(cx - ox, cy - oy)
        return None

    # The shape, cell by cell. The mountain is drawn in whole cells: a cell
    # is the top of a terrace, ground, or the rock between them (a south
    # face, a west or east band, a corner, a flight of stairs). Tops and
    # ground are footprints, each at its region's level; the rock between
    # is a ramp from the footprint above it, planes that start at the
    # footprint's cell edge - so every edge is a straight line, parallel to
    # the next - and turn an outer corner on an ellipse. The drawn rim's
    # wobble stays in the texture, never in the shape.
    cell = [[None] * CW for _ in range(CH)]     # level of a footprint cell

    def ridge_end(cx, cy):
        """A boulder's halves drawn as the ends of a ridge: its top half
        over a band (the ridge's north end), its bottom half under one (its
        south end). Route 105 builds a ridge on its ridge so, the boulder's
        two halves with bands between them; a boulder on its own is a
        boulder, all top."""
        return any(0 <= cy + dy < CH and side[cy + dy][cx] for dy in (-1, 1))

    void = [[True] * CW for _ in range(CH)]
    thin = set()
    level_from_neighbours = set()   # flat cells with no terrace in them
    soil = [[False] * CW for _ in range(CH)]    # a ground footprint: never lifted
    for cy in range(CH):
        for cx in range(CW):
            if stats[cy][cx] is None:
                continue
            n, top, ground, counts = stats[cy][cx]
            void[cy][cx] = False
            boulder = metatile_at(cx, cy) in BOULDER
            if metatile_at(cx, cy) in SEA_CAPS:
                continue    # rock, whatever share of it is sea
            if boulder and ridge_end(cx, cy):
                continue    # the end of a ridge on a ridge, rock
            if flat[cy][cx] == "bridge" or (flat[cy][cx] == "floor"
                                            and not (top >= FOOTPRINT * n or ground >= FOOTPRINT * n)):
                level_from_neighbours.add((cx, cy))
                soil[cy][cx] = True
            elif flat[cy][cx] in ("water", "signpost") and not (top >= FOOTPRINT * n or ground >= FOOTPRINT * n):
                if counts:
                    cell[cy][cx] = level[max(counts, key=counts.get)]
                else:
                    level_from_neighbours.add((cx, cy))
                soil[cy][cx] = True
            elif boulder or top >= FOOTPRINT * n or ground >= FOOTPRINT * n:
                if counts:
                    cell[cy][cx] = level[max(counts, key=counts.get)]
                    soil[cy][cx] = not boulder and top < FOOTPRINT * n
                elif top >= FOOTPRINT * n:
                    thin.add((cx, cy))
    # The ledges between bands are top too, a cell wide: too thin for the
    # regions, their level is counted across the bands from a known one -
    # a band turned west is a level down going west, one turned east a level
    # down going east - or taken from a ledge above or below them.
    while thin:
        found = {}
        for (cx, cy) in thin:
            for dx in (-1, 1):
                x, n = cx + dx, 0
                while 0 <= x < CW and side[cy][x] and cell[cy][x] is None:
                    n += side[cy][x] * dx
                    x += dx
                if x != cx + dx and 0 <= x < CW and cell[cy][x] is not None:
                    found[(cx, cy)] = cell[cy][x] - SIDE_RISE * n
                    break
            else:
                for dy in (-1, 1):
                    y = cy + dy
                    if 0 <= y < CH and cell[y][cx] is not None and (cx, y) not in found:
                        found[(cx, cy)] = cell[y][cx]
                        break
        if not found:
            break
        for (cx, cy), v in found.items():
            cell[cy][cx] = v
            thin.discard((cx, cy))
    # a bridge, and a flat cell drawn all in rock colours, is at the level of
    # the ground it joins - its neighbours' most common level, the water
    # under a bridge left out -, taken along the bridge from its ends
    while level_from_neighbours:
        found = {}
        for (cx, cy) in level_from_neighbours:
            got = collections.Counter(cell[y][x] for (x, y) in ((cx, cy - 1), (cx, cy + 1), (cx - 1, cy), (cx + 1, cy))
                                      if 0 <= x < CW and 0 <= y < CH and cell[y][x] is not None
                                      and flat[y][x] != "water" and (x, y) not in level_from_neighbours)
            if got:
                found[(cx, cy)] = got.most_common(1)[0][0]
        if not found:
            break
        for (cx, cy), v in found.items():
            cell[cy][cx] = v
            level_from_neighbours.discard((cx, cy))

    # every body of water lies at one level: the level most of it is at. It
    # runs on under a bridge.
    seen_water = set()
    for cy0 in range(CH):
        for cx0 in range(CW):
            if flat[cy0][cx0] != "water" or (cx0, cy0) in seen_water:
                continue
            body, stack = [], [(cx0, cy0)]
            seen_water.add((cx0, cy0))
            while stack:
                c = stack.pop()
                body.append(c)
                for d in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    q = (c[0] + d[0], c[1] + d[1])
                    if (0 <= q[0] < CW and 0 <= q[1] < CH and q not in seen_water
                            and flat[q[1]][q[0]] in ("water", "bridge")):
                        seen_water.add(q)
                        stack.append(q)
            body = [(x, y) for (x, y) in body if flat[y][x] == "water"]
            got = collections.Counter(cell[y][x] for (x, y) in body if cell[y][x] is not None)
            if got:
                lv = got.most_common(1)[0][0]
                for (x, y) in body:
                    cell[y][x] = lv
                    soil[y][x] = True
                # a strip of shore walked to from nowhere else, down at the
                # water between two walls, is at the water's level
                for (x, y) in body:
                    for d in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                        q = (x + d[0], y + d[1])
                        if not (0 <= q[0] < CW and 0 <= q[1] < CH) or flat[q[1]][q[0]] != "floor":
                            continue
                        strip, stack = {q}, [q]
                        while stack and len(strip) <= SHORE_STRIP:
                            c = stack.pop()
                            for e in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                                r = (c[0] + e[0], c[1] + e[1])
                                if (0 <= r[0] < CW and 0 <= r[1] < CH and r not in strip
                                        and flat[r[1]][r[0]] == "floor"):
                                    strip.add(r)
                                    stack.append(r)
                        if len(strip) <= SHORE_STRIP:
                            for (sx, sy) in strip:
                                cell[sy][sx] = lv
                                soil[sy][sx] = True

    # A top drawn between bands in its row is a level over the ground at its
    # foot for every band down to it, on both flanks: a ridge built on a
    # ridge (Route 105's), two bands a side over the sea, has its top two
    # levels up, whatever terrace its pixels run on into through the bands'
    # top-coloured halves. Only a ridge's top, a cell or two wide, and only
    # where both flanks say the same: a terrace is its region's.
    def counted(x, cy, dx):
        """The level the bands from x on count up to, None, or "open": the
        flank ends in a cap or a corner, the top of a band starting below
        it, and does not say."""
        n = 0
        while 0 <= x < CW and side[cy][x] == -dx and cell[cy][x] is None:
            n += 1
            x += dx
        if n and 0 <= x < CW and cell[cy][x] is not None:
            return cell[cy][x] + LEVEL * n
        if rock_end(x, cy):
            return "open"
        return None
    def rock_end(x, cy):
        return (0 <= x < CW and cell[cy][x] is None and not side[cy][x] and not void[cy][x]
                and 0 <= cy + 1 < CH and side[cy + 1][x] != 0)
    # from the south up: a capped end takes the top it runs on into
    for cy in range(CH - 1, -1, -1):
        cx = 0
        while cx < CW:
            if cell[cy][cx] is None or soil[cy][cx] or side[cy][cx]:
                cx += 1
                continue
            x0 = cx
            while cx < CW and cell[cy][cx] is not None and not soil[cy][cx] and not side[cy][cx]:
                cx += 1
            west, east = counted(x0 - 1, cy, -1), counted(cx, cy, 1)
            # a flank that ends in a cap or a corner does not say, and the
            # other flank does
            if west == east == "open":
                # capped on both flanks: the north end of a top, as high as
                # the top it runs on into
                below = [cell[cy + 1][x] for x in range(x0, cx) if cy + 1 < CH]
                west = east = below[0] if below and None not in below and len(set(below)) == 1 else None
            if west == "open":
                west = east
            if east == "open":
                east = west
            if west is not None and west == east and cx - x0 <= RIDGE_TOP:
                for x in range(x0, cx):
                    cell[cy][x] = west

    # The rock, tile by tile. Every cell of rock is one level of the drawing,
    # and it hangs from the cell it is drawn below: a south face (or a
    # flight of stairs) from the cell north of it, a band from the cell on
    # its high side, a corner from whichever of its eight neighbours stands
    # highest. Its top is that neighbour's level - the terrace's own, or one
    # level under the rock it hangs from - so rings of rock round a terrace
    # step down a level each, as drawn. Inside the cell the rock falls one
    # level with the distance from what it hangs from: a face straight down
    # its column, a band straight across its row, a corner round a quarter
    # circle of its own. A tile repeated is the same slope all along, and
    # the grass is never bent.
    _CELLS[name] = cell

    def tile(cx, cy):
        m = metatile_at(cx, cy)
        if side[cy][cx]:
            return "band"
        if (m in FACE_SOUTH or free_mid[cy][cx] or flat[cy][cx] == "fall"
                or (cx, cy) in face_low):
            return "face"
        return "corner"

    # A ridge drawn with no top between its bands - two bands back to back,
    # the west one rising east to the east one rising west - is a crest: its
    # top is the line between them, at the level of the ridge it runs on
    # from down the column. Each band still falls away from that line.
    crests = {(cx, cy) for cy in range(CH) for cx in range(CW)
              if side[cy][cx] and cell[cy][cx] is None and not void[cy][cx]
              and 0 <= cx + side[cy][cx] < CW
              and side[cy][cx + side[cy][cx]] == -side[cy][cx]
              and cell[cy][cx + side[cy][cx]] is None}

    def hangs_from(cx, cy):
        t = tile(cx, cy)
        if t == "face":
            return [(cx, cy - 1)]
        if t == "band":
            if (cx, cy) in crests:
                return [(cx, cy - 1)]
            return [(cx + side[cy][cx], cy)]
        return [(cx + dx, cy + dy) for dy in (-1, 0, 1) for dx in (-1, 0, 1) if dx or dy]

    rock = [(cx, cy) for cy in range(CH) for cx in range(CW)
            if cell[cy][cx] is None and not void[cy][cx]]
    top = {}

    def height_of(x, y, asker=None):
        """What a neighbour offers to hang from: a terrace's level, or one
        level under a cell of rock - but a crest offers its own level, and
        so does the band a crest runs on from, to the crest under it."""
        if not (0 <= x < CW and 0 <= y < CH) or void[y][x]:
            return None
        if cell[y][x] is not None:
            return cell[y][x]
        if (x, y) not in top:
            return None
        if (x, y) in crests and asker != (x - side[y][x], y):
            return top[(x, y)]     # but to the band below it on its flank, a level down
        if asker in crests and asker == (x, y + 1) and side[y][x]:
            return top[(x, y)]
        return top[(x, y)] - LEVEL

    # Nor does rock go down past the terrace it falls to: where the drawing
    # has more rings than the terraces' levels allow (both ends held at a
    # seam's level), the rings left over lie flat on it, not dug into a pit.
    def landing(cx, cy):
        t_ = tile(cx, cy)
        if t_ == "corner":
            got = [cell[y][x] for (x, y) in hangs_from(cx, cy)
                   if 0 <= x < CW and 0 <= y < CH and cell[y][x] is not None]
            return min(got) if got else None
        dx, dy = (0, 1) if t_ == "face" else (-side[cy][cx], 0)
        x, y = cx + dx, cy + dy
        while 0 <= x < CW and 0 <= y < CH and not void[y][x]:
            if cell[y][x] is not None:
                return cell[y][x]
            x, y = x + dx, y + dy
        return None
    floor_of = {c: landing(*c) for c in rock}

    # A crest stands a level over its foot for every band down to it, on
    # either flank: two bands a side (Route 105's ridge on its ridge) are a
    # crest two levels over the sea. Its two halves are one line.
    for (cx, cy) in sorted(crests):
        s_, x, n = side[cy][cx], cx, 0
        while 0 <= x < CW and side[cy][x] == s_ and cell[cy][x] is None:
            n += 1
            x -= s_
        if 0 <= x < CW and cell[cy][x] is not None:
            top[(cx, cy)] = cell[cy][x] + LEVEL * n
    for (cx, cy) in sorted(crests):
        o = (cx + side[cy][cx], cy)
        if (cx, cy) in top and o in top:
            top[(cx, cy)] = top[o] = max(top[(cx, cy)], top[o])
        elif o in top and o in crests:
            top[(cx, cy)] = top[o]      # its own flank ends in a corner: the other one says

    changed = True
    while changed:
        changed = False
        for (cx, cy) in rock:
            got = [v for v in (height_of(x, y, (cx, cy)) for (x, y) in hangs_from(cx, cy))
                   if v is not None]
            if (side[cy][cx] and (cx, cy) not in crests and 0 <= cx + side[cy][cx] < CW
                    and cell[cy][cx + side[cy][cx]] is None
                    and tile(cx + side[cy][cx], cy) == "corner"):
                # a band runs on down its column as one slope: the band of
                # the same side above or below it stands where it does (it
                # hangs from an inner corner's high edge, not a level under)
                got += [top[(cx, y)] for y in (cy - 1, cy + 1)
                        if 0 <= y < CH and side[y][cx] == side[cy][cx] and (cx, y) in top
                        and (cx, y) not in crests and cell[y][cx] is None]
            if got:
                v = max(got)
                if floor_of[(cx, cy)] is not None:
                    v = max(v, floor_of[(cx, cy)])
                if top.get((cx, cy)) is None or v > top[(cx, cy)]:
                    top[(cx, cy)] = v
                    changed = True

    _ROCK[name] = (dict(top), {c: tile(*c) for c in rock}, soil, metatile_at)
    _DRAWN_SIDE[name] = side

    LW, LH = CW * PER_CELL + 1, CH * PER_CELL + 1
    h = [[None] * LW for _ in range(LH)]
    fixed = [[False] * LW for _ in range(LH)]
    for cy in range(CH):
        for cx in range(CW):
            lv = cell[cy][cx]
            if lv is None:
                continue
            for j in range(cy * PER_CELL, (cy + 1) * PER_CELL + 1):
                for i in range(cx * PER_CELL, (cx + 1) * PER_CELL + 1):
                    if soil[cy][cx]:
                        if not fixed[j][i] or lv > h[j][i]:
                            h[j][i] = lv
                        fixed[j][i] = True
                    elif not fixed[j][i] and (h[j][i] is None or lv > h[j][i]):
                        h[j][i] = lv
    # Each cell of rock falls from what it hangs from, and it hangs from the
    # edge it shares with it as that edge is - not from the neighbour's level:
    # a face from the edge north of it, a band from its high side, a corner
    # from every point round it. A point of it is as high as the highest of
    # those edge points less its distance from it, a level every tile: a face
    # under a flat top is vertical, a band a 45-degree slope, a corner under
    # a terrace's corner a quarter cone, and the end of a band - a band's
    # profile on its north edge - the band's end wall, rounded as the corners
    # of the terraces are. The edges are the neighbours' own, so the rock is
    # solved until nothing changes.
    shapes = {}
    for (cx, cy) in rock:
        hi = top.get((cx, cy))
        if hi is None:
            continue
        rects = [(x, y) for (x, y) in hangs_from(cx, cy) if height_of(x, y, (cx, cy)) == hi]
        flat_on = not rects
        if not rects:
            # laid flat on the terrace it falls to
            rects = [(x, y) for (x, y) in hangs_from(cx, cy) if height_of(x, y, (cx, cy)) is not None]
        if (cx, cy) in crests:
            rects = [(cx + side[cy][cx], cy)]   # it falls away from the crest line
        # never below the land it falls to: the last ring onto the grass is
        # flat. Only the downhill side counts - a face's south, a band's low
        # side, a corner's neighbours other than what holds it up - never the
        # terrace it hangs from or one beside it.
        t_ = tile(cx, cy)
        if t_ == "face":
            below = [(cx, cy + 1)]
        elif t_ == "band":
            below = [(cx - side[cy][cx], cy)]
        else:
            below = [(x, y) for (x, y) in hangs_from(cx, cy) if (x, y) not in rects]
        land = [cell[y][x] for (x, y) in below
                if 0 <= x < CW and 0 <= y < CH and cell[y][x] is not None and cell[y][x] <= hi]
        # the last ring lands on the terrace below it at that terrace's level,
        # a level or not: where the drawing's count of rings and the
        # terraces' levels disagree, the ring takes up the difference
        floor = min(land) if land else hi - LEVEL
        i0, j0 = cx * PER_CELL, cy * PER_CELL
        edge = {"N": [(i0 + k, j0) for k in range(PER_CELL + 1)],
                "S": [(i0 + k, j0 + PER_CELL) for k in range(PER_CELL + 1)],
                "W": [(i0, j0 + k) for k in range(PER_CELL + 1)],
                "E": [(i0 + PER_CELL, j0 + k) for k in range(PER_CELL + 1)]}
        if (cx, cy) in crests:
            held = [(p, hi) for p in edge["E" if side[cy][cx] > 0 else "W"]]
        elif flat_on:
            held = None     # the rects, as they are
        elif t_ == "face":
            held = [(p, None) for p in edge["N"]]
        elif t_ == "band":
            held = [(p, None) for p in edge["E" if side[cy][cx] > 0 else "W"]]
        else:
            held = [(p, None) for p in dict.fromkeys(edge["N"] + edge["S"] + edge["W"] + edge["E"])]
        shapes[(cx, cy)] = (hi, floor, rects, held)

    def lay(cx, cy):
        hi, floor, rects, held = shapes[(cx, cy)]
        changed = False
        slope = (hi - floor) / 16.0
        if held is not None:
            # the edge points it hangs from that stand where its top is: the
            # rest of the edge is rock beside it or the land it falls to
            src = []
            for (p, v) in held:
                if v is None:
                    v = h[p[1]][p[0]]
                if v is not None and v >= floor:
                    src.append((p[0] * STEP, p[1] * STEP, min(v, hi)))
            if not src:
                return False
            # a level a tile, from where it hangs: a band run down from the
            # flank of a crest falls from the crest's foot, not its line
            if tile(cx, cy) == "band":
                slope = (min(hi, max(v for (_, _, v) in src)) - floor) / 16.0
            # a band or a corner is one tile of rock, one level: where the
            # land below is further down (a single band beside a two-level
            # step, a mountain's two bands meeting the sand at one corner),
            # the rest is a wall at its foot, drawn as the rock's face
            # (cliff_walls) - never a slope twice as steep as the tile, the
            # texture stretched down it
            slope = min(slope, 1.0)
        for j in range(cy * PER_CELL, (cy + 1) * PER_CELL + 1):
            py = j * STEP
            for i in range(cx * PER_CELL, (cx + 1) * PER_CELL + 1):
                if fixed[j][i]:
                    continue
                px = i * STEP
                if held is None:
                    d = min(math.hypot(max(x * 16 - px, 0, px - x * 16 - 16),
                                       max(y * 16 - py, 0, py - y * 16 - 16)) for (x, y) in rects)
                    v = hi - (hi - floor) * min(1.0, d / 16.0)
                else:
                    v = max(floor, max(sv - slope * math.hypot(px - sx, py - sy)
                                       for (sx, sy, sv) in src))
                if h[j][i] is None or v > h[j][i] + 1e-6:
                    h[j][i] = v
                    changed = True
        return changed
    # what each rock cell hangs from comes first: the cells hung from
    # nothing but terraces, then those under them
    order = sorted(shapes, key=lambda c: (-shapes[c][0], c[1], c[0]))
    for _ in range(64):
        if not any([lay(cx, cy) for (cx, cy) in order]):
            break
    # each map's own level, the one most of its ground stands at: the map is
    # lifted to it as a whole, its relief written from there
    base = {}
    for lid, (ox, oy) in members.items():
        L = layouts[lid]
        got = collections.Counter(cell[oy + y][ox + x] for y in range(L.h) for x in range(L.w)
                                  if cell[oy + y][ox + x] is not None and soil[oy + y][ox + x])
        base[lid] = got.most_common(1)[0][0] if got else 0
    # a point nothing stands on (off every map) is at its map's level
    for lid, (ox, oy) in members.items():
        L = layouts[lid]
        for j in range(oy * PER_CELL, (oy + L.h) * PER_CELL + 1):
            for i in range(ox * PER_CELL, (ox + L.w) * PER_CELL + 1):
                if h[j][i] is None:
                    h[j][i] = float(base[lid])
    for j in range(LH):
        for i in range(LW):
            if h[j][i] is None:
                h[j][i] = 0.0

    # every point of the drawing as far south as it is high: (u, h, v + h)
    out = {}
    for lid, (ox, oy) in members.items():
        L = layouts[lid]
        b = base[lid]
        out[lid] = [[v - b for v in row[ox * PER_CELL:(ox + L.w) * PER_CELL + 1]]
                    for row in h[oy * PER_CELL:(oy + L.h) * PER_CELL + 1]]
        if drawn_group(lid, checked=False) == name:
            _BASE[lid] = b
            _SHIFT[lid] = [row[:] for row in out[lid]]
    print("drawn %-10s %d regions (%d terraces), %.0f..%.0f px"
          % (name, len(sizes), sum(big), min(min(r) for r in h), max(max(r) for r in h)))
    _DRAWN[name] = out
    return out





def ledge_cells(layout, junctions=True):
    """{(x, y): jump directions} of every ledge cell of a layout.

    Where a ledge turns a corner by a step, the artist joins the two runs
    with a piece the cartridge does not let anyone jump from: a blocked cell
    with ledges on two sides at right angles. It belongs to the ledge, and
    takes the directions of the ledge cells beside it. Not in a map whose cliffs are solved: there the rock
    round it is relief already.
    """
    out = {}
    for y in range(layout.h):
        for x in range(layout.w):
            d = JUMPS.get(layout.behaviour(x, y))
            if d:
                out[(x, y)] = d
    if not junctions or getattr(layout, "layout_id", None) in ENABLED:
        return out
    joins = {}
    for (x, y) in out:
        for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if (nx, ny) in out or layout.off_map(nx, ny) or not layout.blocked(nx, ny):
                continue
            if not layout.is_ledge_junction(nx, ny):
                continue
            dirs = joins.setdefault((nx, ny), [])
            dirs.extend(d for d in out[(x, y)] if d not in dirs)
    out.update((c, tuple(d)) for c, d in joins.items())
    return out


def ledge_berms(layout, art, h):
    """Raise every ledge's lip off the drawing, added to the lattice `h`.

    The lip's top edge - where the ground behind it meets the brown - stands
    LIP pixels up, its foot on the jump side at the ground: lifted as all
    relief is, (u, h, v + h), the brown is a face turned to where the jump
    goes and the GBA view is still the drawing. The ground behind rises to the
    top edge over LIP_BACK pixels, so the ledge is a low berm, not a step in
    the land.

    A point on the lip is as high as it is far from the lip's foot, walking
    the way the jump goes: LIP at LIP_WIDTH pixels and more. Where the lip
    turns (a corner cell jumps two ways) the nearer foot counts, so an outer
    corner rounds off like the drawing and neither arm is read along its
    length. Where it ends - a point shared with a cell that is no ledge - it
    comes down to the ground, and the drawn end taper does the rest.
    """
    cells, lip = ledge_lip(layout, art)
    if not cells:
        return 0

    def first(px, py, dx, dy, limit):
        for k in range(limit):
            if lip.get((px + k * dx, py + k * dy)):
                return k
        return None

    raised = {}
    for (x, y), dirs in cells.items():
        for j in range(PER_CELL + 1):
            for i in range(PER_CELL + 1):
                reads = ledge_reads(lip, x, y, dirs, i, j)
                if all(f for (_, _, _, _, f, _) in reads):
                    best = min(lip_height(f) for (_, _, _, _, f, _) in reads)
                elif any(b and not f for (_, _, _, _, f, b) in reads):
                    best = 0.0  # in front of the lip: the ground jumped down to
                else:
                    # behind the lip: rising to where the line meets it
                    best = 0.0
                    for (sx, sy, dx, dy, f, b) in reads:
                        k = first(sx, sy, dx, dy, LIP_BACK)
                        if k is not None:
                            top = lip_height(lip_run(lip, sx + k * dx, sy + k * dy, dx, dy))
                            best = max(best, top * (1.0 - k / float(LIP_BACK)))
                key = (x * PER_CELL + i, y * PER_CELL + j)
                raised[key] = max(raised.get(key, 0.0), best)
    for (gx, gy), v in raised.items():
        # a point shared with a cell that is no ledge stays on the ground
        touching = [(cx, cy) for cx in {(gx - 1) // PER_CELL, gx // PER_CELL}
                    for cy in {(gy - 1) // PER_CELL, gy // PER_CELL}
                    if 0 <= cx < layout.w and 0 <= cy < layout.h]
        if not all(c in cells for c in touching):
            raised[(gx, gy)] = 0.0
    # nor does the berm fall south faster than its lip does: where a run
    # going north and south ends at a cell that is no ledge, its last point
    # stays on the ground, and a lip still LIP pixels up a step north of it
    # fell further than the step - (u, h, v + h) folded back over itself and
    # opened a hole in the ground. It comes down at the lip's own slope.
    fall = LIP * STEP / float(LIP_WIDTH)
    for (gx, gy) in sorted(raised, key=lambda k: -k[1]):
        south = raised.get((gx, gy + 1))
        if south is not None and raised[(gx, gy)] > south + fall:
            raised[(gx, gy)] = south + fall
    for (gx, gy), v in raised.items():
        h[gy][gx] += v
    return len(cells)


def lip_height(f):
    """A lip point's height, `f` pixels of brown from its foot."""
    return LIP * min(f, LIP_WIDTH) / float(LIP_WIDTH)


def lip_run(lip, px, py, dx, dy, limit=64):
    """How many lip pixels from (px, py) on, walking (dx, dy)."""
    n = 0
    while n < limit and lip.get((px + n * dx, py + n * dy)):
        n += 1
    return n


def ledge_reads(lip, x, y, dirs, i, j):
    """What lattice point (i, j) of ledge cell (x, y) sees along each way the
    ledge jumps: (sx, sy, dx, dy, f, b), f the lip pixels from the point to
    the lip's foot (on the lip when f), b those behind it (in front of the
    lip when b and not f; else behind it)."""
    px, py = x * 16 + i * STEP, y * 16 + j * STEP
    reads = []
    for dx, dy in dirs:
        # the pixel line through the point, inside the cell across
        # the jump, entered on the jump side of the point along it
        if dx == 0:
            sx = min(max(px, x * 16), x * 16 + 15)
            sy = py - (dy < 0)
        else:
            sx = px - (dx < 0)
            sy = min(max(py, y * 16), y * 16 + 15)
        reads.append((sx, sy, dx, dy, lip_run(lip, sx, sy, dx, dy),
                      lip_run(lip, sx - dx, sy - dy, -dx, -dy)))
    return reads


def ledge_lip(layout, art):
    """({cell: jump directions}, {pixel: True}) of a layout's ledges: every
    pixel of a ledge cell drawn in none of the ground colours round it - the
    brown lip."""
    cells = ledge_cells(layout)
    lip = {}
    for (x, y), dirs in cells.items():
        # ground colours: the cells the jump leaves and lands on, else round it
        ground = set()
        for ring in ([(-dx, -dy) for dx, dy in dirs] + list(dirs),
                     [(dx, dy) for dx in (-1, 0, 1) for dy in (-1, 0, 1)]):
            for dx, dy in ring:
                nx, ny = x + dx, y + dy
                if (nx, ny) in cells or layout.off_map(nx, ny) or layout.blocked(nx, ny):
                    continue
                img = art.cell_image(art.metatile(nx, ny))
                ground.update(img.getpixel((i, j))[:3] for j in range(16) for i in range(16))
            if ground:
                break
        if not ground:
            continue
        img = art.cell_image(art.metatile(x, y)).load()
        for j in range(16):
            for i in range(16):
                if img[i, j][:3] not in ground:
                    lip[(x * 16 + i, y * 16 + j)] = True
    return cells, lip


def ledges_on_ground(layout, h):
    """Lay every ledge cell back on the ground round it, before its berm.

    A ledge is modelled the same everywhere: a berm on the ground, as Route
    101 has it (flat land, the lip raised). Where a map's relief is solved,
    the solver reads the brown lip as rock and sinks the cell up to half a
    level, which with the berm added became a trench, toothed at every
    lattice step - shading the grass beside it in stripes. Each ledge cell's
    points go back to the plane of its four corners, which it shares with the
    ground round it. A point on an edge shared with rock (a blocked cell that
    is no ledge) is the rock's and stays as solved; one shared with walkable
    ground or the map's edge is laid flat too, where the sinking reached.
    """
    def keeps(cx, cy):
        return ((cx, cy) not in cells and not layout.off_map(cx, cy)
                and layout.blocked(cx, cy))

    cells = ledge_cells(layout)
    P = PER_CELL
    for (x, y) in cells:
        x0, y0 = x * P, y * P
        c00, c10 = h[y0][x0], h[y0][x0 + P]
        c01, c11 = h[y0 + P][x0], h[y0 + P][x0 + P]
        for j in range(P + 1):
            for i in range(P + 1):
                if i in (0, P) and j in (0, P):
                    continue  # the corners are the ground's
                sides = []
                if i == 0: sides.append((x - 1, y))
                if i == P: sides.append((x + 1, y))
                if j == 0: sides.append((x, y - 1))
                if j == P: sides.append((x, y + 1))
                if any(keeps(*s) for s in sides):
                    continue
                t, s = i / float(P), j / float(P)
                h[y0 + j][x0 + i] = ((c00 * (1 - t) + c10 * t) * (1 - s)
                                     + (c01 * (1 - t) + c11 * t) * s)


PIER_REACH = 2      # pixels round a lattice point its planks are looked for


def pier_ends(group, layout_id, layout, h):
    """A pier's planks run on into the water cell at its end (Mr Briney's
    starts half a cell into the sea): the points of that cell where they
    are drawn, in the upper layer, stand with the pier, and the drop to the
    water is taken in the water's own pixels beside them, not by bending
    the last plank down to the sea."""
    if group not in _PIER:
        drawn_canvas(group)
    ox, oy = DRAWN[group][layout_id]
    pier = {(x - ox, y - oy) for (x, y) in _PIER[group]
            if 0 <= x - ox < layout.w and 0 <= y - oy < layout.h}
    if not pier:
        return
    art = vc.pair_for(layout.primary, layout.secondary)
    shift = _SHIFT.get(layout_id)
    for (px, py) in pier:
        level = h[py * PER_CELL + PER_CELL // 2][px * PER_CELL + PER_CELL // 2]
        for dx in (-1, 1):
            cx, cy = px + dx, py
            if (cx, cy) in pier or not (0 <= cx < layout.w) or layout.role_at(cx, cy) != "water":
                continue
            drawn = art.layer_pixels(layout.metatile(cx, cy), 1)
            for j in range(PER_CELL + 1):
                for i in range(PER_CELL + 1):
                    x, y = i * STEP, j * STEP
                    if any((a, b) in drawn for a in range(x - PIER_REACH, x + PIER_REACH)
                           for b in range(y - PIER_REACH, y + PIER_REACH)):
                        for grid in (h, shift):
                            if grid is not None:
                                grid[cy * PER_CELL + j][cx * PER_CELL + i] = level


def flat_lattice(layout):
    return [[0.0] * (layout.w * PER_CELL + 1) for _ in range(layout.h * PER_CELL + 1)]


def raise_pits(h):
    """A boulder standing in open ground, as a mound and not a pit.

    Every rock cell of a drawn solve hangs a level under the ground it hangs
    from. A boulder with ground on every side (Route 120's, among its ponds)
    has nothing to hang over, so its lattice dipped a level into the ground:
    the rock sat sunk in a hole, and past the hole's back rim the camera saw
    the clear colour. A dip closed all round by ground at one level, two cells
    across at most and exactly a level deep, is mirrored up - the shape a
    rock at sea is given. Larger dips are something else (Mt Chimney's ash
    pile raised is a spike) and stay. Returns how many were raised.
    """
    LH, LW = len(h), len(h[0])
    span = 2 * PER_CELL
    raised = 0
    done = [[False] * LW for _ in range(LH)]
    for j0 in range(1, LH - 1):
        for i0 in range(1, LW - 1):
            if done[j0][i0]:
                continue
            # the rim's level: the highest of the points round this one
            rim = max(h[j0 + b][i0 + a] for a in (-1, 0, 1) for b in (-1, 0, 1))
            if h[j0][i0] >= rim - 0.5:
                continue
            body, stack, ok = [(i0, j0)], [(i0, j0)], True
            seen = {(i0, j0)}
            lo_i = hi_i = i0
            lo_j = hi_j = j0
            while stack and ok:
                i, j = stack.pop()
                for a in (-1, 0, 1):
                    for b in (-1, 0, 1):
                        q = (i + a, j + b)
                        if q in seen:
                            continue
                        if not (0 <= q[0] < LW and 0 <= q[1] < LH):
                            ok = False
                            break
                        v = h[q[1]][q[0]]
                        if v < rim - 0.5:
                            seen.add(q)
                            body.append(q)
                            stack.append(q)
                            lo_i, hi_i = min(lo_i, q[0]), max(hi_i, q[0])
                            lo_j, hi_j = min(lo_j, q[1]), max(hi_j, q[1])
                            if hi_i - lo_i >= span or hi_j - lo_j >= span:
                                ok = False
                                break
                        elif abs(v - rim) > 0.5:
                            ok = False      # the rim is not one level: a terrace's edge
                            break
                    if not ok:
                        break
            if not ok:
                continue
            for (i, j) in body:
                done[j][i] = True
            if abs(min(h[j][i] for (i, j) in body) - (rim - LEVEL)) > 0.5:
                continue
            for (i, j) in body:
                h[j][i] = 2 * rim - h[j][i]
            raised += 1
    return raised


def layout_heights(layout_id):
    """The lattice of a layout: its relief where it is solved, its ledges."""
    roles_layout = open_roles(layout_id)
    group = drawn_group(layout_id)
    if group:
        h = [row[:] for row in solve_drawn(group)[layout_id]]
        raise_pits(h)
        ledges_on_ground(roles_layout, h)
        pier_ends(group, layout_id, roles_layout, h)
    elif layout_id in ENABLED:
        role, cell, h = solve(roles_layout)
        ledges_on_ground(roles_layout, h)
    else:
        h = flat_lattice(roles_layout)
    art = _ART.get(layout_id)
    if art is None:
        art = layout_art(layout_id)
    # a ledge's berm is read off its own drawing, never the analysis's twin
    ledges = ledge_berms(roles_layout, vb.LayoutArt(layout_id) if alias_of(layout_id) else art, h)
    return roles_layout, h, ledges


def ledge_layouts():
    """Every outdoor layout that has a ledge."""
    layouts = json.load(open(os.path.join(vb.ROOT, "data", "layouts", "layouts.json"),
                             encoding="utf-8"))["layouts"]
    out = []
    for e in layouts:
        path = os.path.join(vb.ROOT, e["blockdata_filepath"])
        if not os.path.exists(path):
            continue
        layout = open_roles(e["id"], connections=False)
        if layout.outdoor and ledge_cells(layout, junctions=False):
            out.append(e["id"])
    return out


def awash(layout, mass, kind):
    """A rock drawn mostly in the colours of the water round it - foam over a
    submerged reef - lies at the water line; only a rock drawn as rock is
    raised out of the sea."""
    art = _ART.get(layout.layout_id)
    if art is None:
        art = layout_art(layout.layout_id)
    water = set()
    for (x, y) in mass:
        for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if 0 <= nx < layout.w and 0 <= ny < layout.h and kind[ny][nx] == "water":
                water.update(art.cell_image(art.metatile(nx, ny)).getdata())
    same = total = 0
    for (x, y) in mass:
        for p in art.cell_image(art.metatile(x, y)).getdata():
            total += 1
            same += p in water
    return total and same > total // 2


def cell_grid(h, x, y):
    return [[h[y * PER_CELL + j][x * PER_CELL + i] for i in range(PER_CELL + 1)]
            for j in range(PER_CELL + 1)]


def relief_cells(layout, h):
    """Cells whose lattice is not all zero: (x, y, grid)."""
    out = []
    for y in range(layout.h):
        for x in range(layout.w):
            g = cell_grid(h, x, y)
            if any(abs(v) > 0.25 for row in g for v in row):
                out.append((x, y, g))
    return out


# ── preview ─────────────────────────────────────────────────────────────────

def preview(layout_id, out_dir, cams):
    art = layout_art(layout_id)
    roles_layout, h, _ = layout_heights(layout_id)
    grids = {(x, y): g for (x, y, g) in relief_cells(art, h)}
    depth = _SHIFT.get(layout_id)
    ids = {}
    for y in range(art.h):
        for x in range(art.w):
            ids.setdefault(art.metatile(x, y), len(ids))
    cols = 32
    from PIL import Image
    tex = Image.new("RGBA", (cols * 16, ((len(ids) + cols - 1) // cols) * 16), (0, 0, 0, 255))
    for m, i in ids.items():
        tex.paste(art.cell_image(m), ((i % cols) * 16, (i // cols) * 16))
    tris = []
    for y in range(art.h):
        for x in range(art.w):
            i = ids[art.metatile(x, y)]
            u0, v0 = (i % cols) * 16, (i // cols) * 16
            g = grids.get((x, y))
            if g is None:
                g = [[0.0] * (PER_CELL + 1) for _ in range(PER_CELL + 1)]
            for j in range(PER_CELL):
                for k in range(PER_CELL):
                    def P(a, b):
                        hh = g[b][a] / 16.0
                        zz = depth[y * PER_CELL + b][x * PER_CELL + a] / 16.0 if depth else hh
                        return (x + a * STEP / 16.0, hh, y + b * STEP / 16.0 + zz,
                                u0 + a * STEP, v0 + b * STEP)
                    A, B, C, D = P(k, j), P(k + 1, j), P(k + 1, j + 1), P(k, j + 1)
                    tris.append(((A, B, C), 1.0))
                    tris.append(((A, C, D), 1.0))
    paths = []
    for name, (tx, tz, pitch, yaw, dist) in cams:
        cam = vb.Camera((tx, 0.0, tz), pitch=pitch, yaw=yaw, distance=dist)
        img = vb.render_scene(cam, [(tris, tex)], scale=2)
        path = os.path.join(out_dir, "relief_%s_%s.png" % (layout_id.lower(), name))
        img.save(path)
        paths.append(path)
        if depth is not None:
            path = os.path.join(out_dir, "relief_%s_%s_lines.png" % (layout_id.lower(), name))
            check_lines(layout_id, cam, img.copy(), h, depth).save(path)
            paths.append(path)
    return paths


def check_lines(layout_id, cam, img, h, depth):
    """A camera proof: the geometry each tile of rock should have, over the
    render. Red is what the drawing asks for - a face's top edge at its
    level and its foot a level lower, a band's high and low edges, straight
    along the tile - and yellow is where the mesh actually is along the
    same edges. Where yellow leaves red, the shape is wrong."""
    from PIL import ImageDraw
    group = drawn_group(layout_id)
    ox, oy = DRAWN[group][layout_id]
    top, kinds, _, _ = _ROCK[group]
    cam2 = vb.Camera(cam.target, cam.pitch, cam.yaw, cam.distance, cam.fov,
                     img.width, img.height)
    d = ImageDraw.Draw(img)

    def line(pts, colour):
        xy = [cam2.project(p) for p in pts]
        xy = [(q[0], q[1]) for q in xy if q is not None]
        if len(xy) > 1:
            d.line(xy, fill=colour, width=2)

    # the lattice is over the map's base (solve_drawn), the levels are not
    base = _BASE.get(layout_id, 0)

    def world(i, j, hh):
        hh -= base
        return (i * STEP / 16.0, hh / 16.0, j * STEP / 16.0 + hh / 16.0)

    def actual(i, j):
        return (i * STEP / 16.0, h[j][i] / 16.0, j * STEP / 16.0 + depth[j][i] / 16.0)

    L = len(h) - 1, len(h[0]) - 1
    for (gx, gy), kind_ in kinds.items():
        cx, cy = gx - ox, gy - oy
        hi = top.get((gx, gy))
        if hi is None or not (0 <= cx < L[1] // PER_CELL and 0 <= cy < L[0] // PER_CELL):
            continue
        i0, j0 = cx * PER_CELL, cy * PER_CELL
        if kind_ == "face":
            edges = [([(i0 + k, j0) for k in range(PER_CELL + 1)], hi),
                     ([(i0 + k, j0 + PER_CELL) for k in range(PER_CELL + 1)], hi - LEVEL)]
        elif kind_ == "band":
            s_ = _DRAWN_SIDE[group][gy][gx]
            high = i0 + (PER_CELL if s_ > 0 else 0)
            low = i0 + (0 if s_ > 0 else PER_CELL)
            edges = [([(high, j0 + k) for k in range(PER_CELL + 1)], hi),
                     ([(low, j0 + k) for k in range(PER_CELL + 1)], hi - LEVEL)]
        else:
            continue
        for pts, want in edges:
            pts = list(pts)
            line([actual(i, j) for (i, j) in pts], (255, 230, 0))
            line([world(i, j, want) for (i, j) in pts], (255, 0, 0))
    return img


_ROLES = []


_MAP_SIDES = {}     # layout -> the sides its maps connect on, read once


def _map_sides():
    if not _MAP_SIDES:
        _MAP_SIDES[None] = set()
        maps = os.path.join(vb.ROOT, "data", "maps")
        for name in os.listdir(maps):
            path = os.path.join(maps, name, "map.json")
            if not os.path.exists(path):
                continue
            m = json.load(open(path, encoding="utf-8"))
            sides = _MAP_SIDES.setdefault(m.get("layout"), set())
            for c in m.get("connections") or []:
                if c.get("direction") in ("up", "down", "left", "right"):
                    sides.add(c["direction"])
    return _MAP_SIDES


def open_roles(layout_id, connections=True):
    entry = next(e for e in vb._layouts_json() if e["id"] == layout_id)
    if not _ROLES:
        _ROLES.append(vc.MapEvents())
    layout = vc.Layout(entry, _ROLES[0])
    layout.layout_id = layout_id
    if not connections:
        return layout
    # the sides some map using this layout is connected to another on
    layout.connected_sides = sorted(_map_sides().get(layout_id, ()))
    layout.layout_id = layout_id
    return layout

def proof(name, path):
    """The model's terraces over the drawing of a drawn group, whole canvas:
    a thick line where each terrace's flat top ends, in its level's colour,
    its level written in each of its cells, and a dot on every point the
    slope above lifts off the land below (a face's foot). What the model
    claims, checked against the art without the game."""
    from PIL import Image, ImageDraw
    out = solve_drawn(name)
    layouts, CW, CH = drawn_canvas(name)[:3]
    cell = _CELLS[name]
    H = [[None] * (CW * PER_CELL + 1) for _ in range(CH * PER_CELL + 1)]
    for lid, (ox, oy) in DRAWN[name].items():
        for j, row in enumerate(out[lid]):
            for i, v in enumerate(row):
                H[oy * PER_CELL + j][ox * PER_CELL + i] = v
    Z = 4
    im = Image.new("RGB", (CW * 16 * Z, CH * 16 * Z))
    for lid, (ox, oy) in DRAWN[name].items():
        A = _ART[lid]
        for cy in range(A.h):
            for cx in range(A.w):
                im.paste(A.cell_image(A.metatile(cx, cy)).convert("RGB").resize(
                    (16 * Z, 16 * Z), Image.NEAREST), ((ox + cx) * 16 * Z, (oy + cy) * 16 * Z))
    d = ImageDraw.Draw(im)
    palette = [(0, 170, 0), (0, 120, 255), (255, 200, 0), (255, 90, 0), (230, 0, 0), (200, 0, 200)]
    colour = lambda lv: palette[int(lv) // LEVEL % len(palette)] if lv >= 0 else (255, 255, 255)
    S = 16 * Z
    for Y in range(CH):
        for X in range(CW):
            lv = cell[Y][X]
            if lv is None:
                continue
            px, py = X * S, Y * S
            d.text((px + S // 2 - 6, py + S // 2 - 6), str(int(lv)), fill=colour(lv))
            for dx, dy, seg in ((0, -1, (px, py, px + S - 1, py)),
                                (0, 1, (px, py + S - 1, px + S - 1, py + S - 1)),
                                (-1, 0, (px, py, px, py + S - 1)),
                                (1, 0, (px + S - 1, py, px + S - 1, py + S - 1))):
                nx, ny = X + dx, Y + dy
                nb = cell[ny][nx] if 0 <= nx < CW and 0 <= ny < CH else lv
                if nb is None or nb < lv:
                    d.line(seg, fill=colour(lv), width=6)
    for j in range(CH * PER_CELL + 1):
        for i in range(CW * PER_CELL + 1):
            v = H[j][i]
            base = cell[min(j // PER_CELL, CH - 1)][min(i // PER_CELL, CW - 1)]
            if v is not None and base is not None and v > base + 0.5:
                x, y = i * STEP * Z, j * STEP * Z
                d.ellipse([x - 3, y - 3, x + 3, y + 3], fill=(255, 255, 255))
    im.save(path)
    return path



# ── export ──────────────────────────────────────────────────────────────────

MAGIC = b"VXL4"
HEIGHT_UNIT = 2      # pixels per stored step of a drawn map's height

# ── cut tiles ───────────────────────────────────────────────────────────────
#
# A tile of rock is drawn over what lies behind it - the sea round a ridge's
# end, the sand at a mountain's corner - and its lattice, a point every 4
# pixels, cannot end where the drawing's rock ends: the sea between the
# rock's outline and the nearest point is lifted with the rock. So the cut
# is made in the texture, pixel for pixel, as a signpost's silhouette is
# (voxel_sign_mask.py): what the cell draws in the colours of the flat ground
# beside it, reached from the cell's border, is the background; where the
# relief would lift it, the cell is drawn twice - whole and flat at the level
# of the ground, and lifted with its background clear.
CUT_LIFT = 2.0      # pixels: background lifted more than this is cut away
CUT_PIXELS = 4      # and only where that many pixels of it are


def _cut_mask(art, layout, x, y, rock_cells):
    """Rows of bits, 1 where cell (x, y) draws the ground behind its rock;
    None where nothing beside it is flat ground."""
    import voxel_sign_mask
    ground = set()
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            nx, ny = x + dx, y + dy
            if (dx or dy) and 0 <= nx < layout.w and 0 <= ny < layout.h and (nx, ny) not in rock_cells:
                img = art.cell_image(art.metatile(nx, ny)).convert("RGB")
                ground.update(c for c in img.getdata() if c not in ROCK_ALL)
    if not ground:
        return None
    img = art.cell_image(art.metatile(x, y)).convert("RGB").load()
    opaque = [(i, j) for j in range(16) for i in range(16) if img[i, j] not in ground]
    rows = voxel_sign_mask.cutout_mask(opaque)
    return [(~r) & 0xFFFF for r in rows]


def _lifted(grid, mask):
    """How many pixels of the background the lattice lifts off its foot,
    and the foot (the lattice's lowest point)."""
    foot = min(min(r) for r in grid)
    n = 0
    for j in range(16):
        for i in range(16):
            if not (mask[j] >> i) & 1:
                continue
            fx, fy = (i + 0.5) / STEP, (j + 0.5) / STEP
            a, b = min(int(fx), PER_CELL - 1), min(int(fy), PER_CELL - 1)
            tx, ty = fx - a, fy - b
            v = ((grid[b][a] * (1 - tx) + grid[b][a + 1] * tx) * (1 - ty)
                 + (grid[b + 1][a] * (1 - tx) + grid[b + 1][a + 1] * tx) * ty)
            n += v - foot > CUT_LIFT
    return n, foot


# where a cut tile's ground runs on under the rock beside it
# (voxel_mesh_builder.c: kCutFill)
FILL = ((0, 1), (-1, 1), (1, 1), (-1, 0), (1, 0))


def cut_cells(lid, roles_layout, h):
    """[(x, y, metatile, mask, foot, ground behind)] of a drawn layout's cut
    cells (their own grids: rim_cells)."""
    group = drawn_group(lid)
    if group is None or group not in _ROCK:
        return []
    ox, oy = DRAWN[group][lid]
    kinds = _ROCK[group][1]
    rock = {(gx - ox, gy - oy) for (gx, gy) in kinds}
    art = layout_art(lid)
    out = []
    # never a ledge: its berm is one mound, the grass behind its lip risen
    # with it the run's whole length. Cut, the middle of a run laid that
    # grass back at its foot and left the lip's top pixels in the air (dark
    # spikes along Route 111's ledges), the ground rising at its two uncut
    # end tiles only
    ledges = set(ledge_cells(roles_layout))
    for (x, y) in sorted(rock - ledges, key=lambda c: (c[1], c[0])):
        if not (0 <= x < roles_layout.w and 0 <= y < roles_layout.h):
            continue
        mask = _cut_mask(art, roles_layout, x, y, rock)
        if mask is None or not any(mask):
            continue
        n, foot = _lifted(cell_grid(h, x, y), mask)
        if n >= CUT_PIXELS:
            out.append((x, y, art.metatile(x, y), mask, foot, _behind(art, roles_layout, x, y, rock, mask)))
    return out


_SIDE_PIXELS = {      # the part of a tile that faces the rock: its south rows, east or west columns
    None: lambda i, j: True,
    "N": lambda i, j: j < 6,
    "S": lambda i, j: j >= 10,
    "E": lambda i, j: i >= 10,
    "W": lambda i, j: i < 6,
}


def plain_ground(art, layout, content, x, y, side=None, kind=None, half=False):
    """The plainest drawing of the ground at (x, y), as a metatile: what
    runs on under a rock or a rim is the ground itself, not its shore or
    its edge, which tiled again would show seams. Of the ground round it
    whose commonest colour is this one's, the tile most of that colour (the
    plain sand of a shore, never the sea beyond it).
    `half`: only a tile that colour at least half over (voxel_terraces, a
    map read in its own colours: Route 114's dirt edged half in orange has
    fewer far colours than its plain dirt, flecked darker)."""
    def main(m):
        """(its commonest colour, how many pixels of it, how many pixels are
        another colour altogether - a shore's few drops of the sea on a sand)
        Cached on the art: an id over 0x200 is its secondary tileset's own
        (keyed by the id alone, Route 114's Fallarbor tiles gave Pacifidlog
        its ground)."""
        known = art.__dict__.setdefault("_plain", {})
        if m not in known:
            px = list(art.cell_image(m).convert("RGB").getdata())
            c = commonest(px)
            foreign = sum(1 for p in px if sum(abs(p[i] - c[i]) for i in range(3)) > 150)
            known[m] = (c, px.count(c), foreign)
        return known[m]
    m0 = art.metatile(x, y)
    forced = kind is not None
    if forced:
        pass
    elif side is None:
        kind = main(m0)[0]
    else:
        # a shore tile's commonest colour can be the sea beyond it: what the
        # rock stands on is the ground at the edge the rock is on
        im = art.cell_image(m0).convert("RGB")
        part = [im.getpixel((i, j)) for j in range(16) for i in range(16) if _SIDE_PIXELS[side](i, j)]
        kind = commonest(part)
    best = None
    for dy in range(-24, 25):
        for dx in range(-24, 25):
            c = (x + dx, y + dy)
            if c in content or not (0 <= c[0] < layout.w and 0 <= c[1] < layout.h):
                continue
            m = art.metatile(*c)
            if main(m)[0] != kind or (half and 2 * main(m)[1] < 256):
                continue
            k = (main(m)[2], -main(m)[1], abs(dx) + abs(dy))
            if best is None or k < best[0]:
                best = (k, m)
    if best:
        return best[1]
    return None if forced else art.metatile(x, y)


def _plain_tile(art, m):
    """Is metatile m ground with no rock drawn on it (under a tenth)?"""
    # cached on the art itself: keyed by id(art), a short-lived art (the
    # ledges' own reading of an aliased map) could reuse a freed one's id
    known = art.__dict__.setdefault("_plain_tile", {})
    if m not in known:
        px = list(art.cell_image(m).convert("RGB").getdata())
        known[m] = sum(1 for p in px if p in ROCK_ALL) < len(px) // 10
    return known[m]


def _plain_background(art, m, mask, ground):
    """True where the tile's background (what its cut clears) is all the
    plain ground behind it: its flat layer is then that ground, and the
    rock pixels it carries - flat at the foot, seen wherever the lifted
    rock does not cover them as a dark wedge on the sand - are gone."""
    img = art.cell_image(m).convert("RGB").load()
    g = art.cell_image(ground).convert("RGB")
    gp = list(g.getdata())
    gc = commonest(gp)
    bg = [img[i, j] for j in range(16) for i in range(16) if (mask[j] >> i) & 1]
    if len(bg) < 8:
        return False
    near = sum(1 for p in bg if sum(abs(p[k] - gc[k]) for k in range(3)) <= 150)
    return near >= 0.9 * len(bg)


def _behind(art, layout, x, y, rock, mask=None, half=False, content=None, ignore=()):
    """The ground a cut tile is drawn over, as a metatile: what lies north of
    it, or beside it. It runs on under the rock south of the tile - never
    drawn, the rock covers it at 45 degrees - where, seen from above, the
    clear background shows it between the rock's outline and its wall.

    What it is, the tile says itself: the commonest colour of its own
    background (the pixels the cut clears) is the ground it was drawn over,
    where the tile north of it can be the shore of another ground.
    `content`: the cells the plainest tile is not looked for in (`rock` by
    default; voxel_terraces leaves the plain tops in, a head's ground on the
    next terrace). `ignore`: colours of the background that are no ground
    (the grey outline round the rock); where too little is left to tell, the
    ground is the commonest of the ground drawn round the cell."""
    if content is None:
        content = rock
    if mask is not None:
        img = art.cell_image(art.metatile(x, y)).convert("RGB").load()
        bg = [img[i, j] for j in range(16) for i in range(16)
              if (mask[j] >> i) & 1 and img[i, j] not in ignore]
        if ignore and len(bg) < 32:
            around = []
            for ny in (y - 1, y, y + 1):
                for nx in (x - 1, x, x + 1):
                    if (nx, ny) != (x, y) and 0 <= nx < layout.w and 0 <= ny < layout.h:
                        around += [p for p in art.cell_image(art.metatile(nx, ny)).convert("RGB").getdata()
                                   if p not in ROCK_ALL and p not in ignore]
            # (a background of a few pixels in a colour the ground round the
            # cell has little of - under a quarter of its commonest - is the
            # rock's own outline, no ground to tell by: the deep blue round
            # Route 120's boulder, laid under its cut, stood as a dark block
            # in the river. In a colour round it, it is that ground: its tip's
            # drops of the river, read by the ground round it, were the deep
            # water north of it)
            if len(bg) < 8 or (around and 4 * around.count(commonest(bg)) < around.count(commonest(around))):
                bg = around
        if len(bg) >= 8:
            kind = commonest(bg)
            # (south last: walled in by rock and props, the ground it stands
            # on - Verdanturf's corner between the Battle Tent and a sign)
            for (dx, dy, side) in ((0, -1, "S"), (-1, 0, "E"), (1, 0, "W"), (0, 0, None), (0, 1, None)):
                nx, ny = x + dx, y + dy
                if 0 <= nx < layout.w and 0 <= ny < layout.h and (nx, ny) not in rock or (dx, dy) == (0, 0):
                    m = plain_ground(art, layout, content, nx, ny, side, kind, half)
                    if m is not None:
                        return m
    for (dx, dy, side) in ((0, -1, "S"), (-1, 0, "E"), (1, 0, "W"), (0, 1, None)):
        nx, ny = x + dx, y + dy
        if 0 <= nx < layout.w and 0 <= ny < layout.h and (nx, ny) not in rock:
            return plain_ground(art, layout, rock, nx, ny, side, half=half)
    return None


def rim_cells(lid, roles_layout, h, cut, base=None):
    """The rims of a drawn layout's terraces over lower ground behind them:
    ({(x, y): grid}, [(x, y, foot, ground)]).

    A terrace's top shares the lattice points of its north edge with the
    ground behind it, a level or two lower: its first row of quads is then a
    wall within one lattice step, the rim's top pixels stretched up it - out
    of sight at 45 degrees, plain from a higher camera. So a top's own grid
    stands level up to its north edge (the ground's grid is left as it is,
    and drawn as it is), and the ground behind runs on, flat at its level,
    under the cells the step leaves open from above: a level of height is a
    row south (a point stands as far south as it is high). The wall itself
    faces north, away from the camera; nothing of it is drawn."""
    group = drawn_group(lid)
    if group is None or group not in _ROCK:
        return {}, []
    ox, oy = DRAWN[group][lid]
    kinds, soil = _ROCK[group][1], _ROCK[group][2]
    levels = _CELLS[group]
    W, Hh = roles_layout.w, roles_layout.h
    rock = {(gx - ox, gy - oy) for (gx, gy) in kinds}
    content = rock | {(x, y) for y in range(Hh) for x in range(W)
                      if levels[oy + y][ox + x] is not None and not soil[oy + y][ox + x]}
    art = layout_art(lid)
    taken = {(x, y) for (x, y, *_) in cut}
    cut_behind = {(c[0], c[1]): c[5] for c in cut if len(c) > 5 and c[5] is not None}
    plainest = lambda x, y: plain_ground(art, roles_layout, content, x, y, "S")
    ledges = set(ledge_cells(roles_layout))
    grids, fills = {}, {}

    def level(x, y):
        """A tile loses only its back: its north edge stands as its next
        row, never a wall or a slope down to the ground behind it, which no
        camera of the game looks at; the rest is the shape the pattern gives
        it, Route 116's. A corner with nothing behind it is all back: it is
        the band or the top south of it going on to its outline, never a
        cone falling north."""
        g = cell_grid(rock_h, x, y)
        corner = kinds.get((ox + x, oy + y)) == "corner"
        for j in range(PER_CELL - 1 if corner else 0, -1, -1):
            for i in range(PER_CELL + 1):
                g[j][i] = max(g[j][i], g[j + 1][i])
        if g != cell_grid(h, x, y):
            grids[(x, y)] = g
    rock_h = base if base is not None else h
    for (x, y) in sorted(content, key=lambda c: (c[1], c[0])):
        if not (0 <= x < W and 1 <= y < Hh) or (x, y - 1) in content:
            continue
        g = cell_grid(rock_h, x, y)
        behind = cell_grid(h, x, y - 1)
        foot = max(max(r) for r in behind)
        if min(min(r) for r in behind) < foot - 0.5:
            continue            # not flat ground behind
        below = cell_grid(rock_h, x, y + 1) if y + 1 < Hh else g
        top = max(max(max(r) for r in g), max(max(r) for r in below))
        if group in WRAP_GROUPS and max(max(r) for r in g[1:PER_CELL]) - min(min(r) for r in g[1:PER_CELL]) <= 1.0:
            # a terrace's top of its own (its edge rows are shared with the
            # cells north and south of it): what stands south of it, higher,
            # has this top behind it, not the ground (Route 106's ledge
            # behind its plateau)
            top = max(max(r) for r in g[1:PER_CELL])
        if top <= foot + 0.5:
            continue
        level(x, y)
        if (x, y) not in grids:
            continue
        # under a cut tile the ground is the one its own drawing names, the
        # tile north of it can be a shore tile of the sea. Under a ledge's
        # berm it is the grass its own tile draws behind the lip: the strip
        # its risen back leaves open is drawn there, and the tile north of
        # it can be the end of a sand path (Route 111: a band of plain sand
        # or of a streaked road under the berm, past the path's round end)
        if (x, y) in ledges:
            ground = plain_ground(art, roles_layout, content, x, y, "N")
        else:
            ground = cut_behind.get((x, y)) or plainest(x, y - 1)
        for k in range(int(math.ceil((top - foot) / LEVEL))):
            c = (x, y + k)
            if not (0 <= c[1] < Hh):
                break
            if group in WRAP_GROUPS and k and min(min(r) for r in cell_grid(rock_h, x, y + k - 1)[1:]) > foot + 0.5:
                break           # a raised top stands behind it: that goes on under it, not the ground
            if c in taken:
                continue        # a cut tile lies flat at its own foot
            if c not in fills or foot < fills[c][0]:
                fills[c] = (foot, ground)
    return grids, [(x, y, foot, ground) for (x, y), (foot, ground) in sorted(fills.items())]


# How much steeper than its true shape a tile may stand where the drawing
# has fewer rows of face, or tiles of band, than the step it must fall: the
# difference spread evenly over all of them (Ever Grande's cliff: 8 faces
# for 10 levels) is a few hundredths on screen, where all of it in one row is
# the drawing stretched.
SPREAD = 1.25
NO_FACE = 0xFFFE       # voxel_relief.h: VOXEL_RELIEF_NO_FACE


def spread(h, content, W, Hh):
    """The lattice with every fall of the rock spread evenly: down each
    column (a face) and across each row (a band), a run of steps falling
    one way that falls more than a true shape anywhere falls evenly over
    the whole run, if that is within SPREAD of true. Points on the map's
    edge (its seams) stay."""
    P = PER_CELL
    h = [row[:] for row in h]

    def rock(a, b):
        return (a // P, b // P) in content

    def even(points):
        """points: [(i, j)] of a run; spread its fall evenly if it must."""
        vals = [h[j][i] for (i, j) in points]
        steps = [vals[k + 1] - vals[k] for k in range(len(vals) - 1)]
        if not steps or max(abs(d) for d in steps) <= STEP + 0.01:
            return
        n = len(steps)
        if abs(vals[-1] - vals[0]) / n > STEP * SPREAD + 0.01:
            return
        for k, (i, j) in enumerate(points[1:-1], 1):
            h[j][i] = vals[0] + (vals[-1] - vals[0]) * k / n

    for i in range(1, W * P):
        run = []
        for j in range(Hh * P):
            inside = (rock(i - 1, j) or rock(i, j)) and 0 < j < Hh * P
            fall = h[j + 1][i] < h[j][i] - 0.01
            if inside and fall:
                run = run or [(i, j)]
                run.append((i, j + 1))
                continue
            if len(run) > 2:
                even(run)
            run = []
        if len(run) > 2:
            even(run)
    for j in range(1, Hh * P):
        for sign in (1, -1):
            run = []
            for i in range(W * P):
                inside = (rock(i, j - 1) or rock(i, j)) and 0 < i < W * P
                d = (h[j][i + 1] - h[j][i]) * sign
                if inside and d < -0.01:
                    run = run or [(i, j)]
                    run.append((i + 1, j))
                    continue
                if len(run) > 2:
                    even(run)
                run = []
            if len(run) > 2:
                even(run)
    return h


def cell_shapes(lid, roles_layout, h, cut):
    """Every cell's own grid where it is not the shared lattice's, the
    ground run on behind (rim_cells) and the cliff walls: ({(x, y): grid},
    [(x, y, foot, ground)], {(x, y): face metatile}).

    A tile of rock is one tile of its shape - a band one level at 45
    degrees, a top level - and where the ground beside it lies further down
    than the tile reaches (a single band beside a two-level step), the rock
    goes on to its edge in its own shape and the rest is a cliff: a wall
    from its edge down to the ground's, drawn as the mountain's own face,
    never the tile's outline stretched down it. The ground keeps its level
    to its edge. Behind - north - nothing stands: the top ends over the
    ground run on under it (rim_cells)."""
    group = drawn_group(lid)
    if group is None or group not in _ROCK:
        grids, fills = rim_cells(lid, roles_layout, h, cut)
        return grids, fills, {}
    ox, oy = DRAWN[group][lid]
    kinds, soil = _ROCK[group][1], _ROCK[group][2]
    levels = _CELLS[group]
    W, Hh = roles_layout.w, roles_layout.h
    rock = {(gx - ox, gy - oy) for (gx, gy) in kinds}
    content = {c for c in rock if 0 <= c[0] < W and 0 <= c[1] < Hh}
    content |= {(x, y) for y in range(Hh) for x in range(W)
                if levels[oy + y][ox + x] is not None and not soil[oy + y][ox + x]}
    rock_h = spread(h, content, W, Hh)
    grids, fills = rim_cells(lid, roles_layout, h, cut, base=rock_h)
    for c in content:
        if c not in grids and cell_grid(rock_h, *c) != cell_grid(h, *c):
            grids[c] = cell_grid(rock_h, *c)
    art = layout_art(lid)
    P = PER_CELL
    # ledges and berry soil are not mountains: the rules below for rock
    # (a top's back, cleared points, what goes on behind) leave them as
    # they are
    keep_out = set(ledge_cells(roles_layout))
    berry = vc.behaviours().get("MB_BERRY_TREE_SOIL")
    keep_out |= {(x, y) for y in range(Hh) for x in range(W)
                 if art.metatile(x, y) in DIRT or roles_layout.behaviour(x, y) == berry}
    rock_cells = content - keep_out

    def rock_point(c, i, j):
        """A point the rules for rock may move: any of the rock's, and of
        a ledge or berry soil only one it shares with rock, which they move
        too (a crack opens there otherwise). Its others are its own."""
        if c not in keep_out:
            return True
        X, Y = c[0] * P + i, c[1] * P + j
        return any((cx, cy) in rock_cells for cx in {(X - 1) // P, X // P}
                   for cy in {(Y - 1) // P, Y // P})

    def grid(c):
        return grids.get(c) or cell_grid(h, *c)

    def inside(c):
        return 0 <= c[0] < W and 0 <= c[1] < Hh
    # the rock to its edge in its own shape, beside lower ground
    for c in sorted(content, key=lambda c: (c[1], c[0])):
        g = [row[:] for row in grid(c)]
        for (dx, e, a, b) in ((-1, 0, 1, 2), (1, P, P - 1, P - 2)):
            n = (c[0] + dx, c[1])
            if not inside(n) or n in content:
                continue
            for j in range(P + 1):
                v = min(g[j][a], 2 * g[j][a] - g[j][b])
                if v >= g[j][e] + STEP:      # a step, not a pebble's swell
                    g[j][e] = v
        if g != grid(c):
            grids[c] = g
    # a cut tile's clear background is no rock: an edge point that touches
    # nothing else stays on the ground beside it, so a cliff stands only
    # where the tile draws rock to its edge
    masks = {(x, y): mask for (x, y, m, mask, foot, ground) in cut}

    def clear_quad(mask, a, b):
        return all((mask[j] >> i) & 1 for j in range(b * STEP, b * STEP + STEP)
                   for i in range(a * STEP, a * STEP + STEP))
    for c, mask in masks.items():
        g = [row[:] for row in grid(c)]
        for (dx, dy) in ((-1, 0), (1, 0), (0, 1)):
            n = (c[0] + dx, c[1] + dy)
            if not inside(n) or n in content:
                continue
            o = grid(n)
            for k in range(P + 1):
                if dx:
                    i, j = (0 if dx < 0 else P), k
                    oi, oj = (P if dx < 0 else 0), k
                else:
                    i, j, oi, oj = k, P, k, 0
                quads = [(a, b) for a in (i - 1, i) for b in (j - 1, j) if 0 <= a < P and 0 <= b < P]
                if all(clear_quad(mask, a, b) for (a, b) in quads):
                    g[j][i] = min(g[j][i], o[oj][oi])
        if g != grid(c):
            grids[c] = g
    # the ground level to its edge
    for y in range(Hh):
        for x in range(W):
            c = (x, y)
            if c in content:
                continue
            g = [row[:] for row in grid(c)]
            inner = [g[j][i] for j in range(1, P) for i in range(1, P)]
            if max(inner) - min(inner) > 0.5:
                continue            # not level ground: as it is
            for (dx, dy) in ((-1, 0), (1, 0), (0, -1), (0, 1)):
                n = (x + dx, y + dy)
                if not inside(n) or n not in content:
                    continue
                for k in range(P + 1):
                    if dx == -1:
                        g[k][0] = g[k][1]
                    elif dx == 1:
                        g[k][P] = g[k][P - 1]
                    elif dy == -1:
                        g[0][k] = g[1][k]
                    else:
                        g[P][k] = g[P - 1][k]
            if g != grid(c):
                grids[c] = g
    # no tile rises southward under what it draws: a slope turned north is
    # its back, which no camera looks at and which, seen from above, is the
    # drawing stretched. Each column of each cell stands as high as the
    # point south of it; a column is shared whole with the cell beside it,
    # which reads it the same - only between a cell and the one north of it
    # may the grids part, and that step is a back, the ground behind run on
    # under it
    #
    # Nor is anything it draws steeper than a tile's true shapes: across, a
    # band's 45 degrees; down, a south face's pixel a pixel. Where the
    # pattern leaves a drawn quad steeper - a band's end against the
    # terrace over it, a single band bridging two levels - its low corner is
    # raised to the true shape, the way Route 116's corners fill in.
    def drawn(c, a, b):
        m = masks.get(c)
        return m is None or not clear_quad(m, a, b)

    def seam(c, i, j):
        """A point on the map's own edge: the seam with the next map, which
        meets it there (voxel_mesh_builder.c's seam skirt), keeps it."""
        return c[0] * P + i in (0, W * P) or c[1] * P + j in (0, Hh * P)

    def follow(c):
        """Once: a point on the cell's edge that stands steeper than a true
        shape from the cell's own surface beside it follows that surface -
        two surfaces side by side in the drawing may stand far apart in the
        world, and the step between them is a silhouette, the camera's, not
        the tile's."""
        g = [row[:] for row in grid(c)]
        T = STEP * SPREAD

        def far(a, b):
            # more than a level apart within a step: two surfaces, not one
            return abs(a - b) > LEVEL + 0.01
        for j in range(P + 1):
            for (e, n) in ((0, 1), (P, P - 1)):
                if (not seam(c, e, j) and rock_point(c, e, j) and drawn(c, min(e, n), min(j, P - 1))
                        and far(g[j][e], g[j][n])):
                    g[j][e] = min(max(g[j][e], g[j][n] - T), g[j][n] + T)
        for i in range(P + 1):
            # north: no lower than the row south of it, a pixel a pixel at
            # most higher; south: the other way round
            if not seam(c, i, 0) and rock_point(c, i, 0) and drawn(c, min(i, P - 1), 0) and far(g[0][i], g[1][i]):
                g[0][i] = min(max(g[0][i], g[1][i]), g[1][i] + T)
            if (not seam(c, i, P) and rock_point(c, i, P) and drawn(c, min(i, P - 1), P - 1)
                    and far(g[P][i], g[P - 1][i])):
                g[P][i] = min(max(g[P][i], g[P - 1][i] - T), g[P - 1][i])
        if g != grid(c):
            grids[c] = g

    def settle(c):
        """The cell's drawn quads in true shapes, raising only: a point
        rising southward lifts the one north of it, a quad steeper than a
        band across or a face down lifts its low point. Raising only, it
        comes to rest."""
        g = [row[:] for row in grid(c)]

        def free(p):
            return not seam(c, *p) and rock_point(c, *p)
        moved = True
        while moved:
            moved = False
            for b_ in range(P):
                for a_ in range(P):
                    if not drawn(c, a_, b_):
                        continue
                    for (p0, p1, d) in (((a_, b_), (a_ + 1, b_), "u"), ((a_, b_ + 1), (a_ + 1, b_ + 1), "u"),
                                        ((a_, b_), (a_, b_ + 1), "v"), ((a_ + 1, b_), (a_ + 1, b_ + 1), "v")):
                        h0, h1 = g[p0[1]][p0[0]], g[p1[1]][p1[0]]
                        T = STEP * SPREAD
                        if d == "u":
                            if h1 < h0 - T - 0.01 and free(p1):
                                g[p1[1]][p1[0]] = h0 - T
                                moved = True
                            elif h0 < h1 - T - 0.01 and free(p0):
                                g[p0[1]][p0[0]] = h1 - T
                                moved = True
                        else:
                            if h1 > h0 + 0.01 and free(p0):
                                g[p0[1]][p0[0]] = h1          # rising south: its back
                                moved = True
                            elif h1 < h0 - T - 0.01 and free(p1):
                                g[p1[1]][p1[0]] = h0 - T      # falling faster than a face
                                moved = True
        if g != grid(c):
            grids[c] = g
            return True
        return False
    # one point, one height, between rock and terrace cells side by side:
    # what one took for itself (its edge) the other shares - only against
    # the ground, or north and south, may two grids part, where a cliff or
    # the ground behind closes them
    shared = collections.defaultdict(list)
    for c in content:
        for j in range(P + 1):
            for i in (0, P):
                shared[(c[0] * P + i, c[1], j)].append((c, i, j))
    for c in sorted(content, key=lambda c: (c[1], c[0])):
        follow(c)
    # a top's back is the edge it shares with the rock north of it: where a
    # cell's last row rises to the top south of it, that rise is the back,
    # and the cell keeps its own shape to its edge - a flank ending against a
    # wider plateau's back raised to the plateau whole stood as a flat flank
    # with a wall down its outline, a spike over the sea
    for c in sorted(content, key=lambda c: (c[1], c[0])):
        s_ = (c[0], c[1] + 1)
        if s_ not in content or c in keep_out or s_ in keep_out:
            continue
        # only against a level top's back: two bands one under the other
        # are one band, and parted they open a crack across it
        top_row = grid(s_)[0]
        g = [row[:] for row in grid(c)]
        inner = [v for row in g[1:P] for v in row]
        # - or a terrace's own top, level, against another terrace south of
        # it (Route 106's ledge behind its plateau): the plateau's corner is
        # not the ledge's to climb. Against rock hung from it, it is.
        terrace = (group in WRAP_GROUPS and max(inner) - min(inner) <= 0.5
                   and (ox + c[0], oy + c[1]) not in kinds and (ox + s_[0], oy + s_[1]) not in kinds)
        if sum(1 for v in top_row if v >= max(top_row) - 0.5) < 3 and not terrace:
            continue
        rise = [g[P][i] - g[P - 1][i] for i in range(P + 1)]
        if max(rise) <= STEP + 0.01:
            continue        # a slope's step, not a back
        for i in range(P + 1):
            if rise[i] > 0.01 and not seam(c, i, P):
                g[P][i] = g[P - 1][i]
        if g != grid(c):
            grids[c] = g
    for _ in range(64):
        changed = False
        for c in sorted(content, key=lambda c: (c[1], c[0])):
            changed |= settle(c)
        for at, users in shared.items():
            if len(users) < 2:
                continue
            top = max(grid(c)[j][i] for (c, i, j) in users)
            if top - min(grid(c)[j][i] for (c, i, j) in users) > STEP + 0.01:
                continue        # a silhouette: each its own
            for (c, i, j) in users:
                if grid(c)[j][i] < top:
                    g = [row[:] for row in grid(c)]
                    g[j][i] = top
                    grids[c] = g
                    changed = True
        if not changed:
            break
    # a flank is as steep as it is drawn: beside the ground to its west or
    # east, the rock's outline is followed by a run of shaded pixels - the
    # flank itself, often a dozen pixels across for a drop of two levels, or
    # a single pixel's outline for a cliff - and then the plateau's own top.
    # Across that run the rock rises from the ground to its full height,
    # in the proportion the run is crossed, whatever slope the cells'
    # patterns made of it. The same function of the drawing for every cell,
    # so the points cells share stay shared; heights only come down. The
    # ground has to extend over and under the pixel (beside): the ground over
    # a plateau's scalloped north edge is not a flank's.
    FLANK = 40
    bgmask = {}
    for c in content:
        m = masks.get(c)
        bgmask[c] = m if m is not None else _cut_mask(art, roles_layout, c[0], c[1], content)
    plain_cache, pix_cache = {}, {}
    # nothing they read changes while the flanks are measured: each answer
    # is kept, a pixel being asked about again and again
    ground_cache, crossed_cache = {}, {}
    light = ROCK_TOP | ROCK_RIM

    def is_ground(X, Y):
        v = ground_cache.get((X, Y))
        if v is None:
            v = ground_cache[(X, Y)] = _is_ground(X, Y)
        return v

    def _is_ground(X, Y):
        c = (X // 16, Y // 16)
        if not inside(c) or Y < 0 or X < 0:
            return False
        if c in content:
            m = bgmask.get(c)
            return m is not None and bool((m[Y % 16] >> (X % 16)) & 1)
        if c not in plain_cache:
            plain_cache[c] = max(max(r) for r in grid(c)) <= 0.5
        return plain_cache[c]

    def is_light(X, Y):
        c = (X // 16, Y // 16)
        if c not in pix_cache:
            pix_cache[c] = art.cell_image(art.metatile(*c)).convert("RGB").load()
        return pix_cache[c][X % 16, Y % 16] in light

    def crossed(X, Y):
        if (X, Y) not in crossed_cache:
            crossed_cache[(X, Y)] = _crossed(X, Y)
        return crossed_cache[(X, Y)]

    ground_rows, beside_rows = {}, {}

    def ground_row(Y):
        """[is_ground(x, Y) for every x of the row], a cell at a time."""
        row = ground_rows.get(Y)
        if row is None:
            row = []
            for cx in range(W):
                if Y < 0 or not inside((cx, Y // 16)):
                    row += [False] * 16
                elif (cx, Y // 16) in content:
                    m = bgmask.get((cx, Y // 16))
                    bits = m[Y % 16] if m is not None else 0
                    row += [bool((bits >> x) & 1) for x in range(16)]
                else:
                    c = (cx, Y // 16)
                    if c not in plain_cache:
                        plain_cache[c] = max(max(r) for r in grid(c)) <= 0.5
                    row += [plain_cache[c]] * 16
            ground_rows[Y] = row
        return row

    def beside_row(Y):
        """For row Y: the nearest x beside the ground - the ground over and
        under it too, 2 and 5 rows away - at or west of each x (-1: none),
        and at or east of it (W * 16: none)."""
        near = beside_rows.get(Y)
        if near is None:
            rows = [ground_row(Y + k) for k in (0, -2, 2, -5, 5)]
            n = W * 16
            west, east = [-1] * n, [n] * n
            last = -1
            for x in range(n):
                if all(r[x] for r in rows):
                    last = x
                west[x] = last
            last = n
            for x in range(n - 1, -1, -1):
                if west[x] == x:
                    last = x
                east[x] = last
            near = beside_rows[Y] = (west, east)
        return near

    def _crossed(X, Y):
        """How much of the flank on row Y is behind corner X, 0..1, or None
        where the row has no flank beside it."""
        best = None
        west, east = beside_row(Y)
        for sign in (-1, 1):
            # the first x beside the ground, FLANK + 1 pixels at most from X
            # (X - 1 and down to the west, X and up to the east)
            xg = None
            if sign < 0:
                if X >= 1 and 0 <= west[min(X - 1, W * 16 - 1)] >= X - 1 - FLANK:
                    xg = west[min(X - 1, W * 16 - 1)]
            elif X < W * 16 and east[X] < W * 16 and east[X] <= X + FLANK:
                xg = east[X]
            if xg is None:
                continue
            # the outline's first rock pixel, and the shaded run from it
            x0 = xg + (1 if sign < 0 else -1)
            width = 0
            while width < FLANK and 0 <= x0 + sign * width < W * 16 and not is_ground(x0 + sign * width, Y)                     and not is_light(x0 + sign * width, Y):
                width += 1
            width = max(width, 1)
            far = (X - x0) if sign < 0 else (x0 + 1 - X)  # pixels in from the outline
            frac = min(1.0, max(0.0, far / width))
            best = frac if best is None else min(best, frac)
        return best

    flank_frac = {}
    # a ledge's berm is no flank: brought down across its lip, the run of
    # ledge cells read as rock lay flat while the end of the run, no rock,
    # kept its berm, and the step between them opened a hole
    for c in content:
        for j in range(P + 1):
            for i in range(P + 1):
                if seam(c, i, j) or not rock_point(c, i, j):
                    continue
                X, Y = c[0] * 16 + i * STEP, c[1] * 16 + j * STEP
                around = [crossed(X, row) for row in (Y - 2, Y - 1, Y, Y + 1)]
                if sum(1 for r in around if r is not None) < 3:
                    continue
                d = [r for r in around[1:3] if r is not None]
                if d:
                    flank_frac[(c, i, j)] = sum(d) / len(d)
    for c in sorted(content, key=lambda c: (c[1], c[0])):
        g = [row[:] for row in grid(c)]
        for j in range(P + 1):
            for i in range(P + 1):
                f = flank_frac.get((c, i, j))
                if f is not None and f < 1.0 and g[j][i] > 0:
                    g[j][i] = g[j][i] * f
        if g != grid(c):
            grids[c] = g
    # what a cut tile clears is not there: a point of its lattice beside
    # cleared pixels carries little or none of the drawing, and its height
    # only shapes the rock pixels of the quads it shares with them. Left on
    # the ground, it pulls them down: a band whose north end the drawing cuts
    # on a diagonal (its back, seen at 45 degrees: the band's edge at one
    # depth, each pixel at the band's height) stood its last pixels up as a
    # wall a level tall - shards over the sea, a saw along the outline. Such
    # a point stands as the whole rock south of it in its column does (a
    # band's height goes with its column; no top slopes away north), or,
    # with none there, goes on along its row at the band's own slope. The
    # south edge stays: a face is hung from it (EmitCliffWalls), opaque.
    def rock_quad(mask, a, b):
        return not any((mask[j] >> i) & 1 for j in range(b * STEP, b * STEP + STEP)
                       for i in range(a * STEP, a * STEP + STEP))

    for c, mask in masks.items():
        if c not in content or mask is None or c in keep_out:
            continue
        g0 = grid(c)
        g = [row[:] for row in g0]

        def quads(i, j):
            return [(a, b) for a in (i - 1, i) for b in (j - 1, j) if 0 <= a < P and 0 <= b < P]
        whole = {(i, j) for j in range(P + 1) for i in range(P + 1)
                 if all(rock_quad(mask, a, b) for (a, b) in quads(i, j))}
        bare = {(i, j) for j in range(P + 1) for i in range(P + 1)
                if all(clear_quad(mask, a, b) for (a, b) in quads(i, j))}
        lo = min(min(r) for r in g0)
        hi = max(max(r) for r in g0)
        if hi - lo < LEVEL - 0.5:
            continue        # a ledge's lip, not a mountain's rock: as it is
        T = STEP * SPREAD
        for j in range(P - 1, -1, -1):
            for i in range(P + 1):
                if (i, j) in whole or seam(c, i, j):
                    continue
                if (i == 0 and (c[0] - 1, c[1]) in content) or (i == P and (c[0] + 1, c[1]) in content):
                    continue
                v = None
                if (i, j + 1) not in bare:
                    v = g[j + 1][i]         # the rock below it, already set
                else:
                    best = None
                    for d in (-1, 1):
                        side = [k for k in range(i + d, -1 if d < 0 else P + 1, d) if (k, j) in whole]
                        if not side:
                            continue
                        k1 = side[0]
                        slope = 0.0
                        if len(side) > 1:
                            slope = max(-T, min(T, (g0[j][k1] - g0[j][side[1]]) / float(k1 - side[1])))
                        if best is None or abs(k1 - i) < best[0]:
                            best = (abs(k1 - i), g0[j][k1] + slope * (i - k1))
                    if best is not None:
                        v = best[1]
                if v is not None:
                    g[j][i] = max(lo, min(hi, v))
        if g != g0:
            grids[c] = g
    # the ground behind runs on, too, where what is north of a cell is rock
    # lower than its edge (a band's foot behind a terrace): the ground the
    # rock stands on, the nearest plain ground's drawing
    taken = {(x, y) for (x, y, *_) in cut}
    cut_ground = {(c[0], c[1]): c[5] for c in cut if len(c) > 5 and c[5] is not None}
    have = {(x, y) for (x, y, *_) in fills}
    for c in sorted(content, key=lambda c: (c[1], c[0])):
        n = (c[0], c[1] - 1)
        if n not in content or not inside(n) or c in taken:
            continue
        g, o = grid(c), grid(n)
        # the ground the rock north of it stands on: its lowest point, not
        # the middle of a slope - a flank's row of fills hung a level up
        # (0 for the -16 it stands on) showed as flat sand patches on it
        foot = min(min(r) for r in o)
        if group in WRAP_GROUPS and (n[0], n[1] - 1) not in content:
            # its north edge is shared with the ground behind it: not its foot
            foot = min(min(r) for r in o[1:])
        if max(g[0]) <= foot + 0.5:
            continue
        near = sorted((abs(a) + abs(b), (n[0] + a, n[1] + b)) for a in range(-3, 4) for b in range(-3, 4)
                      if inside((n[0] + a, n[1] + b)) and (n[0] + a, n[1] + b) not in content)
        if not near:
            continue
        # the cell north of it is itself a cut tile: the ground behind that
        # one, which its own drawing names, is the ground behind this
        ground = cut_ground.get(n)
        if ground is None:
            ground = plain_ground(art, roles_layout, content, *near[0][1])
        top = max(max(r) for r in g)
        # a flank or a level north of it - the same height down each of its
        # columns - goes on under this cell's back, a row on: the camera,
        # looking down more steeply than the drawing, finds it past the back
        # - hidden there by this cell, a level top all of rock, as the
        # drawing hides it; anything less leaves it hanging out of the rock
        level = (max(max(r) for r in g) - min(min(r) for r in g) <= 1.0
                 and c not in keep_out and n not in keep_out)
        # - or any rock north of it lower than all of it by half a level
        # (the end of a ledge behind a plateau, a cone): it cannot show over it
        goes_on = level and (all(abs(o[j][i] - o[P][i]) <= 1.0 for j in range(P + 1) for i in range(P + 1))
                             or (group in WRAP_GROUPS
                                 and max(max(r) for r in o) <= min(min(r) for r in g) - LEVEL / 2))
        # what a cut tile north of a top clears is its own ground: seen
        # through it, past the top's back, that ground and not the top's
        # drawing (a light slab of the top flat on the sea behind a cap)
        behind_n = cut_ground.get(n) if level else None
        if behind_n is not None and not _plain_tile(art, behind_n):
            behind_n = None     # no ground found for it: the tile itself, rock and all
        for k in range(int(math.ceil((top - foot) / LEVEL))):
            f = (c[0], c[1] + k)
            if not inside(f) or f in taken or f in have:
                continue
            if min(min(r) for r in grid(f)) < foot - 0.5:
                break           # never over what the cell draws
            if k == 0 and goes_on:
                under = cut_ground.get(n)
                fills.append((f[0], f[1], foot, under if under is not None and _plain_tile(art, under) else None,
                              GOES_ON))
                have.add(f)
                continue
            if behind_n is not None:
                fills.append((f[0], f[1], foot, behind_n))
                have.add(f)
                continue
            # what is seen under a rock tile standing over another, through
            # the gap a camera's perspective opens beside a flank, is more
            # of the rock: the cell's own drawing at the ground's level, not
            # the sand beside (flat yellow patches in the mountain)
            fills.append((f[0], f[1], foot, art.metatile(f[0], f[1])))
            have.add(f)
    # the cliffs: wherever a cell's west, east or south edge stands over its
    # neighbour's (the north one is the back)
    faces = collections.Counter(art.metatile(gx - ox, gy - oy) for (gx, gy), k in kinds.items()
                                if k == "face" and inside((gx - ox, gy - oy)))
    face = faces.most_common(1)[0][0] if faces else NO_FACE
    walls = {}

    def edge_rock(x, y):
        """Bits of the cell's west (0-3) and east (4-7) edge columns, a bit per
        4 pixels down it, set where the column has rock: a wall goes down only
        from the rock's own pixels, never from the sand a flank's corner has."""
        mask = _cut_mask(art, roles_layout, x, y, content)
        if mask is None:
            return 0xFF
        bits = 0
        for k in range(4):
            for j in range(4 * k, 4 * k + 4):
                if not (mask[j] >> 0) & 1:
                    bits |= 1 << k
                if not (mask[j] >> 15) & 1:
                    bits |= 1 << (4 + k)
        return bits

    for y in range(Hh):
        for x in range(W):
            if (x, y) not in content:
                continue
            g = grid((x, y))
            for (dx, dy) in ((-1, 0), (1, 0), (0, 1)):
                n = (x + dx, y + dy)
                if not inside(n):
                    continue
                o = grid(n)
                if dx < 0:
                    gap = max(g[k][0] - o[k][P] for k in range(P + 1))
                elif dx > 0:
                    gap = max(g[k][P] - o[k][0] for k in range(P + 1))
                else:
                    gap = max(g[P][k] - o[0][k] for k in range(P + 1))
                if gap > 0.5:
                    # west and east the rock's own edge goes down; south
                    # the mountain's face (VOXEL_RELIEF_NO_FACE: none)
                    walls[(x, y)] = (face, edge_rock(x, y))
    if alias_of(lid):
        # a map read off its drawing only now (alias_of): its ledges and its
        # berry soil stay exactly as they were, the berm on the ground
        for c in keep_out:
            grids.pop(c, None)
            walls.pop(c, None)
        fills = [f for f in fills if (f[0], f[1]) not in keep_out]
    return grids, fills, walls


def export(layout_ids, path):
    """"VXL4", u16 layouts, u16 per-cell lattice side (5), then per layout
    u16 layout id, u16 cells, u16 width, u16 height, u32 offset, s16 base; cells
    are u8 x, u8 y and 25 signed bytes of height, row major, over the base: the
    level the whole map is lifted to (world_levels), pixels. A map with a base
    and no relief is written with no cells. Bit 15 of the height marks
    a map read off its drawing (DRAWN): its relief is the whole of the terrain,
    and every blocked cell of rock is written, level or not. Bit 14 says its
    heights are in units of HEIGHT_UNIT pixels (a mountain stands taller than
    a byte of pixels). A point's depth is its height, so it is not written;
    bit 15 of the width (set by sea_ramps) says it has none, the heights
    lifting the map straight up as its base does. Bit 14 of the width (WIDE,
    voxel_relief_fixes.pack: hand marks past 16 levels) says its cells hold
    25 s16 heights in pixels instead of bytes."""
    layouts = json.load(open(os.path.join(vb.ROOT, "data", "layouts", "layouts.json"),
                             encoding="utf-8"))["layouts"]
    index = {e["id"]: i + 1 for i, e in enumerate(layouts)}
    tables = []
    world = world_levels()["base"]
    layout_ids = list(layout_ids) + sorted(
        l for l, b in world.items() if b and l not in layout_ids and drawn_group(l) is None)
    variants, cuts = {}, []     # (pair, metatile, mask) -> index; (layout, x, y, index, foot)
    for lid in layout_ids:
        roles_layout, h, ledges = layout_heights(lid)
        drawn = drawn_group(lid) is not None
        # the cut tiles first: they level the lattice under their rock
        cut = cut_cells(lid, roles_layout, h) if drawn else []
        cells = relief_cells(roles_layout, h)
        # a drawn map's heights in steps of HEIGHT_UNIT (a mountain is taller
        # than a byte of pixels); one only now drawn (alias_of) whose heights
        # fit keeps every pixel, its ledges' berms exactly as they were
        unit = HEIGHT_UNIT if drawn else 1
        if drawn and alias_of(lid) and all(-120 <= v <= 120 for row in h for v in row if v is not None):
            unit = 1
        if drawn:
            art = layout_art(lid)
            have = {(x, y) for (x, y, _) in cells}
            flatp = {}
            for (x, y, m, mask, foot, ground) in cut:
                if ground is not None and _plain_background(art, m, mask, ground):
                    flatp[(x, y)] = 1
                # the console's own tile there (alias_of), cut as its twin is
                m = art.own_metatile(x, y) if hasattr(art, "own_metatile") else m
                ground = None if ground is None else own_id(lid, ground)
                key = (art.primary if m < 512 else art.secondary, m, tuple(mask))
                if key not in variants:
                    variants[key] = (len(variants), index[lid])
                k = variants[key][0]
                # on the stored lattice's own steps, which the engine
                # compares it with
                cuts.append((index[lid], x, y, k, unit * int(round(foot / unit)),
                             0xFFFF if ground is None else ground))
                if (x, y) not in have:      # a level cell must be written to be drawn so
                    cells.append((x, y, cell_grid(h, x, y)))
                    have.add((x, y))
            # every cell's own shape (cell_shapes): its grid, the ground
            # behind run on under it (variant 0xFFFF), its cliff walls
            grids, fills, walls = cell_shapes(lid, roles_layout, h, cut)
            goes_on = set()
            for (x, y, foot, ground, *flag) in fills:
                cuts.append((index[lid], x, y, 0xFFFF, unit * int(round(foot / unit)),
                             0xFFFF if ground is None else own_id(lid, ground)))
                if flag and flag[0] == GOES_ON:
                    goes_on.add((x, y))
            got = {(x, y): k for k, (lay, x, y, *rest) in enumerate(cuts) if lay == index[lid]}
            for (x, y), face in walls.items():
                if (x, y) not in got:
                    cuts.append((index[lid], x, y, 0xFFFF, 0, 0xFFFF))
                    got[(x, y)] = len(cuts) - 1
            for (x, y), k in got.items():
                wall = walls.get((x, y), (0xFFFF, 0xFF))
                if wall[0] not in (0xFFFF, NO_FACE):
                    wall = (own_id(lid, wall[0]),) + tuple(wall[1:])
                cuts[k] = cuts[k][:6] + wall + (
                    flatp.get((x, y), 0) | (GOES_ON if (x, y) in goes_on else 0),)
            for (x, y) in list(got) + list(grids):
                if (x, y) not in have:
                    cells.append((x, y, cell_grid(h, x, y)))
                    have.add((x, y))
            cells = [(x, y, grids.get((x, y), g)) for (x, y, g) in cells]
            cells.sort(key=lambda c: (c[1], c[0]))
        if drawn:
            have = {(x, y) for (x, y, _) in cells}
            cells += [(x, y, cell_grid(h, x, y)) for y in range(roles_layout.h)
                      for x in range(roles_layout.w)
                      if (x, y) not in have and (
                          (roles_layout.blocked(x, y)
                           and roles_layout.role_at(x, y) in RELIEF_ROLES)
                          or any(abs(v) > 0.25 for row in cell_grid(_SHIFT[lid], x, y)
                                 for v in row))]
            cells.sort(key=lambda c: (c[1], c[0]))
        lift = _BASE.get(lid, 0) if drawn else world.get(lid, 0)
        if not cells and not lift:
            continue
        cells = [(x, y, [[v / unit for v in row] for row in g]) for (x, y, g) in cells]
        tables.append((index[lid], cells, roles_layout.w,
                       roles_layout.h | (0x8000 if drawn else 0) | (0x4000 if unit == 2 else 0), lift))
        print("relief %-34s %4d cells lifted, %3d ledge cells, map at %+d px"
              % (lid, len(cells), ledges, lift))
    side = PER_CELL + 1
    head = MAGIC + struct.pack("<HH", len(tables), side)
    offset = len(head) + 14 * len(tables)
    body = bytearray()
    idx = bytearray()
    for lid, cells, w, hh, lift in sorted(tables):
        idx += struct.pack("<HHHHIh", lid, len(cells), w, hh, offset + len(body), int(lift))
        for (x, y, g) in cells:
            body += struct.pack("<BB", x, y)
            body += struct.pack("<%db" % (len(g) * len(g[0])),
                                *(max(-128, min(127, int(round(v)))) for row in g for v in row))
    blob = head + idx + body
    # the cut tiles, after the cells: u16 variants, u16 cells; variants x (u16
    # layout drawing its tileset, u16 metatile, u16 rows[16], bit set where
    # the background is); cells x (u16 layout, u8 x, u8 y, u16 variant, s16
    # foot in pixels over the base, u16 the metatile of the ground behind it
    # or 0xFFFF, u16 the metatile its cliff walls are drawn with or 0xFFFF,
    # u8 sides, u8 flags: voxel_relief.c has their bits; 1, plain ground
    # behind, is no longer read), by layout, x and y; variant 0xFFFF is no
    # cut: a cell under which the ground behind a terrace's rim runs on
    # (rim_cells), or one with cliffs only (cell_shapes). Then u32 the
    # table's offset and "CUTS"
    cuts = [c + ((0xFFFF, 0xFF, 0)[len(c) - 6:] if len(c) < 9 else ()) for c in cuts]
    table = struct.pack("<HH", len(variants), len(cuts))
    for (pair, m, mask), (k, lay) in sorted(variants.items(), key=lambda kv: kv[1][0]):
        table += struct.pack("<HH16H", lay, m, *mask)
    for (lay, x, y, k, foot, behind, wall, sides, flat) in sorted(cuts):
        table += struct.pack("<HBBHhHHBB", lay, x, y, k, foot, behind, wall, sides, flat)
    blob = bytes(blob) + table + struct.pack("<I", len(blob)) + b"CUTS"
    print("voxel relief: %d cut tiles over %d cells" % (len(variants), len(cuts)))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, "wb").write(blob)
    print("voxel relief: %d layouts, %.1f KiB -> %s" % (len(tables), len(blob) / 1024.0, path))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preview", default=None)
    ap.add_argument("--layouts", default=None,
                    help="default: the solved maps and every map with a ledge")
    ap.add_argument("--output", default=None)
    ap.add_argument("--at", default=None, help="x,z of the preview target")
    ap.add_argument("--proof", default=None,
                    help="directory: each drawn group's terraces over its drawing")
    args = ap.parse_args()
    if args.layouts:
        lids = args.layouts.split(",")
    else:
        drawn = [l for members in DRAWN.values() for l in members]
        lids = list(ENABLED) + [l for l in drawn if l not in ENABLED]
        lids += [l for l in ledge_layouts() if l not in lids]
        lids = list(dict.fromkeys(lids))   # a neighbour is in its alternate's group too
    if args.preview:
        os.makedirs(args.preview, exist_ok=True)
        tx, tz = (float(v) for v in args.at.split(",")) if args.at else (20.0, 60.0)
        cams = [("game", (tx, tz, 40.0, 0.0, 12.0)), ("yaw40", (tx, tz, 35.0, 40.0, 11.0)),
                ("yaw-45", (tx, tz, 30.0, -45.0, 10.0)), ("high", (tx, tz, 60.0, 15.0, 12.0)),
                ("near", (tx, tz, 40.0, 0.0, 7.0))]
        for lid in lids:
            for p in preview(lid, args.preview, cams):
                print("preview:", p)
    if args.proof:
        os.makedirs(args.proof, exist_ok=True)
        for name in DRAWN:
            print("proof:", proof(name, os.path.join(args.proof, "terraces_%s.png" % name)))
    if args.output:
        export(lids, args.output)
        # the export as the terraces read it (their reference: the world
        # placement they keep), for the voxel editor to solve its groups
        # against the same relief the build does
        if os.environ.get("VOXEL_RELIEF_EXPORT"):
            shutil.copy2(args.output, os.environ["VOXEL_RELIEF_EXPORT"])
        # the mountains of the maps chosen so far, counted tile by tile
        # (voxel_terraces.py) over the solve, in the world it placed
        import voxel_terraces
        voxel_terraces.apply_file(args.output)
        # the level a step seam gives up, as a slope across the open sea
        apply_sea_ramps(args.output)
        # no cut tile's ground in a pit under the ground beside it
        apply_cut_feet(args.output)
        # the hand corrections (assets/voxel/relief_fixes.json) over the drawing's
        if not os.environ.get("VOXEL_RELIEF_NO_FIXES"):
            voxel_relief_fixes.apply_file(args.output)


if __name__ == "__main__":
    main()
