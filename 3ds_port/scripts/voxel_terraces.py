#!/usr/bin/env python3
"""Mountains read as the tileset builds them: whole levels, one tile a level.

The General tileset draws a mountain with a handful of square pieces, and
every one of them spans exactly one level (LEVEL pixels):

  top    flat, the terrace's surface (a rim tile is a top whose north edge
         is its back: the ground behind it is lower, never seen falling)
  face   the south wall, vertical: one tile down is one level down
  band   the west or east side, a 45-degree slope one tile wide: one tile
         across is one level down (high side towards the terrace)
  end    a face's end (074, 07d, 089, 07b): one level, falling the way its
         neighbours fall; down a column, the end of one row's face over the
         other end of the next row's is a diagonal edge - the two are one
         level, not two

So the heights of a mountain are not solved, they are counted, and every
point of it lies on a plane:

1. Cells are read off their pixels: ground (sand, grass, water), top, band
   (by a table: the hatching does not say which way it falls), other rock,
   or unknown (trees, boulders, props: the ground under them is not drawn).
2. A column of k levels of rock between two flat cells says the upper one
   stands k levels over the lower; a row of k bands says the same across.
   Terraces are the tops joined top to top (not across a rim's back) and the
   grounds joined ground to ground - but never two cells a face apart (a
   pier joining a beach to the land over it belongs to one of them).
   Every column and row votes; the levels are taken greatest vote first, a
   vote that contradicts what is already decided is dropped (it becomes a
   wall, never a bent surface). Each block of terraces the votes tie is
   then lifted to where the world already stands (the relief the world
   solver placed), so no seam with a neighbouring map moves.
3. Every rock cell has its upper level U: the one it hangs from, a level
   less for each level of rock above it in its stack. Where nothing counts
   it, it takes the level that leaves the most of its corners shared.
4. The heights live on the CORNERS of the cells (a 16-pixel lattice). A
   corner shared by cells that can all reach one level is that level for
   all of them, so neighbours meet without a crack; where they cannot, the
   edge is a step (a terrace's back, a wall drawn with the face).
5. A cell's surface follows from its four corners and nothing else: level,
   a plane (face or band), a hip (one corner high: two planes meeting on
   the diagonal, a convex corner) or a valley (one corner low: concave).

The result is exact at 45 degrees ((u, h, v + h) for any h) and made only of
planes, at any angle.

    python voxel_terraces.py LAYOUT_ROUTE106 [--png OUT --box x0,y0,x1,y1]
                             [--relief IN.bin OUT.bin] [--check]

gen_voxel_relief.py runs it over its own output for the maps in GROUPS
(VOXEL_TERRACES=all|none|LAYOUT_A,LAYOUT_B to choose others).
"""

import argparse
import collections
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import gen_voxel_relief as gr  # noqa: E402
import voxel_building as vb  # noqa: E402
import voxel_props  # noqa: E402

LEVEL = 16
N = 4                       # lattice steps per cell side (5 x 5 points)
TOP_COLOURS = gr.ROCK_TOP | gr.ROCK_RIM
FACE_COLOURS = gr.ROCK_FACE
FLECK = gr.ROCK_FLECK

# The sides, by the way they fall (gen_voxel_relief.SIDE_WEST/EAST and their
# heads, drawn under a rim at the top of a band, and feet over the ground).
# The same drawn over the sea (Route 105's ridges, the rocks off Route 106)
# are 172-17c.
BAND_WEST = {0x070, 0x073, 0x088, 0x06b, 0x080, 0x083, 0x068,     # high side east
             0x172, 0x175, 0x177, 0x17a}
BAND_EAST = {0x072, 0x075, 0x0a2, 0x08a, 0x06d, 0x082, 0x084, 0x06a,  # high side west
             0x174, 0x176, 0x17c,
             0x0a4}     # (080 mirrored: the outer flank over sand - Route 111's cave mound)
# A band's head over the ground behind it (over grass, over sand): its
# diagonal is the end of what stands under it. Over a band of its side (or
# over the ground) it is that band's head; over a terrace's top it is the
# corner where that terrace's back turns (Route 116's back stepping north a
# row: the corner read as a band rose out of the terrace's outline as a
# ramp) - a top, the ground behind cut off along the diagonal.
HEADS = {0x068: "W", 0x083: "W", 0x080: "W", 0x06a: "E", 0x084: "E", 0x082: "E"}
# Drawn whole on a terrace: a boulder's two halves are its top.
ON_TOP = gr.BOULDER
BOULDER_FOOT = {0x09b, 0x09c}
BOULDER_HEAD = {0x093, 0x094}
# Boulders drawn on whatever ground they stand on: the ground round them.
LOOSE = {0x0df, 0x0e0, 0x0e1, 0x0e2}
# The General tileset's steps carved in a face (Route 111's flights through
# its two-level cliffs): the regions read them as floor, their bands too
# soft for treads, and as ground they sank to the foot of the face, a notch
# with walls either side.
CARVED_STEPS = {0x0e8, 0x0e9, 0x0ea, 0x0eb}
# A diagonal edge is drawn as a staircase of face ends: the end of one
# row's face over the other end of the next row's. Down a column the two are
# one wall, one level - not two faces.
UPPER_END = {0x074, 0x089}
LOWER_END = {0x07d, 0x07b, 0x078, 0x092, 0x090}

# The mountain's own tiles (the General tileset's rock, its rock over the
# sea, faces with doors and caves): nothing else is ever rock, whatever
# colours it is drawn in - soil, a sign, a house's wall share them.
MOUNTAIN = (set(range(0x064, 0x0c0)) | set(range(0x172, 0x193)) | {0x1a0, 0x1a8, 0x1b0, 0x1f0, 0x1f1}
            | gr.FACE_SOUTH | gr.BOULDER) - gr.DIRT

GROUND, TOP, ROCK, UNKNOWN = "G", "T", "R", "?"
WORLD = ("world",)      # the level the world is counted from
MAX_FALL = 3 * 16       # pixels: the most one tile of rock is stretched to fall
ANCHOR_MIN = 12         # cells: ground this big reaching a map's edge keeps its place


# -- 1. cells -----------------------------------------------------------------

_CLASSES = {}


def pixel_classes(art, m):
    """16 x 16 of 'T' (top or rim), 'F' (face) or 'G' (anything else) for
    metatile m; a fleck (a colour of both) goes with the most of its
    neighbours."""
    key = (art.primary, art.secondary, m, gr.reading(art))
    if key in _CLASSES:
        return _CLASSES[key]
    img = art.cell_image(m).load()
    cls = [["G"] * 16 for _ in range(16)]
    # a tile of one flat colour is no face (a face is always textured):
    # a flat floor - the dark floor of Sootopolis's crater seen from Route
    # 126 shares the General face's darkest colour
    if m >= 0x200 and len({img[x, y][:3] for y in range(16) for x in range(16)}) == 1:
        if img[0, 0][:3] in FACE_COLOURS:
            cls = [["T"] * 16 for _ in range(16)]
        _CLASSES[key] = cls
        return cls
    flecks = []
    top, face, fleck = TOP_COLOURS, FACE_COLOURS, FLECK
    for y in range(16):
        for x in range(16):
            c = img[x, y][:3]
            if c in top:
                cls[y][x] = "T"
            elif c in face:
                cls[y][x] = "F"
            elif c in fleck:
                flecks.append((x, y))
    for x, y in flecks:
        votes = collections.Counter(cls[ny][nx] for ny in range(max(0, y - 1), min(16, y + 2))
                                    for nx in range(max(0, x - 1), min(16, x + 2))
                                    if (nx, ny) not in flecks)
        votes.pop("G", None)
        cls[y][x] = votes.most_common(1)[0][0] if votes else "T"
    _CLASSES[key] = cls
    return cls


def rim_pixels(art, m):
    img = art.cell_image(m).load()
    return sum(1 for y in range(6) for x in range(16) if img[x, y][:3] in gr.ROCK_RIM)


_TWINS = {}
TWIN_MATCH = 0.85       # share of rock pixels (and their kind) that must agree
HEAD_MATCH = 0.6        # share of a band head's face pixels (twin, maps read in alias)


def twin(art, m):
    """The General tileset's mountain tile a map's own tile is drawn as (a
    copy in its secondary tileset: Dewford's bands), or m itself."""
    if m < 0x200:
        return m
    key = (art.primary, art.secondary, m, gr.reading(art))
    if key not in _TWINS:
        mine = pixel_classes(art, m)
        rock = {(x, y) for y in range(16) for x in range(16) if mine[y][x] != "G"}
        best, score = m, 0.0
        # the very drawing of one first (a tile recoloured, read in the
        # General's colours: gen_voxel_relief.alias_of): pixel classes alone
        # cannot tell a terrace's top from its rim (both all top), and
        # Route 115's tops were read as rims, every row a terrace of its own
        drawing = art.cell_image(m).convert("RGB").tobytes()
        exact = [r for r in sorted(MOUNTAIN) if r < 0x200
                 and art.cell_image(r).convert("RGB").tobytes() == drawing]
        if exact and art.primary == "gTileset_General":
            best, score = exact[0], 1.0
        elif len(rock) >= 64 and art.primary == "gTileset_General":
            # a tie goes to the tile drawn nearest it, colour by colour: a
            # top drawn all top is the plain top, not the rim with its lip
            # (Lavaridge's 271 read as 06c, every row of its top a terrace
            # of its own, the face between two of them laid flat; Route
            # 113's patches of soil read in the 068 set, not the 080 set
            # they are drawn as, stood its west end up in blocks)
            img = art.cell_image(m).convert("RGB").getdata()
            far = None
            for r in sorted(MOUNTAIN):
                if r >= 0x200:
                    continue
                theirs = pixel_classes(art, r)
                both = {(x, y) for y in range(16) for x in range(16) if theirs[y][x] != "G"} | rock
                same = sum(1 for (x, y) in both if theirs[y][x] == mine[y][x]) / len(both)
                if same < score or same < TWIN_MATCH:
                    continue
                d = sum(abs(a - b) for p, q in zip(img, art.cell_image(r).convert("RGB").getdata())
                        for a, b in zip(p, q))
                if same > score or d < far:
                    best, score, far = r, same, d
        if best == m and hasattr(art, "own_metatile"):
            # a band's head drawn over the map's own ground (Route 114's
            # 220/222 over Fallarbor's dirt, a rim along their top): its
            # face is the head's, the rest of it is not to be matched
            face = {(x, y) for y in range(16) for x in range(16) if mine[y][x] == "F"}
            for r in sorted(HEADS):
                cls = pixel_classes(art, r)
                theirs = {(x, y) for y in range(16) for x in range(16) if cls[y][x] == "F"}
                same = len(face & theirs) / float(len(face | theirs) or 1)
                if same > score and same >= HEAD_MATCH:
                    best, score = r, same
        _TWINS[key] = best
    return _TWINS[key]


_COPIES = {}


def drawn_copy(art, m):
    """The General tileset's mountain tile whose rock tile m (of the General,
    not one of the mountain's) draws pixel for pixel, whatever its ground, or
    m itself: 16e is 17d, the foot of a face, over shallower water and with
    the sea's behaviour."""
    if m >= 0x200 or m in MOUNTAIN or art.primary != "gTileset_General":
        return m
    key = (art.primary, art.secondary, m, gr.reading(art))
    if key not in _COPIES:
        def rock(t):
            img, cls = art.cell_image(t).convert("RGB").load(), pixel_classes(art, t)
            return {(x, y): img[x, y] for y in range(16) for x in range(16) if cls[y][x] != "G"}
        mine = rock(m)
        _COPIES[key] = m
        if len(mine) >= 64:
            _COPIES[key] = next((r for r in sorted(MOUNTAIN) if r < 0x200 and rock(r) == mine), m)
    return _COPIES[key]


class Cells:
    """What each cell of a group of layouts is drawn as, on one canvas: the
    maps placed as they connect (gen_voxel_relief.DRAWN), so a mountain
    drawn across a seam is read as one."""

    def __init__(self, members):
        import voxel_cells as vc
        self.members = list(members)        # [(layout, x offset, y offset)]
        self.id = self.members[0][0]
        self.arts, self.roles_of = {}, {}
        self.src = {}                       # canvas cell -> (layout, x, y)
        self.kind, self.band, self.rim = {}, {}, set()
        self.ledges, self.keep = set(), set()
        self.jumps = {}                     # ledge cell -> the ways it is jumped ((dx, dy), ...)
        self.props = set()                  # cells a modelled object stands on (voxel_props)
        self.prop_of = {}                   # cell -> the object placed over it
        self.walked_tops = set()            # tops of a map's own tileset, walked (read as ground)
        self.models = modelled(self.members)    # cell -> the cell its building's model stands on
        self._over_water = {}
        self._drawn_on = {}
        berry = vc.behaviours().get("MB_BERRY_TREE_SOIL")
        # a bridge over a pond or the sea: a deck, flat at the banks it
        # joins (walked, so one level with them) - never rock, though its
        # ends are drawn over a band's head (read as the band, it was
        # stretched down the slope like the mountain)
        self.bridge_codes = {v for k, v in vc.behaviours().items() if k.startswith("MB_BRIDGE_OVER")}
        self.bridges = set()
        # a waterfall: the river above it goes over the edge, the pool at
        # its foot is another body a cliff lower (joined as one water, the
        # pool rose to the river's level above the bridge across it)
        falls = vc.behaviours().get("MB_WATERFALL")
        self.falls = set()
        # puddles (Route 120's marsh) are walked through: ground, not water
        # (as water the marsh joined the plateau beside it by its shore and
        # floated four levels over the grass it lies in)
        puddle = vc.behaviours().get("MB_PUDDLE")
        self.puddles = set()
        # a muddy slope cut down a face (Route 115's, slid down to the
        # terrace under it): the face's levels, as stairs up a face are -
        # read as the ground it is walked as, it lay at the foot as a pit
        self.mud = vc.behaviours().get("MB_MUDDY_SLOPE")
        for lid, ox, oy in self.members:
            art = self.arts[lid] = gr.terrace_art(lid)
            roles = self.roles_of[lid] = gr.open_roles(lid)
            ledges = gr.ledge_cells(roles)
            entry = next(e for e in vb._layouts_json() if e["id"] == lid)
            props = voxel_props.cells_in(entry)
            # each object placed: a patch of its own (unknown_levels)
            for k, (name, sx, sy, _) in enumerate(voxel_props.find(entry)):
                for (px, py) in voxel_props.cells_of(name, sx, sy):
                    self.prop_of[(px + ox, py + oy)] = (lid, k)
            for y in range(art.h):
                for x in range(art.w):
                    cell = (x + ox, y + oy)
                    if cell in self.src:
                        continue
                    self.src[cell] = (lid, x, y)
                    m = art.metatile(x, y)
                    if (x, y) in ledges:
                        self.ledges.add(cell)
                        self.jumps[cell] = tuple(ledges[(x, y)])
                    if roles.behaviour(x, y) in self.bridge_codes:
                        self.bridges.add(cell)
                    if roles.behaviour(x, y) == falls:
                        self.falls.add(cell)
                    if roles.behaviour(x, y) == puddle:
                        self.puddles.add(cell)
                    if (x, y) in ledges or roles.behaviour(x, y) == berry or m in gr.DIRT:
                        self.keep.add(cell)
                    self._read(cell, art, roles, m, (x, y) in ledges, (x, y) in props)
                    if cell in self.keep and self.kind[cell] == ROCK:
                        # berry soil, a patch of dirt: ground, whatever brown
                        # it is drawn in (Route 115's dirt patch, read as a
                        # face two rows tall, sank the grass south of it and
                        # the wood beside it two levels into a trench)
                        self.kind[cell] = GROUND
                        self.band.pop(cell, None)
                        self.rim.discard(cell)
        # a walkway of treads across the water (Route 114's bridge over its
        # pond: planks drawn as steps, railings either side, the water past
        # them): a deck, level with the banks it joins - read as stairs cut
        # down a face, it sank the pond thirteen levels
        self.lone_bridge = set()
        for cell in sorted(self.kind):
            if self.role(cell) != "stair" or cell in self.bridges:
                continue
            x, y = cell
            reach = {}
            for side in (-1, 1):
                for d in (1, 2, 3):
                    n = (x + side * d, y)
                    if self.water(n):
                        reach[side] = d
                        break
                    if n not in self.src or self.role(n) not in ("floor", "stair"):
                        break
            if len(reach) == 2:
                for side, d in reach.items():
                    for k in range(d):
                        n = (x + side * k, y)
                        self.bridges.add(n)
                        self.kind[n] = GROUND
                        self.band.pop(n, None)
        # a lone bridge cell (the deck's shadow on the pond beside it, drawn
        # as water): the water, not a deck
        self.lone_bridge = {b for b in self.bridges
                            if (b[0] - 1, b[1]) not in self.bridges and (b[0] + 1, b[1]) not in self.bridges}
        self.bridges -= self.lone_bridge
        self.cheeks = set()
        self.rough = set()                  # cells laid as rough rock (rough_fields)
        # the cheeks of stairs climbing a face (their balustrades, drawn
        # in the face's colours in the town's own tiles): the face too
        for cell, k in list(self.kind.items()):
            if k != GROUND or self.role(cell) == "floor":
                continue
            x, y = cell
            if not any(self.kind.get(n) == ROCK and self.is_stair(n) for n in ((x - 1, y), (x + 1, y))):
                continue
            cls = pixel_classes(self.art(cell), self.metatile(cell))
            if sum(r.count("F") + r.count("T") for r in cls) >= 0.4 * 256:
                self.kind[cell] = ROCK
                self.cheeks.add(cell)
        # a boulder drawn wide (Route 111's mounds with a secret base's
        # door): its halves 093/094 over 09b/09c at its ends, a rim and a
        # face between them. The whole of it is drawn on the terrace it
        # stands in, one with it: the middle read as a level of cliff stood
        # up as a block, walls against the halves at its ends; and taken as
        # a boulder capping a ridge (its own terrace) it sank two levels
        # into the plateau round it.
        self.wide_boulder = set()
        for (x, y), k in self.kind.items():
            if self.metatile((x, y)) != 0x093:
                continue
            e = x + 1
            while (e, y) in self.src and self.metatile((e, y)) not in BOULDER_HEAD | BOULDER_FOOT:
                e += 1
            ends = [((e, y), 0x094), ((x, y + 1), 0x09b), ((e, y + 1), 0x09c)]
            if e == x + 1 or any(c not in self.src or self.metatile(c) != m for c, m in ends):
                continue
            middle = [(i, j) for i in range(x + 1, e) for j in (y, y + 1)]
            if all(self.kind.get(c) in (TOP, ROCK) for c in middle):
                whole = [(i, j) for i in range(x, e + 1) for j in (y, y + 1)]
                for c in whole:
                    self.kind[c] = TOP
                    self.rim.discard(c)
                self.wide_boulder.update(whole)
        xs = [x for x, _ in self.src]
        ys = [y for _, y in self.src]
        self.xs = range(min(xs), max(xs) + 1)
        self.ys = range(min(ys), max(ys) + 1)

    def _read(self, cell, art, roles, m, ledge, prop):
        lx, ly = self.src[cell][1:]
        cls = pixel_classes(art, m)
        # a door of the map's own tileset is a building's (Slateport's
        # museum draws its door in a cave mouth's colours): no twin of rock -
        # unless rock is drawn either side of it (a cave's mouth in a face:
        # Route 120's mound)
        def rock_beside():
            for nx in (lx - 1, lx + 1):
                if 0 <= nx < art.w:
                    nc = pixel_classes(art, art.metatile(nx, ly))
                    if sum(r.count("F") + r.count("T") for r in nc) >= 128:
                        return True
            return False
        own = m
        if not (m >= 0x200 and (lx, ly) in roles.warps) or rock_beside():
            m = twin(art, m)
        t = sum(r.count("T") for r in cls) / 256.0
        f = sum(r.count("F") for r in cls) / 256.0
        role = roles.role_at(lx, ly)
        # the sea, blocked, drawn as the mountain's own tile (the foot of a
        # face standing in it): the rock it draws. Read as the sea it lay
        # flat on the water, the face painted on it beside the face standing
        if role == "water" and roles.blocked(lx, ly) and drawn_copy(art, m) != m:
            m, role = drawn_copy(art, m), "cliff"
        # in a map read in the General's colours (gr.terrace_art) every tile
        # of its own looks drawn in rock: only one that is a General mountain
        # tile drawn again is rock (Route 114's berry soil read as faces)
        alien = hasattr(art, "own_metatile") and m >= vb.NUM_PRIMARY and m not in MOUNTAIN
        # and walked on, only a tile drawn again exactly (a cave's mouth, a
        # door) is the face it is cut in, not one merely like it (Route 114's
        # garden borders, like faces once in the General's colours)
        if hasattr(art, "own_metatile") and own >= vb.NUM_PRIMARY and role == "floor":
            alien = True
            # a top merely like one, walked (Route 114's plateau over its
            # valley): ground, but the level its bands give it, not the
            # level of the grass walked onto it from (terraces)
            if m in MOUNTAIN and f < 0.08 and t >= 0.45:
                self.walked_tops.add(cell)
            # and a floor laid on a top, the top's colours across its first
            # row (Lavaridge's boardwalk over its spring): the top's ground
            elif f == 0 and all(p == "T" for p in cls[0]):
                self.walked_tops.add(cell)
        # the mountain's own tiles drawn over grass (a band's corner, 068)
        # read as a tree by their green: they are the rock they draw
        tree = role == "tree" and not (m in MOUNTAIN and t + f >= 0.25)
        if prop:
            self.props.add(cell)
        if tree or prop or m in LOOSE:
            k = UNKNOWN     # a sea stack's model is drawn with band heads too
        elif (m in HEADS and not ledge and ly + 1 < art.h and self._top_tile(art, art.metatile(lx, ly + 1))
              and not (hasattr(art, "own_metatile") and rim_pixels(art, art.metatile(lx, ly + 1)) >= 3)):
            # a terrace's corner, its back turning (in a map read in alias,
            # not over a top that starts with its own lip: Route 114's head
            # at (37, 26) is its terrace's band, up from the dirt beside it,
            # over the next terrace's edge)
            k = TOP
        elif (m in BAND_WEST or m in BAND_EAST) and not ledge:
            # a band, its head over the sea behind it too: rock
            k = ROCK
            self.band[cell] = "W" if m in BAND_WEST else "E"
        elif ledge or role in ("water", "ledge") or cell in self.bridges:
            k = GROUND
        elif ((role == "stair" or roles.behaviour(lx, ly) == self.mud
               or (role == "floor" and art.metatile(lx, ly) in CARVED_STEPS))
              and self._in_face(art, lx, ly)):
            k = ROCK        # stairs (a muddy slope) up a face: a level, as the face
        elif role == "floor" and (lx, ly) in roles.warps and self._in_face(art, lx, ly, both=True):
            # a door in a face, rock drawn either side of it (Lavaridge's
            # Center's back door under its boardwalk): the face it is cut in
            # - as the ground it was a notch down the face, walls either side
            k = ROCK
        elif role == "floor" and m in MOUNTAIN and f >= 0.5 and not alien:
            # a cave's mouth, a door: walked into (a warp), drawn as the
            # face. Walked over, it is the ground the face's colours are
            # drawn on: Route 113's patches of ash in a ground of the
            # mountain's colours (as rock they sank as pits with faces)
            k = ROCK if (lx, ly) in roles.warps else TOP
        elif role == "floor" and m in MOUNTAIN and f < 0.08 and t >= 0.45 and not alien:
            k = TOP         # a path along a ridge: the top it is drawn as
        elif (role == "wall" and m not in MOUNTAIN and t + f < 0.9
              and sum(r.count("G") for r in cls[:4]) <= 4):
            # a roof drawn over the mountain behind it, the rock across its
            # top rows (Lavaridge's Mart under its terrace): what stands
            # there the model hides, the rock round it says - read as the
            # ground the house stands on, it held the terrace over it one
            # level over the town, its face laid flat beside the house
            k = UNKNOWN
        elif role in ("floor", "stair", "signpost") or (m not in MOUNTAIN and t + f < 0.9) or alien:
            # (a tile drawn all in rock outside the mountain's own set - the
            # rounded shoulders 0ee/0ef of a mound - is rock all the same)
            # walked on, a sign, a patch of soil, a building's wall: never
            # the mountain, whatever colours it is drawn in
            k = GROUND
        elif m in BOULDER_FOOT and ly > 0 and twin(art, art.metatile(lx, ly - 1)) in BAND_WEST | BAND_EAST:
            # a boulder's lower half under a band: the rounded end of a
            # ridge, where its flank meets the face it ends in - rock, its
            # corner (a top there left a slanted step under the band)
            k = ROCK
        elif m in ON_TOP:
            k = TOP
        elif t + f < 0.25:
            k = GROUND
        elif sum(r.count("F") for r in cls[2:]) < 0.08 * 256 and t >= 0.45:
            # (the face drawn along its first rows is the foot of the band
            # over it, as 0b4 draws it: Lavaridge's 27f, a little more of
            # it, read as rock, split its south rim a level apart)
            k = TOP
        else:
            k = ROCK
        self.kind[cell] = k
        if k in (TOP, ROCK) or (cell in self.walked_tops and m in MOUNTAIN):
            g_top = sum(1 for yy in range(4) for xx in range(16) if cls[yy][xx] == "G")
            if rim_pixels(art, m) >= 3 or g_top >= 12:
                self.rim.add(cell)

    def art(self, cell):
        return self.arts[self.src[cell][0]]

    def is_stair(self, cell):
        """A flight of steps: walked up a face as stairs (or drawn as the
        General's carved steps, which the regions read as floor)."""
        return self.role(cell) == "stair" or (self.kind.get(cell) == ROCK
                                             and self.metatile(cell) in CARVED_STEPS)

    def on_top(self, cell):
        """A boulder drawn whole on a ridge's end, capping it (a wide one
        lies in its terrace, one with it)."""
        return self.metatile(cell) in ON_TOP and cell not in self.wide_boulder

    def at_edge(self, cell):
        """On its map's border: where it meets the next map."""
        lid, x, y = self.src[cell]
        art = self.arts[lid]
        return x in (0, art.w - 1) or y in (0, art.h - 1)

    def metatile(self, cell):
        lid, x, y = self.src[cell]
        return self.arts[lid].metatile(x, y)

    def role_id(self, cell):
        """The mountain tile a cell is drawn as (its General twin)."""
        lid, x, y = self.src[cell]
        return twin(self.arts[lid], self.arts[lid].metatile(x, y))

    def role(self, cell):
        if cell in getattr(self, "puddles", ()):
            return "floor"
        lid, x, y = self.src[cell]
        return self.roles_of[lid].role_at(x, y)

    def water(self, cell):
        """Water, or a tree's crown drawn over it: the crown's lower layer is
        the drawing of the water beside it (Route 106's wood over the sea)."""
        if cell not in self.src:
            return False
        if self.role(cell) == "water" or cell in self.lone_bridge:
            return True
        if cell not in self._over_water:
            self._over_water[cell] = self.crown(cell) and any(
                self._lower(cell) == self._lower(n) for n in self._four(cell)
                if n in self.src and self.role(n) == "water")
        return self._over_water[cell]

    def _four(self, cell):
        x, y = cell
        return ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1))

    def _lower(self, cell):
        art = self.art(cell)
        return art.cell_image(self.metatile(cell), (0,)).convert("RGB").tobytes()

    def crown(self, cell):
        """The top of a tree drawn over what is behind it: the cell north of
        a tree, a quarter of it at least on the upper layer. What it stands
        for is the tree's; what is under it is not seen."""
        if cell not in self.src:
            return False
        lid, x, y = self.src[cell]
        art, roles = self.arts[lid], self.roles_of[lid]
        if y + 1 >= art.h or roles.role_at(x, y) == "tree" or roles.role_at(x, y + 1) != "tree":
            return False
        m = art.metatile(x, y)
        hi, lo = art.cell_image(m).load(), art.cell_image(m, (0,)).load()
        return sum(1 for j in range(16) for i in range(16) if hi[i, j] != lo[i, j]) >= 64

    def muddy(self, cell):
        """A muddy slope (MB_MUDDY_SLOPE)."""
        if cell not in self.src:
            return False
        lid, x, y = self.src[cell]
        return self.mud is not None and self.roles_of[lid].behaviour(x, y) == self.mud

    def drawn_on(self, cell):
        """What a cell's lower layer is drawn as: 'T' the mountain's top,
        'F' its face's colours, 'G' anything else (grass, sand, a floor)."""
        art = self.art(cell)
        key = (art.primary, art.secondary, self.metatile(cell), gr.reading(art))
        if key not in self._drawn_on:
            img = art.cell_image(self.metatile(cell), (0,)).convert("RGB").load()
            n = collections.Counter("T" if img[x, y] in TOP_COLOURS else "F" if img[x, y] in FACE_COLOURS
                                    else "G" for y in range(16) for x in range(16))
            self._drawn_on[key] = "T" if n["T"] >= 128 else "F" if n["F"] >= 128 else "G"
        return self._drawn_on[key]

    def tree_on_rock(self, cell):
        """A tree drawn over the mountain's own ground (Lavaridge's trees on
        its red rock, on its top's colours or its dark ash)."""
        return (self.kind.get(cell) == UNKNOWN and self.role(cell) == "tree"
                and self.drawn_on(cell) != "G")

    def walked(self, cell):
        """Walked on (the collision map): a floor, not water, a ledge, stairs."""
        return cell in self.src and self.role(cell) == "floor" and cell not in self.lone_bridge

    @staticmethod
    def _top_tile(art, m):
        """A mountain tile drawn as a terrace's top (its rim's included)."""
        if twin(art, m) not in MOUNTAIN:
            return False
        cls = pixel_classes(art, m)
        return (sum(r.count("F") for r in cls) < 0.08 * 256
                and sum(r.count("T") for r in cls) >= 0.45 * 256)

    def house(self, cell):
        """A house's wall or roof its model stands over: the console draws
        no relief there (voxel_mesh_builder.c), only the ground the house
        was painted on, flat at the cell's centre - a face or a corner
        given to it was a plate half a level up, a void either side."""
        return cell in self.models and self.role(cell) == "wall"

    @staticmethod
    def _in_face(art, x, y, both=False):
        """Stairs cut through a face: rock drawn beside them, west or east
        (`both`: west and east)."""
        sides = [0 <= nx < art.w and sum(r.count("F") for r in pixel_classes(art, art.metatile(nx, y))) >= 96
                 for nx in (x - 1, x + 1)]
        return all(sides) if both else any(sides)


# -- 2. terraces and their levels ---------------------------------------------

class Offsets:
    """Union-find keeping each item's level over its root's."""

    def __init__(self):
        self.parent, self.off = {}, {}

    def find(self, a):
        if a not in self.parent:
            self.parent[a], self.off[a] = a, 0
            return a, 0
        path = []
        while self.parent[a] != a:
            path.append(a)
            a = self.parent[a]
        root = a
        total = 0
        for p in reversed(path):
            total += self.off[p]
            self.off[p] = total
            self.parent[p] = root
        return root, (self.off[path[0]] if path else 0)

    def join(self, a, b, d):
        """level(a) - level(b) = d. False when it contradicts what is known."""
        ra, oa = self.find(a)
        rb, ob = self.find(b)
        if ra == rb:
            return oa - ob == d
        # level(a) = L(ra) + oa, level(b) = L(rb) + ob: L(rb) = L(ra) + oa - ob - d
        self.parent[rb] = ra
        self.off[rb] = oa - ob - d
        return True


def runs_of(c):
    """[(direction, upper cell, rock cells from the upper end, lower cell)]:
    every column of rock cells between two flat cells (not ending at a rim's
    back, which is no face, nor hanging from water), every row of bands."""
    def flat(cell):
        return c.kind.get(cell) in (TOP, GROUND)

    def on_top(cell):
        return c.tree_on_rock(cell) and c.drawn_on(cell) == "T"

    runs = []
    for x in c.xs:
        for y in c.ys:
            nxt = (x, y + 1)
            hi = (x, y)
            # a tree drawn standing on the top over a face (Lavaridge's
            # trees on its red rock): the face hangs from the top behind it
            while on_top(hi):
                hi = (x, hi[1] - 1)
            if not flat(hi) or c.kind.get(nxt) != ROCK or nxt in c.band or c.water(hi):
                continue
            k = 1
            while c.kind.get((x, y + 1 + k)) == ROCK and (x, y + 1 + k) not in c.band:
                k += 1
            end = (x, y + 1 + k)
            cells = [(x, y + 1 + i) for i in range(k)]
            if flat(end) and end not in c.rim:
                runs.append(("S", hi, cells, end))
    for y in c.ys:
        for x in c.xs:
            nxt = (x + 1, y)
            if not flat((x, y)) or nxt not in c.band:
                continue
            side = c.band[nxt]
            k = 1
            while c.band.get((x + 1 + k, y)) == side:
                k += 1
            end = (x + 1 + k, y)
            cells = [(x + 1 + i, y) for i in range(k)]
            if not flat(end):
                continue
            if side == "E" and not c.water((x, y)):
                runs.append(("E", (x, y), cells, end))
            elif side == "W" and not c.water(end):
                runs.append(("W", end, cells[::-1], (x, y)))
    return runs


def terraces(c, runs):
    """{cell: terrace id} of the tops and grounds, joined like to like: tops
    top to top (not across a rim's back), grounds ground to ground and
    through what stands on them (trees, boulders, props). Never two cells a
    face or a band apart: a pier or a path that joins a beach to the land
    above it is one or the other, not both."""
    uf = {}
    apart = collections.defaultdict(set)
    for _, hi, cells, lo in runs:
        apart[hi].add(lo)
        apart[lo].add(hi)
    members = {}

    def find(a):
        while uf.setdefault(a, a) != a:
            uf[a] = uf[uf[a]]
            a = uf[a]
        return a

    def join(a, b):
        ra, rb = find(a), find(b)
        if ra == rb:
            return
        ma, mb = members.get(ra, ()), members.get(rb, ())
        # a cell kept apart from one of the other side's
        if any(find(o) == rb for m in ma for o in apart[m]) or \
           any(find(o) == ra for m in mb for o in apart[m]):
            return
        uf[rb] = ra
        if mb:
            members.setdefault(ra, set()).update(mb)
            members.pop(rb, None)

    for cell in apart:
        members[cell] = {cell}
    edges = []
    for (x, y), k in c.kind.items():
        if k not in (TOP, GROUND, UNKNOWN):
            continue
        find((x, y))
        for n in ((x + 1, y), (x, y + 1)):
            nk = c.kind.get(n)
            if k == TOP:
                # a rim's back: the top behind it is another terrace
                if nk != TOP or (n[1] == y + 1 and n in c.rim):
                    continue
                # a boulder drawn on a ridge's end caps it: the end is its
                # own terrace (a ridge falls at its end, a band less high)
                if c.on_top((x, y)) != c.on_top(n):
                    continue
            elif nk not in (GROUND, UNKNOWN):
                continue
            elif n[1] == y + 1 and n in c.walked_tops and n in c.rim:
                # a walked top's rim: its back too is another terrace's
                # (Route 114's lips stacked up to its summit, row on row)
                continue
            # a tree drawn over the mountain's own ground stands on that
            # ground: it joins only ground drawn the same (Route 112's
            # plateau, its trees on the rock, joined through the wood at its
            # west end to the grass three levels under its cliff)
            if ((c.tree_on_rock((x, y)) or c.tree_on_rock(n))
                    and (c.drawn_on((x, y)) == "G") != (c.drawn_on(n) == "G")):
                continue
            # first what the collision map ties: ground walked from one cell
            # to the next with no ledge or stairs between is one level (the
            # player walks it level), water surfed across is one body; then
            # the ground's own edges (a shore, the land round a house); last
            # what a map's own tileset draws on it
            ma, mb = c.metatile((x, y)), c.metatile(n)
            wa, wb = c.water((x, y)), c.water(n)
            if (x, y) in c.falls and n not in c.falls and n[1] == y + 1:
                # the pool at a waterfall's foot: not the river above it
                continue
            if (wa and wb) or (k == GROUND and nk == GROUND and c.walked((x, y)) and c.walked(n)):
                rank = 0
            elif ma < 0x200 and mb < 0x200:
                rank = 1
            else:
                rank = 2
            # a top walked onto from the grass (behind its rim, or past its
            # band's head) is joined to that grass last: the band down its
            # side holds them apart first (Route 114's plateau over its valley,
            # Lavaridge's boardwalk over its spring)
            if ((x, y) in c.walked_tops) != (n in c.walked_tops):
                rank = 3
            # a shore last: land is joined to land first, so a wood on a
            # ridge's top is one terrace (kept apart from the sea at the
            # ridge's foot) before a tree drawn over the water could tie a
            # few of its cells to the sea (Route 106's east end)
            if wa != wb:
                rank += 3
            edges.append((rank, (x, y), n))
    for _, a, b in sorted(edges):
        join(a, b)
    # the ground a tree stands on (the terrace it was joined to): what is
    # behind a rim when a wood stands there
    c.tree_ground = {cell: find(cell) for cell in uf if c.kind[cell] == UNKNOWN}
    return {cell: find(cell) for cell in uf if c.kind[cell] != UNKNOWN}


def steps(c, cells, direction):
    """The levels down a run of rock cells: one a cell, but an upper face
    end over a lower one is one diagonal wall. Returns the level each cell
    hangs from, counted from the top (0, 1, ...), and the run's levels."""
    out, n = [], 0
    for i, cell in enumerate(cells):
        if direction == "S" and i > 0 and c.role_id(cells[i - 1]) in UPPER_END \
                and c.role_id(cell) in LOWER_END and out[-1] == n - 1:
            out.append(out[-1])
            continue
        out.append(n)
        n += 1
    return out, n


def solve_levels(c, comp, runs, reference=None, meets=None, blocks_out=None):
    """{terrace: level}, the votes that lost. `reference`: {cell: height} of
    the world as it stands (the relief the world solver placed); each block
    of terraces tied by votes is lifted to agree with it most, so no seam
    with a neighbouring map moves. Without it, its biggest ground is 0.
    `meets`: the edge cells with a map across them (seam_faces) - only those
    that meet it hold a terrace where it stands, and only along a seam most
    of the terrace meets."""
    runs = [r for r in runs if r[1] in comp and r[3] in comp and comp[r[1]] != comp[r[3]]]
    # a band's head under a rim turns the corner (the band beside it may
    # start a row lower): it says the least
    vote = collections.Counter()
    hidden = collections.Counter()
    for d, hi, cells, lo in runs:
        head = d != "S" and any(cell in c.rim for cell in cells)
        key = (comp[hi], comp[lo], steps(c, cells, d)[1])
        vote[key] += 1 if head else 2
        # a face whose foot a house or a wood stands on: what it does there
        # is not seen
        hidden[key] += d == "S" and ((c.role(lo) == "wall" and c.metatile(lo) >= 0x200)
                                     or c.kind.get((lo[0], lo[1] + 1)) == UNKNOWN)
    # a rim's lip: the terrace it starts stands over the one behind it,
    # how high the drawing does not say - one level, the least, and only
    # where no face or band says more (a terrace no run reaches - Route
    # 114's row of lips up to its summit - sank to the world's ground)
    # (a wood behind it: the ground the wood stands on - Route 112's crags
    # behind its meadow's trees sank under the meadow, no lip holding them)
    lip = collections.Counter()
    behind = dict(getattr(c, "tree_ground", {}))
    behind.update(comp)
    for cell in c.rim:
        n = (cell[0], cell[1] - 1)
        if cell in comp and n in behind and comp[cell] != behind[n] and c.src[n][0] == c.src[cell][0]:
            lip[(comp[cell], behind[n], 1)] += 1
    off = Offsets()
    dropped = []
    size = collections.Counter(comp.values())
    cells_of = collections.defaultdict(list)
    for cell, t in comp.items():
        cells_of[t].append(cell)
    # first where the world already stands: the ground of each map that
    # reaches its edges where it meets the next map keeps its level - not an
    # edge with no map beyond it, nor one the next map's ground does not
    # meet (the wood on Route 106's ridge, read as the sea it is drawn over
    # by the world's solve, under the same wood on Dewford's): there it
    # holds nothing, and the votes say where it stands
    # a terrace's kind is its cells' (its id may be a tree it was joined through)
    kind_of = {t: c.kind[cells[0]] for t, cells in cells_of.items()}
    if reference is not None:
        for t, cells in sorted(cells_of.items()):
            if kind_of[t] != GROUND or len(cells) < ANCHOR_MIN:
                continue
            edge = [cell for cell in cells if c.at_edge(cell) and cell in reference
                    and (meets is None or meets.get(cell, (None, False))[1])]
            if len(edge) < 2:
                continue
            if meets is not None:
                # a couple of cells that happen to meet hold nothing: along
                # Route 115's seam with Rustboro two of Rustboro's sea rocks
                # stood at the level of 115's sea, a level over Rustboro's
                # own sea - and held the beach level with the grass over it
                met = collections.Counter(meets[cell][0] for cell in edge)
                along = collections.Counter(meets[cell][0] for cell in cells if cell in meets)
                if not any(2 * n >= along[b] for b, n in met.items()):
                    continue
            got = collections.Counter(LEVEL * round(reference[cell] / LEVEL) for cell in cells
                                      if cell in reference and cell not in c.keep)
            if got:
                off.join(WORLD, t, -got.most_common(1)[0][0])
    # within a map before across a seam (where two maps the world placed
    # apart disagree, the step is at their seam, not in a mountain), then
    # the most columns first; a tie goes to the one-level step (the commonest)
    def across(a, b):
        return c.src[a][0] != c.src[b][0]

    # the weight a vote is kept by: a seam's half (two maps the world placed
    # apart: the step goes to their seam rather than into a mountain), a
    # hidden face's quarter (the step goes where a house or a wood hides it)
    def weight(key, n):
        w = n * (0.5 if across(key[0], key[1]) else 1.0)
        if hidden[key] * 2 >= n:
            w *= 0.25
        return w

    for (a, b, k), n in sorted(vote.items(), key=lambda kv: (-weight(kv[0], kv[1]), kv[0][2],
                                                             kv[0][0], kv[0][1])):
        if not off.join(a, b, k * LEVEL):
            dropped.append(((a, b, k), n))
    for (a, b, k), n in sorted(lip.items(), key=lambda kv: (-kv[1], kv[0])):
        off.join(a, b, k * LEVEL)
    roots = collections.defaultdict(list)
    for t in set(comp.values()):
        roots[off.find(t)[0]].append(t)
    level = {}
    world_root = off.find(WORLD)[0]
    for r, members in roots.items():
        if blocks_out is not None:
            blocks_out.update((t, r) for t in members)
        rel = {t: off.find(t)[1] for t in members}
        grounds = [t for t in members if kind_of[t] == GROUND]
        lift = None
        if r == world_root:
            lift = -off.find(WORLD)[1]
        elif reference is not None:
            diffs = collections.Counter()
            for t in (grounds or members):
                for cell in cells_of[t]:
                    if cell in reference and cell not in c.keep:
                        diffs[LEVEL * round((reference[cell] - rel[t]) / LEVEL)] += 1
            if diffs:
                lift = max(diffs.items(), key=lambda kv: (kv[1], -abs(kv[0])))[0]
        if lift is None:
            ref = max(grounds or members, key=lambda t: (size[t], t))
            lift = -rel[ref]
        for t in members:
            level[t] = rel[t] + lift
    return level, dropped


# -- 3. the level each rock cell hangs from -----------------------------------

def upper_levels(c, comp, level, runs):
    U = {}
    for side, hi, cells, lo in runs:
        if hi not in comp or lo not in comp:
            continue
        top, bottom = level[comp[hi]], level[comp[lo]]
        if top <= bottom:
            continue        # a vote that lost: says nothing
        idx, _ = steps(c, cells, side)
        for i, cell in zip(idx, cells):
            # more cells than levels: the last ones stand on the lowest
            u = max(top - LEVEL * i, bottom + LEVEL)
            if cell not in U or u > U[cell]:
                U[cell] = u
    # a ridge drawn with bands alone (no top between its two flanks): each
    # flank counts up from the ground at its foot to the crest
    crests = set()
    for y in c.ys:
        for x in c.xs:
            if (x, y) not in comp or (x + 1, y) not in c.band:
                continue
            # west flank: ground, W bands..., then E bands, then ground
            if c.band[(x + 1, y)] != "W":
                continue
            k = 1
            while c.band.get((x + 1 + k, y)) == "W":
                k += 1
            if c.band.get((x + 1 + k, y)) != "E":
                continue
            j = 1
            while c.band.get((x + 1 + k + j, y)) == "E":
                j += 1
            end = (x + 1 + k + j, y)
            if end not in comp:
                continue
            lo_w, lo_e = level[comp[(x, y)]], level[comp[end]]
            crest = max(lo_w + LEVEL * k, lo_e + LEVEL * j)
            for i in range(k):
                U.setdefault((x + 1 + i, y), crest - LEVEL * (k - 1 - i))
                crests.add((x + 1 + i, y))
            for i in range(j):
                U.setdefault((x + 1 + k + i, y), crest - LEVEL * i)
                crests.add((x + 1 + k + i, y))
    # a flank whose terrace is off the map (Route 111's desert walled east
    # by bands climbing to its edge, no map beyond): it counts up from the
    # ground at its foot, a level a band - kept to the levels round it, it
    # sank under the desert as a trench
    for y in c.ys:
        for x in c.xs:
            if (x, y) not in comp:
                continue
            for side, step in (("W", 1), ("E", -1)):
                k, run = 1, []
                while c.band.get((x + step * k, y)) == side:
                    run.append((x + step * k, y))
                    k += 1
                if run and (x + step * k, y) not in c.src:
                    for i, cell in enumerate(run):
                        if cell not in U:
                            U[cell] = level[comp[(x, y)]] + LEVEL * (i + 1)
                            crests.add(cell)
    # bands down a column are one band: the same level
    changed = True
    while changed:
        changed = False
        for (x, y), side in c.band.items():
            if (x, y) in U:
                continue
            for n in ((x, y - 1), (x, y + 1)):
                if c.band.get(n) == side and n in U:
                    U[(x, y)] = U[n]
                    changed = True
                    break
    # the rest from what is round them: first what holds them (the surface
    # above, the high side of a band), then what is beside, then the foot
    todo = [cell for cell, k in c.kind.items() if k == ROCK and cell not in U]
    for _ in range(16):
        left = []
        for (x, y) in todo:
            side = c.band.get((x, y))
            high = (x - 1, y) if side == "E" else (x + 1, y) if side == "W" else (x, y - 1)
            # under a face, a level down; under a band (its foot), beside
            under = 0 if side or (x, y - 1) in c.band else -LEVEL
            tiers = [[(high, 0, under)],
                     [((x - 1, y), 0, 0), ((x + 1, y), 0, 0)] + (
                         [((x, y - 1), 0, 0), ((x, y + 1), 0, 0)] if side else []),
                     [((x, y + 1), LEVEL, LEVEL)]]
            got = None
            for tier in tiers:
                cand = []
                for n, node, rock in tier:
                    if n in comp:
                        cand.append(level[comp[n]] + node)
                    elif n in U:
                        cand.append(U[n] + rock)
                if cand:
                    got = max(cand)
                    break
            if got is None:
                left.append((x, y))
            else:
                U[(x, y)] = got
        if len(left) == len(todo):
            break
        todo = left
    for cell in todo:
        U[cell] = LEVEL
    # what nothing counted stays within the levels round it: no lower than
    # the lowest surface near it, no higher than the highest (the surface it
    # hangs from may be the only one near: it is not lifted over it)
    counted = {cell for _, hi, cells, lo in runs for cell in cells} | crests
    for (x, y) in [cell for cell, k in c.kind.items() if k == ROCK and cell not in counted]:
        near = [level[comp[n]] for n in ((x + dx, y + dy) for dx in (-2, -1, 0, 1, 2)
                                         for dy in (-2, -1, 0, 1, 2)) if n in comp]
        if near:
            U[(x, y)] = max(min(near), min(max(near), U[(x, y)]))
    return U


def unknown_levels(c, comp, level, U):
    """Trees, boulders, props: what is under them is not drawn. A patch of
    them stands at one level, the lowest ground it can stand on - beside it,
    or at the foot of the rock above it: a wood in front of a cliff hides
    the cliff, it does not climb it."""
    out = {}
    seen = set()
    for start, k in sorted(c.kind.items()):
        if k != UNKNOWN or start in seen:
            continue
        patch, todo = [], [start]
        seen.add(start)
        while todo:
            x, y = todo.pop()
            patch.append((x, y))
            for n in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
                # (a modelled object is a patch of its own: a loose boulder
                # drawn against it on the terrace below is not on its ground)
                if c.kind.get(n) == UNKNOWN and n not in seen and c.prop_of.get(n) == c.prop_of.get((x, y)):
                    seen.add(n)
                    todo.append(n)
        # a rim's back is no ground for what stands behind it: the ground
        # there is lower, never seen (Route 112's wood north of its crags
        # took their tops and stood a level over Route 113 at the seam)
        def backs(cell, n):
            return n == (cell[0], cell[1] + 1) and n in c.rim and c.kind.get(n) == TOP

        cand, wet, feet, behind = [], [], [], []
        for (x, y) in patch:
            for n in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
                if n in comp and not backs((x, y), n):
                    (wet if c.water(n) else cand).append(level[comp[n]])
                    if n != (x, y + 1) and not c.water(n):
                        behind.append(level[comp[n]])
            if (x, y - 1) in U:
                feet.append(U[(x, y - 1)] - LEVEL)
        # a modelled object (a boulder, a mound) lies on the ground round
        # it; only where none is beside it, at the foot of the rock above
        # (the boulder on Route 115's ledge by Meteor Falls, put at the foot
        # the face over it was counted to, sank a level into the ledge)
        modelled = all(cell in c.props for cell in patch)
        if cand and modelled:
            feet = []
            # the ground it is drawn on is behind it and beside it: its foot
            # may hang over the edge of a lower terrace, whose drop it hides
            # (Route 111's boulders on the terraces south of the pond: put on
            # the terrace below, the band beside them stood a level over
            # their ground, a dark wall)
            if behind:
                cand = behind
        # what stands mostly in the sea (a rock off the shore) stands on the
        # sea; what stands mostly on land (a wood by the beach) on the land
        use_wet = len(wet) > len(cand) + len(feet)
        # ... but a patch drawn over a face is that face's top, whatever
        # water is round it (Route 133's sandbar: its sand, read as nothing,
        # stood on the sea - the face along its front had nothing to fall
        # and lay flat on the water)
        if use_wet and not modelled:
            tops = set()
            for (x, y) in patch:
                run = []
                while c.kind.get((x, y + 1 + len(run))) == ROCK and (x, y + 1 + len(run)) not in c.band:
                    run.append((x, y + 1 + len(run)))
                foot = (x, y + 1 + len(run))
                if run and foot in comp and c.kind.get(foot) in (TOP, GROUND):
                    tops.add(level[comp[foot]] + LEVEL * steps(c, run, "S")[1])
            if len(tops) == 1:
                top = tops.pop()
                for cell in patch:
                    out[cell] = top
                continue
        # the foot of the rock over a tree is never under every ground the
        # patch stands beside: a face counted down past its foot (the end
        # of Route 111's east cliff over the fence at Mauville's seam) sank
        # the fence and the wood a level into a pit
        ground = wet if use_wet else cand
        floor = min(ground) if ground else None
        # a wood of large trees (cards of four cells, each lifted with its
        # own cell) whose every edge meets ground of one level: it is all
        # on that ground (Route 115's wood under the cliff by Route 114:
        # split by the counts of the faces over it into three levels, the
        # trees stood on walls with the void between them)
        if ground and not use_wet and max(ground) - min(ground) < 0.5 and all(
                c.metatile(cell) in TREE_CARDS - SMALL_TRUNKS for cell in patch):
            for cell in patch:
                out[cell] = ground[0]
            continue
        feet = [f if floor is None else max(f, floor) for f in feet]
        cand = wet if use_wet else (cand + feet or wet)
        v = min(cand) if cand else 0
        # a big wood spans terraces: each tree stands on the ground nearest
        # it (the lowest of the nearest), not the whole wood on the lowest
        # ground any of it touches (a wood beside a high meadow sank three
        # levels under it, a cliff the drawing never has)
        own, spread = {}, {}
        for (x, y) in patch:
            mine = []
            # a tree whose top is drawn in a face's row (rock either side,
            # its trunk on the ground below): the top of the cliff over it
            # is not its ground - the face runs on behind it (behind_trees).
            # Taken as its ground, it stood a level or two over its trunk,
            # dark wedges of wall either side of it
            in_face = (c.kind.get((x - 1, y)) == ROCK and c.kind.get((x + 1, y)) == ROCK
                       and c.kind.get((x, y + 1)) == UNKNOWN)
            # the top of a small tree (a card standing on its trunk's cell):
            # what it is drawn over is behind the tree, not its ground (a
            # wood of two rows in front of a face: the back row's tops took
            # the terrace over the face and stood a level over the front
            # row, a wall between their trunks). Whether the wood spans more
            # than a step is still read with them (`spread`).
            top = (c.metatile((x, y)) not in TREE_CARDS and (x, y + 1) in c.src
                   and c.metatile((x, y + 1)) in SMALL_TRUNKS)
            behind = []
            for n in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
                if (in_face and n == (x, y - 1)) or backs((x, y), n):
                    continue
                if n in comp and c.water(n) == use_wet:
                    if top and n == (x, y - 1):
                        behind.append(level[comp[n]])
                    else:
                        mine.append(level[comp[n]])
            if (x, y - 1) in U and (x, y - 1) not in c.band and not (modelled and mine):
                # (under a band the foot is not one level down: its slope
                # meets the ground beside it - that ground is the tree's)
                f = U[(x, y - 1)] - LEVEL
                mine.append(f if floor is None else max(f, floor))
            if mine:
                own[(x, y)] = min(mine)
            if mine or behind:
                spread[(x, y)] = min(mine + behind)
        if not spread or max(spread.values()) - min(spread.values()) <= LEVEL + 0.5:
            # a wood over one step (a hedge along a terrace's edge) lies
            # whole on the lower ground: split, it climbed the edge in a
            # zigzag of walls between the trees
            for cell in patch:
                out[cell] = v
            continue
        dist = {cell: 0 for cell in own}
        front = list(own)
        res = dict(own)
        while front:
            nxt = []
            for (x, y) in front:
                for n in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
                    if c.kind.get(n) != UNKNOWN or n not in seen:
                        continue
                    d = dist[(x, y)] + 1
                    if n not in dist:
                        dist[n] = d
                        res[n] = res[(x, y)]
                        nxt.append(n)
                    elif dist[n] == d:
                        res[n] = min(res[n], res[(x, y)])
            front = nxt
        # the top of a small tree stands where its trunk does
        for (x, y) in sorted(patch, key=lambda cl: -cl[1]):
            if (c.metatile((x, y)) not in TREE_CARDS and (x, y + 1) in res
                    and c.metatile((x, y + 1)) in SMALL_TRUNKS):
                res[(x, y)] = res[(x, y + 1)]
        for cell in patch:
            out[cell] = res.get(cell, v)
    return out


def settle(c, U, flat, fixed):
    """Each rock cell takes, of its level and the ones a level up or down,
    the one that leaves the most of its corners shared with every cell round
    them (a corner all of them can reach is one point: no crack, no wall).
    Cells a column or a row of faces counted (`fixed`) move only for a clear
    gain. Repeated until nothing moves."""
    def options(cell):
        if cell in flat:
            return {flat[cell]}
        return {U[cell], U[cell] - LEVEL}

    def score(x, y):
        n = 0
        for gx, gy in ((x, y), (x + 1, y), (x, y + 1), (x + 1, y + 1)):
            common = None
            for cell in ((gx - 1, gy - 1), (gx, gy - 1), (gx - 1, gy), (gx, gy)):
                if cell in c.kind:
                    o = options(cell)
                    common = set(o) if common is None else common & o
            n += bool(common)
        return n

    # a band is the step of the level it climbs to: it is not moved
    rock = sorted(cell for cell, k in c.kind.items() if k == ROCK and cell not in c.band)
    for _ in range(8):
        moved = 0
        for cell in rock:
            here = U[cell]
            best, best_score = here, score(*cell)
            x, y = cell
            for u in (here + LEVEL, here - LEVEL):
                U[cell] = u
                sc = score(*cell)
                need = 2 if cell in fixed else 1
                # as good, and hung from the rock above or beside at that
                # level: a face's end goes on with its face
                held = u > here and cell not in fixed and any(
                    U.get(n) == u or flat.get(n) == u for n in ((x, y - 1), (x - 1, y), (x + 1, y)))
                if sc >= best_score + need or (sc == best_score and held):
                    best, best_score = u, sc
            U[cell] = best
            moved += best != here
        if not moved:
            break
    return U


# -- 4. corners ---------------------------------------------------------------

NAMES = ("NW", "NE", "SW", "SE")


def corner_pixel_top(c, cell, corner):
    """Is the drawing top-coloured at that corner of the cell?"""
    cls = pixel_classes(c.art(cell), c.metatile(cell))
    xs = range(0, 3) if corner in ("NW", "SW") else range(13, 16)
    ys = range(0, 3) if corner in ("NW", "NE") else range(13, 16)
    return sum(1 for yy in ys for xx in xs if cls[yy][xx] == "T") >= 5


def default_high(c, cell, k):
    """Is corner k of a rock cell high in its own piece? A face is high
    along its north edge, a band along its high side."""
    side = c.band.get(cell)
    if side == "W":
        return NAMES[k] in ("NE", "SE")
    if side == "E":
        return NAMES[k] in ("NW", "SW")
    return NAMES[k] in ("NW", "NE")


def corners(c, U, flat):
    """{cell: [NW, NE, SW, SE]} levels."""
    def options(cell):
        if cell in flat:
            return {flat[cell]}
        return {U[cell], U[cell] - LEVEL}

    def resolve(cells):
        common = None
        for cell, k, opt in cells:
            common = set(opt) if common is None else common & opt
        if common and len(common) == 1:
            return next(iter(common))
        if common:
            # rock only round it, either level: the pieces say, if they
            # agree; else the drawing (top colour at that corner)
            votes = {default_high(c, cell, k) for cell, k, _ in cells if cell not in flat}
            if len(votes) == 1:
                top = votes.pop()
            else:
                top = any(corner_pixel_top(c, cell, NAMES[k]) for cell, k, _ in cells)
            return max(common) if top else min(common)
        return None

    out = {cell: [None] * 4 for cell in c.kind}
    # the corner of each of the four cells round lattice point (gx, gy)
    around = (((-1, -1), 3), ((0, -1), 2), ((-1, 0), 1), ((0, 0), 0))
    for gy in range(c.ys.start, c.ys.stop + 1):
        for gx in range(c.xs.start, c.xs.stop + 1):
            cells = []
            for (dx, dy), k in around:
                cell = (gx + dx, gy + dy)
                if cell in c.kind:
                    cells.append((cell, k, options(cell)))
            if not cells:
                continue
            # a rim's north edge is its back: what is behind it is not tied
            # to it, the cells north of the point and south of it part
            lower = [e for e in cells if e[1] in (0, 1)]
            if any(e[0] in c.rim for e in lower):
                groups = [lower, [e for e in cells if e[1] in (2, 3)]]
            else:
                groups = [cells]
            for group in groups:
                if not group:
                    continue
                v = resolve(group)
                flats = [flat[cell] for cell, k, opt in group if cell in flat]
                for cell, k, opt in group:
                    if v is not None:
                        out[cell][k] = v
                    elif cell in flat:
                        # a step: each cell keeps its own, rock its own piece's
                        out[cell][k] = flat[cell]
                    elif default_high(c, cell, k):
                        out[cell][k] = U[cell]
                    else:
                        # rock reaches the ground at its foot, however far
                        # down it is: its fall stretches, it leaves no step
                        low = U[cell] - LEVEL
                        below = [f for f in flats if f < low]
                        out[cell][k] = max(min(below), U[cell] - MAX_FALL) if below else low
    return out


def piece_cost(c, cell, v):
    """How far four corners [NW, NE, SW, SE] are from what a tile of rock is:
    one level of fall (no more, no less, never flat), towards its own side -
    a face falls south, a band towards its low side."""
    a, b, d, e = v
    r = max(v) - min(v)
    cost = 4.0 if r == 0 else abs(r - LEVEL) / LEVEL * 1.5
    side = c.band.get(cell)
    if side == "E":
        wrong = max(0, b - a) + max(0, e - d)
    elif side == "W":
        wrong = max(0, a - b) + max(0, d - e)
    else:
        wrong = max(0, d - a) + max(0, e - b)
    return cost + 2.0 * wrong / LEVEL


def lattice(c, flat, init):
    """The corners solved together. A lattice point is one height for every
    cell round it, so no two cells part there - except where the drawing
    says they part: a rim's back (the cells behind it), and two flat cells
    at different levels (a step no rock is drawn in; each keeps its own).
    A point a flat cell touches is that cell's level; the rest, touched by
    rock only, take the heights that leave every tile of rock closest to
    one level of fall towards its own side (piece_cost), all at once."""
    around = (((-1, -1), 3), ((0, -1), 2), ((-1, 0), 1), ((0, 0), 0))
    owner = {}          # (cell, k) -> point id
    fixed = {}          # point id -> level
    domain = {}         # point id -> allowed levels (None: any)
    value = {}
    for gy in range(c.ys.start, c.ys.stop + 1):
        for gx in range(c.xs.start, c.xs.stop + 1):
            cells = [((gx + dx, gy + dy), k) for (dx, dy), k in around if (gx + dx, gy + dy) in c.kind]
            if not cells:
                continue
            lower = [e for e in cells if e[1] in (0, 1)]
            if any(e[0] in c.rim for e in lower):
                groups = [("lo", lower), ("hi", [e for e in cells if e[1] in (2, 3)])]
            else:
                groups = [("all", cells)]
            for tag, group in groups:
                if not group:
                    continue
                pid = (gx, gy, tag)
                levels = {flat[cell] for cell, k in group if cell in flat}
                rocks = [(cell, k) for cell, k in group if cell not in flat]
                if len(levels) == 1:
                    fixed[pid] = value[pid] = levels.pop()
                    for e in group:
                        owner[e] = pid
                    continue
                for cell, k in group:
                    if cell in flat:        # a flat step: each its own
                        own = (gx, gy, tag, cell)
                        fixed[own] = value[own] = flat[cell]
                        owner[(cell, k)] = own
                for e in rocks:
                    owner[e] = pid
                if rocks:
                    domain[pid] = sorted(levels) if levels else None
                    vals = [init[cell][k] for cell, k in rocks]
                    v = max(set(vals), key=lambda x: (vals.count(x), x))
                    if levels and v not in levels:
                        v = min(levels, key=lambda f: abs(f - v))
                    value[pid] = v
    rock_cells = [cell for cell, k in c.kind.items() if cell not in flat]
    points_of = {cell: [owner[(cell, k)] for k in range(4)] for cell in rock_cells}
    cells_at = collections.defaultdict(list)
    for cell, pts in points_of.items():
        for p in set(pts):
            cells_at[p].append(cell)

    def cost(cell):
        return piece_cost(c, cell, [value[p] for p in points_of[cell]])

    free = sorted(p for p in cells_at if p not in fixed)
    for _ in range(24):
        moved = 0
        for p in free:
            here = value[p]
            if domain[p] is not None:
                cand = list(domain[p])
            else:
                near = {value[q] for cell in cells_at[p] for q in points_of[cell]}
                cand = sorted(near | {here - LEVEL, here + LEVEL})
            best, best_cost = here, None
            for v in cand:
                value[p] = v
                total = sum(cost(cell) for cell in cells_at[p]) + 0.01 * abs(v - init_of(p, cells_at, points_of, init)) / LEVEL
                if best_cost is None or total < best_cost - 1e-9:
                    best, best_cost = v, total
            value[p] = best
            moved += best != here
        if not moved:
            break
    out = {}
    for cell in c.kind:
        if cell in flat:
            out[cell] = [flat[cell]] * 4
        else:
            out[cell] = [value[p] for p in points_of[cell]]
    return out


def init_of(p, cells_at, points_of, init):
    """The height a point had before the solve (the first cell's)."""
    cell = cells_at[p][0]
    return init[cell][points_of[cell].index(p)]


# -- 5. shapes ----------------------------------------------------------------

def shape(cn):
    """5 x 5 heights of a cell from its corners [NW, NE, SW, SE]: two flat
    triangles, split along the diagonal that falls the most (a hip's or a
    valley's crease runs through its odd corner). Every edge is the straight
    line between its two corners, so cells sharing corners meet exactly."""
    nw, ne, sw, se = cn
    grid = [[0.0] * (N + 1) for _ in range(N + 1)]
    main = abs(nw - se) >= abs(ne - sw)
    for j in range(N + 1):
        for i in range(N + 1):
            u, v = i / N, j / N
            if main:        # crease NW-SE
                if u >= v:
                    h = nw + (ne - nw) * u + (se - ne) * v
                else:
                    h = nw + (sw - nw) * v + (se - sw) * u
            else:           # crease NE-SW
                if u + v <= 1:
                    h = nw + (ne - nw) * u + (sw - nw) * v
                else:
                    h = se + (sw - se) * (1 - u) + (ne - se) * (1 - v)
            grid[j][i] = h
    return grid


# -- 5. blocks ----------------------------------------------------------------
#
# Every cell is one piece of the tile grid, kept inside its own square:
#
#   F  a top: level all over
#   S  a face: the vertical south wall of one level (at 45 degrees a drawn
#      pixel at (u, h, v + h) with h falling a pixel per pixel down the tile
#      is the wall z = row + level)
#   W  a band falling west: a 45-degree slope one level across the tile,
#   E  high on its east side (W) or on its west side (E); W2/E2 the same
#      falling two levels (where a diagonal flank has one tile for two)
#   cSE/cSW/vSE/vSW  a face and a band in one tile (a terrace's corner, a
#      face's end turning into the band of the next step: what 07d draws)
#
# Wherever two pieces differ along their shared edge, the engine stands a
# wall there (records): no piece is ever bent, stretched or cut across.

FIN = 3.0               # a band climbing past the terrace it flanks
STEEP = 0.4             # a band falling two levels in its tile (a flank's
                        # head on a diagonal edge, one tile for two levels)
FACE_SHARE = 0.4        # of its pixels drawn as face: a face, else a top
# A face meeting a band in one tile (a face's end turning into the band of
# the next step, a terrace's corner): cSE/cSW convex, vSE/vSW concave, the
# band falling east/west.
CORNERS = ("cSE", "cSW", "vSE", "vSW")
CORNER_KINDS = list(CORNERS) if os.environ.get("VOXEL_TERRACES_CORNERS", "1") != "0" else []
CORNER = 0.3            # taking a corner piece instead of the drawing's kind


def is_face(c, cell):
    if cell in c.band:
        return False
    if c.kind.get(cell) == ROCK and (c.is_stair(cell) or c.muddy(cell)):
        return True     # stairs up a face climb its level as the face does
    cls = pixel_classes(c.art(cell), c.metatile(cell))
    f = sum(row.count("F") for row in cls)
    return f >= FACE_SHARE * 256


def foot_of(piece):
    kind, top = piece
    return top - LEVEL if kind == "S" or kind in CORNERS else top


def blocks(c, U, flat, kept=None):
    """{cell: (kind, top level)}: one piece a cell.

    The kind is the drawing's (a band a ramp its way, a tile mostly drawn as
    face a face, else a top) and the level the one its column or row counts
    (U). Then the pieces of rock are settled together: each takes the kind
    and level, of a few round its own, that leaves it meeting its neighbours
    best - an edge two pieces share at the same heights costs nothing, a
    step costs by its height. A step west or east is a block's side (half);
    a top standing over lower ground south of it (no face drawn for the
    drop) a fault; a back (the ground behind a top lower than it) is how
    terraces end and costs little. Moving off the counted level costs, and so does taking the
    other kind than the drawing's. A pit (rock rising behind a face's foot
    or a band's: open to the sky between two walls) is never worth it. Flat
    terraces, ledges and berry soil do
    not move; nothing is ever bent: every piece stays one of four."""
    out = {}
    pref = {}
    for cell in c.kind:
        if cell in flat:
            out[cell] = ("F", flat[cell])
            x, y = cell
            if c.metatile(cell) in BOULDER_HEAD:
                # a boulder's upper half beside a terrace a level higher: the
                # flank of that terrace's rounded head, drawn over the lower
                # one - a band up to it, not a side wall the drawing lacks
                for n, side in (((x + 1, y), "W"), ((x - 1, y), "E")):
                    if c.kind.get(n) == TOP and flat.get(n) == flat[cell] + LEVEL:
                        out[cell] = (side, flat[n])
        elif cell in c.band:
            x, y = cell
            side = c.band[cell]
            hi, lo = ((x + 1, y), (x - 1, y)) if side == "W" else ((x - 1, y), (x + 1, y))
            if hi in flat and lo in flat and flat[hi] <= flat[lo]:
                # a band between two grounds at one level (a sand bank the
                # solve put level with the sea): nothing to fall, it lies flat
                out[cell] = ("F", flat[hi])
                continue
            pref[cell] = c.band[cell]
            out[cell] = (c.band[cell], U[cell])
        else:
            pref[cell] = "S" if is_face(c, cell) else "F"
            out[cell] = (pref[cell], U[cell])
    grid = {cell: piece_grid(*p) for cell, p in out.items()}
    kept = kept or {}
    grid.update(kept)
    trees = {cell for cell, k in c.kind.items() if k == UNKNOWN and c.role(cell) == "tree"}
    free = sorted(cell for cell in out if cell not in flat and cell not in c.keep
                  and c.kind[cell] in (ROCK, TOP))

    def step(p, q, back):
        """The cost of an edge point at heights p (north/west) and q. `back`:
        north/south, and what the cost of the south one rising is (a top's
        back over the ground behind it: cheap; rock rising behind a face's
        foot: a pit between two walls, open to the sky)."""
        d = abs(p - q)
        if d < 0.5:
            return 0.0
        k = 1.0 + 0.5 * (d / LEVEL - 1.0)
        if back is None:
            return 0.5 * k          # west/east: a block's side, a wall
        return k * (back if q > p else 1.0)

    def kind_at(n, piece):
        return piece[0] if n is None else (out[n][0] if n in out and n not in kept else "F")

    def cost(cell, g, piece):
        x, y = cell
        total = 0.0
        for n, mine, theirs, ns in (((x - 1, y), "W", "E", False), ((x + 1, y), "E", "W", False),
                                    ((x, y - 1), "N", "S", True), ((x, y + 1), "S", "N", True)):
            if n not in grid or n in trees:
                # (a tree's ground is not drawn: the face behind it goes on
                # down to it, whatever meets it - behind_trees)
                continue
            a, b = edge(g, mine), edge(grid[n], theirs)
            if n in kept and any(q - p > 0.5 for p, q in zip(a, b)):
                # a ledge or berry soil keeps its own relief and walls: it
                # never stands over a piece beside it (no wall would close it)
                total += 10.0
            if mine in ("W", "N"):
                a, b = b, a         # p is the north / west one
            back = None
            if not ns:
                # a band standing over the terrace on its high side (or a
                # neighbour band over us on its high side) is a fin: the
                # flank climbs past what it is the flank of
                west, east = (n, cell) if mine == "W" else (cell, n)
                kw, ke = kind_at(west, piece) if west != cell else piece[0],                     kind_at(east, piece) if east != cell else piece[0]
                kw, ke = kw[0], ke[0]
                fin = (kw == "W" and max(p - q for p, q in zip(a, b)) > 0.5) or                       (ke == "E" and max(q - p for p, q in zip(a, b)) > 0.5)
                if fin:
                    total += FIN
            if ns:
                north = kind_at(n, piece) if mine == "N" else piece[0]
                south = piece[0] if mine == "N" else kind_at(n, piece)
                # behind a top the ground runs on under the next (a back);
                # behind a face's foot, or a band's under a face or a band,
                # rock rising is a pit between two walls
                back = 0.3 if north == "F" or (north != "S" and south == "F") else pit[0]
            total += sum(step(p, q, back) for p, q in zip(a, b)) / (N + 1)
        total += 0.25 * abs(piece[1] - U[cell]) / LEVEL
        if piece[0] in CORNERS:
            total += CORNER
        elif cell in pref and piece[0][0] != pref[cell]:
            total += 0.6
        elif piece[0] in ("W2", "E2"):
            total += STEEP
        return total

    # the pits' cost rises in steps: with it low every piece first finds
    # the level its own kind and its neighbours want; raised, the pieces
    # round a pit move to close it (one at a time could not: a band's foot
    # over a face needs the face to give way first)
    pit = [0.3]
    for pit[0], sweeps in ((0.3, 10), (1.0, 10), (3.0, 10), (10.0, 30)):
        for _ in range(sweeps):
            moved = 0
            for cell in free:
                kind, top = out[cell]
                if cell in c.band and kind == "F":
                    continue
                kinds = [c.band[cell], c.band[cell] + "2"] if cell in c.band else ["F", "S"] + CORNER_KINDS
                tops = {U[cell] + LEVEL * k for k in (-2, -1, 0, 1, 2)} | {top}
                best, best_cost = out[cell], cost(cell, grid[cell], out[cell])
                for k2 in kinds:
                    for t in sorted(tops):
                        piece = (k2, t)
                        if piece == out[cell]:
                            continue
                        cc = cost(cell, piece_grid(*piece), piece)
                        if cc < best_cost - 1e-6:
                            best, best_cost = piece, cc
                if best != out[cell]:
                    out[cell] = best
                    grid[cell] = piece_grid(*best)
                    moved += 1
            if not moved:
                break
    # a boulder's upper half over a band: the rounded head a crest starts
    # with, drawn on the ridge it rises from - the crest's own band from
    # there (as a flat top the crest rose at the next row, a cap left on it)
    for cell in sorted(out, key=lambda cl: -cl[1]):
        x, y = cell
        s = (x, y + 1)
        if c.metatile(cell) in BOULDER_HEAD and cell in flat and s in c.band and out[s][0][0] in "WE":
            out[cell] = out[s]
    return out


def split_kind(kind):
    """(kind, drop in pixels) of a piece's kind: "S:8" is a face falling 8
    pixels (a fall shared out between more rows or columns of rock than it
    has levels), "S" one level."""
    if ":" in kind:
        k, d = kind.split(":")
        return k, float(d)
    return kind, None


def piece_grid(kind, top):
    """5 x 5 heights of a piece."""
    kind, drop = split_kind(kind)
    if drop is not None:
        if kind in CORNERS:
            east = kind[2] == "E"
            pick = max if kind[0] == "c" else min
            return [[top - drop / N * pick(i if east else N - i, j) for i in range(N + 1)]
                    for j in range(N + 1)]
        if kind == "S":
            return [[top - drop * j / N] * (N + 1) for j in range(N + 1)]
        if kind == "W":
            return [[top - drop + drop * i / N for i in range(N + 1)] for _ in range(N + 1)]
        if kind == "E":
            return [[top - drop * i / N for i in range(N + 1)] for _ in range(N + 1)]
    if kind in CORNERS:
        # a face and a band meeting: one level down, the face falling south
        # and the band east or west, the nearer of the two (convex: a
        # terrace's corner, face under the diagonal, band over it) or the
        # farther (concave: the two walls meeting inside)
        east = kind[2] == "E"
        pick = max if kind[0] == "c" else min
        return [[top - LEVEL / N * pick(i if east else N - i, j) for i in range(N + 1)] for j in range(N + 1)]
    if kind == "S":
        return [[top - LEVEL * j / N] * (N + 1) for j in range(N + 1)]
    if kind in ("W", "W2"):
        drop = LEVEL * (2 if kind == "W2" else 1)
        return [[top - drop + drop * i / N for i in range(N + 1)] for _ in range(N + 1)]
    if kind in ("E", "E2"):
        drop = LEVEL * (2 if kind == "E2" else 1)
        return [[top - drop * i / N for i in range(N + 1)] for _ in range(N + 1)]
    return [[top] * (N + 1) for _ in range(N + 1)]


# A band's column is drawn with its ends: a head where it starts under what
# is behind it (074 falling east, 089 west) and a foot where it turns into
# the face below (07a/07d/092 east, 07b/078/090 west).
BAND_HEAD = {"E": {0x074}, "W": {0x089}}
BAND_FOOT = {"E": {0x07a, 0x07d, 0x092}, "W": {0x07b, 0x078, 0x090}}


def segments(c):
    """[(side, head or None, [band cells], foot or None)]: every column of
    bands of one side with the ends drawn above and below it."""
    out = []
    for (x, y), side in sorted(c.band.items()):
        if c.band.get((x, y - 1)) == side:
            continue
        cells = [(x, y)]
        while c.band.get((x, cells[-1][1] + 1)) == side:
            cells.append((x, cells[-1][1] + 1))
        head, foot = (x, y - 1), (x, cells[-1][1] + 1)
        head = head if head in c.src and c.kind.get(head) == ROCK and head not in c.band \
            and c.role_id(head) in BAND_HEAD[side] else None
        foot = foot if foot in c.src and c.kind.get(foot) == ROCK and foot not in c.band \
            and c.role_id(foot) in BAND_FOOT[side] else None
        out.append((side, head, cells, foot))
    # a face's end drawn as a head straight over a foot, no band between
    # (074 over 07a: one diagonal level, steps()): a column of no band
    for (x, y), k in sorted(c.kind.items()):
        if k != ROCK or (x, y) in c.band or (x, y + 1) not in c.src:
            continue
        for side in ("E", "W"):
            foot = (x, y + 1)
            # (with ground on its low side: the end of a face, the rock
            # ending there - a run of such ends drawn rock against rock is
            # the face's own pattern, read by the counts)
            low = 1 if side == "E" else -1
            if c.role_id((x, y)) in BAND_HEAD[side] and c.kind.get(foot) == ROCK \
                    and foot not in c.band and c.role_id(foot) in BAND_FOOT[side] \
                    and all(n in c.src and c.kind.get(n) != ROCK for n in ((x + low, y), (x + low, y + 1))):
                out.append((side, (x, y), [], foot))
    return out


def staircases(c, pieces, flat, kept):
    """A diagonal edge drawn as a staircase of band columns (Route 111's
    cliffs falling south-east by the stairs: 074 over 072/075 over 07a/07d,
    the next column a level lower two rows on) is one band a column: its
    head, its bands and its foot one level, the level of what its high side
    meets - the terrace beside it, or the low edge of the column before it.
    Counted by rows and settled cell by cell, the columns of two steps were
    mixed (a band a level up beside its own foot: a wall between them, the
    dark slit; a head as a top, a box), as the corners of a staircase hold
    no run of the counts.

    Each column is then the pieces the drawing has: the head a concave
    corner (vSE/vSW: what is behind it meets its top, the band below it its
    slope), the bands their ramp, the foot a convex corner (cSE/cSW: the
    band turning into the face under it) - the pieces of the same level
    meet each other, the column before and the one after along whole
    edges."""
    # (a band lying flat between two grounds at one level, or falling two
    # levels down a flank of one tile, is left as the blocks chose it)
    # (nor a column with water on its low side: a rock's flank in the sea is
    # no staircase but a mound's own side - the level of the top beside it
    # stood the band over the water and its corner below, a step the width
    # of the rock: Route 130's rocks; a cliff's end beside a waterfall has
    # nothing to fall to - its corners fell a level under the river's edge:
    # Safari Zone's waterfalls)
    def dry(s):
        out = 1 if s[0] == "E" else -1
        return not any(c.water((cell[0] + out, cell[1])) for cell in [s[1], s[3]] + list(s[2]) if cell)
    segs = [s for s in segments(c) if dry(s) and all(cell not in c.keep and pieces.get(cell, ("",))[0] == s[0]
                                                     for cell in s[2])]

    def north_edge(n):
        """The heights along the south edge of `n` (north of a head)."""
        if n in kept:
            return None
        if n in flat and c.kind.get(n) != ROCK:
            return [flat[n]] * (N + 1)
        p = pieces.get(n)
        return None if p is None else edge(piece_grid(*p), "S")

    def high_edge(n, side):
        """The heights along the edge of `n` facing a band of `side`, or None."""
        if n in kept:
            return None
        if n in flat and c.kind.get(n) != ROCK:
            return [flat[n]] * (N + 1)
        p = pieces.get(n)
        if p is None:
            return None
        return edge(piece_grid(*p), "E" if side == "E" else "W")

    # a fin: rock standing over the ground on three sides of it, west, east
    # and behind (Route 111's staircase of face ends by the logs: an end
    # between two patches of a ledge, read as a band, stood a level over
    # them, dark walls either side) - it is that ground's level
    def facing_we(n, side):
        if n in kept:
            return None
        if n in flat and c.kind.get(n) != ROCK:
            return [flat[n]] * (N + 1)
        p = pieces.get(n)
        return None if p is None else edge(piece_grid(*p), "E" if side == "W" else "W")
    for cell, p in sorted(pieces.items()):
        if c.kind.get(cell) != ROCK or cell in c.keep or cell in kept:
            continue
        x, y = cell
        # (the sea round a rock's end is no ground it stands on: Route 133's
        # sandbars end in rock with water west, east and behind)
        if any(c.water(n) for n in ((x - 1, y), (x + 1, y), (x, y - 1))):
            continue
        sides = [facing_we((x - 1, y), "W"), facing_we((x + 1, y), "E"),
                 facing_ns(c, pieces, flat, kept, (x, y - 1), "N")]
        if any(e is None or max(e) - min(e) > 0.5 for e in sides):
            continue
        level = sides[0][0]
        if any(abs(e[0] - level) > 0.5 for e in sides):
            continue
        if max(max(r) for r in piece_grid(*p)) > level + 0.5:
            pieces[cell] = ("F", level)
    for _ in range(4):
        moved = 0
        for side, head, cells, foot in segs:
            step = -1 if side == "E" else 1        # towards the high side
            votes = collections.Counter()
            # (a head straight over its foot: what is behind the head and on
            # its high side)
            ask = cells or [head]
            for cell in ask:
                n = (cell[0] + step, cell[1])
                e = high_edge(n, side)
                if e is None or max(e) - min(e) > 0.5:
                    continue
                # a tree's ground says the least (a wood beside a band stands
                # where the solve put it); the terrace or the band before it
                votes[e[0]] += 1 if c.kind.get(n) == UNKNOWN else 2
            if not cells:
                n = (head[0], head[1] - 1)
                e = north_edge(n)
                if e is not None and max(e) - min(e) <= 0.5:
                    votes[e[0]] += 1 if c.kind.get(n) == UNKNOWN else 2
            # a column along the map's edge, nothing on its high side: it
            # stands a level over what its low side meets (Lavaridge's west
            # cliff, two columns of bands down from the edge: the outer one
            # stood a level low by its head, a step across it two rows down)
            edge_column = bool(cells) and all((cell[0] + step, cell[1]) not in c.src for cell in cells)
            if edge_column:
                for cell in cells:
                    e = high_edge((cell[0] - step, cell[1]), "W" if side == "E" else "E")
                    if e is not None and max(e) - min(e) <= 0.5:
                        votes[e[0] + LEVEL] += 2
            if not votes:
                continue
            top = max(votes.items(), key=lambda kv: (kv[1], kv[0]))[0]
            # a column whose ends meet ground of another level - the ground
            # behind its head not at its top, or the ground its foot's face
            # falls to not a level under it - is no step of a staircase: the
            # drawing has the level change along the column (Safari Zone's
            # lanes: a head under a terrace a level over the band's, drawn
            # as the face it is). As one level it stood a step facing the
            # camera across its head or its foot: left as the blocks chose
            # (a head under a face whose foot is a level under the band's top
            # is the band itself, starting under the face: the face falls to
            # the band's low edge, and the band goes on up behind it - as a
            # concave corner its top stood a level over the face's foot, or,
            # failing the check, it lay flat with the band drawn on it)
            head_piece = None if head is None else ("vS" + side, top)
            if cells:
                ends = []
                if head is not None:
                    n = (head[0], head[1] - 1)
                    e = north_edge(n)
                    p = pieces.get(n)
                    if (c.kind.get(n) == ROCK and p is not None and p[0] == "S" and n not in kept
                            and e is not None and max(e) - min(e) <= 0.5 and abs(e[0] + LEVEL - top) <= 0.5):
                        head_piece = (side, top)
                    elif e is not None and max(e) - min(e) <= 0.5 and c.kind.get(n) != UNKNOWN:
                        if edge_column and abs(e[0] - top) > 0.5:
                            head_piece = None   # left as the blocks chose
                        else:
                            ends.append(e[0] - top)
                if foot is not None:
                    n = (foot[0], foot[1] + 1)
                    if n in flat and n not in kept and c.kind.get(n) in (TOP, GROUND):
                        ends.append(flat[n] + LEVEL - top)
                if any(abs(d) > 0.5 for d in ends):
                    continue
            want = [(cell, (side, top)) for cell in cells]
            if head_piece is not None:
                want.append((head, head_piece))
            if foot is not None:
                want.append((foot, ("cS" + side, top)))
            for cell, piece in want:
                if cell in kept or cell in c.keep:
                    continue
                if pieces.get(cell) != piece:
                    pieces[cell] = piece
                    moved += 1
        if not moved:
            break


def level_bands(c, pieces, flat, kept, measured=frozenset()):
    """A band with nothing to fall: the ground on its low side stands as
    high as the edge on its high side (a flank drawn two bands wide for the
    one level between a terrace and the meadow beside it - Route 111 by the
    Winstrates' stairs: the outer band fell a level more, under the meadow,
    and the meadow's edge hung over a black wedge where the band's head is
    drawn on its grass). It lies flat at that level, its drawing on it; and
    where every band of a column lies flat so do the head and the foot drawn
    at its ends (corners of a fall there is not)."""
    def facing(n, side):
        """The heights along the edge of `n` that faces a cell on its
        `side` ("W": n is west of it), or None."""
        if n in kept:
            return None
        if n in flat and c.kind.get(n) != ROCK:
            return [flat[n]] * (N + 1)
        p = pieces.get(n)
        return None if p is None else edge(piece_grid(*p), "E" if side == "W" else "W")

    segs = segments(c)
    # the same for a face: a column of faces drawn between ground above and
    # ground below fewer levels apart than it has rows (Route 111's rocky
    # slope by the logs: a strip of face between two patches of grass of a
    # level, two rows of face for one level under a ledge) has rows with
    # nothing to fall - as faces they stood over the ground above them, a
    # pit walled round. The upper ones lie flat at the level above, the
    # rest fall a level a row.
    def plain(cell):
        return c.kind.get(cell) == ROCK and cell not in c.band and cell not in c.keep \
            and cell not in kept and pieces.get(cell, ("",))[0] in ("S",) + tuple(CORNERS)
    for (x, y) in sorted(pieces):
        # (from under ground, or under a corner - the foot a band ends in,
        # level along its south edge)
        if not plain((x, y)) or (c.kind.get((x, y - 1)) == ROCK and pieces.get((x, y - 1), ("",))[0]
                                 not in CORNERS):
            continue
        if plain((x, y - 1)) and pieces[(x, y - 1)][0] not in CORNERS:
            continue
        run = [(x, y)]
        while plain((x, run[-1][1] + 1)):
            run.append((x, run[-1][1] + 1))
        below = (x, run[-1][1] + 1)
        if c.kind.get(below) == ROCK:
            continue
        # (nor hanging from water: the frilled rim of a sandbar's top reads
        # as the sea round it - Route 133; nor standing in it: a rock's
        # corner in the sea, between the rim of its top and the sea at its
        # foot, has the rock beside it to fall from - laid flat, Route 134's
        # rocks lost a corner and stood walled)
        if c.water((x, y - 1)) or c.water(below):
            continue
        north = facing_ns(c, pieces, flat, kept, (x, y - 1), "N")
        # (a wood at the foot stands where the face's count put it: its
        # level says nothing of the face's - unless the count stood the
        # face over the ground behind it, a fin: Route 111's two rows of
        # face over the wood south of the logs, counted two levels for the
        # one between the meadow and the wood, a peak over the meadow; or
        # sunk under it, a pit: Safari Zone's face of two rows between two
        # woods, counted a level under the wood behind it)
        if c.kind.get(below) == UNKNOWN and (north is None or abs(max(
                max(r) for r in piece_grid(*pieces[run[0]])) - max(north)) <= 0.5):
            continue
        south = facing_ns(c, pieces, flat, kept, below, "S")
        if north is None or south is None or max(north) - min(north) > 0.5 \
                or max(south) - min(south) > 0.5:
            continue
        fall = (north[0] - south[0]) / LEVEL
        if abs(fall - round(fall)) > 0.01 or not 0 <= round(fall) <= len(run):
            continue
        fall = int(round(fall))
        if fall and any(pieces[cell][0] != "S" for cell in run):
            continue        # (corners in a run that falls: left as counted)
        if not fall:
            # nothing to fall at all. Where a count of the drawing measured
            # this face, the solve dropped it against others: the face stands
            # as counted (laid flat, Safari Zone's middle terrace was rock
            # painted on the meadow below it). Where none did, it is the
            # drawing's own contradiction (Route 111's face end between two
            # scraps of one meadow by the logs): flat
            if ((x, y - 1), below) in measured:
                continue
            for cell in run:
                pieces[cell] = ("F", north[0])
            continue
        # whole faces from the top, one level a row, as many as the fall
        # has; the rows left over lie flat at the foot (a face of a share of
        # a level was neither face nor top - a twisted strip breaking the
        # cliff's shape; the flat rows at its foot are mostly hidden by what
        # stands in front of it, a wood)
        for i, cell in enumerate(run):
            pieces[cell] = ("S", north[0] - LEVEL * i) if i < fall else ("F", north[0] - LEVEL * fall)
    # a row of bands of one side between two grounds (west and east of it)
    # fewer levels apart than it has bands: the fall shared out between them,
    # each a band of its share (the flank of a terrace drawn two bands wide
    # for its one level: the outer one fell a level more, under the meadow);
    # none to fall at all, they lie flat. The ends drawn at a column's ends
    # go with the band beside them: a head with the first band's share, a
    # foot with the last's (a column falling its share at the foot's row
    # gives the next column down the flank its edge to fall from) - so
    # rows and ends are worked out turn by turn.
    def chains():
        moved = 0
        wants = {}
        for (x, y), side in sorted(c.band.items()):
            up = 1 if side == "W" else -1           # towards the high side
            if c.band.get((x + up, y)) == side:
                continue                            # not the chain's high end
            chain = [(x, y)]
            while c.band.get((chain[-1][0] - up, y)) == side:
                chain.append((chain[-1][0] - up, y))
            if any(pieces.get(cell, ("",))[0] not in (side, "F") and split_kind(
                    pieces.get(cell, ("",))[0])[0] != side or cell in c.keep for cell in chain):
                continue
            hi_n, lo_n = (x + up, y), (chain[-1][0] - up, y)
            hi = facing(hi_n, "E" if side == "W" else "W")
            lo = facing(lo_n, "W" if side == "W" else "E")
            if hi is None or lo is None or max(hi) - min(hi) > 0.5 or max(lo) - min(lo) > 0.5:
                continue
            fall = hi[0] - lo[0]
            if fall >= LEVEL * len(chain) - 0.5:
                continue                            # as counted
            if fall <= 0.5:
                want = [("F", hi[0])] * len(chain)
            elif abs(fall / LEVEL - round(fall / LEVEL)) > 0.01:
                share = fall / float(len(chain))
                want = [("%s:%g" % (side, share), hi[0] - share * i) for i in range(len(chain))]
            else:
                # whole bands from the high side, the rest flat at the foot
                full = int(round(fall / LEVEL))
                want = [(side, hi[0] - LEVEL * i) if i < full else ("F", hi[0] - LEVEL * full)
                        for i in range(len(chain))]
            wants[tuple(chain)] = (side, want)
        # a band laid flat where the column it is drawn in goes on falling
        # above or below it (Safari Zone's lanes: the top row of a column
        # beside the terrace it starts under, one level between its ends
        # for two bands) breaks that column - a step across it, the drawn
        # band lying as ground: the row stands as counted
        flat_at = {cell for chain, (side, want) in wants.items()
                   for cell, piece in zip(chain, want) if piece[0] == "F"}
        for chain, (side, want) in wants.items():
            if any(piece[0] == "F" and any(
                    c.band.get(n) == side and n not in flat_at and n not in c.keep
                    and split_kind(pieces.get(n, ("",))[0])[0] == side
                    for n in ((cell[0], cell[1] - 1), (cell[0], cell[1] + 1)))
                   for cell, piece in zip(chain, want)):
                continue
            for cell, piece in zip(chain, want):
                if pieces.get(cell) != piece:
                    pieces[cell] = piece
                    moved += 1
        return moved

    def ends():
        moved = 0
        for side, head, cells, foot in segs:
            if not cells:
                continue
            tops = {pieces[cell] for cell in cells if cell in pieces}
            if len(tops) == 1 and next(iter(tops))[0] == "F":
                # all flat: the ends too, where what is past them stands at
                # that level as well (a head drawn in the row of a face above
                # is that face's)
                level = next(iter(tops))[1]
                for end, beyond, mine in ((head, 0, "N"), (foot, 1, "S")):
                    if end is None or end in c.keep or end in kept:
                        continue
                    n = (end[0], end[1] - 1 if beyond == 0 else end[1] + 1)
                    e = facing_ns(c, pieces, flat, kept, n, mine)
                    if e is not None and all(abs(v - level) < 0.5 for v in e) \
                            and pieces.get(end) != ("F", level):
                        pieces[end] = ("F", level)
                        moved += 1
                continue
            for end, band, corner in ((head, cells[0], "vS"), (foot, cells[-1], "cS")):
                if end is None or end in c.keep or end in kept or band not in pieces:
                    continue
                k, share = split_kind(pieces[band][0])
                if share is None or k != side:
                    continue
                piece = ("%s%s:%g" % (corner, side, share), pieces[band][1])
                if pieces.get(end) != piece:
                    pieces[end] = piece
                    moved += 1
        return moved

    for _ in range(6):
        if not chains() + ends():
            break


def unstand(c, pieces, flat, kept):
    """A face (or a band, or a corner of the two) is the front of the
    ground on top of it: its top edge never stands over the ground behind
    it, a fin with lower ground at its back - from the game's camera a box
    over the meadow (Route 111's foot of a band by the logs south of the
    meadow, voted a level over the meadow and the faces beside it). Where
    one does it comes down to the ground behind it, as itself or flat,
    whichever meets the four cells round it best."""
    def nb(n, its):
        if n in kept:
            return None
        if n in flat and c.kind.get(n) != ROCK:
            return [flat[n]] * (N + 1)
        q = pieces.get(n)
        return None if q is None else edge(piece_grid(*q), its)

    def round_it(cell):
        x, y = cell
        return (((x, y - 1), "N", "S"), ((x, y + 1), "S", "N"), ((x - 1, y), "W", "E"), ((x + 1, y), "E", "W"))

    def misfit(cell, piece):
        g = piece_grid(*piece)
        total = 0.0
        for n, mine, its in round_it(cell):
            e = nb(n, its)
            if e is not None:
                total += sum(abs(a - b) for a, b in zip(edge(g, mine), e))
        return total

    for _ in range(2):
        moved = 0
        for cell, p in sorted(pieces.items()):
            if c.kind.get(cell) != ROCK or cell in c.keep or cell in kept or p[0] == "F":
                continue
            x, y = cell
            g = piece_grid(*p)
            behind = set()
            # (the edge it falls from, the edge it falls to, the cell behind)
            for back, hi, lo, its in (((x, y - 1), "N", "S", "S"), ((x + 1, y), "E", "W", "W"),
                                      ((x - 1, y), "W", "E", "E")):
                e = nb(back, its)
                if e is None:
                    continue
                top, foot = edge(g, hi), edge(g, lo)
                if sum(1 for t, f, b in zip(top, foot, e) if t - f > 0.5 and t - b > 0.5) >= 2:
                    behind |= {max(e), min(e)}
            if not behind:
                continue
            kind = p[0]
            cands = [(k, lv) for lv in sorted(behind) for k in (kind, "F")]
            best = min(cands, key=lambda q: misfit(cell, q))
            if misfit(cell, best) < misfit(cell, p) - 0.5:
                pieces[cell] = best
                moved += 1
        if not moved:
            break


def face_ends(c, pieces, flat, kept):
    """A face's drawn end (07d, 089...) beside a top at the face's own top
    level turns the corner into it: the concave corner (vSW with the top
    east of it, vSE west), its edge on the top's side the top's level all
    along. As a face its end stood a wall over the top's flank, from the
    game's camera a dark slit (Lavaridge's face under its north-west top
    beside the boardwalk's ledge)."""
    def nb(n, its):
        if n in kept:
            return None
        # (ground made a face - a gap in a face, stairs - is that face)
        if n in flat and c.kind.get(n) != ROCK and pieces.get(n, ("F",))[0] == "F":
            return [flat[n]] * (N + 1)
        q = pieces.get(n)
        return None if q is None else edge(piece_grid(*q), its)

    for cell, p in sorted(pieces.items()):
        if (c.kind.get(cell) != ROCK or cell in c.keep or cell in kept or p[0] != "S"
                or c.metatile(cell) not in UPPER_END | LOWER_END):
            continue
        x, y = cell
        g = piece_grid(*p)
        for kind, top_side, other, mine, theirs in (("vSW", (x + 1, y), (x - 1, y), "E", "W"),
                                                    ("vSE", (x - 1, y), (x + 1, y), "W", "E")):
            e, o = nb(top_side, theirs), nb(other, mine)
            # (a top, not a wood or a model's cell: their level is the
            # ground's round them, a guess - Route 133's islet traded a side
            # for a slanted strip under its tree; nor water: a waterfall is
            # no top, the face ends square beside it - turned, Safari Zone's
            # by its fall was a slanted strip into the water)
            if (e is None or c.kind.get(top_side) == UNKNOWN or c.water(top_side)
                    or any(abs(h - p[1]) > 0.5 for h in e)):
                continue
            # the face goes on along its other side as it was
            back = "W" if mine == "E" else "E"
            if o is not None and any(abs(a - b) > 0.5 for a, b in zip(edge(g, back), o)):
                continue
            # and flat ground in front meets its foot, the top's end of it
            # raised: a slanted strip between the two (Route 133's islets,
            # Safari Zone's), unless a model stands there (Lavaridge's
            # Center, its spring's stones)
            south = (x, y + 1)
            q = pieces.get(south)
            if q is not None and q[0] == "F" and south not in c.models and any(
                    abs(a - q[1]) > 0.5 for a in edge(piece_grid(kind, p[1]), "S")):
                continue
            pieces[cell] = (kind, p[1])
            break


_BUILDINGS = []


def modelled(members):
    """{cell: the cell its model stands on}: every cell a building's model
    owns (romfs/voxel/buildings.bin, as voxel_engine_view reads it), the
    model standing on its rectangle's bottom-left cell, as voxel_building.c
    stands it. Empty without the file, and empty unless VOXEL_RELIEF_BUILDINGS
    is set: the builder runs the relief and the buildings side by side
    (builder/emerald3ds_builder/voxel.py), so whether the file is there yet
    would decide the relief, and the release's CRC with it."""
    import voxel_engine_view as ev
    import voxel_relief_fixes as vrf
    if not os.environ.get("VOXEL_RELIEF_BUILDINGS"):
        return {}
    if not _BUILDINGS:
        try:
            _BUILDINGS.append(ev.Buildings())
        except (OSError, ValueError):
            _BUILDINGS.append(None)
    bld = _BUILDINGS[0]
    out = {}
    if bld is None:
        return out
    for lid, ox, oy in members:
        index = vrf.layout_index(lid)
        for lay, pm, x, y, _, _, _ in bld.placements:
            if lay != index:
                continue
            model = bld.page_models[pm][0]
            w, h = bld.models[model][:2]
            for j in range(h):
                for i in range(w):
                    if bld.heights[bld.model_heights[model] + j * w + i] != 0xFF:
                        out.setdefault((x + i + ox, y + j + oy), (x + ox, y + h - 1 + oy))
    return out


def seam_edges(c, pieces, ref, meets, kept):
    """An edge cell that met the map across its seam (seam_faces: the two
    within half a level in the world's placement) meets it still: where the
    terraces stood it a level off the seam's height (Lavaridge's south rim,
    a level over the town, against Route 112's own rim at the town's level),
    it is the band between the two, falling to the seam or from it. A
    face's back is never drawn: a seam north or south a level off is left."""
    def top(art, m):
        return sum(r.count("T") for r in pixel_classes(art, m)) >= 128

    for cell, (other, meet, side, (bx, by)) in sorted(meets.items()):
        p = pieces.get(cell)
        if (not meet or p is None or p[0] != "F" or cell not in ref or cell in kept or cell in c.keep
                or cell in c.ledges or side not in ("left", "right")):
            continue
        # one top drawn across the seam (Lavaridge's south rim going on as
        # Route 112's corner): each side stands it where its terraces put it
        # - the same level, the rim's - not down at the seam's old height,
        # a trench along the seam
        b_art = gr.terrace_art(other)
        if c.kind.get(cell) == TOP and top(c.art(cell), c.metatile(cell)) and top(b_art, b_art.metatile(bx, by)):
            continue
        r, h = ref[cell], p[1]
        if abs(abs(h - r) - LEVEL) > 0.5:
            continue
        # (a band west or east: "E" falls east, its west column the top)
        if side == "right":
            pieces[cell] = ("E", h) if h > r else ("W", r)
        else:
            pieces[cell] = ("W", h) if h > r else ("E", r)


def hidden_face_ends(c, pieces, flat, kept):
    """A cell a model hides (no level of its own) drawn with a face's end,
    between the face and the ground at its foot: the face's convex corner,
    closing it (Lavaridge's spring rim stone under the face behind the
    Center: flat, the face's end was a wall and its rock lay on the
    ground)."""
    def nb(n, its):
        if n in kept:
            return None
        if n in flat and c.kind.get(n) != ROCK:
            return [flat[n]] * (N + 1)
        q = pieces.get(n)
        return None if q is None else edge(piece_grid(*q), its)

    for cell, p in sorted(pieces.items()):
        if (c.kind.get(cell) != UNKNOWN or c.role(cell) == "tree" or c.house(cell) or cell in kept
                or p[0] != "F"):
            continue
        if sum(r.count("F") for r in pixel_classes(c.art(cell), c.metatile(cell))) < 48:
            continue
        x, y = cell
        for kind, face, low in (("cSW", (x + 1, y), (x - 1, y)), ("cSE", (x - 1, y), (x + 1, y))):
            q = pieces.get(face)
            if q is None or q[0] != "S" or face in kept or c.kind.get(face) != ROCK:
                continue
            foot = q[1] - LEVEL
            lows = [nb(low, "E" if kind == "cSW" else "W"), nb((x, y + 1), "N")]
            if abs(p[1] - foot) > 0.5 or any(e is None or any(abs(h - foot) > 0.5 for h in e) for e in lows):
                continue
            pieces[cell] = (kind, q[1])
            break


def rim_backs(c, pieces, flat, kept):
    """A band's head drawn again at the foot of its band, on the ground
    before a rim a level up (a diagonal flank drawn as a staircase of heads
    down to the rim): no band can fall from its high side there - the band
    beside it ends at its toe, a side wall - so it lay flat, its rock on
    the ground (Lavaridge's 26d at the south-west corner, the flat fault the
    scan saw). It is the rim's back, the band seen from behind: it rises
    from the toe to the rim's level, its high side the half its head cuts
    away (records), its rock meeting the rim at its corner."""
    def nb(n, its):
        if n in kept:
            return None
        if n in flat and c.kind.get(n) != ROCK and pieces.get(n, ("F",))[0] == "F":
            return [flat[n]] * (N + 1)
        q = pieces.get(n)
        return None if q is None else edge(piece_grid(*q), its)

    for cell, p in sorted(pieces.items()):
        side = c.band.get(cell)
        if (side is None or c.kind.get(cell) != ROCK or cell in c.keep or cell in kept or p[0] != "F"
                or head_of(c.art(cell), c.metatile(cell)) != HEAD_DRAWN[side]):
            continue
        x, y = cell
        before = (x - 1, y) if side == "E" else (x + 1, y)
        toe = nb(before, "E" if side == "E" else "W")
        rim = nb((x, y + 1), "N")
        if (c.band.get(before) != side or toe is None or rim is None
                or any(abs(h - p[1]) > 0.5 for h in toe) or any(abs(h - p[1] - LEVEL) > 0.5 for h in rim)):
            continue
        pieces[cell] = ("W" if side == "E" else "E", p[1] + LEVEL)


def hidden_drops(c, pieces, flat, kept):
    """Ground a level under the terrace north of it with no face drawn for
    the drop, a model or the stones of a pool drawn in front of it
    (Lavaridge's spring rim under its boardwalk): the cell is the face,
    or the face's corner, whichever meets all four cells round it. Left
    flat the drop was a slanted strip between the two. Only a piece that
    meets every neighbour exactly is taken."""
    def nb(n, its):
        if n in kept:
            return None
        if n in flat and c.kind.get(n) != ROCK and pieces.get(n, ("F",))[0] == "F":
            return [flat[n]] * (N + 1)
        q = pieces.get(n)
        return None if q is None else edge(piece_grid(*q), its)

    def misfit(cell, piece):
        x, y = cell
        g = piece_grid(*piece)
        total = 0.0
        for n, mine, its in (((x, y - 1), "N", "S"), ((x, y + 1), "S", "N"),
                             ((x - 1, y), "W", "E"), ((x + 1, y), "E", "W")):
            e = nb(n, its)
            if e is not None:
                total += sum(abs(a - b) for a, b in zip(edge(g, mine), e))
        return total

    cands = {}
    for cell, p in sorted(pieces.items()):
        if (c.kind.get(cell) not in (GROUND, UNKNOWN) or c.role(cell) == "tree" or p[0] != "F"
                or c.house(cell) or cell in kept or cell in c.keep or cell in c.ledges or cell in c.bridges):
            continue
        lid, lx, ly = c.src[cell]
        if c.water(cell) and not c.roles_of[lid].blocked(lx, ly):
            continue        # the water itself
        e = nb((cell[0], cell[1] - 1), "S")
        if e is not None and max(e) >= p[1] + LEVEL - 0.5 and misfit(cell, p) > 0.5:
            cands[cell] = p
    # (the cells of one drop are settled together: a face along it, its
    # corners at its ends - one at a time none of them met its neighbours)
    for cell, p in cands.items():
        pieces[cell] = ("S", p[1] + LEVEL)
    for _ in range(6):
        moved = 0
        for cell, p in cands.items():
            options = [p] + [(k, p[1] + LEVEL) for k in ("S", "cSW", "cSE", "vSW", "vSE")]
            best = min(options, key=lambda q: (misfit(cell, q), q != p))
            if best != pieces[cell] and misfit(cell, best) < misfit(cell, pieces[cell]) - 0.5:
                pieces[cell] = best
                moved += 1
        if not moved:
            break
    # a drop that does not close is left as it was
    for cell, p in cands.items():
        if misfit(cell, pieces[cell]) > 0.5:
            for n in [cell] + [m for m in cands if abs(m[0] - cell[0]) + abs(m[1] - cell[1]) == 1]:
                pieces[n] = cands[n]


def facing_ns(c, pieces, flat, kept, n, mine):
    """The heights along the edge of `n` that faces a cell north ("N": n
    is north of it) or south of it, or None."""
    if n in kept:
        return None
    if n in flat and c.kind.get(n) != ROCK:
        return [flat[n]] * (N + 1)
    p = pieces.get(n)
    return None if p is None else edge(piece_grid(*p), "S" if mine == "N" else "N")


# voxel_tree.c VoxelTree_Part: the General tileset's trees drawn as cards
# (a large tree's four parts, a small tree's trunk); and the small trees'
# trunks, whose cell alone the tree stands on.
SMALL_TRUNKS = {0x016, 0x017, 0x0C6, 0x0C7, 0x1F4, 0x1F5}
TREE_CARDS = SMALL_TRUNKS | {0x1D4, 0x1D5, 0x1D6, 0x1D7, 0x1DC, 0x1DD, 0x1DE, 0x1DF, 0x1E4, 0x1E5,
                             0x1E6, 0x1E7, 0x1EC, 0x1ED, 0x1F2, 0x1F3, 0x1FC, 0x1FD}


def behind_trees(c, flat, grids, pieces=None, marked=()):
    """A wood standing at a cliff's foot hides the cliff, it does not cut it
    off: the tree's cell lies flat on the ground the wood stands on, and the
    face over it goes straight down to it as a wall along the cell's edge -
    the mountain as if the trees were not there (they are models of their
    own, drawn over it). (A face run on down a pixel a row through the
    tree's cell was a slope with a toothed foot: a cliff broken where it
    met the wood.)"""
    for cell in sorted(grids, key=lambda cl: (cl[1], cl[0])):
        if c.kind.get(cell) != UNKNOWN or c.role(cell) != "tree" or cell in c.keep or cell in marked:
            continue
        if pieces is not None and pieces.get(cell, ("F",))[0] != "F":
            continue        # read as the face it is drawn in (a door's top in a face)
        x, y = cell
        north = grids.get((x, y - 1))
        if north is None or (x, y - 1) in c.keep:
            continue
        low = flat.get(cell, min(min(r) for r in grids[cell]))
        top = north[N]
        if max(top) <= low + 0.5:
            continue
        trunk = (x, y + 1)

        def face_beside(n):
            # (the face it goes on from, west or east of it in its row: a
            # face, a corner of one at its top - the tree drawn in front of
            # a face that runs on behind it, not at a face's end)
            p = pieces.get(n) if pieces is not None else None
            return p is not None and (p[0] == "S" or p[0] in CORNERS) and abs(p[1] - top[0]) < 0.5
        if (pieces is not None and c.metatile(cell) not in TREE_CARDS and trunk in c.src
                and c.metatile(trunk) in SMALL_TRUNKS and max(top) - min(top) < 0.5
                and abs(top[0] - LEVEL - flat.get(trunk, low)) < 0.5
                and (face_beside((x - 1, y)) or face_beside((x + 1, y)))):
            # the top of a small tree drawn in the row of a face, one level
            # over its trunk's ground: the tree stands on its trunk's cell
            # alone (voxel_tree.c: a small tree's crown is a card from its
            # trunk), so its top's cell is the face, going on behind it as
            # it does either side (flat at the trunk's level, it was a notch
            # in the cliff, a dark slit of wall either side of the tree)
            pieces[cell] = ("S", top[0])
            grids[cell] = piece_grid("S", top[0])
            continue
        grids[cell] = [[low] * (N + 1) for _ in range(N + 1)]


# Maps solved on one canvas though the world's solve reads them apart: their
# mountain is one it does not read (gr.ALIAS_TERRACES). Route 115's beige
# mountain goes on into Route 114's south (apart, Route 115's ended at the
# seam in a cliff six levels tall over Route 114's flat drawing).
JOIN = (("LAYOUT_ROUTE115", "LAYOUT_ROUTE114"),)

# Layouts that are one map drawn twice (the game swaps them as the story
# goes): the second takes the hand marks of the first, where it has none of
# its own - Route 111 without its Mirage Tower is Route 111, and its marks
# were lost the day the tower fell.
TWINS = {"LAYOUT_ROUTE111_NO_MIRAGE_TOWER": "LAYOUT_ROUTE111"}


def group_of(layout_id):
    """[(layout, x, y)]: the drawn group a layout is solved in, placed, with
    the groups JOIN ties to it placed as their maps connect."""
    def drawn(lid):
        for name, members in gr.DRAWN.items():
            if lid in members:
                return dict(members)
        return {lid: (0, 0)}

    placed = drawn(layout_id)
    grown = True
    while grown:
        grown = False
        for a, b in JOIN:
            for have, other in ((a, b), (b, a)):
                if have not in placed or other in placed:
                    continue
                link = next(((dx, dy) for n, dx, dy in voxel_props.connections().get(have, ())
                             if n == other), None)
                if link is None:
                    continue
                ox, oy = placed[have][0] + link[0], placed[have][1] + link[1]
                theirs = drawn(other)
                bx, by = theirs[other]
                for lid, (x, y) in theirs.items():
                    placed.setdefault(lid, (x - bx + ox, y - by + oy))
                grown = True
    return sorted(((lid, ox, oy) for lid, (ox, oy) in placed.items()),
                  key=lambda m: (m[0] != layout_id, m[0]))


def reference_heights(relief, members):
    """{canvas cell: height in the world} of a parsed relief.bin's cells
    (centre point), the base where a cell is not lifted."""
    import voxel_relief_fixes as vrf
    out = {}
    for lid, ox, oy in members:
        t = relief["layouts"].get(vrf.layout_index(lid))
        if t is None:
            continue
        art = gr.layout_art(lid)
        for y in range(art.h):
            for x in range(art.w):
                g = t["cells"].get((x, y))
                out[(x + ox, y + oy)] = t["base"] + (g[12] if g else 0)
    return out


def seam_meets(relief, members):
    """{canvas cell}: the edge cells of a group whose height in a parsed
    relief.bin is the height of the cell across the seam, on the map
    connected there (within a half level) - where the two maps meet."""
    return {cell for cell, (_, meet, _) in seam_faces(relief, members).items() if meet}


def seam_faces(relief, members):
    """{canvas cell: (map across, meets, edge, cell across)}: every edge
    cell of a group with a map connected across it, whether the two meet
    (seam_meets), the edge it is on ("left", "right", "up", "down") and the
    cell it meets there, in the map across."""
    import voxel_relief_fixes as vrf
    size = {lid: (gr.layout_art(lid).w, gr.layout_art(lid).h) for lid, _, _ in members}
    at = {lid: (ox, oy) for lid, ox, oy in members}

    def height(lid, x, y):
        t = relief["layouts"].get(vrf.layout_index(lid))
        if t is None:
            # a map with no table stands level at the world's 0 (the export
            # leaves out a map with no cell lifted and no base): Mauville
            # under Route 111 - read as no height, the seam anchored nothing
            # and the routes' terraces were lifted off the town's ground
            return 0
        g = t["cells"].get((x, y))
        return t["base"] + (g[12] if g else 0)

    def cell_at(lid, edge, i):
        w, h = size[lid]
        return {"down": (i, h - 1), "up": (i, 0), "right": (w - 1, i), "left": (0, i)}[edge]

    # a layout a script swaps in for a member (Route 111 without the Mirage
    # Tower) is the member itself: its seams are the group's own, and
    # taken as a neighbour's they pinned the group to the old solve's
    # heights of its twin
    import voxel_cells as vc
    alt = vc.alternate_layouts()
    same = set(at) | {k for k, v in alt.items() if v in at} | {v for k, v in alt.items() if k in at}
    out = {}
    for a, b, direction, offset in gr.map_links():
        if a not in at or b in same:
            continue
        a_art, b_art = gr.layout_art(a), gr.layout_art(b)
        size[b] = (b_art.w, b_art.h)
        for ea, ia, eb, ib in gr._seam_cells(a, b, direction, offset, size):
            ax, ay = cell_at(a, ea, ia)
            ha, hb = height(a, ax, ay), height(b, *cell_at(b, eb, ib))
            if ha is not None and hb is not None:
                cell = (ax + at[a][0], ay + at[a][1])
                out[cell] = (b, out.get(cell, (b, False))[1] or abs(ha - hb) <= LEVEL // 2, ea,
                             cell_at(b, eb, ib))
    return out


# -- rough rock -----------------------------------------------------------------

# A mountain drawn in a map's own rough rock and read in the General's colours
# (Lavaridge's, over Route 112's slope) is no drawing of terraces: its crags
# are band heads standing on its tops (269/26a/268/27f/27e, a band's head
# drawn over a top in the tileset's own rock), its hatching is drawn as bands and faces that
# start and end on one top. Counted as steps, each crag took a level, the
# votes fell out with each other and the slope stood up in blocks five levels
# apart, open to the void between them. It is laid as one surface instead:
# every point of the lattice one height shared by the pieces round it (no
# wall west or east, none facing south), each piece one of the eight, the one
# that best shows its tile - its rock falling where the tile is drawn falling
# (a band its way, a face south), flat where it is a top. A crag whose ends
# are one top is then the texture on that top; a ridge is a band one level
# high; the slope never falls north but at a rim (a top's back, seen past).
# (by tileset, its own ids: twins of the General's heads - Fallarbor's rock
# on Routes 114 and 115 among them - are heads drawn as heads, read by the
# terraces)
CRAGS = {"gTileset_Lavaridge": {0x268, 0x269, 0x26a, 0x27e, 0x27f}}
ROUGH_MIN = 8           # crags a mountain has to be rough rock
ROUGH_HOLE = 6          # cells a scrap the rock closes round may have
# a piece by its corners (NW, NE, SW, SE), in levels under its top
BY_CORNERS = {(0, 0, 0, 0): "F", (0, 0, -1, -1): "S", (0, -1, 0, -1): "E", (-1, 0, -1, 0): "W",
              (0, -1, -1, -1): "cSE", (-1, 0, -1, -1): "cSW", (0, 0, 0, -1): "vSE", (0, 0, -1, 0): "vSW"}
BACK = 3.0              # a level of back (the ground rising south of a top, unseen)
BACK_RIM = 0.2          # ...where the drawing shows that top's edge (a rim)
BACK_MAX = 4            # levels a back may rise: past it the game's camera, 13
                        # tiles off at 34 degrees over the ground behind, is
                        # inside the rock
WALL = 20.0             # a level of wall facing south, west or east
DRAWN = 2.0             # a tile's rock laid flat
RIDGE = 0.6             # a top's tile as the flank of a ridge (west or east)
WALKED = 2.0            # a level off the terraces' reading, walked ground
BACK_IN = 0.5           # a level of back inside the mountain (a ridge's end)
FACE_LIT = 1.0          # ...as a face


def seam_cells(c):
    """The cells of a group on a border where its map meets another (the
    next map's own: never moved); a border with no map past it is free."""
    size = {lid: (art.w, art.h) for lid, art in c.arts.items()}
    at = {lid: (ox, oy) for lid, ox, oy in c.members}
    out = set()
    for a, b, direction, offset in gr.map_links():
        if a not in at:
            continue
        w, h = size[a]
        if b not in size:
            entry = next(e for e in vb._layouts_json() if e["id"] == b)
            size[b] = (entry["width"], entry["height"])
        for edge_a, i, _, _ in gr._seam_cells(a, b, direction, offset, size):
            x, y = {"up": (i, 0), "down": (i, h - 1), "left": (0, i), "right": (w - 1, i)}[edge_a]
            out.add((x + at[a][0], y + at[a][1]))
    return out


def rough_fields(c, pieces, flat, kept):
    """The pieces of each rough-rock mountain, laid as one surface (above).
    Its cells' flat levels are its own (the lowest of each piece): a tree
    on it was laid flat at the terraces' level by behind_trees, a pit down
    to the old reading walled all round - Route 112's side walls and
    standing slabs."""
    def own_id(cell):
        lid, x, y = c.src[cell]
        art = c.arts[lid]
        return art.own_metatile(x, y) if hasattr(art, "own_metatile") else None

    def own(cell):
        m = own_id(cell)
        return m is not None and m >= vb.NUM_PRIMARY

    seams = seam_cells(c)
    sets = {e["id"]: e["secondary_tileset"] for e in vb._layouts_json()}

    def tileset(cell):
        return sets[c.src[cell][0]]

    def member(cell):
        if cell not in c.kind or cell in c.keep or cell in kept or cell in seams:
            return False
        if c.water(cell) or c.is_stair(cell) or cell in c.bridges or not own(cell):
            return False
        return c.kind[cell] in (TOP, ROCK) or c.drawn_on(cell) != "G"

    seen = set()
    for start in sorted(c.kind):
        if start in seen or not member(start):
            continue
        field, todo = set(), [start]
        seen.add(start)
        while todo:
            cell = todo.pop()
            field.add(cell)
            for n in c._four(cell):
                if n not in seen and member(n):
                    seen.add(n)
                    todo.append(n)
        crags = sum(1 for cell in field if own_id(cell) in CRAGS.get(tileset(cell), ()))
        if crags < ROUGH_MIN:
            continue
        # a few cells the rock closes round (a cave's mouth, a scrap of
        # grass between crags) are its own: laid apart, each met the rock
        # round it with a wall
        tried = set()
        for start in sorted({n for cell in field for n in c._four(cell)} - field):
            if start in tried:
                continue
            hole, todo, closed = set(), [start], True
            while todo:
                cell = todo.pop()
                hole.add(cell)
                if cell not in c.kind or cell in c.keep or cell in kept or cell in seams \
                        or c.is_stair(cell) or c.water(cell) or len(hole) > ROUGH_HOLE:
                    closed = False
                    break
                for n in c._four(cell):
                    if n not in field and n not in hole and n not in todo:
                        todo.append(n)
            tried |= hole
            if closed:
                field |= hole
        _woods_at_foot(c, field, pieces, flat, kept, seams)
        laid = _lay_rough(c, field, pieces, kept)
        pieces.update(laid)
        c.rough |= field
        for cell, p in laid.items():
            flat[cell] = min(map(min, piece_grid(*p)))


def _woods_at_foot(c, field, pieces, flat, kept, seams):
    """A wood drawn on grass at a rough mountain's foot stands on that grass:
    the lowest plain ground beside it (Route 112's west wood, its front rows
    before the hidden cliff: joined to the plateau over the cliff by the
    terraces, a tree stood three levels over the trees beside it, a pit
    walled west and east between them)."""
    def tree(n):
        return (n in pieces and pieces[n][0] == "F" and n not in field and n not in kept
                and n not in c.keep and n not in seams and c.role(n) == "tree" and c.drawn_on(n) == "G")

    seen = set()
    for start in sorted({n for cell in field for n in c._four(cell)} - field):
        if start in seen or not tree(start):
            continue
        wood, todo = set(), [start]
        seen.add(start)
        while todo:
            cell = todo.pop()
            wood.add(cell)
            for n in c._four(cell):
                if n not in seen and tree(n):
                    seen.add(n)
                    todo.append(n)
        ground = [pieces[n][1] for cell in wood for n in c._four(cell)
                  if n not in wood and n not in field and n in pieces and pieces[n][0] == "F"
                  and c.kind.get(n) == GROUND and c.role(n) != "tree"]
        if ground:
            for cell in wood:
                pieces[cell] = ("F", min(ground))
                flat[cell] = min(ground)


def _source_side(n, arcs, s, t):
    """The nodes on the source's side of a minimum s-t cut (Dinic's
    blocking flows) of a graph of `n` nodes and arcs (u, v, capacity)."""
    to, cap, nxt, head = [], [], [], [-1] * n
    for u, v, w in arcs:
        to += [v, u]
        cap += [w, 0.0]
        nxt += [head[u], head[v]]
        head[u], head[v] = len(to) - 2, len(to) - 1
    eps = 1e-9
    while True:
        level = [-1] * n
        level[s] = 0
        queue = [s]
        for u in queue:
            e = head[u]
            while e >= 0:
                if cap[e] > eps and level[to[e]] < 0:
                    level[to[e]] = level[u] + 1
                    queue.append(to[e])
                e = nxt[e]
        if level[t] < 0:
            break
        it = head[:]
        while True:
            # one path along the levels, as deep as it goes
            path, u = [], s
            while u != t:
                e = it[u]
                while e >= 0 and not (cap[e] > eps and level[to[e]] == level[u] + 1):
                    e = nxt[e]
                it[u] = e
                if e < 0:
                    if not path:
                        break
                    level[u] = -1           # a dead end: never again this phase
                    e = path.pop()
                    u = to[e ^ 1]
                    it[u] = nxt[it[u]]
                    continue
                path.append(e)
                u = to[e]
            if u != t:
                break
            push = min(cap[e] for e in path)
            for e in path:
                cap[e] -= push
                cap[e ^ 1] += push
    seen = [False] * n
    seen[s] = True
    queue = [s]
    for u in queue:
        e = head[u]
        while e >= 0:
            if cap[e] > eps and not seen[to[e]]:
                seen[to[e]] = True
                queue.append(to[e])
            e = nxt[e]
    return seen


def _convex_labels(K, unary, hinges):
    """Integer labels 0..K-1, one a variable, minimising the sum of
    unary[v][label] and of w * max(0, l_i - l_j - t) for each hinge
    (i, j, t, w) (w None: a constraint, l_i - l_j <= t): exact, by one
    minimum cut (Ishikawa's graph, the hinges convex)."""
    INF = 1e12
    unary = [list(u) for u in unary]
    arcs = []
    nv = len(unary)

    def node(v, k):                 # [l_v >= k], 1 <= k <= K - 1
        return 2 + v * (K - 1) + (k - 1)
    for i, j, t, w in hinges:
        w = INF if w is None else w
        for k in range(t + 1, K + max(0, t) + 1):
            if k <= 0:
                # always l_i >= k: w while l_j < k - t
                for lab in range(0, min(K, k - t)):
                    unary[j][lab] += w
            elif k > K - 1:
                continue
            elif k - t > K - 1:
                for lab in range(k, K):         # l_j < k - t always
                    unary[i][lab] += w
            elif k - t >= 1:
                arcs.append((node(i, k), node(j, k - t), w))
    for v in range(nv):
        low = min(unary[v])
        u = [min(INF, x - low) for x in unary[v]]
        arcs.append((0, node(v, 1), u[0]) if K > 1 else (0, 1, 0.0))
        for k in range(1, K - 1):
            arcs.append((node(v, k), node(v, k + 1), u[k]))
            arcs.append((node(v, k + 1), node(v, k), INF))
        if K > 1:
            arcs.append((node(v, K - 1), 1, u[K - 1]))
    side = _source_side(2 + nv * (K - 1), arcs, 0, 1)
    return [sum(1 for k in range(1, K) if side[node(v, k)]) for v in range(nv)]


def _lay_rough(c, field, pieces, kept):
    def fixed(n):
        if n in field or n not in c.kind:
            return None
        if n in kept:
            return kept[n]
        return piece_grid(*pieces[n]) if n in pieces else None

    # what each tile draws: the way its rock falls (a band its way, a face
    # south, a top not at all) and the share of it drawn as falling rock
    info = {}
    for cell in field:
        cls = pixel_classes(c.art(cell), c.metatile(cell))
        f = sum(r.count("F") for r in cls) / 256.0
        pref = c.band[cell] if cell in c.band else "S" if is_face(c, cell) else "F"
        if pref in ("W", "E"):
            # a crag with no top on its high side (the next of a row of
            # bands) is hatching on the top under it, not another level
            x, y = cell
            high = (x + 1, y) if pref == "W" else (x - 1, y)
            if high in c.band or c.kind.get(high) == ROCK:
                pref, f = "F", 0.0
        if cell in c.walked_tops or c.role(cell) == "floor":
            pref = "_"
        info[cell] = (pref, f)

    # the lattice: a height a corner, shared west and east by the pieces of
    # a row (U, a row's north edge; D, its south edge: the row north of a
    # line and the row south of it meet there, or stand a back apart)
    verts = sorted({("U", x + i, y) for (x, y) in field for i in (0, 1)}
                   | {("D", x + i, y + 1) for (x, y) in field for i in (0, 1)})
    index = {p: k for k, p in enumerate(verts)}
    tops = [max(map(max, fixed(n))) for cell in field for n in c._four(cell) if fixed(n) is not None]
    lo = int(math.floor(min(tops) / LEVEL + 1e-6))      # never under the ground round it
    K = int(math.ceil(max(tops) / LEVEL - 1e-6)) + BACK_MAX - lo + 1
    unary = [[0.0] * K for _ in verts]
    hinges = []

    def linear(v, w):
        """w a level of v's height."""
        for lab in range(K):
            unary[v][lab] += w * lab

    def meet(v, p, below, above):
        """Corner v against a height p (pixels) a piece that stays has
        there: `below` a level under it costs, `above` over it (a function
        of the levels)."""
        for lab in range(K):
            d = lo + lab - p / LEVEL
            unary[v][lab] += 0.5 * (above(d) if d > 0 else below(-d))

    def wall(d):
        return WALL * d

    # never more than a back may rise over the ground north of it, up its
    # column (the camera behind the ground there stood inside the rock)
    for (x, y) in field:
        n = (x, y - 1)
        while n in field:
            n = (n[0], n[1] - 1)
        g = fixed(n)
        if g is None:
            continue
        cap = int(math.floor(max(edge(g, "S")) / LEVEL + 1e-6)) + BACK_MAX - lo
        for v in (("U", x, y), ("U", x + 1, y), ("D", x, y + 1), ("D", x + 1, y + 1)):
            row = unary[index[v]]
            for lab in range(max(0, cap + 1), K):
                row[lab] += WALL * (lab - cap)
    for (_, i, j), u in index.items():
        if _ != "U" or ("D", i, j) not in index:
            continue
        # a line between two rows: the south one never under the north one
        # (a wall facing south), over it at most a level - a back, unseen,
        # where a ridge starts - but never behind a face
        dn = index[("D", i, j)]
        face = any(info.get(n, ("F",))[0] == "S" for n in ((i - 1, j), (i, j)))
        hinges += [(dn, u, 0, None), (u, dn, 0 if face else 1, None)]
        linear(u, BACK_IN)
        linear(dn, -BACK_IN)
    for (x, y) in field:
        a, b = index[("U", x, y)], index[("U", x + 1, y)]
        cc, d = index[("D", x, y + 1)], index[("D", x + 1, y + 1)]
        # one of the eight pieces: each side falls a level at most, south
        # or either way west and east, and never both at once
        hinges += [(cc, a, 0, None), (a, cc, 1, None), (d, b, 0, None), (b, d, 1, None),
                   (a, b, 1, None), (b, a, 1, None), (cc, d, 1, None), (d, cc, 1, None),
                   (a, d, 1, None), (b, cc, 1, None)]
        pref, f = info[(x, y)]
        # the drawing: each side falling or not, as the tile shows it
        if pref == "S":
            fall, ew = (DRAWN * f / 2, 0.0), (CORNER, 0.0, CORNER)
        elif pref == "E":
            fall, ew = (0.0, CORNER), (DRAWN * f + FIN / 2, DRAWN * f / 2, 0.0)
        elif pref == "W":
            fall, ew = (0.0, CORNER), (0.0, DRAWN * f / 2, DRAWN * f + FIN / 2)
        elif pref == "_":
            # walked: flat, where the terraces put it unless the rock round
            # it says otherwise
            fall, ew = (0.0, WALL), (WALL, 0.0, WALL)
            for v in (a, b, cc, d):
                for lab in range(K):
                    unary[v][lab] += 0.25 * WALKED * abs(lo + lab - pieces[(x, y)][1] / LEVEL)
        else:
            # (a top's flank: the light side of a ridge whose dark side is
            # the next tile's band - or, south, a face drawn in the light)
            up = max(0.0, (RIDGE - DRAWN * f) / 2)
            fall, ew = (DRAWN * f / 2, FACE_LIT / 2), (up, 0.0, up)
        for n, s in ((a, cc), (b, d)):
            # (falling: n - s = 1)
            linear(n, fall[1] - fall[0])
            linear(s, fall[0] - fall[1])
        for w_, e_ in ((a, b), (cc, d)):
            # (w_ - e_ = -1, 0, 1: rising, level, falling east)
            s1 = ew[1] - ew[0]
            linear(w_, s1)
            linear(e_, -s1)
            hinges.append((w_, e_, 0, (ew[2] - ew[1]) - s1))
        # the pieces that stay round it
        for n, mine, theirs in (((x - 1, y), (a, cc), "E"), ((x + 1, y), (b, d), "W"),
                                ((x, y - 1), (a, b), "S"), ((x, y + 1), (cc, d), "N")):
            g = fixed(n)
            if g is None:
                continue
            e = edge(g, theirs)
            if theirs in ("E", "W"):
                for v, p in zip(mine, (e[0], e[N])):
                    meet(v, p, wall, wall)
            elif theirs == "S":
                # behind it: a back, where the drawing shows the top's edge
                # (never behind rock that falls: a fin)
                back = BACK_RIM if (x, y) in c.rim and pref == "F" else BACK if pref in ("F", "_") else WALL
                for v, p in zip(mine, (e[0], e[N])):
                    meet(v, p, wall, lambda d, back=back: back * d + WALL * max(0.0, d - BACK_MAX))
            else:
                for v, p in zip(mine, (e[0], e[N])):
                    meet(v, p, lambda d: BACK * d, wall)
    labels = _convex_labels(K, unary, hinges)
    height = {p: lo + labels[k] for p, k in index.items()}
    out = {}
    for (x, y) in field:
        a, b = height[("U", x, y)], height[("U", x + 1, y)]
        cc, d = height[("D", x, y + 1)], height[("D", x + 1, y + 1)]
        h = max(a, b, cc, d)
        out[(x, y)] = (BY_CORNERS[(a - h, b - h, cc - h, d - h)], h * LEVEL)
    return out


# -- ledges between two levels -------------------------------------------------
#
# A ledge is jumped down, never up. Most are a berm on level land
# (gen_voxel_relief.ledge_berms): the ground either side at one level, kept
# as the generator made it. Where the ground a ledge is jumped from lies
# higher than the ground it is jumped to, the ledge is a step: the ground
# behind its lip at the high level, the brown lip falling to the low one at
# its foot - the lip is the step, no berm over it. Ground walled in by
# ledges alone, jumped into from one level and out of to a lower one
# (Lilycove's rings of ledges on the way down its hill), is a landing half
# way between the two: each ledge a step of half a level, about as tall as
# its lip is drawn. The hand can set a ledge's top and foot (its marks'
# "level" and "foot") and a landing's level, in half levels.

_LIPS = {}


def ledge_lip(lid):
    """gen_voxel_relief.ledge_lip of a layout, read off its own drawing."""
    if lid not in _LIPS:
        art = vb.LayoutArt(lid) if gr.alias_of(lid) else gr.layout_art(lid)
        _LIPS[lid] = gr.ledge_lip(gr.open_roles(lid), art)[1]
    return _LIPS[lid]


def ledge_level(c, pieces, flat, cell):
    """The level of the ground a ledge meets at `cell`: a flat piece's (the
    terraces' flat before the pieces), None for rock, other ledges, berry
    soil and what is off the canvas."""
    if cell in c.keep or cell not in c.src:
        return None
    if pieces is not None:
        p = pieces.get(cell)
        return p[1] if p is not None and p[0] == "F" else None
    if c.kind.get(cell) == ROCK:
        return None
    return flat.get(cell)


def ledge_landings(c, flat):
    """Ground no way in or out of but ledges (walled round by rock, trees,
    houses), jumped into from a level higher than the one it is jumped out
    to: half way between (chains of them in even steps), in place in
    `flat`. {cell: level}."""
    def floor(n):
        return (n in flat and n not in c.keep and not c.water(n)
                and (c.kind.get(n) == GROUND or c.walked(n)))

    landings, seen = [], set()
    for start in sorted(flat):
        if start in seen or not floor(start):
            continue
        comp, todo = [], [start]
        seen.add(start)
        while todo:
            x, y = todo.pop()
            comp.append((x, y))
            for n in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
                if n not in seen and floor(n):
                    seen.add(n)
                    todo.append(n)
        inside = set(comp)
        ins, outs, closed = [], [], True
        for (x, y) in comp:
            for n in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
                if n in inside:
                    continue
                dirs = c.jumps.get(n)
                if dirs:
                    for dx, dy in dirs:
                        if (n[0] + dx, n[1] + dy) == (x, y):
                            ins.append((n[0] - dx, n[1] - dy))
                        elif (n[0] - dx, n[1] - dy) == (x, y):
                            outs.append((n[0] + dx, n[1] + dy))
                elif (n not in c.src or c.walked(n) or c.water(n) or n in c.keep
                      or c.role(n) == "stair"):
                    closed = False      # walked out of, or the canvas ends
            if not closed:
                break
        if closed and ins and outs:
            # a blocked cell in it that is no rock (the cliff's end drawn
            # into Lilycove's lower ring, a sign): the landing's, else a pit
            for (x, y) in list(comp):
                for n in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
                    if (n in inside or n not in flat or n in c.keep or c.kind.get(n) != UNKNOWN
                            or c.walked(n) or c.role(n) == "stair" or c.house(n)):
                        continue
                    if all(m in inside or m in c.jumps or c.kind.get(m) == ROCK
                           for m in ((n[0] - 1, n[1]), (n[0] + 1, n[1]), (n[0], n[1] - 1), (n[0], n[1] + 1))):
                        inside.add(n)
                        comp.append(n)
            landings.append((comp, ins, outs))
    out = {}
    for _ in range(8):
        moved = False
        for comp, ins, outs in landings:
            hi = [v for v in (ledge_level(c, None, flat, n) for n in ins) if v is not None]
            lo = [v for v in (ledge_level(c, None, flat, n) for n in outs) if v is not None]
            if not hi or not lo or max(hi) <= min(lo) + 0.5:
                continue
            v = (max(hi) + min(lo)) / 2.0
            for cell in comp:
                if abs(flat[cell] - v) > 0.01:
                    flat[cell] = out[cell] = v
                    moved = True
        if not moved:
            break
    return out


def ledge_sides(c, pieces, flat, marks=None):
    """{ledge cell: {(dx, dy): [high, low]}}: the level each way a ledge is
    jumped from and to. A ledge cell with rock or another ledge on a side
    takes that side from the ledges beside it jumped the same way (a ring's
    corner, a run's end against the cliff); the marks' "level" sets the
    top, "foot" the foot."""
    out = {}
    for cell, dirs in c.jumps.items():
        x, y = cell
        out[cell] = {d: [ledge_level(c, pieces, flat, (x - d[0], y - d[1])),
                         ledge_level(c, pieces, flat, (x + d[0], y + d[1]))] for d in dirs}
    for _ in range(64):
        changed = False
        for cell in sorted(out):
            x, y = cell
            for d, hl in out[cell].items():
                for k in (0, 1):
                    if hl[k] is not None:
                        continue
                    for n in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
                        v = out.get(n, {}).get(d)
                        if v is not None and v[k] is not None:
                            hl[k] = v[k]
                            changed = True
                            break
        if not changed:
            break
    for cell, m in (marks or {}).items():
        for hl in out.get(cell, {}).values():
            if m.get("level") is not None:
                hl[0] = m["level"]
            if m.get("foot") is not None:
                hl[1] = m["foot"]
    return out


def step_grid(lip, x, y, sides):
    """A ledge cell (x, y of its map) as a step: (N+1)^2 heights, rows north
    first. Each lattice point reads its place along each way the ledge is
    jumped (gen_voxel_relief.ledge_reads, as the berm does): behind the lip
    at the top, in front of it at the foot, on it as far up as it is from
    its foot; at a corner the nearer foot counts."""
    dirs = list(sides)
    grid = []
    for j in range(N + 1):
        row = []
        for i in range(N + 1):
            reads = gr.ledge_reads(lip, x, y, dirs, i, j)
            on, front, behind = [], [], []
            for (_, _, dx, dy, f, b) in reads:
                hi, lo = sides[(dx, dy)]
                if f:
                    on.append(lo + (hi - lo) * min(f, gr.LIP_WIDTH) / float(gr.LIP_WIDTH))
                elif b:
                    front.append(lo)
                else:
                    behind.append(hi)
            if on and not front and not behind:
                v = min(on)
            elif front:
                v = min(front)
            else:
                v = max(on + behind)
            row.append(2 * round(v / 2.0))     # (relief.bin keeps 2-pixel units)
        grid.append(row)
    return grid


def meet_edges(c, pieces, cell, grid):
    """A step's edge points where it meets a cell that is no ledge: that
    cell's (the cliff a ring of ledges runs into, the ground at a run's open
    end) - as the berm comes down to the ground there; else a wedge of void
    between the two. At a corner the side nearest the step's own height."""
    x, y = cell
    sides = {(-1, 0): [(0, j) for j in range(N + 1)], (1, 0): [(N, j) for j in range(N + 1)],
             (0, -1): [(i, 0) for i in range(N + 1)], (0, 1): [(i, N) for i in range(N + 1)]}
    want = collections.defaultdict(list)
    for (dx, dy), points in sides.items():
        n = (x + dx, y + dy)
        if n in c.keep or n not in pieces:
            continue
        g = piece_grid(*pieces[n])
        for (i, j) in points:
            want[(i, j)].append(g[j - dy * N][i - dx * N])
    for (i, j), vs in want.items():
        grid[j][i] = min(vs, key=lambda v: abs(v - grid[j][i]))


def lay_ledges(c, pieces, flat, kept, cuts, marks=None):
    """Every ledge between two levels as a step, in place in `kept` (its
    cut record dropped from `cuts`): {"sides":
    ledge_sides, "steps": cells laid as steps, "uphill": cells jumped up
    (laid level at the higher side: the levels round them are wrong)}."""
    sides = ledge_sides(c, pieces, flat, marks)
    steps, uphill = set(), set()
    if pieces is not None:
        # a cliff's face ending at a step (Lilycove's, where its ring of
        # ledges meets it): read as an inner corner against the step's
        # high back, its side stood a level tall over the step's foot -
        # it is the face, falling whole to the ground south of it
        for cell, ways in sides.items():
            if not any(hl[0] is not None and hl[1] is not None and hl[0] - hl[1] > 0.5 for hl in ways.values()):
                continue
            x, y = cell
            for n, kind in (((x - 1, y), "vSW"), ((x + 1, y), "vSE")):
                p, below = pieces.get(n), pieces.get((n[0], n[1] + 1))
                if (p is not None and p[0] == kind and c.kind.get(n) == ROCK and n not in c.keep
                        and n not in (marks or {}) and below is not None and below[0] == "F"
                        and abs(below[1] - (p[1] - LEVEL)) < 0.5):
                    pieces[n] = ("S", p[1])
    for cell, ways in sides.items():
        known = {d: hl for d, hl in ways.items() if hl[0] is not None and hl[1] is not None}
        if not known:
            continue
        for d, hl in known.items():
            if hl[0] < hl[1] - 0.5:
                uphill.add(cell)
                hl[0] = hl[1]
        if all(abs(hl[0] - hl[1]) <= 0.5 for hl in known.values()):
            # a berm on level land: as kept, standing on that land (a run
            # of ledges moved as one stood a level off where it met another)
            lo = min(hl[1] for hl in known.values())
            d = lo - min(min(row) for row in kept[cell]) if cell in kept else 0
            # (a few pixels off is the berm's own ditch at its lip, not a move)
            if abs(d) >= LEVEL / 2:
                kept[cell] = [[v + d for v in row] for row in kept[cell]]
                if cell in cuts:
                    cuts[cell] = [cuts[cell][0], cuts[cell][1] + d] + list(cuts[cell][2:])
            continue
        for d in ways:
            if d not in known:
                v = next(iter(known.values()))
                ways[d] = [v[0], v[1]]
        lid, x, y = c.src[cell]
        kept[cell] = step_grid(ledge_lip(lid), x, y, ways)
        if pieces is not None:
            meet_edges(c, pieces, cell, kept[cell])
        steps.add(cell)
        # (no cut: the berm's cuts the ground it lifts behind the lip back
        # down to its foot - behind a step that ground is the top)
        cuts.pop(cell, None)
    # a corner's lip is drawn as an arc, its arms' straight: where they
    # meet, the corner takes its arm's edge (read apart, a wedge of void
    # opened between them)
    for cell in sorted(steps):
        if len(c.jumps[cell]) < 2:
            continue
        # inside, the drawing's own regions (the arc read point by point
        # along the jumps stood up as spikes of grass, a patch of its edges
        # as twisted slants)
        lid, x, y = c.src[cell]
        g = corner_grid(c, ledge_lip(lid), cell, kept, pieces, flat)
        if g is not None:
            kept[cell] = g
            if pieces is not None:
                meet_edges(c, pieces, cell, kept[cell])
        x, y = cell
        for (dx, dy) in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            n = (x + dx, y + dy)
            if n not in kept or len(c.jumps.get(n, ())) != 1:
                continue
            for k in range(N + 1):
                i, j = (N if dx > 0 else 0, k) if dx else (k, N if dy > 0 else 0)
                kept[cell][j][i] = kept[n][j - dy * N][i - dx * N]
    return {"sides": sides, "steps": steps, "uphill": uphill}


def corner_grid(c, lip, cell, kept, pieces, flat):
    """A ledge's corner as drawn: the lip parts its ground into regions,
    each at the height of what it meets across the cell's edge (an arm's
    edge there, the cliff's, the ground's); the lip falls from one region
    to the next as far as it is from each. None where no region meets
    anything."""
    _, lx, ly = c.src[cell]
    ox, oy = lx * 16, ly * 16
    on_lip = [[bool(lip.get((ox + i, oy + j))) for i in range(16)] for j in range(16)]
    comp = [[None] * 16 for _ in range(16)]
    regions = []
    for j0 in range(16):
        for i0 in range(16):
            if on_lip[j0][i0] or comp[j0][i0] is not None:
                continue
            k = len(regions)
            todo, px = [(i0, j0)], []
            comp[j0][i0] = k
            while todo:
                i, j = todo.pop()
                px.append((i, j))
                for a, b in ((i - 1, j), (i + 1, j), (i, j - 1), (i, j + 1)):
                    if 0 <= a < 16 and 0 <= b < 16 and not on_lip[b][a] and comp[b][a] is None:
                        comp[b][a] = k
                        todo.append((a, b))
            regions.append(px)
    x, y = cell

    def across(dx, dy, k):
        """The neighbour's height at lattice point k of the shared edge."""
        n = (x + dx, y + dy)
        if n in kept:
            g = kept[n]
        elif pieces is not None and n in pieces:
            g = piece_grid(*pieces[n])
        elif n in flat and n not in c.keep:
            return flat[n]
        else:
            return None
        return g[k][0] if dx > 0 else g[k][N] if dx < 0 else g[0][k] if dy > 0 else g[N][k]

    level = []
    for px in regions:
        vs = []
        for (i, j) in px:
            for (dx, dy), on in (((-1, 0), i == 0), ((1, 0), i == 15), ((0, -1), j == 0), ((0, 1), j == 15)):
                if on:
                    v = across(dx, dy, min(N, (j if dx else i) // (16 // N)))
                    if v is not None:
                        vs.append(v)
        level.append(sorted(vs)[len(vs) // 2] if vs else None)
    known = [(k, v) for k, v in enumerate(level) if v is not None]
    if not known:
        return None
    grid = []
    for j in range(N + 1):
        row = []
        for i in range(N + 1):
            near = {comp[b][a] for a in (i * 4 - 1, i * 4) for b in (j * 4 - 1, j * 4)
                    if 0 <= a < 16 and 0 <= b < 16 and comp[b][a] is not None and level[comp[b][a]] is not None}
            if near:
                v = sum(level[k] for k in near) / float(len(near))
            else:
                # on the lip: between the nearest regions of each height,
                # by how far it is from each
                best = {}
                for k, v in known:
                    d = min(abs(a - i * 4 + 0.5) + abs(b - j * 4 + 0.5) for a, b in regions[k])
                    if v not in best or d < best[v]:
                        best[v] = d
                (h1, d1), (h2, d2) = (sorted(best.items(), key=lambda kv: kv[1]) * 2)[:2]
                v = h1 if h1 == h2 else (h1 * d2 + h2 * d1) / (d1 + d2)
            row.append(2 * round(v / 2.0))
        grid.append(row)
    return grid


def lay_soil(c, pieces, kept, cuts, marks, marked=()):
    """Berry soil and dirt patches on the ground round them as the marks
    leave it, in place in `kept` (and their cut records' feet): each patch
    moved as one to the level most of the flat ground beside it lies at, or
    to its marks' (sunk where a hand raised the lawn round it). Only patches
    a mark touches, or whose ground round them was marked, move."""
    seen = set()
    for start in sorted(cell for cell in c.keep if cell not in c.jumps and cell in kept):
        if start in seen:
            continue
        run, todo = [], [start]
        seen.add(start)
        while todo:
            x, y = todo.pop()
            run.append((x, y))
            for n in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
                if n in c.keep and n not in c.jumps and n in kept and n not in seen:
                    seen.add(n)
                    todo.append(n)
        now = collections.Counter(min(min(row) for row in kept[cell]) for cell in run).most_common(1)[0][0]
        target = [marks[cell]["level"] for cell in run if marks.get(cell, {}).get("level") is not None]
        near = {n for (x, y) in run for n in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1))}
        if target:
            want = max(set(target), key=target.count)
        elif not near & set(marked):
            continue
        else:
            round_it = collections.Counter(
                pieces[n][1] for (x, y) in run for n in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1))
                if n not in c.keep and n in pieces and pieces[n][0] == "F")
            if not round_it:
                continue
            want = round_it.most_common(1)[0][0]
        d = want - now
        if abs(d) < 0.5:
            continue
        for cell in run:
            kept[cell] = [[v + d for v in row] for row in kept[cell]]
            if cell in cuts:
                cuts[cell] = [cuts[cell][0], cuts[cell][1] + d] + list(cuts[cell][2:])


def faces_at_landings(c, pieces, grids, steps=()):
    """A rock face ending beside ground that lies half way up it (a landing
    between ledges, Lilycove's lower ring): its side edge
    no higher than that ground, in place in `grids` - else the face's end
    stands as a wall over the landing. And a landing beside a step takes the
    step's edge: where the step's foot, the face's and the landing meet at
    one corner the step has the foot's, and the landing comes down to it."""
    for cell in steps:
        x, y = cell
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            n = (x + dx, y + dy)
            q = pieces.get(n)
            if n in c.keep or q is None or q[0] != "F" or n not in grids:
                continue
            g, s = [list(row) for row in grids[n]], grids[cell]
            if abs((q[1] - min(min(row) for row in s)) / float(LEVEL) % 1 - 0.5) > 0.1:
                continue        # (whole levels: the ground is as drawn)
            for k in range(N + 1):
                i, j = (0 if dx > 0 else N, k) if dx else (k, 0 if dy > 0 else N)
                g[j][i] = s[j + dy * N][i + dx * N]
            grids[n] = g
    for cell, p in pieces.items():
        if cell in c.keep or c.kind.get(cell) != ROCK or cell not in grids:
            continue
        g = grids[cell] = [list(row) for row in grids[cell]]
        top, foot = max(max(row) for row in g), min(min(row) for row in g)
        x, y = cell
        for dx, i in ((-1, 0), (1, N)):
            n = (x + dx, y)
            q = pieces.get(n)
            # (only half a level off: ground a whole level up a taller face
            # is a terrace of its own, the face's end beside it as drawn)
            if (n in c.keep or q is None or q[0] != "F" or not foot + 0.5 < q[1] < top - 0.5
                    or abs((q[1] - foot) / float(LEVEL) % 1 - 0.5) > 0.1):
                continue
            for j in range(N + 1):
                g[j][i] = min(g[j][i], q[1])


def solve(layout_id, reference=None, marks=None):
    """The terraces of the group a layout is drawn in. `reference`: a parsed
    relief.bin whose world placement is kept. `marks`: the hand marks over
    them ({canvas cell: mark}, load_marks), the fixes file's if None."""
    return finish(solve_auto(layout_id, reference), marks)


def solve_auto(layout_id, reference=None):
    """solve before the hand marks: what the drawing alone reads (the
    editor keeps it, and lays the marks over it as they change)."""
    members = group_of(layout_id)
    c = Cells(members)
    runs = runs_of(c)
    comp = terraces(c, runs)
    ref = reference_heights(reference, members) if reference is not None else None
    meets = seam_faces(reference, members) if reference is not None else None
    level, dropped = solve_levels(c, comp, runs, ref, meets)
    # a scrap of ground no face or band counts, held apart from the ground
    # round it (Route 115's two cells of grass between the dirt patch and the
    # wood: lifted to where the old relief had them, two levels down, a
    # trench in the wood): the level of the ground round it
    counted = {cell for _, hi, cells, lo in runs for cell in (hi, lo)}
    members = collections.defaultdict(list)
    for cell, t in comp.items():
        members[t].append(cell)
    for t, cells in members.items():
        if len(cells) > 4 or any(cell in counted or c.kind.get(cell) != GROUND or c.water(cell)
                                 for cell in cells):
            continue
        round_it = collections.Counter(
            level[comp[n]] for (x, y) in cells for n in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1))
            if n in comp and comp[n] != t and not c.water(n))
        if round_it:
            best, k = round_it.most_common(1)[0]
            if best != level[t] and 2 * k > sum(round_it.values()):
                level[t] = best
    U = upper_levels(c, comp, level, runs)
    fixed = {cell for d, hi, cells, lo in runs if hi in comp and lo in comp
             and level[comp[hi]] - level[comp[lo]] == LEVEL * steps(c, cells, d)[1] for cell in cells}
    flat = {cell: level[t] for cell, t in comp.items()}
    flat.update(unknown_levels(c, comp, level, U))
    U = settle(c, U, flat, fixed)
    # a flank of bands between two grounds: when the ground on its high side
    # and the one at its foot are as many levels apart as the flank has
    # bands, each band falls one level from the ground beside it (a band
    # pair under a wood took its top off the terrace beyond the wood, a
    # level too high, and the other band fell two levels to make up)
    for cell in sorted(c.band):
        side = c.band[cell]
        x, y = cell
        up = 1 if side == "W" else -1           # the high side
        hi = (x + up, y)
        if c.band.get(hi) == side:
            continue                            # not the chain's high end
        chain = [cell]
        while c.band.get((chain[-1][0] - up, y)) == side:
            chain.append((chain[-1][0] - up, y))
        lo = (chain[-1][0] - up, y)
        if hi not in flat or lo not in flat or c.kind.get(hi) == ROCK or c.kind.get(lo) == ROCK:
            continue
        if abs(flat[hi] - flat[lo] - LEVEL * len(chain)) > 0.5:
            continue
        for i, b in enumerate(chain):
            U[b] = flat[hi] - LEVEL * i
    # ground walled in by ledges, between the level they are jumped from
    # and the one they are jumped to: half way
    landings = ledge_landings(c, flat)
    kept = {}
    keep_shift = {}
    kept_cuts = {}
    if reference is not None:
        # ledges and berry soil keep the relief they had: their neighbours
        # meet (and wall themselves against) what is really there
        import voxel_relief_fixes as vrf
        for cell in c.keep:
            lid, x, y = c.src[cell]
            t = reference["layouts"].get(vrf.layout_index(lid))
            if t is None:
                continue
            g = t["cells"].get((x, y))
            kept[cell] = [[t["base"] + (g[j * (N + 1) + i] if g else 0) for i in range(N + 1)]
                          for j in range(N + 1)]
        # their shape is kept, not their height in the world: where the
        # terraces put the ground round a run of ledges at another level
        # than the old relief had it, the run moves with that ground (kept
        # where it was, it sank into a trench or stood as a wall)
        def ref_at(cell):
            lid, x, y = c.src[cell]
            t = reference["layouts"].get(vrf.layout_index(lid))
            if t is None:
                return None
            g = t["cells"].get((x, y))
            return t["base"] + (min(g) if g else 0)
        seen = set()
        for start in list(kept):
            if start in seen:
                continue
            run, todo = [], [start]
            seen.add(start)
            while todo:
                x, y = todo.pop()
                run.append((x, y))
                for n in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
                    if n in kept and n not in seen:
                        seen.add(n)
                        todo.append(n)
            shifts = collections.Counter()
            for (x, y) in run:
                for n in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
                    if n in kept or n not in flat or c.kind.get(n) != GROUND or c.water(n):
                        continue
                    r = ref_at(n)
                    if r is not None:
                        shifts[round(flat[n] - r)] += 1
            if not shifts:
                continue
            d = shifts.most_common(1)[0][0]
            if d:
                for cell in run:
                    kept[cell] = [[v + d for v in row] for row in kept[cell]]
                    keep_shift[cell] = d
        # (their cut records too, a foot in the world: merge writes them
        # over the map's base as it is then, not moved again each time)
        for cell in kept:
            lid, x, y = c.src[cell]
            lay = vrf.layout_index(lid)
            rec = reference["cuts"].get((lay, x, y))
            if rec is not None:
                kept_cuts[cell] = [rec[0], rec[1] + reference["layouts"][lay]["base"] + keep_shift.get(cell, 0)]                     + list(rec[2:])
    kept_berm, kept_cuts_berm = dict(kept), dict(kept_cuts)
    ledges = lay_ledges(c, None, flat, kept, kept_cuts)
    pieces = blocks(c, U, flat, kept)
    # a gap in a face drawn in a town's own tiles (Lilycove's stairs between
    # two stretches of cliff): ground flat at the top's level over lower
    # ground, it stood as a slab with a two-level step at its front and no
    # wall - it is the face, stepping down with the cliff either side
    # (south rows first: the row over a converted one steps down onto it)
    # The same at the face's foot (Route 111's Trainer Hill: the face over
    # its door drawn in the building's tiles, the door's top among them, read
    # as ground a level under the face either side and under the rim over
    # it - a trench open to the void the length of the door): the face too,
    # when the rim over it stands at the face's top.
    def gap_cell(n, h):
        q = pieces.get(n)
        if q is None or q[0] != "F" or n in c.keep or n in c.bridges:
            return False
        k = c.kind.get(n)
        # (the door's top half among them, green enough for the regions to
        # read it as a tree: a General tree is what stands on its own)
        if not (k == GROUND or (k == UNKNOWN and (c.role(n) != "tree" or c.metatile(n) >= vb.NUM_PRIMARY))):
            return False
        if q[1] >= h - 0.5:
            return k == GROUND
        north = pieces.get((n[0], n[1] - 1))
        return abs(q[1] - (h - LEVEL)) < 0.5 and north is not None and north[1] >= h - 0.5 \
            and c.kind.get((n[0], n[1] - 1)) != ROCK
    for (x, y), p in sorted(pieces.items(), key=lambda kv: (-kv[0][1], kv[0][0])):
        p = pieces[(x, y)]
        left = pieces.get((x - 1, y))
        if left is None or left[0] != "S":
            continue
        h = left[1]
        if not gap_cell((x, y), h):
            continue
        run = [(x, y)]
        while True:
            n = (run[-1][0] + 1, y)
            q = pieces.get(n)
            if q is None or len(run) > 6:
                run = None
                break
            if q[0] == "S" and abs(q[1] - h) < 0.5:
                break
            if not gap_cell(n, h):
                run = None
                break
            run.append(n)
        if not run:
            continue
        if all((cx, y + 1) in pieces and pieces[(cx, y + 1)][1] < h - 0.5 for cx, _ in run):
            for cell in run:
                pieces[cell] = ("S", h)
            # the face's ends either side are its cheeks: whole, not cut
            # (cut, their edge beside the gap opened wedges to the void)
            c.cheeks.update({(x - 1, y), (run[-1][0] + 1, y)})
    # a waterfall falls a level a row, as a face does, from the river above
    # it to the pool at its foot (flat at the river's level, the pool's
    # edge stood under it as a wall of water)
    for cell in sorted(c.falls):
        x, y = cell
        if (x, y - 1) in c.falls:
            continue
        run = [cell]
        while (x, run[-1][1] + 1) in c.falls:
            run.append((x, run[-1][1] + 1))
        above, below = (x, y - 1), (x, run[-1][1] + 1)
        if above not in flat or below not in flat:
            continue
        top, foot = flat[above], flat[below]
        if abs(top - foot - len(run) * LEVEL) > 0.5:
            continue
        for k, f in enumerate(run):
            pieces[f] = ("S", top - k * LEVEL)
        # the cliffs it falls between are one face with it: each row of
        # theirs a level down beside its own (the east one started a level
        # higher, a dark wedge of wall standing over the water)
        for side in (-1, 1):
            beside = [(f[0] + side, f[1]) for f in run]
            if all(b in pieces and c.kind.get(b) == ROCK and b not in c.keep and b not in c.band
                   for b in beside):
                for k, b in enumerate(beside):
                    pieces[b] = ("S", top - k * LEVEL)
    # the shoulders of a mound (rock drawn beside its top, over the face's
    # corners): the top they flank, not ground - left at the ground's level
    # the top stood alone, a pillar walled on both sides
    for _ in range(2):
        for cell, p in list(pieces.items()):
            if c.kind.get(cell) != ROCK or p[0] not in ("F", "T") or cell in c.keep:
                continue
            x, y = cell
            south = pieces.get((x, y + 1))
            if south is not None and south[0] in ("S",) + tuple(CORNERS):
                south = (south[0], south[1])
            for n in ((x - 1, y), (x + 1, y)):
                q = pieces.get(n)
                if q is None or q[0] not in ("F", "T") or q[1] <= p[1]:
                    continue
                if c.kind.get(n) not in (TOP, ROCK) or south is None or south[1] < q[1]:
                    continue
                # a flank falling one level away from the top (the mound
                # narrows up to it, drawn round)
                pieces[cell] = ("W" if n[0] > x else "E", q[1])
                break
    # the flanks of a mound: rock at the end of a row of faces, lower than
    # the face beside it, with the water or ground at its other side - the
    # flank falls from that face's top to that ground as a band (one or two
    # levels): the mound's outline narrows upwards, as drawn, instead of
    # stepping in as a box on a box
    for cell, p in list(pieces.items()):
        if c.kind.get(cell) != ROCK or cell in c.keep or cell in c.band:
            continue
        x, y = cell
        for inner, outer, kind in (((x + 1, y), (x - 1, y), "W"), ((x - 1, y), (x + 1, y), "E")):
            q = pieces.get(inner)
            if q is None or c.kind.get(inner) != ROCK or q[0] not in ("S", "W", "E") + tuple(CORNERS)                     or q[1] <= p[1]:
                continue
            if outer not in flat or c.kind.get(outer) == ROCK:
                continue
            drop = q[1] - flat[outer]
            if abs(drop - LEVEL) < 0.5:
                pieces[cell] = (kind, q[1])
            elif abs(drop - 2 * LEVEL) < 0.5:
                pieces[cell] = (kind + "2", q[1])
            break
    # (each reads the other's pieces: a staircase's foot voted by a face
    # beside it that the faces' fall then lowered stood a peak over it)
    measured = {(hi, lo) for d, hi, cells, lo in runs if d == "S"}
    for _ in range(3):
        before = dict(pieces)
        staircases(c, pieces, flat, kept)
        level_bands(c, pieces, flat, kept, measured)
        if pieces == before:
            break
    unstand(c, pieces, flat, kept)
    face_ends(c, pieces, flat, kept)
    hidden_drops(c, pieces, flat, kept)
    hidden_face_ends(c, pieces, flat, kept)
    rim_backs(c, pieces, flat, kept)
    rough_fields(c, pieces, flat, kept)
    if meets is not None:
        seam_edges(c, pieces, ref, meets, kept)
    return {"cells": c, "comp": comp, "level": level, "U": U, "flat": flat, "kept": kept,
            "pieces": pieces, "dropped": dropped, "keep_shift": keep_shift,
            "kept_cuts": kept_cuts, "kept_berm": kept_berm, "kept_cuts_berm": kept_cuts_berm,
            "landings": landings, "ledges": ledges}


def finish(auto, marks=None):
    """A solve_auto with the hand marks laid over it, and its grids (`auto`
    is left as it was)."""
    c, kept = auto["cells"], auto["kept"]
    pieces, flat = dict(auto["pieces"]), dict(auto["flat"])
    if marks is None:
        marks = load_marks(c, zero=zero_of(auto))
    marked = apply_marks(c, pieces, flat, kept, marks)
    # the ledges between the levels the marks leave, and as marked
    kept, cuts = dict(auto.get("kept_berm", kept)), dict(auto.get("kept_cuts_berm", auto.get("kept_cuts", {})))
    ledge_marks = {cell: m for cell, m in marks.items() if cell in c.jumps}
    ledges = lay_ledges(c, pieces, flat, kept, cuts, ledge_marks)
    soil_marks = {cell: m for cell, m in marks.items() if cell in c.keep and cell not in c.jumps}
    lay_soil(c, pieces, kept, cuts, soil_marks, marked)
    under = {cell: m["under"] for cell, m in marks.items() if m.get("under") and cell in c.bridges}
    marked |= set(ledge_marks) | set(soil_marks) | set(under)
    grids = {cell: piece_grid(*p) for cell, p in pieces.items()}
    grids.update(kept)
    faces_at_landings(c, pieces, grids, ledges["steps"])
    behind_trees(c, flat, grids, pieces, marked)
    return dict(auto, pieces=pieces, flat=flat, grids=grids, marked=marked, kept=kept,
                kept_cuts=cuts, ledges=ledges, under=under)


# -- 7. hand marks ------------------------------------------------------------
#
# Where the drawing is read wrong, a hand says what a cell is (the editor's
# mountain page, tools/private/voxel_editor: relief_fixes.json, each map's
# "terraces": {"x,y": {"level": 2, "piece": "S"}}). A level alone and the
# piece follows from the levels round it, as the tiles build a mountain:
# lower ground south is a face, west or east a band, both a corner (convex
# where the corner's three sides fall, concave where only the diagonal
# does). A face drawn more rows tall than one, or a band more columns wide,
# all marked at its top's level, falls a level a row (a column) to the
# ground at its foot. A piece given is taken as it is. Ledges and berry soil
# are never marked. A neighbour the marks leave no longer meeting them
# follows its levels too.
#
# A mark's level counts from the group's main ground (zero_of: the level
# most of its ground lies at as the drawing alone reads it), not from the
# world's: the world solver may place the group a level higher or lower
# when its seams are read again, and the marks must stand where the hand
# put them over the town, not at the height they had then.

MARK_PIECES = ("F", "S", "W", "E", "W2", "E2") + CORNERS


def zero_of(auto):
    """The group's main ground, px: the level most of its flat ground lies at
    in a solve_auto (the drawing alone, no marks)."""
    c = auto["cells"]
    tops = collections.Counter(LEVEL * round(p[1] / LEVEL) for cell, p in auto["pieces"].items()
                               if p[0] == "F" and c.kind.get(cell) == GROUND)
    return tops.most_common(1)[0][0] if tops else 0


def load_marks(c, fixes=None, zero=0):
    """{canvas cell: {"level": px or None, "piece": kind or None[, "foot": px]}} of a group's
    maps in the fixes file (VOXEL_TERRACE_MARKS=0: none); `zero` the group's
    main ground (zero_of), which the marks' levels count from."""
    import voxel_relief_fixes as vrf
    if os.environ.get("VOXEL_TERRACE_MARKS", "1") == "0":
        return {}
    fixes = vrf.load_fixes() if fixes is None else fixes
    out = {}
    layouts = fixes.get("layouts", {})
    for lid, ox, oy in c.members:
        own = layouts.get(lid, {}).get("terraces") or layouts.get(TWINS.get(lid), {}).get("terraces", {})
        for key, m in own.items():
            x, y = (int(v) for v in key.split(","))
            level = m.get("level")
            piece = m.get("piece")
            if piece is not None and split_kind(piece)[0] not in MARK_PIECES:
                raise ValueError("%s %s: no piece %r" % (lid, key, piece))
            out[(x + ox, y + oy)] = {"level": None if level is None else zero + LEVEL * level, "piece": piece}
            if m.get("foot") is not None:
                out[(x + ox, y + oy)]["foot"] = zero + LEVEL * m["foot"]     # a ledge's
            if m.get("under") in ("rock", "open"):
                out[(x + ox, y + oy)]["under"] = m["under"]                  # a bridge's
    return out


def marked_layouts(fixes=None):
    import voxel_relief_fixes as vrf
    fixes = vrf.load_fixes() if fixes is None else fixes
    marked = [lid for lid, spec in sorted(fixes.get("layouts", {}).items()) if spec.get("terraces")]
    return marked + sorted(twin for twin, lid in TWINS.items() if lid in marked and twin not in marked)


def _named(kind, drop):
    """A piece falling `drop` pixels: "S", "W2", or "S:8" for any other fall."""
    if abs(drop - LEVEL) < 0.5:
        return kind
    if kind in ("W", "E") and abs(drop - 2 * LEVEL) < 0.5:
        return kind + "2"
    return "%s:%g" % (kind, round(drop, 2))


def auto_piece(level, cell):
    """The piece of a cell at level[cell] from the levels round it: each
    corner falls to the lowest cell sharing it west, east or south (a
    mountain's north side is its back, never seen falling)."""
    x, y = cell
    t = level[cell]

    def fall(*ns):
        hs = [level[n] for n in ns if n in level]
        return max(0.0, t - min(hs)) if hs else 0.0
    w, e, s = (x - 1, y), (x + 1, y), (x, y + 1)
    drops = {"NW": fall(w), "NE": fall(e), "SW": fall(w, s, (x - 1, y + 1)), "SE": fall(e, s, (x + 1, y + 1))}
    low = frozenset(k for k, d in drops.items() if d > 0.5)
    if not low:
        return ("F", t)
    drop = max(drops[k] for k in low)
    kind = {frozenset(("SW", "SE")): "S", frozenset(("NW", "SW")): "W", frozenset(("NE", "SE")): "E",
            frozenset(("NW", "SW", "SE")): "cSW", frozenset(("NE", "SE", "SW")): "cSE",
            frozenset(("SW",)): "vSW", frozenset(("SE",)): "vSE"}.get(low)
    if kind is None:
        # falling both west and east (a ridge a tile wide): a face if it
        # falls south, else level
        if fall(s) <= 0.5:
            return ("F", t)
        kind, drop = "S", fall(s)
    return (_named(kind, drop), t)


def _mark_runs(c, auto, level):
    """{cell: piece} of the marked faces drawn more than a row tall and the
    marked bands more than a column wide, all at one level over lower
    ground at their foot: a level a row (column) down, or the fall shared
    out between them where it has fewer levels than they have rows."""
    out = {}

    def share(run, top, foot, kind):
        k, d = len(run), top - foot
        each = d / k if d < k * LEVEL else LEVEL
        for i, cell in enumerate(run):
            fall = each if i < k - 1 else d - each * (k - 1)
            out[cell] = (_named(kind, fall), top - each * i)
            level[cell] = top - each * i

    def run_from(cell, step, ok):
        run = [cell]
        while True:
            n = (run[-1][0] + step[0], run[-1][1] + step[1])
            if n not in auto or not ok(n) or abs(level[n] - level[cell]) > 0.5:
                return run
            run.append(n)
    seen = set()
    for cell in sorted(auto, key=lambda cl: (cl[1], cl[0])):
        if cell in seen or cell in c.band or not is_face(c, cell):
            continue
        x, y = cell
        if (x, y - 1) in auto and is_face(c, (x, y - 1)) and abs(level[(x, y - 1)] - level[cell]) < 0.5:
            continue                                # not the top of its run
        run = run_from(cell, (0, 1), lambda n: n not in c.band and is_face(c, n))
        seen.update(run)
        foot = (x, run[-1][1] + 1)
        if len(run) > 1 and foot in level and level[foot] < level[cell] - 0.5:
            share(run, level[cell], level[foot], "S")
    for cell in sorted(auto):
        side = c.band.get(cell)
        if cell in seen or side is None:
            continue
        down = (-1, 0) if side == "W" else (1, 0)   # W falls west, its high side east
        x, y = cell
        if (x - down[0], y) in auto and c.band.get((x - down[0], y)) == side \
                and abs(level[(x - down[0], y)] - level[cell]) < 0.5:
            continue                                # not the high end of its chain
        run = run_from(cell, down, lambda n: c.band.get(n) == side)
        seen.update(run)
        foot = (run[-1][0] + down[0], y)
        if len(run) > 1 and foot in level and level[foot] < level[cell] - 0.5:
            share(run, level[cell], level[foot], side)
    return out


def _meet(pieces, a, b):
    """Whether two neighbouring cells' pieces share their edge (or, diagonal,
    their corner) at one height."""
    ga, gb = piece_grid(*pieces[a]), piece_grid(*pieces[b])
    dx, dy = b[0] - a[0], b[1] - a[1]
    if dx and dy:
        return abs(ga[N if dy > 0 else 0][N if dx > 0 else 0] - gb[0 if dy > 0 else N][0 if dx > 0 else N]) < 0.5
    side = {(1, 0): ("E", "W"), (-1, 0): ("W", "E"), (0, 1): ("S", "N"), (0, -1): ("N", "S")}[(dx, dy)]
    return all(abs(p - q) < 0.5 for p, q in zip(edge(ga, side[0]), edge(gb, side[1])))


def apply_marks(c, pieces, flat, kept, marks):
    """The hand marks over a solve's pieces (and flat, the level a tree's
    cell lies at), in place: the cells marked."""
    marks = {cell: m for cell, m in marks.items() if cell in pieces and cell not in kept and cell not in c.keep}
    if not marks:
        return set()
    level = {cell: p[1] for cell, p in pieces.items()}
    for cell, m in marks.items():
        if m.get("level") is not None:
            level[cell] = m["level"]
    auto = {cell for cell, m in marks.items() if m.get("piece") in (None, "auto")}
    runs = _mark_runs(c, auto, level)
    for cell, m in marks.items():
        if cell in runs:
            pieces[cell] = runs[cell]
        elif cell in auto:
            pieces[cell] = auto_piece(level, cell)
        else:
            pieces[cell] = (m["piece"], level[cell])
        if cell in c.bridges:
            # a bridge's deck is level whatever shape is painted over it (a
            # bank's column painted a band, Route 120's east end: the deck
            # tipped down the bank, broken off the planks beside it)
            pieces[cell] = ("F", level[cell])
        if pieces[cell][0] == "F":
            flat[cell] = level[cell]
    for cell in sorted(marks):
        x, y = cell
        for n in ((x + i, y + j) for j in (-1, 0, 1) for i in (-1, 0, 1) if i or j):
            if n in marks or n not in pieces or n in kept or n in c.keep or _meet(pieces, cell, n):
                continue
            # (not water: the river beside a marked boulder stood up as its
            # slopes, climbing it - Route 120)
            if c.water(n):
                continue
            pieces[n] = auto_piece(level, n)
            if pieces[n][0] == "F":
                flat[n] = level[n]
    return set(marks)


# -- into relief.bin ----------------------------------------------------------

CUT_LIFT = 2          # pixels: background lifted more than this is cut away
FACES = (0x07c, 0x0a9, 0x079, 0x091)   # a step's wall: the first face the map draws
BLOCK_WALLS = 4       # voxel_relief.h VOXEL_RELIEF_BLOCK_WALLS >> 8: west and east walls of the face too
PLAIN = 1             # its flat layer is the ground behind it, plain
THIN_DECK = 8         # voxel_relief.h VOXEL_RELIEF_THIN_DECK >> 8: a deck's walls are its edge alone
# the grey the General draws round its rock where it meets the ground (the
# last columns of a band, the foot of a face, the edge of a corner)
OUTLINE_GREY = {(0x83, 0x83, 0x8b)}
GOES_ON = gr.GOES_ON  # voxel_relief.h VOXEL_RELIEF_GOES_ON >> 8: the band north of it goes on under it
GOES_ON_FACE = 32     # voxel_relief.h VOXEL_RELIEF_GOES_ON_FACE >> 8: ...whole, as the face in its wall
GOES_ON_SOUTH = 64    # voxel_relief.h VOXEL_RELIEF_GOES_ON_SOUTH >> 8: the rock south of it, a row back
FACE_TOP = 16         # voxel_relief.h VOXEL_RELIEF_FACE_TOP >> 8: its relief drawn with its wall
CANOPY = 128          # voxel_relief.h VOXEL_RELIEF_CANOPY >> 8: its variant whole, over a plate of its wall
# the small tree's canopy top the General draws over the cell north of its
# trunk (voxel_tree.c draws that crown from the trunk's cell)
CANOPY_TOP = 0x00E
# a band's side face: a band tile drawn mostly in the face's colours (not a
# head, not a corner over a top), and the General's own when a map has none
SIDE_FACE = {"W": 0x070, "E": 0x072}


def _raw_pixels(art, m):
    """Metatile m as the GBA composes it, row major: [(rgb, upper layer?)],
    None where neither layer draws (the backdrop shows) - in the tileset's
    own colours, as voxel_tree.c's atlas has them."""
    out = [None] * 256
    for layer in (0, 1):
        for q, block in enumerate(art.subtiles(m, layer)):
            for j in range(8):
                for i in range(8):
                    rgb, ci = block[j * 8 + i]
                    if ci == 0:
                        continue
                    out[((q // 2) * 8 + j) * 16 + (q % 2) * 8 + i] = (tuple(rgb)[:3], layer == 1)
    return out


def canopy_mask(art, m):
    """Rows of bits, 1 where metatile m draws the canopy of the small tree
    south of it (the General's CANOPY_TOP over another ground: Fortree's 284,
    a crown over the rim of Route 120's plateau); None where it draws none.
    The crown is the tree model's: drawn in the cell's own tile as well, the
    leaves lay flat on the rim behind the tree."""
    known = art.__dict__.setdefault("_canopy", {})
    if m not in known:
        ref = _raw_pixels(art, CANOPY_TOP)
        crown = [k for k in range(256) if ref[k] is not None and ref[k][1]]
        own = _raw_pixels(art, m)
        same = [k for k in crown if own[k] is not None and own[k][0] == ref[k][0]]
        rows = None
        if m != CANOPY_TOP and len(same) >= 0.9 * len(crown):
            # (and what the tile leaves undrawn round it: the backdrop, a
            # dark speck at the crown's foot)
            rows = [0] * 16
            for k in same + [k for k in range(256) if own[k] is None]:
                rows[k // 16] |= 1 << (k % 16)
        known[m] = rows
    return known[m]


def runs_south(art, m):
    """Is metatile m drawn the same in every row, near enough (three in four
    pixels as in its first row): tiled down a column, it shows no seam."""
    known = art.__dict__.setdefault("_runs_south", {})
    if m not in known:
        img = art.cell_image(m).convert("RGB").load()
        same = sum(1 for j in range(16) for i in range(16) if img[i, j] == img[i, 0])
        known[m] = same >= 192
    return known[m]


def tree_ground(c, cell):
    """The plain ground a tree's cell (a trunk, or a canopy over its north
    neighbour) is drawn over: of the ground round it, the tile most of the
    colour its own drawing has where the tree is not (a trunk's lower
    layer; a canopy's tile outside the crown). None if it shows none."""
    lid, x, y = c.src[cell]
    art, m = c.arts[lid], c.metatile(cell)
    if m in TREE_CARDS:
        px = list(art.cell_image(m, (0,)).convert("RGB").getdata())
    else:
        img = list(art.cell_image(m).convert("RGB").getdata())
        rows = canopy_mask(art, m) or [0] * 16
        px = [p for k, p in enumerate(img) if not (rows[k // 16] >> (k % 16)) & 1]
    px = [p for p in px if p not in gr.ROCK_ALL and p not in OUTLINE_GREY]
    if len(px) < 8:
        return None
    rock_of = c.__dict__.setdefault("_rock_of", {})
    if lid not in rock_of:
        rock_of[lid] = {c.src[r][1:] for r, k in c.kind.items() if k in (ROCK, TOP) and c.src[r][0] == lid}
    return gr.plain_ground(art, c.roles_of[lid], rock_of[lid], x, y, None, gr.commonest(px))


def side_face(c, cell, side):
    """The metatile of the side face the band `side` of cell's map draws:
    the nearest band tile of that side that is all band (0x070, 0x075...)."""
    lid, x, y = c.src[cell]
    art = c.arts[lid]
    tiles = BAND_WEST if side == "W" else BAND_EAST
    best = None
    for n, s in c.band.items():
        if s != side or c.src[n][0] != lid:
            continue
        m = c.metatile(n)
        if twin(art, m) not in tiles or twin(art, m) in HEADS:
            continue
        cls = pixel_classes(art, m)
        if sum(r.count("F") for r in cls) < 0.5 * 256:
            continue
        d = abs(c.src[n][1] - x) + abs(c.src[n][2] - y)
        if best is None or d < best[0]:
            best = (d, m)
    return best[1] if best else SIDE_FACE[side]


# the band heads every other head is drawn from: the same rock over another
# background (the next terrace's top, the sea, a boulder's ground)
HEAD_DRAWN = {"W": 0x068, "E": 0x06a}
_HEAD_OF = {}


def head_of(art, m):
    """The head (HEAD_DRAWN) tile m draws over another background - its
    face pixels that head's - or None. A corner is the same corner
    whatever its background: cut by its own background (0x06d, a head on the
    next terrace's top, cut along the top beyond the band's foot) it had a
    strip, slivers and a sliver of wall that 0x06a over the grass has not.
    The boulders' heads (BOULDER_HEAD) have their own rule."""
    m = twin(art, m)
    if m >= 0x200 or m in BOULDER_HEAD:
        return None
    key = (art.primary, art.secondary, m, gr.reading(art))
    if key not in _HEAD_OF:
        def face(t):
            cls = pixel_classes(art, t)
            return {(i, j) for j in range(16) for i in range(16) if cls[j][i] == "F"}
        mine = face(m)
        # (a fleck at the face's edge goes with its neighbours, which differ
        # with the background: a few pixels either way)
        _HEAD_OF[key] = next((h for h in HEAD_DRAWN.values()
                              if len(mine) >= 32 and len(face(h) ^ mine) <= 4), None)
    return _HEAD_OF[key]


def head_mask(h):
    """Rows of bits of head h's background: the half of the cell off the
    diagonal through the corner it rises from (W heads rise to the east).
    Its rock is cut along that diagonal, corner to corner, whatever its
    background and whatever colours the map is read in (in alias, read off
    0x068's drawing, the General's grass was no ground and the head was left
    uncut, a band with a wall for a side)."""
    west = h == HEAD_DRAWN["W"]
    return [sum(1 << i for i in range(16) if (i + j <= 15 if west else i >= j)) for j in range(16)]


def outline(art, m, mask, open_sides):
    """Rows of bits, 1 where tile m draws the grey outline of its rock
    against the ground: the grey pixels reached, through grey, from its
    background (`mask`) or from an edge of the tile on one of `open_sides`
    (N/S/W/E: the ground lies past it). In the drawing a line a pixel or two
    wide between the rock and the ground; lifted with the rock, a grey strip
    along every band's and face's foot, on no ground the map has."""
    img = art.cell_image(m).convert("RGB").load()
    grey = {(i, j) for j in range(16) for i in range(16) if img[i, j] in OUTLINE_GREY}
    if not grey:
        return None
    seen = set()
    todo = [(i, j) for (i, j) in grey
            if ("W" in open_sides and i == 0) or ("E" in open_sides and i == 15)
            or ("N" in open_sides and j == 0) or ("S" in open_sides and j == 15)
            or (mask and any(0 <= a < 16 and 0 <= b < 16 and (mask[b] >> a) & 1
                             for a, b in ((i - 1, j), (i + 1, j), (i, j - 1), (i, j + 1))))]
    while todo:
        p = todo.pop()
        if p in seen:
            continue
        seen.add(p)
        i, j = p
        todo += [q for q in ((i - 1, j), (i + 1, j), (i, j - 1), (i, j + 1)) if q in grey and q not in seen]
    if not seen:
        return None
    return [sum(1 << i for i in range(16) if (i, j) in seen) for j in range(16)]


def edge(grid, side):
    """The five heights along one edge of a cell, west to east / north to south."""
    if side == "N":
        return grid[0]
    if side == "S":
        return grid[N]
    return [grid[j][0 if side == "W" else N] for j in range(N + 1)]


def _sample(g, px, py):
    fx, fy = px / (16 / N), py / (16 / N)
    a, b = min(int(fx), N - 1), min(int(fy), N - 1)
    tx, ty = fx - a, fy - b
    return ((g[b][a] * (1 - tx) + g[b][a + 1] * tx) * (1 - ty)
            + (g[b + 1][a] * (1 - tx) + g[b + 1][a + 1] * tx) * ty)


def behind(c, out, cell, lid, grids=None, level=None):
    """The ground that runs on under a cell, seen past its back: what the
    cell north of it is (flat) or stands on (its own cut's ground), else the
    first flat drawing up its column - in the map's own tileset. Right under
    rock (a band's or a face's foot, a back the flat drawing has touching
    the rock over it) the gap the 3D view opens is that rock's own block:
    the drawing never shows ground there.
    Only a cell that comes down to the fill's `level` (in `grids`): a rim
    or its corner lifted whole with the terrace this cell tops stands
    nowhere near the ground behind, and its block (or the plain top of its
    colour) there was the mountain painted on the meadow past the rim -
    what runs on is the ground it is drawn over (its own cut's), or the
    ground further up the column."""
    x, y = cell
    for k in range(1, 4):
        n = (x, y - k)
        if n not in c.src or c.src[n][0] != lid:
            break
        if grids is not None and level is not None and n in grids \
                and min(edge(grids[n], "S")) > level + LEVEL / 2:
            # lifted over the fill: not the ground behind, but what it is
            # drawn over (its cut's ground), or what lies further up
            if n in out and out[n][2] is not None:
                return out[n][2]
            continue
        if k == 1 and c.kind[n] == ROCK:
            return c.metatile(n)
        if (c.kind[n] == UNKNOWN and c.role(n) == "tree") or c.crown(n):
            # a wood: the ground its trees stand on, not the crowns its tiles
            # draw (laid flat, the leaves lay on the river under Route 120's
            # wood), nor the fill run on under the tree (the river, under the
            # landing of grass its bank steps down to)
            m = tree_ground(c, n)
            if m is not None:
                return m
        if c.kind[n] == GROUND and c.water(n) and runs_south(c.art(n), c.metatile(n)):
            # a shore drawn the same all down its rows (the river's edge
            # along a cliff, its shadow in a darker band): it runs on as it
            # is. Plain, the band stopped a row short and the plain water
            # past it stood out as a notch (Route 120's east bank)
            return c.metatile(n)
        if c.kind[n] in (GROUND, TOP):
            # the plain ground, not the drawing of its edge: a shore tile's
            # foam ran on under the rock as a white line along its foot
            # (cached on the cells themselves: keyed by id(c), a group solved
            # later could reuse a freed group's id and read its ground)
            l2, nx, ny = c.src[n]
            rock_of = c.__dict__.setdefault("_rock_of", {})
            plain_of = c.__dict__.setdefault("_plain_of", {})
            if lid not in rock_of:
                rock_of[lid] = {c.src[r][1:] for r, k in c.kind.items()
                                if k in (ROCK, TOP) and c.src[r][0] == lid}
            key = (lid, nx, ny)
            if key not in plain_of:
                # (in the map's own colours: see records)
                art = c.arts[lid]
                seen = gr.own_art(lid) if hasattr(art, "own_metatile") else art
                past = rock_of[lid]
                if seen is not art:
                    past = past | {c.src[r][1:] for r in c.rim if c.src[r][0] == lid}
                m = gr.plain_ground(seen, c.roles_of[lid], past, nx, ny, "S", half=seen is not art)
                plain_of[key] = art.alias.get(m, m) if seen is not art else m
            m = plain_of[key]
            return c.metatile(n) if m is None else m
        if n in out and out[n][2] is not None:
            return out[n][2]
    return 0x071


def low_side(c, cell):
    """Rows of bits, 1 where a band's tile draws the top beyond the band's
    foot: past the band's last face-coloured pixel on its low side, a row at
    a time, and above where the band starts (its head) past that edge
    carried on up. None for a cell that is no band."""
    side = c.band.get(cell)
    if side is None:
        return None
    cls = pixel_classes(c.art(cell), c.metatile(cell))
    rows = {}
    for j in range(16):
        fx = [i for i in range(16) if cls[j][i] == "F"]
        if fx:
            rows[j] = max(fx) if side == "E" else min(fx)
    if len(rows) < 3:
        return None
    js = sorted(rows)
    # the edge carried on above (and below) the band: a straight line
    n = len(js)
    mj = sum(js) / n
    mx = sum(rows[j] for j in js) / n
    sj = sum((j - mj) ** 2 for j in js) or 1.0
    slope = sum((j - mj) * (rows[j] - mx) for j in js) / sj
    out = []
    for j in range(16):
        e = rows[j] if j in rows else mx + slope * (j - mj)
        bits = 0
        for i in range(16):
            beyond = i > e + 0.5 if side == "E" else i < e - 0.5
            if beyond and cls[j][i] == "T":
                bits |= 1 << i
        out.append(bits)
    return out


def face_goes_on(c, n, side):
    """Is n water whose edge next to a band on its `side` (the band's low
    side: n is west of a W band) is drawn in the mountains' colours, the
    band's face going on into it?"""
    if n not in c.src or c.kind.get(n) != GROUND or not c.water(n):
        return False
    img = c.art(n).cell_image(c.metatile(n)).convert("RGB")
    i = 15 if side == "W" else 0
    return sum(1 for j in range(16) if img.getpixel((i, j)) in gr.ROCK_ALL) >= 12


def edge_strip(mask):
    """A cut mask whose ground is only a strip one pixel thin along an edge
    of the tile is drawing, not shape (a rim's grass between the bumps of its
    highlight): no cut - cut, the rim's back stood a pixel behind the uncut
    top beside it."""
    cleared = [(i, j) for j in range(16) for i in range(16) if (mask[j] >> i) & 1]
    if cleared and (all(j == 0 for _, j in cleared) or all(j == 15 for _, j in cleared)
                    or all(i == 0 for i, _ in cleared) or all(i == 15 for i, _ in cleared)):
        return [0] * 16
    return list(mask)


def drawn_rock_pixels(art, m):
    """How many pixels of metatile m are drawn in the mountains' colours."""
    img = art.cell_image(m).convert("RGB")
    return sum(1 for px in img.getdata() if px in gr.ROCK_ALL)


def records(sol):
    """{canvas cell: [variant mask, "fill" or None, foot, ground metatile or
    None, wall, sides, flags]}: cut tiles, the ground behind run on, cliff
    walls. Heights in the world's levels."""
    c, grids = sol["cells"], sol["grids"]
    rock = {cell for cell, k in c.kind.items() if k in (ROCK, TOP)}
    local_rock = collections.defaultdict(set)
    # (and the rock alone, no tops: the plain top is the ground a head on
    # the next terrace is drawn over - without it, the plainest tile of a
    # top's colour left was a boulder's, rock laid flat at the head's foot)
    local_falls = collections.defaultdict(set)
    for cell in rock:
        lid, x, y = c.src[cell]
        local_rock[lid].add((x, y))
        if c.kind[cell] == ROCK:
            local_falls[lid].add((x, y))
    # (the ground a map read in alias stands its cuts on is looked for past
    # its rims too: a rim is the ground's edge, its lip and the grass behind
    # it drawn - Route 114's 239 ran under every cut, a pale line and green
    # flecks across its terraces)
    past_rims = collections.defaultdict(set)
    for cell in c.rim:
        lid, x, y = c.src[cell]
        past_rims[lid].add((x, y))
    # (nor is a house or a prop: a cut beside one took its roof for the
    # ground behind it, an orange corner under Verdanturf's rock by the
    # Battle Tent - and, past the tent, the signpost's plate)
    houses = collections.defaultdict(set)
    for cell in c.src:
        if c.house(cell) or c.role(cell) in ("wall", "signpost", "fence", "shelf"):
            lid, x, y = c.src[cell]
            houses[lid].add((x, y))
    # water that draws rock and stands over the ground beside it, the hand
    # lifted with the rock it draws (a boulder's top in the river round it,
    # Route 120): cut as the rock is, its water laid at the foot (whole, the
    # river stood a level up round the boulder). Its water still counts as
    # ground for its neighbours: the river beside it is another blue
    drawn_rock = {}         # {cell: its foot, the water beside it}
    for cell, g in grids.items():
        # (water only: grass drawn with a top's shade, a rim's lip, are the
        # ground's own drawing, the terrace they stand on)
        if c.kind.get(cell) in (ROCK, TOP) or cell in c.keep or cell in c.bridges or not c.water(cell):
            continue
        x, y = cell
        low = [min(min(r) for r in grids[n]) for n in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1))
               if n in grids and c.kind.get(n) not in (ROCK, TOP)]
        # (lifted anywhere: a slope beside the boulder lifts its water too)
        if not low or max(max(r) for r in g) < min(low) + LEVEL - 0.5:
            continue
        lid = c.src[cell][0]
        if drawn_rock_pixels(c.arts[lid], c.metatile(cell)) >= 32:
            drawn_rock[cell] = min(low)
    face_of = {}
    for lid, ox, oy in c.members:
        art = c.arts[lid]
        drawn = {art.metatile(x, y) for y in range(art.h) for x in range(art.w)}
        face_of[lid] = next((f for f in FACES if f in drawn), FACES[0])
    out = {}
    # rows north first: a fill takes what the cell north of it stands on
    for (x, y) in sorted(grids, key=lambda cell: (cell[1], cell[0])):
        g = grids[(x, y)]
        lid, lx, ly = c.src[(x, y)]
        art, roles = c.arts[lid], c.roles_of[lid]
        rec = [None, 0, None, None, 0, 0]
        # cliff walls: an edge standing over its neighbour's
        for n, mine, theirs in (((x - 1, y), "W", "E"), ((x + 1, y), "E", "W"), ((x, y + 1), "S", "N")):
            if n not in grids:
                continue
            if any(p - q > 0.5 for p, q in zip(edge(g, mine), edge(grids[n], theirs))):
                # a bridge's deck over the water: its sides are the deck's
                # own planks, not a cliff's rock
                # (a wood's step is the mountain's face: drawn as the trees'
                # own tile, a wall of crowns stood under the wood - Route
                # 120's bank, "always mountain")
                own = (x, y) in c.bridges
                rec[3] = c.metatile((x, y)) if own else face_of[lid]
                rec[4] = 0xFF
                rec[5] |= BLOCK_WALLS
        # its background, lifted off the ground it is: cut away
        # the ground a rock tile draws round its rock (a band's sand corner,
        # the sea past a ridge's flank) is not rock: cut away, it lies flat
        # at the foot as the ground it is (the pieces stay whole)
        # (stairs cut through a face are all stairs: their shaded steps are
        # no ground to cut away - cut, they showed the void)
        # (nor is rough rock's: its pale grit, the cable car's line and posts
        # over it are drawn on the rock, the mountain's own surface - cut and
        # laid at the foot, each left a speck of the void at its edge,
        # Route 112's crags and the cliff's west end - nor the rock beside
        # it: a band's head cut along its diagonal there laid its other
        # half at the foot of the field's slopes, a wedge of the void where
        # four of them met, Route 112's valley under the pale rock at 36,34)
        mask = (gr._cut_mask(art, roles, lx, ly, local_rock[lid])
                if (c.kind[(x, y)] in (ROCK, TOP) or (x, y) in drawn_rock)
                and not c.is_stair((x, y)) and not c.muddy((x, y))
                and (x, y) not in c.cheeks and (x, y) not in c.rough
                and not any(n in c.rough for n in c._four((x, y)))
                else None)
        if mask and hasattr(art, "own_metatile"):
            # a band's head drawn over the map's own ground (twin: Route
            # 114's 220/222 on Fallarbor's dirt, flecked in the rock's
            # colours once read in the General's): cut as the head it is,
            # along its one diagonal - cut by its colours it fell to slivers
            m = c.metatile((x, y))
            head = twin(art, m)
            if head != m and head in HEADS:
                # (its ground the triangle off the diagonal through the
                # corner it rises from: W heads rise to the east)
                west = HEADS[head] == "W"
                mask = [sum(1 << i for i in range(16) if (i + j <= 15 if west else i >= j)) for j in range(16)]
        if mask and (x, y + 1) in c.src and c.role((x, y + 1)) == "tree" and is_face(c, (x, y)):
            # a face whose foot the wood south of it is drawn over: the
            # crowns are the trees' (models of their own), not ground - cut
            # away they broke the cliff's foot into the trees' outline
            mask = None
        # the grey outline a band draws round its rock against the ground is
        # the tile's own shade, drawn with the rock: never cut. Cut away, a
        # band whose only background it was stood two pixels short of the
        # band above it (a jog in its side) and opened a window at its foot;
        # painted over with the rock, the band's texture lay stretched on
        # the ground where the drawing has its grey (Route 111)
        if mask and c.kind[(x, y)] in (ROCK, TOP) and not c.is_stair((x, y)) and not c.muddy((x, y)) \
                and (x, y) not in c.cheeks \
                and (sol["pieces"].get((x, y)) or ("",))[0] in ("W", "E", "W2", "E2") + tuple(CORNERS):
            open_sides = [d for d, n in (("N", (x, y - 1)), ("S", (x, y + 1)), ("W", (x - 1, y)), ("E", (x + 1, y)))
                          if n in c.src and c.src[n][0] == lid and c.kind.get(n) not in (ROCK, TOP)
                          and c.role(n) != "tree"]
            grey = outline(art, c.metatile((x, y)), mask, open_sides)
            if grey is not None and any(grey):
                mask = [a & ~b for a, b in zip(mask, grey)]
                if not any(mask):
                    mask = None
        low = low_side(c, (x, y))
        head = head_of(art, c.metatile((x, y))) if c.kind[(x, y)] in (ROCK, TOP) else None
        if head is not None and (any(mask or []) or any(low or [])):
            # a head over any background is cut as the head it is drawn from
            mask, low = head_mask(head), None
        side = c.band.get((x, y))
        foot = None
        if head is not None and mask is not None:
            # its foot is its low side (as a band's top beyond its foot):
            # the ground beside it can be the terrace it rises to
            foot = min(min(r) for r in g)
        if low and side and c.band.get((x - 1, y) if side == "W" else (x + 1, y)) == side:
            # the next band down the same flank on its low side: the slope
            # goes on there - no terrace behind to cut down to (cut, the
            # sliver of top it draws at its foot opened a slit to the void)
            low = None
        elif low and side and face_goes_on(c, (x - 1, y) if side == "W" else (x + 1, y), side):
            # nor where the water on its low side draws the face going on
            # into it: its pale columns there are the face's lit edge, not a
            # top beyond its foot (cut, Route 120's band by the boulder lost
            # them, the river showing between it and the rest of its face)
            low = None
        if low and any(low):
            # the top beyond a band's foot, drawn in its tile: the terrace a
            # level down, behind it
            mask = [(a | b) for a, b in zip(mask or [0] * 16, low)]
            foot = min(min(r) for r in g)
        if mask and any(mask):
            grounds = [sol["flat"][n] for n in ((x, y - 1), (x - 1, y), (x + 1, y), (x, y + 1))
                       if n in sol["flat"] and c.kind.get(n) in (GROUND, UNKNOWN)]
            if (x, y) in drawn_rock:
                # (the flat a solve leaves water at is no height: marked, it
                # stood two levels under the river)
                foot = drawn_rock[(x, y)]
            elif foot is None:
                foot = min(grounds) if grounds else min(min(r) for r in g)
            elif grounds:
                foot = min([foot] + grounds)
            lifted = sum(1 for j in range(16) for i in range(16)
                         if (mask[j] >> i) & 1 and _sample(g, i + 0.5, j + 0.5) - foot > CUT_LIFT)
            if lifted >= 4:
                # its outline as drawn, pixel for pixel: straightened (a
                # polygon round the rock, half cells), the background the
                # polygon took in stood lifted with the rock, painted its
                # colour - the ground behind a north face's top, a top
                # corner's (all rock where the ground it draws is a one-pixel
                # strip: still a cut record, the ground it runs on under its
                # neighbours kept)
                drawn, mask = mask, edge_strip(mask)
                rec[0], rec[1] = mask, foot
                # (what it is drawn over, read off the ground it draws - in
                # the map's own colours: read in the General's, Route 114's
                # dirt is the General's grass, and its band heads stood on it)
                seen = gr.own_art(lid) if hasattr(art, "own_metatile") else art
                past = (local_rock[lid] | past_rims[lid] if seen is not art else local_rock[lid]) | houses[lid]
                # (what it is drawn over is read off the ground it draws,
                # never off the grey outline round its rock; and a rim - a
                # top's lip and the grass behind it - is no plain ground: a
                # head's ground was 0x21c, a line of grass at its corner)
                ground = gr._behind(seen, roles, lx, ly, past, drawn, half=seen is not art,
                                    content=past if seen is not art else local_falls[lid] | past_rims[lid],
                                    ignore=OUTLINE_GREY)
                if ground is not None and gr._plain_background(seen, seen.metatile(lx, ly), drawn, ground):
                    rec[5] |= PLAIN
                rec[2] = art.alias.get(ground, ground) if seen is not art else ground
                if rec[3] is not None:
                    # a cut tile's sides go down only from its rock's own
                    # pixels (the cleared ground is no wall: seen through
                    # it, a face-textured side stood as a dark slab), as
                    # the edge column of its own drawing
                    bits = 0
                    for k in range(4):
                        for j in range(4 * k, 4 * k + 4):
                            if not mask[j] & 1:
                                bits |= 1 << k
                            if not (mask[j] >> 15) & 1:
                                bits |= 1 << (4 + k)
                    rec[4] = bits
                    rec[5] &= ~BLOCK_WALLS
        # its back over lower ground: that ground runs on under it
        if rec[0] is None and (x, y - 1) in grids:
            north = edge(grids[(x, y - 1)], "S")
            if any(p - q > 0.5 for p, q in zip(edge(g, "N"), north)):
                rec[0] = "fill"
                rec[1] = min(north)
                rec[2] = behind(c, out, (x, y), lid, grids, rec[1])
                # a band ending against this top's back (its foot on the
                # terrace in front): its slope goes on under the top, a row
                # on - what the camera, a little steeper than the drawing,
                # finds past the back. Flat at the band's foot, a wedge of
                # the ground below opened between the band and the top's
                # edge at every corner where a band meets the next terrace
                # (only under it all: a band falling past the top's level,
                # higher than it at one end, went on through it there - a
                # wedge of rock stood on the top)
                n = (x, y - 1)
                gn = grids[n]
                # (a band the hand made of cells drawn otherwise - Route
                # 120's west bank, laid down a column of trees - is a band
                # all the same: ended against the top without going on, a
                # wedge of the river showed at its foot)
                piece = (sol["pieces"].get(n) or ("",))[0]
                side = c.band.get(n) or (piece[0] if piece in ("W", "E", "W2", "E2") else None)
                if side and c.src[n][0] == lid and all(
                        q <= p + 0.5 for p, q in zip(edge(g, "N"), north)) and all(
                        abs(gn[j][i] - gn[N][i]) <= 1.0 for j in range(N + 1) for i in range(N + 1)):
                    rec[5] |= GOES_ON
                    # (a corner cut along its rock - a band's head at the
                    # end of a rim - goes on whole, as the band's side face:
                    # cut, it left a gap beside the corner a camera steeper
                    # than the drawing saw the terrace below through)
                    if out.get(n) and isinstance(out[n][0], list) and rec[3] is None:
                        rec[3] = side_face(c, n, side)
                        rec[5] |= GOES_ON_FACE
                    # (a tree's cell on the band goes on as the rock it is
                    # drawn with, not as the leaves of its own tile)
                    elif out.get(n) and out[n][5] & FACE_TOP and out[n][3] is not None and rec[3] is None:
                        rec[3] = out[n][3]
                        rec[5] |= GOES_ON_FACE
        water = [min(min(r) for r in grids[n]) for n in ((x, y + 1), (x - 1, y), (x + 1, y), (x, y - 1))
                 if n in grids and n not in c.bridges and c.water(n)]
        under = sol.get("under", {}).get((x, y))
        # what the bridge crosses there: the first cell south of its deck
        south = (x, y + 1)
        while south in c.bridges:
            south = (south[0], south[1] + 1)
        over_rock = (c.kind.get(south) in (ROCK, TOP) and south in grids
                     and min(min(r) for r in g) - min(min(r) for r in grids[south]) >= LEVEL - 0.5)
        # over a band (a bank's slope down to the river): the band goes on
        # under the deck - from the band north of it, else south of it (a
        # row of planks between) - and the deck is a deck. Filled as a block
        # it stood over the river as a vertical slab of rock
        band_under = 0
        if (x, y) in c.bridges and under is None and over_rock:
            for n, flag in (((x, y - 1), GOES_ON), ((x, y + 1), GOES_ON_SOUTH)):
                p = sol["pieces"].get(n)
                if n not in c.bridges and n in grids and p is not None and p[0] in ("W", "E", "W2", "E2"):
                    band_under = flag
                    break
        if band_under:
            rec[0], rec[1], rec[2] = "fill", min(min(r) for r in grids[(x, y - 1) if band_under == GOES_ON else (x, y + 1)]), None
            rec[3] = c.metatile((x, y))
            rec[4] = 0xFF
            rec[5] = (rec[5] & ~BLOCK_WALLS) | band_under | THIN_DECK
        elif (x, y) in c.bridges and (under == "rock" or (under != "open" and over_rock)):
            # a bridge's end over the mountain it crosses (Route 120's over
            # the cliffs either side of its river, two levels up): the
            # mountain goes on under it, its face on the deck's sides - open,
            # the void stood under the planks. Over the water it is a deck
            # (filled, a dam of rock stood under it the river's width). The
            # hand can say "open" or "rock"
            rec[3] = face_of[lid]
            rec[4] = 0xFF
            rec[5] |= BLOCK_WALLS
        elif (x, y) in c.bridges:
            # a bridge: a deck over the water, its walls only the deck's
            # edge (walled down to the water it stood as a dam), and the
            # water under it seen through the gap
            # (only over water all round: at its ends, over a lower bank,
            # the gap under a thin deck showed the void)
            under = [n for n, mine, theirs in (((x - 1, y), "W", "E"), ((x + 1, y), "E", "W"),
                                               ((x, y + 1), "S", "N"))
                     if n in grids and any(p - q > 0.5 for p, q in zip(edge(g, mine), edge(grids[n], theirs)))]
            thin = rec[3] is not None and all(c.water(n) or n in c.bridges for n in under)
            if thin:
                rec[5] |= THIN_DECK
            low = [n for n in ((x, y + 1), (x - 1, y), (x + 1, y), (x, y - 1))
                   if n in grids and n not in c.bridges and c.water(n)]
            if low:
                n = min(low, key=lambda n: min(min(r) for r in grids[n]))
                bottom = min(min(r) for r in grids[n])
                if thin and rec[0] is None and bottom < min(min(r) for r in g) - 0.5:
                    rec[0], rec[1], rec[2] = "fill", bottom, c.metatile(n)
        if rec[3] is None and (c.role((x, y)) == "tree" or c.crown((x, y)))                 and max(max(r) for r in g) - min(min(r) for r in g) == LEVEL:
            # a wood on a band or a face one level tall: the console lays
            # the trees' ground on that slope (voxel_tree.c EmitGround) -
            # with the rock named here instead, the band under the trees
            # ("mountain, always": a slope of grass and trunks stood down
            # Route 120's bank)
            p = sol["pieces"].get((x, y)) or ("",)
            rec[3] = side_face(c, (x, y), p[0][0]) if p[0] in ("W", "E", "W2", "E2") else face_of[lid]
            # (and the relief pass lays that rock on the slope too: drawn
            # with the cell's own tile, the crown lay on the band as a slab
            # of leaves - the tree pass draws the tree)
            rec[5] |= FACE_TOP
        if rec[0] is not None or rec[3] is not None:
            out[(x, y)] = rec
    # beside a thin deck (a bridge's end, a bank): its own side goes down to
    # the water under the deck (voxel_mesh_builder walls it there)
    for (x, y) in [cell for cell, rec in out.items() if rec[5] & THIN_DECK]:
        for n in ((x - 1, y), (x + 1, y)):
            if n not in grids:
                continue
            other = out.setdefault(n, [None, 0, None, None, 0, 0])
            if other[5] & THIN_DECK:
                continue
            if n in c.bridges:
                # another plank of the same deck: a deck too (walled down to
                # the water as a bank, it stood under the planks as a slab
                # from the deck to the river - Route 120's east end, the row
                # of planks north of the deck over the band)
                other[5] |= THIN_DECK
                other[5] &= ~BLOCK_WALLS
            if other[3] is None:
                other[3] = c.metatile(n) if n in c.bridges else face_of[c.src[n][0]]
            if not other[4]:
                other[4] = 0xFF
    # under every piece of rock standing over ground round it, the lowest of
    # that ground runs on: wherever two pieces leave a gap the eye can find
    # (where three or four cells meet at different levels), it finds ground
    # there, never the void
    for (x, y) in sorted(grids, key=lambda cell: (cell[1], cell[0])):
        # (and a wood's step: a tree over lower trees beside it)
        if c.kind.get((x, y)) not in (ROCK, TOP, UNKNOWN) or (x, y) in c.keep:
            continue
        rec = out.get((x, y))
        if rec is not None and rec[0] is not None:
            continue
        lid = c.src[(x, y)][0]
        near = [min(min(r) for r in grids[n]) for n in ((x + i, y + j) for i in (-1, 0, 1) for j in (-1, 0, 1))
                if n in grids and n != (x, y) and c.src[n][0] == lid]
        own = min(min(r) for r in grids[(x, y)])
        if not near or min(near) > own - 0.5:
            continue
        rec = out.setdefault((x, y), [None, 0, None, None, 0, 0])
        rec[0], rec[1], rec[2] = "fill", min(near), behind(c, out, (x, y), lid, grids, min(near))
    # a back k levels over the ground behind it stands k rows south of that
    # ground (a pixel at height h is drawn h rows north of where it is): the
    # ground runs on under every one of those rows, not only the first
    for (x, y), rec in sorted(out.items(), key=lambda kv: (kv[0][1], kv[0][0])):
        if rec[0] != "fill":
            continue
        rise = max(edge(grids[(x, y)], "N")) - rec[1]
        # (the fill lies two pixels under the ground it runs on, so a rise of
        # one level leaves a sliver at the back unless the next row has it too)
        for k in range(1, int((rise + 2.5) // LEVEL) + 1):
            n = (x, y + k)
            if n not in grids or c.src[n][0] != c.src[(x, y)][0]:
                break
            if min(min(r) for r in grids[n]) <= rec[1] + 0.5:
                break       # down to that ground already
            m = out.setdefault(n, [None, 0, None, None, 0, 0])
            if m[0] is None:
                m[0], m[1], m[2] = "fill", rec[1], rec[2]
    # a canopy the tile draws over the ground north of its small tree (a
    # secondary tileset's own: the General's, voxel_tree.c draws as their
    # ground): its crown cut away, the ground it is drawn over laid under
    # it at its own height - the tree model stands for the crown. Only where
    # no wall needs the record's own: the plate is its wall
    for cell in sorted(grids, key=lambda cl: (cl[1], cl[0])):
        lid, x, y = c.src[cell]
        art = c.arts[lid]
        below = (cell[0], cell[1] + 1)
        if (hasattr(art, "own_metatile") or c.metatile(cell) < vb.NUM_PRIMARY or cell in c.keep
                or below not in c.src or c.metatile(below) not in SMALL_TRUNKS):
            continue
        rows = canopy_mask(art, c.metatile(cell))
        rec = out.get(cell)
        if rows is None or (rec is not None and (isinstance(rec[0], list) or rec[3] is not None)):
            continue
        plate = tree_ground(c, cell)
        if plate is None:
            continue
        rec = out.setdefault(cell, [None, 0, None, None, 0, 0])
        if rec[0] != "fill":
            rec[1], rec[2] = min(min(r) for r in grids[cell]), None
        rec[0], rec[3], rec[4] = rows, plate, 0
        rec[5] |= CANOPY
    return out


def merge(sol, relief):
    """Put a solved group into a parsed relief.bin (voxel_relief_fixes.parse),
    in place of what the generator had there, each layout over its own base.
    Ledges and berry soil keep their own cells and records."""
    import voxel_relief_fixes as vrf
    c = sol["cells"]
    recs = records(sol)
    # a variant is a metatile cut so: the secondary tileset's ids are its own
    entries = vb._layouts_json()

    def vkey(lay, m, rows):
        return (entries[lay - 1]["secondary_tileset"] if m >= vb.NUM_PRIMARY else None, m, tuple(rows))

    keyof = {vkey(l2, m, rows): i for i, (l2, m, rows) in enumerate(relief["variants"])}
    for lid, ox, oy in c.members:
        lay = vrf.layout_index(lid)
        art = c.arts[lid]
        # a map read in the General's tiles (gr.terrace_art): the console has
        # its own tiles - the cut tile drawn as it is, the ground behind as
        # the map draws it, and a wall of its own rock in its own face
        aliased = hasattr(art, "own_metatile")
        own_xy = art.own_metatile if aliased else art.metatile

        def own(m):
            return m if m is None else gr.terrace_own_id(lid, m)

        def recoloured(x, y):
            """Rock of the map's own tileset (its recoloured mountain), not
            the General's: most of the rock round the cell (Route 115's
            beige mountain beside its brown cliffs over the sea)."""
            mine = theirs = 0
            for j in (-1, 0, 1):
                for i in (-1, 0, 1):
                    n = (x + i + ox, y + j + oy)
                    if n in c.src and c.src[n][0] == lid and c.kind.get(n) in (ROCK, TOP):
                        if own_xy(x + i, y + j) >= vb.NUM_PRIMARY:
                            mine += 1
                        else:
                            theirs += 1
            return mine > theirs
        t = relief["layouts"].setdefault(lay, {"w": art.w, "h": art.h, "flags": vrf.DRAWN,
                                               "base": 0, "cells": {}})
        base = t["base"]
        mine = [cell for cell, src in c.src.items() if src[0] == lid]
        keep = {c.src[cell][1:] for cell in mine if cell in c.keep}
        # the kept cells as the solve laid them (in the world: written over
        # the base as it is now, so merging the same solve again changes
        # nothing); one the solve did not lay keeps what the relief has
        laid = {c.src[cell][1:]: cell for cell in mine if cell in sol.get("kept", {})}
        cells = {cell: g for cell, g in t["cells"].items() if cell in keep and cell not in laid}
        for xy, cell in laid.items():
            g = [v - base for row in sol["grids"][cell] for v in row]
            if any(abs(v) > 0.25 for v in g) or xy in t["cells"]:
                cells[xy] = g
        for cell in mine:
            xy = c.src[cell][1:]
            if xy in keep:
                continue
            g = [v - base for row in sol["grids"][cell] for v in row]
            # a level cell with a wall down its side needs its own grid: the
            # engine stands walls only from cells it has relief for
            if c.kind[cell] in (ROCK, TOP) or any(abs(v) > 0.25 for v in g) or cell in recs:
                cells[xy] = g
        t["cells"] = cells
        t["flags"] |= vrf.DRAWN
        cuts = {k: v for k, v in relief["cuts"].items()
                if k[0] != lay or ((k[1], k[2]) in keep and (k[1], k[2]) not in laid)}
        for xy, cell in laid.items():
            rec = sol.get("kept_cuts", {}).get(cell)
            if rec is not None:
                cuts[(lay,) + xy] = [rec[0], int(round(rec[1] - base))] + list(rec[2:])
        for cell in mine:
            xy = c.src[cell][1:]
            if cell not in recs or xy in keep:
                continue
            mask, foot, ground, wall, sides, flags = recs[cell]
            if aliased:
                ground = own(ground)
                if wall is not None and recoloured(*xy):
                    wall = own(wall)
            if mask is None or mask == "fill":
                k = vrf.NO_VARIANT
            else:
                m = own_xy(*xy)
                key = vkey(lay, m, mask)
                if key not in keyof:
                    keyof[key] = len(relief["variants"])
                    relief["variants"].append((lay, m, list(mask)))
                k = keyof[key]
            cuts[(lay,) + xy] = [k, int(round(foot - base)), vrf.NONE16 if ground is None else ground,
                                 vrf.NONE16 if wall is None else wall, sides, flags]
        relief["cuts"] = cuts
    return relief


def cracks(sol):
    """[(cell, neighbour, edge)]: every edge where two cells part with no
    wall closing it (the higher one west, east or north of the step) and no
    ground run on behind it (the higher one south): must be empty."""
    g = sol["grids"]
    rec = records(sol)
    out = []
    for (x, y) in g:
        for n, mine, theirs in (((x + 1, y), "E", "W"), ((x, y + 1), "S", "N")):
            if n not in g:
                continue
            a, b = edge(g[(x, y)], mine), edge(g[n], theirs)
            if max(p - q for p, q in zip(a, b)) > 0.5 and not (rec.get((x, y)) and rec[(x, y)][3]):
                out.append(((x, y), n, mine))
            if max(q - p for p, q in zip(a, b)) > 0.5:
                closed = rec.get(n) and (rec[n][3] if mine == "E" else rec[n][0])
                if not closed:
                    out.append(((x, y), n, mine))
    return out


# The maps built this way, by the order the player reaches them (and Route
# 116, the reference): the rest keep gen_voxel_relief.py's solve until they
# are looked at. VOXEL_TERRACES=all for every drawn group, =none for none,
# or a list of layouts.
GROUPS = ("LAYOUT_ROUTE104", "LAYOUT_ROUTE105", "LAYOUT_ROUTE106", "LAYOUT_ROUTE116",
          # 2026-10-09: Slateport and the routes round it
          "LAYOUT_SLATEPORT_CITY", "LAYOUT_ROUTE103", "LAYOUT_ROUTE109", "LAYOUT_DEWFORD_TOWN",
          "LAYOUT_ROUTE117", "LAYOUT_ROUTE115",
          # Route 114's south, the same mountain as Route 115's (JOIN)
          "LAYOUT_ROUTE114",
          # Route 123: clean (deformed 2 -> 0, side 2 -> 0)
          "LAYOUT_ROUTE123",
          # the southern sea: Pacifidlog with 130-132, 133, 134 (deformed 58/54/43/21/45/14 -> 1/3/1/1/0/0,
          # holes 0 -> 3 cells of <= 252 px)
          "LAYOUT_PACIFIDLOG_TOWN", "LAYOUT_ROUTE133", "LAYOUT_ROUTE134",
          # the Safari Zone (deformed 54/25/100/7/11 -> 1/15/11/2/11, holes 22 -> 0 in the north-east)
          "LAYOUT_SAFARI_ZONE_NORTH",
          # Route 111 with 112 and 113, both of its layouts (the Mirage Tower's
          # comes and goes): placed in the world by this count (region_ties),
          # level with Mauville
          "LAYOUT_ROUTE111", "LAYOUT_ROUTE111_NO_MIRAGE_TOWER",
          # Lavaridge, its red rock read in the General's (gr.ALIAS_LAYOUTS):
          # deformed 16 -> 3 cells, side 9 -> 0, flat 19 -> 1, holes 10 -> 0
          "LAYOUT_LAVARIDGE_TOWN")


def region_ties(name, prep):
    """{(a, b): d}: the levels of a group's regions as the world solver
    reads them (gen_voxel_relief.drawn_prepare), counted as the terraces
    count them - h[a] - h[b] = d between regions on terraces the votes tie.

    The world places a group by its seams from that reading; a group built
    as terraces must be placed by their count, or the world and the group
    disagree and the terraces are lifted off a seam: Route 111's harmonic
    solve put three levels between the hollow at Mauville and the plateau
    the stairs climb to, where the drawing has two flights of one, and the
    world, pinned at Fallarbor and Lavaridge, left Mauville's seam a level
    apart."""
    members = [(lid, ox, oy) for lid, (ox, oy) in gr.DRAWN[name].items()]
    c = Cells(members)
    runs = runs_of(c)
    comp = terraces(c, runs)
    block = {}
    level, _ = solve_levels(c, comp, runs, blocks_out=block)
    region, big, sizes = prep["region"], prep["big"], prep["sizes"]
    votes = collections.defaultdict(collections.Counter)
    for (cx, cy), t in comp.items():
        py, px = cy * 16 + 8, cx * 16 + 8
        if py >= len(region) or px >= len(region[0]):
            continue
        r = region[py][px]
        if r >= 0 and big[r]:
            votes[r][(block[t], level[t])] += 1
    by_block = collections.defaultdict(list)
    for r, got in votes.items():
        (b, lv), n = got.most_common(1)[0]
        if n * 5 >= sum(got.values()) * 3:     # most of the region on one terrace
            by_block[b].append((r, lv))
    ties = {}
    for b, rs in by_block.items():
        r0, l0 = max(rs, key=lambda e: (sizes[e[0]], -e[0]))
        for r, lv in rs:
            if r != r0:
                ties[(r0, r)] = l0 - lv
    return ties


def chosen_layouts():
    env = os.environ.get("VOXEL_TERRACES", "").strip()
    if env == "none":
        return []
    if env == "all":
        return sorted(min(members) for members in gr.DRAWN.values())
    if env:
        return [l if l.startswith("LAYOUT_") else "LAYOUT_" + l.upper() for l in env.split(",")]
    # (and every map marked by hand: its marks are laid over its terraces)
    marked = marked_layouts() if os.environ.get("VOXEL_TERRACE_MARKS", "1") != "0" else []
    return list(GROUPS) + [l for l in marked if l not in GROUPS]


def apply_file(path, layouts=None, quiet=False):
    """Rebuild the chosen groups of a relief.bin written by gen_voxel_relief
    (its world placement kept: the bases and every map's ground stay where
    the world solver put them)."""
    import voxel_relief_fixes as vrf
    blob = open(path, "rb").read()
    relief = vrf.parse(blob)
    reference = vrf.parse(blob)
    done = set()
    for lid in (chosen_layouts() if layouts is None else layouts):
        if lid in done:
            continue
        sol = solve(lid, reference)
        done.update(m[0] for m in sol["cells"].members)
        merge(sol, relief)
        if not quiet:
            print("terraces %-40s %3d terraces, %d votes dropped" % (
                "+".join(m[0][7:] for m in sol["cells"].members), len(set(sol["comp"].values())),
                len(sol["dropped"])))
    open(path, "wb").write(vrf.pack(relief))


# -- views --------------------------------------------------------------------

def overlay(sol, path, box=None, scale=4):
    """The drawing with each cell's level (white: flat; yellow U: the level a
    rock cell hangs from), its kind and its corners (magenta), in levels."""
    from PIL import Image, ImageDraw
    c = sol["cells"]
    x0, y0, x1, y1 = box or (c.xs.start, c.ys.start, c.xs.stop, c.ys.stop)
    img = Image.new("RGB", ((x1 - x0) * 16, (y1 - y0) * 16))
    for y in range(y0, y1):
        for x in range(x0, x1):
            if (x, y) in c.src:
                img.paste(c.art((x, y)).cell_image(c.metatile((x, y))).convert("RGB"),
                          ((x - x0) * 16, (y - y0) * 16))
    img = img.resize((img.width * scale, img.height * scale), Image.NEAREST)
    d = ImageDraw.Draw(img)
    S = 16 * scale
    for y in range(y0, y1):
        for x in range(x0, x1):
            if (x, y) not in c.kind:
                continue
            X, Y = (x - x0) * S, (y - y0) * S
            k = c.kind[(x, y)]
            if (x, y) in sol["flat"]:
                txt, col = "%d" % (sol["flat"][(x, y)] // LEVEL), (255, 255, 255)
            else:
                txt, col = "U%d" % (sol["U"][(x, y)] // LEVEL), (255, 255, 0)
            d.rectangle([X, Y, X + S - 1, Y + S - 1], outline=(0, 0, 0))
            d.text((X + S // 2 - 6, Y + S // 2 - 6), txt, fill=(0, 0, 0))
            d.text((X + S // 2 - 7, Y + S // 2 - 7), txt, fill=col)
            d.text((X + 2, Y + 1), k + c.band.get((x, y), "") + ("r" if (x, y) in c.rim else ""),
                   fill=(0, 255, 255))
            d.text((X + 3, Y + S - 12), sol["pieces"][(x, y)][0], fill=(255, 128, 255))
    img.save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("layout")
    ap.add_argument("--png", default=None)
    ap.add_argument("--box", default=None)
    ap.add_argument("--relief", nargs=2, default=None, metavar=("IN", "OUT"),
                    help="write the group into a copy of a relief.bin, keeping its world placement")
    ap.add_argument("--check", action="store_true", help="list edges left open (must be none)")
    args = ap.parse_args()
    import voxel_relief_fixes as vrf
    relief = vrf.parse(open(args.relief[0], "rb").read()) if args.relief else None
    sol = solve(args.layout, relief)
    print("%s: %d terraces, %d votes dropped" % (
        "+".join(m[0] for m in sol["cells"].members), len(set(sol["comp"].values())), len(sol["dropped"])))
    for v, n in sol["dropped"]:
        print("  dropped", v, n)
    if args.check:
        open_edges = cracks(sol)
        print("open edges:", len(open_edges), open_edges[:12])
    if args.png:
        box = tuple(int(t) for t in args.box.split(",")) if args.box else None
        overlay(sol, args.png, box)
    if args.relief:
        merge(sol, relief)
        open(args.relief[1], "wb").write(vrf.pack(relief))
        print("relief ->", args.relief[1])


if __name__ == "__main__":
    main()
