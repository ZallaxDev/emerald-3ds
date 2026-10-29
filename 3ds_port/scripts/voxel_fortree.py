#!/usr/bin/env python3
"""Fortree City: the tree houses and the rope bridges between them.

Fortree is a town in the treetops. Its houses stand on decks laid over the
wood, the decks are joined by bridges of planks, and a ladder comes down
from each deck to the forest floor. The cartridge says how high: the decks
and bridges are walked at elevation 4 over the elevation-3 ground, the
ladders are elevation 0 (any), and where the path crosses under the
southern bridge (x 30-33, row 14) the cells are elevation 15 - walked over
at 4 and under at 3. A height field cannot hold that, so the decks are
models, like the Seaside Cycling Road (voxel_bridge.py), and the console
lifts whoever stands on them (the deck table of buildings.bin).

How high
--------
The drawing is the 45-degree view: a point (X, Y, Z) lands on the art at
u = X, v = Z - Y. A ladder is drawn from the deck's front edge (row 80 of
the town) to its foot on the path (row 108): 28 rows, so it stands 28 px
tall, and the deck is DECK = 28 px over the ground. Every row of the deck
is drawn where the projection puts it at that height, so the whole house
is built at its drawn rows and lifted by DECK (vb.Lifted: v = Z - Y does
not change) - exact at 45 degrees, standing on the treetops from the
game's camera.

  the deck     planks running east-west, rows 36-79 of a house, its front
               edge three rows (80-82): a slab DECK - 3 to DECK
  the hut      walls 25 px tall on the deck, its front corners cut at 45
               degrees (the slanting walls beside the door)
  the roof     fronds over the hut: a low frustum of flat faces, the
               drawing projected on it; the drawing's outline cuts it
  the ladder   one upright face at the deck's front edge, its rungs and
               rails the drawing, the forest seen between them
  a bridge     its planks drawn 13 rows deep and two of front: a slab
               DECK - 2 to DECK under each plank, the gaps between them
               open, the two ropes across a strip at the planks' height

Under them
----------
The drawing shows the forest's canopy under a deck and between a bridge's
planks; in the voxel world the wood is its trees. The cells the houses,
decks and bridges are drawn on are small trees (voxel_tree.c FortreeWood),
their crowns (23 px) under the decks' planks (25 px), so the wood goes on
under the town and nothing is left hollow.

The palms
---------
See `palm`: a card cut out of the drawing, leaning back like the General's
trees, exact at 45 degrees.
"""

import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import voxel_building as vb  # noqa: E402

LAYOUT = "LAYOUT_FORTREE_CITY"

DECK = 28           # px: the decks and bridges over the forest floor
SLAB = 3            # px: a deck's front edge, drawn rows 80-82
PLANK = 2           # px: a bridge plank's front, drawn under its top
WALL = 25           # px: the hut's walls over the deck
EAVE = 21           # px: the roof's foot over the deck, behind the walls' top

# A bridge's planks: the colours of their tops and fronts (the ropes across
# are the outline's dark blue, k)
PLANK_RGB = {(65, 74, 106), (238, 246, 189), (205, 213, 164), (172, 180, 123), (131, 139, 74)}

# The metatiles the decks, bridges and ladders are drawn with (Fortree's own)
DECK_TILES = {0x233, 0x237, 0x23B, 0x23C, 0x23D, 0x23E, 0x23F}
DOOR = 0x235
BRIDGE_TILES = {0x256, 0x24E}
LADDER_TOP, LADDER_FOOT = 0x245, 0x24D


class ProjPoly:
    """A flat polygon (x, y, z) laid with the drawing projected: exact at
    45 degrees wherever it stands (a ladder's face)."""

    def __init__(self, name, pts):
        self.name, self.pts = name, [tuple(map(float, p)) for p in pts]

    def emit(self, mesh):
        q = [(x, y, z, x, z - y) for (x, y, z) in self.pts]
        mesh.poly(q, vb.SHADE_ART, self.name + "~proj")


class LeafRoof:
    """A roof of fronds: a frustum from the plan `base` at y0 to `top` at
    y1, its first face the front (base[0]-base[1] under top[0]-top[1]).

    The faces the 45-degree view sees in front take the drawing projected:
    exact, the drawing's outline cutting the fronds' tips. Every other
    face is laid with the front's own fronds, as the front shows them: a
    point a way along its foot and up its slope takes the front's texel
    as far along and as far up (the sides, the cut corners behind, the
    back, which the game's camera never sees). The top takes the drawing
    on from the front's top edge, mirrored there, so the fronds run on
    over it without a seam. Those faces read the grown band (`~behind`),
    so the tips' gaps are no holes in them."""

    def __init__(self, name, base, y0, run, y1, cols, foot):
        self.name = name
        self.base = [tuple(map(float, p)) for p in base]
        # every face rises `run` inwards: one pitch all round
        self.top = vb.inset_plan(self.base, run)
        self.y0, self.y1 = float(y0), float(y1)
        # the front's columns of fronds the other faces take, and the row
        # its foot is drawn on
        self.cols, self.foot = cols, foot

    def emit(self, mesh):
        b, t, n = self.base, self.top, len(self.base)
        cx = sum(p[0] for p in b) / n
        cz = sum(p[1] for p in b) / n
        for i in range(n):
            j = (i + 1) % n
            f = [(b[i][0], self.y0, b[i][1]), (b[j][0], self.y0, b[j][1]),
                 (t[j][0], self.y1, t[j][1]), (t[i][0], self.y1, t[i][1])]
            e1 = [f[1][k] - f[0][k] for k in range(3)]
            e2 = [f[3][k] - f[0][k] for k in range(3)]
            nrm = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                   e1[0] * e2[1] - e1[1] * e2[0]]
            mx, mz = (f[0][0] + f[1][0]) / 2 - cx, (f[0][2] + f[1][2]) / 2 - cz
            if nrm[0] * mx + nrm[2] * mz < 0:
                nrm = [-k for k in nrm]
            L = math.sqrt(sum(c * c for c in nrm))
            if nrm[2] / L > 0.3:
                # turned south: the front and its cut corners, as drawn
                q = [p + (p[0], p[2] - p[1]) for p in f]
                vb.PlanPrism._tri(mesh, (q[0], q[1], q[2]), nrm, vb.SHADE_ART, self.name + "~proj")
                vb.PlanPrism._tri(mesh, (q[0], q[2], q[3]), nrm, vb.SHADE_ART, self.name + "~proj")
                continue
            # the front's fronds, row for row at the same height (the
            # faces share the front's pitch): a texel a pixel along the
            # face, the fall line as the projection lays it on the front
            P0 = f[0]
            h = _unit([f[1][c] - f[0][c] for c in range(3)])
            rise = [f[3][c] - f[0][c] for c in range(3)]
            hr = sum(rise[c] * h[c] for c in range(3))
            up = [rise[c] - hr * h[c] for c in range(3)]
            D = math.sqrt(sum(c * c for c in up))
            up = [c / D for c in up]
            beta = math.asin(min(1.0, math.hypot(nrm[0], nrm[2]) / L))
            k = math.cos(beta) + math.sin(beta)
            tile = vb.Tile(self.cols[0], self.foot - D * k, self.cols[1], self.foot, s0=7.0 * i)
            loc = []
            for p in f:
                d = [p[c] - P0[c] for c in range(3)]
                loc.append((sum(d[c] * h[c] for c in range(3)),
                            (D - sum(d[c] * up[c] for c in range(3))) * k))
            shade = vb.SHADE_EAST if nrm[0] / L > 0.5 else vb.SHADE_WEST if nrm[0] / L < -0.5 \
                else vb.SHADE_BACK
            for piece in vb.tile_pieces(loc, tile, None, None):
                pts = [tuple(P0[c] + ps * h[c] + (D - pt / k) * up[c] for c in range(3)) + (u, v)
                       for (ps, pt, u, v) in piece]
                for m in range(1, len(pts) - 1):
                    vb.PlanPrism._tri(mesh, (pts[0], pts[m], pts[m + 1]), nrm, shade,
                                      self.name + ".side~behind")
        # the top: the drawing mirrored at the front's top edge
        seam = t[0][1]
        edge = seam - self.y1
        top = [(x, self.y1, z, x, edge + (seam - z)) for (x, z) in t]
        for k in range(1, n - 1):
            vb.PlanPrism._tri(mesh, (top[0], top[k], top[k + 1]), (0.0, 1.0, 0.0), vb.SHADE_ART,
                              self.name + ".top~behind")


def _unit(v):
    L = math.sqrt(sum(c * c for c in v))
    return [c / L for c in v]


def plank_runs(art, row, x0=0, x1=None):
    """[(u0, u1)]: the bridge planks crossing art row `row` (their tops'
    middle), as runs of their colours at least 4 px wide."""
    px = art.load()
    W = art.size[0] if x1 is None else x1
    runs, start = [], None
    for u in range(x0, W + 1):
        on = u < W and px[u, row][3] >= 128 and px[u, row][:3] in PLANK_RGB
        if on and start is None:
            start = u
        elif not on and start is not None:
            if u - start >= 4:
                runs.append((start, u))
            start = None
    return runs


def bridge_planks(art, top, x0=0, x1=None):
    """The planks of a bridge drawn in the art between columns x0-x1, their
    tops from row `top` (13 rows, then PLANK rows of front): a slab under
    each, and the ropes drawn across the gaps (rows top + 3 and top + 9) as
    flat strips at the planks' height. Built at the ground, to be lifted."""
    parts = []
    for k, (u0, u1) in enumerate(plank_runs(art, top + 6, x0, x1)):
        parts.append(vb.PlanPrism("plank%d_%d" % (top, k),
                                  [(u0, top), (u1, top), (u1, top + 12), (u0, top + 12)],
                                  -PLANK, 0, (u0, u1, top + 12 + PLANK)))
    W = art.size[0] if x1 is None else x1
    px = art.load()
    for r in (top + 3, top + 9):
        run = None
        for u in range(x0, W + 1):
            on = u < W and px[u, r][3] >= 128 and px[u, r][:3] == (65, 74, 106)
            if on and run is None:
                run = u
            elif not on and run is not None:
                parts.append(ProjPoly("rope%d_%d" % (r, run),
                                      [(run, -0.2, r), (u, -0.2, r), (u, -0.2, r + 1), (run, -0.2, r + 1)]))
                run = None
    return parts


def treehouse(art, front, ladder=None):
    """A tree house drawn in a 5-cell-wide rectangle whose top row holds
    the roof's fronds: the hut's front wall foot on row 48 of the art, the
    deck's front edge on row `front` (64, or 80 for the deeper deck), the
    ladder (if any) in columns ladder..ladder + 16 from there to its foot
    DECK rows lower. Every part at its drawn rows, lifted by DECK."""
    deck_back = 19
    hut_front, hut_back = 48, 16
    deck = vb.PlanPrism("deck", [(8, deck_back), (72, deck_back), (72, front), (8, front)],
                        -SLAB, 0, (9, 17, front + SLAB))
    hut = vb.PlanPrism("hut", [(24.5, hut_front), (55.5, hut_front), (62, hut_front - 6.5),
                               (62, hut_back), (18, hut_back), (18, hut_front - 6.5)],
                       0, WALL, (27, 31, hut_front))
    # the fronds: their foot along the walls' top at the front (row 39 of
    # the town), down to row 36 over the cut corners; their tips on rows
    # 17-20, the frustum's top edge there
    roof = LeafRoof("roof", [(24.5, 44), (55.5, 44), (63.5, 41), (63.5, 11), (56, 8), (24, 8),
                             (16.5, 11), (16.5, 41)], EAVE, 8, EAVE + 13, (26, 54), 23)
    lifted = [deck, hut, roof]
    # the planks of a bridge drawn in the deck's end cells, past its edge
    lifted += bridge_planks(art, front - 15, 0, 8) + bridge_planks(art, front - 15, 72, 80)
    parts = [vb.Lifted(p, DECK) for p in lifted]
    if ladder is not None:
        z = front + DECK + 0.5
        parts.append(ProjPoly("ladder", [(ladder, 0, z), (ladder + 16, 0, z),
                                         (ladder + 16, DECK + 2, z), (ladder, DECK + 2, z)]))
    return parts


def treehouse_exact(front, ladder=None):
    """What a tree house reproduces pixel for pixel: the roof, hut and deck,
    and the ladder."""
    # (the fronds' tips past the roof's sides, x 16-17 and 62-63 over row
    # 17, are its sides', laid with the front's fronds)
    out = [(18, 0, 62, 17), (0, 17, 80, front + SLAB)]
    if ladder is not None:
        out.append((ladder, front, ladder + 16, front + DECK))
    return out


def bridge_cell(art):
    """One cell of a bridge (Fortree's 0x256 over the wood, 0x24E over the
    path): its planks, lifted."""
    return [vb.Lifted(p, DECK) for p in bridge_planks(art, 1)]


# ── The deck table ─────────────────────────────────────────────────────────

def deck_table():
    """[(layout id, x, y, top, bottom, elevation)]: the cells walked up on a
    deck or a bridge (lifted DECK; where the path passes under, only at
    elevation 4) and the ladders (lifted as the player climbs: the foot of
    the ladder stays where it is drawn, DECK rows south of the deck's
    front - a 45-degree ramp, top to bottom of each cell)."""
    la = vb.LayoutArt(LAYOUT)
    out = []
    for y in range(la.h):
        for x in range(la.w):
            m = la.metatile(x, y)
            elev = (la.blocks[y * la.w + x] >> 12) & 0xF
            if m in DECK_TILES or m in BRIDGE_TILES or m == DOOR:
                out.append((LAYOUT, x, y, DECK, DECK, 4 if elev == 15 else 0))
            elif m == LADDER_TOP:
                out.append((LAYOUT, x, y, DECK, DECK - 16, 0))
            elif m == LADDER_FOOT:
                out.append((LAYOUT, x, y, DECK - 16, 0, 0))
    return out


# ── The palms ──────────────────────────────────────────────────────────────
#
# Fortree's tall trees are drawn three cells wide: a crown of fronds two
# rows deep on the upper layer over the wood's canopy, and a trunk on the
# lower layer, one cell (0x25C) or two (0x241 over 0x273) - the lowest with
# the crown of the small tree in front drawn over its foot. Like the
# General's trees (voxel_tree.c) a palm is a card leaning back from its foot
# as the camera looks down on it; here the card is the drawing itself, cut
# out of the map: its fronds and trunk, the trunk whole down to the ground
# (the small tree in front is a tree of its own). It leans PALM_TILT
# degrees, and is as long as makes it, at 45 degrees, the drawing pixel for
# pixel: every point of it is the drawing projected.

PALM_TILT = 50.0
PALM_TRUNKS = {0x25C: 0x241, 0x273: 0x273}    # trunk cell: whose lower layer


def palm(art, trunk_cells):
    """A palm's card over its art (three cells wide): the drawing with the
    trunk's cells drawn from their lower layer alone, leaning back from the
    foot of the art."""
    from PIL import Image
    la = _layout()
    tex = art.copy()
    px = tex.load()
    for (cx, cy, m) in trunk_cells:
        low = la.subtiles(PALM_TRUNKS[m], 0)
        for q in range(4):
            for k in range(64):
                rgb = low[q][k][0]
                px[cx * 16 + (q & 1) * 8 + k % 8, cy * 16 + (q >> 1) * 8 + k // 8] = tuple(rgb) + (255,)
    W, H = tex.size
    a = math.radians(PALM_TILT)
    s = H / (math.cos(a) + math.sin(a))
    top_y, top_z = s * math.sin(a), H - s * math.cos(a)
    card = ProjPoly("palm", [(0, 0, H), (W, 0, H), (W, top_y, top_z), (0, top_y, top_z)])
    return [card], tex


_LA = []


def _layout():
    if not _LA:
        _LA.append(vb.LayoutArt(LAYOUT))
    return _LA[0]
