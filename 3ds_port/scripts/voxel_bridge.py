#!/usr/bin/env python3
"""The Seaside Cycling Road: a deck on piers, read off its own drawing.

Route 110's cycling road is a bridge. The cartridge says so itself: its deck
is elevation 4 over the elevation-3 land and the elevation-1 sea, and where a
path or the sea passes under it the cells are elevation 15 - walked over at
4, walked or surfed under at 3 or 1. A height field (relief.bin) cannot hold
that: a cell has one height. So the road is a model, like a building, with
open space under it, and the console lifts whoever is up on it (the deck
table of buildings.bin, `decks()` below).

The drawing
-----------
An Emerald drawing is what an orthographic camera at 45 degrees sees: a
point (X, Y, Z) - pixels, X east, Y up, Z south - lands on the art at
u = X, v = Z - Y (voxel_building.py). The road is drawn with:

  the deck     its road surface (two light colours in stripes), ringed by a
               dark outline (62627b) that runs along the foot of the rails
  the rails    13 rows of railing standing on the deck's edge: north of the
               road above its outline, south of it on the road's last rows;
               west and east a strip seen from above
  the front    under the south rail: the kerb (the outline again), the
               girder, and every few cells a pier: its head and a truss
               down to the water
  the ramps    where the road comes down to the land at the gatehouses, the
               road's stripes turn across

Every pixel of it is placed by that rule: the road at the deck's height H,
the south rail, girder and piers on one upright face at the deck's south
edge, the north rail on one at its north edge, the side rails as their tops
H + RAIL over the deck's sides. Seen at 45 degrees it is the drawing; seen
from the game's camera it stands on its piers with the sea under it.

Its height
----------
The piers end 42 rows under the kerb, so the drawing's deck stands 42 px
over the water. Its ramps cannot: they are drawn three cells long, and
exact at 45 degrees they would fall 42 px in 6 - a wall. The deck stands at
DECK (40 px, two and a half levels): the trusses' last two rows would stand
under the water and are laid on it instead. A ramp ends where it is drawn
ending, on the row its foot is drawn on, and comes down over RAMP_RUN
pixels: so it begins up in the deck, three rows over its drawn top, and
falls in a slot cut through the deck's south edge - its rows, the deck's
road over its drawn top among them, laid exactly at 45 degrees. The slot's
sides are walls (RAMP_SIDE thick, where the rails are drawn coming down
beside it) from the ground to the deck, and the deck's front runs straight
on over them to the slot (`_corner`): the drawing turns its rail down the
ramp in a curve, which in the deck's corner stood as a staircase of road
and a rail sticking out over the slot. On the grass at the foot nothing of
it stands (a ramp laid on two cells more of it, its parapets standing on
cells walked by, stood in the trainer by Route 110's southern ramp and in
the fence by its northern one). Under the deck there is room for a walker:
the girder takes GIRDER px under the deck's edges, the deck between them is
a slab of SLAB px.

Its corners
-----------
A corner is drawn as a staircase of pixels; built as one it would be a
staircase of walls. So a front turning at a corner is a wall at 45 degrees
(every column of it showing its own drawn rows), a side rail's strip ends
cut along the same diagonal (`_strips`: at an inner corner the road drawn
against the strip's head is laid on it, at an outer one its head runs to the
end of the drawing and its foot is cut by the front's chamfer), and the
strip's rows meet their neighbours' edges halfway, through no pixel's
middle; a row of the strip takes all of its own pixels (where the road
widens at an inner corner one row took its columns from a shorter run, a
notch in the rail's side). The corners on the deck's north side, its inner
ones on its south side and a ramp's corners are built square instead
(`_north_corner`, `_inner_north_corner`, `_south_corner`, `_corner`):
there the drawing rounds the rail, and built as drawn a column of the rail
stood up past it like a post, the strip's head rose in a staircase and a
crack stood between them; turning into a strip the front's girder, its
stripes drawn across the turn, was a wall of teeth. The flank going on
straight there is laid at the map's own phase, its posts where the drawn
rail's stand. Only the outer corners on the south side keep their chamfer.
The piers drawn beside a span are boxes standing in the water (`_pier`).

What it stands among
--------------------
Its cells draw more than the bridge: a shore's edge, a tree's leaves over
the rail, the long grass a span shades, a pile's edge in the shadowed
water's colour. Whatever of a cell's drawing is not the bridge's and not
the plain ground the model lays under it is laid on that ground as drawn,
a ground patch of the placement (`lay`): the console lights a patch as the
terrain, so the deck's shadow falls across it (a model's own faces take
the sun's facing alone). Nothing of the drawing is lost. A lawn's curb
that goes on under a span is no part of the bridge either (`_curbs`): it
runs on under the deck as the `curb` component builds it beside, the
stones the drawing shows its own. The houses beside the road are named
(`apart`): a door's reach through the blocked cells (voxel_cells houses)
runs on into the deck's front.

Its hidden faces
----------------
A model is proved alone without the faces the drawing never shows (~depth:
a side's rail and girder, the slab under the deck, the road under the south
rail). They must still stand behind what is drawn, model by model:
`_cut_corners` cuts them at an outer corner's chamfer and under the end of
its drawing, and they stand a hair back from the faces
they meet (INSET, BEHIND) so no pixel is a tie. gen_voxel_buildings
`bridge_check` lays every model in the map with all its faces and its
patches over the ground each placement lays, and holds them to the
drawing pixel for pixel.
"""

import collections
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import voxel_building as vb  # noqa: E402
import voxel_cells as vc  # noqa: E402

# The bridges modelled, by layout: the elevation their deck is walked at,
# and the one of the cells walked both over and under.
# `apart`: cell rectangles drawn against the road that are not of it (the
# gatehouses and the Trick House, modelled on their own). Only these: the
# cells a house's door reaches (voxel_cells houses) run on through the
# blocked cells round it into the deck's south front and a span's side.
BRIDGES = {"LAYOUT_ROUTE110": {"deck": 4, "both": 15,
                               "apart": [(14, 12, 6, 5), (15, 84, 6, 5), (9, 61, 5, 6)]}}

DECK = 40           # px: the deck's road over the ground
RAIL = 13           # px: the rails' height over the deck (the drawing's)
NORTH_RAIL = 14     # px: the north rail's, drawn without the front's top
                    # highlight and over two rows of its kerb
GIRDER = 18         # px: kerb and girder under the deck's south edge (drawn),
                    # and as deep down its other sides, which the drawing
                    # never shows (shallower, the front hung past them at a
                    # corner; a walker crossing under a side now has the beam
                    # at its head for a step, as under the front)
SLAB = 4            # px: the deck's own thickness between its girders
RAMP_RUN = 56       # px: what a ramp runs south while it falls DECK
BLOCK = 1           # cells: a model is this much of the road - one cell, as a
                    # span's cells repeat and each is one model, placed at all
SIDE = 10           # px: the most a side rail's strip is wide (8 or 9 drawn)
RAMP_SIDE = 16      # px: a ramp's rails, drawn coming down beside it
FLAT = 0.25         # px: what is drawn on the ground, over it
INSET = 0.25        # px: a face the drawing never shows, behind the one that
                    # stands where it would meet the drawing's (not level)
BEHIND = 1.0        # px: the deck under the south rail, and its underside,
                    # short of the front (merged, the two run straight on
                    # lines up to half a pixel apart)

LINE = (0x62, 0x62, 0x7b)                   # the outline round the deck
ROAD = {(0xbd, 0xcd, 0xe6), (0xde, 0xe6, 0xee)}
SHADE = (0x9c, 0xb4, 0xde)                  # the road's shading, and the rails' too
ZONE = 4            # cells round the deck searched for its parts


class Bridge:
    """One layout's bridge read off its drawing."""

    def __init__(self, layout_id):
        conf = BRIDGES[layout_id]
        self.id = layout_id
        self.art_layout = vb.LayoutArt(layout_id)
        entry = next(e for e in vc.load_layouts() if e.get("id") == layout_id)
        self.cells = vc.Layout(entry, vc.MapEvents())
        self.w, self.h = self.art_layout.w, self.art_layout.h
        self.deck_elevation, self.both = conf["deck"], conf["both"]
        self.apart = {(x0 + i, y0 + j) for (x0, y0, w, h) in conf.get("apart", ())
                      for j in range(h) for i in range(w)}
        self.deck_cells = {(x, y) for y in range(self.h) for x in range(self.w)
                           if self.cells.elevation(x, y) in (self.deck_elevation, self.both)}
        self._read()

    # -- the drawing ----------------------------------------------------------

    def _deck_zone(self, radius):
        """Cells within the given distance of a deck cell, in map order."""
        near = set()
        for x, y in self.deck_cells:
            for row in range(max(0, y - radius), min(self.h, y + radius + 1)):
                near.update((column, row) for column in
                            range(max(0, x - radius), min(self.w, x + radius + 1)))
        # Preserve the insertion order used to build the zone set.
        return {(x, y) for y in range(self.h) for x in range(self.w) if (x, y) in near}

    def _curbs(self):
        """Rows where a curb (the `curb` component's stones, a lawn's edge)
        goes on under the deck: a run of at most 8 cells, a deck's among
        them, between two cells of the same curb tile. self.curbs: [(y, x0,
        x1, tile)], the cells [x0, x1) of row y; self.curb_px: {pixel: rgb}
        of those cells drawn as the tile draws its stones, joined to the
        stones either side - they stand at the curb's height (block_specs
        lays the whole curb under the bridge, the drawing on it)."""
        import voxel_building_specs as specs
        spec = next(sp for sp in specs.SPECS if sp.get("name") == "curb")
        tiles = set(spec["components"]["tiles"]) - set(spec["components"].get("fill", ()))
        L = self.art_layout
        lawn = L.cell_image(spec["ground"][0]).load()
        self.curbs, self.curb_px = [], {}
        for y in range(self.h):
            x = 0
            while x < self.w:
                t = L.metatile(x, y)
                if t not in tiles:
                    x += 1
                    continue
                x1 = x + 1
                while x1 < self.w and L.metatile(x1, y) not in tiles:
                    x1 += 1
                if x1 >= self.w or L.metatile(x1, y) != t \
                        or not 1 <= x1 - x - 1 <= 8 \
                        or not any((c, y) in self.deck_cells for c in range(x + 1, x1)):
                    x = x1
                    continue
                tile = L.cell_image(t).load()
                same = {}
                for c in range(x + 1, x1):
                    drawn = L.cell_image(L.metatile(c, y)).load()
                    for j in range(16):
                        for i in range(16):
                            if tile[i, j][:3] != lawn[i, j][:3] and drawn[i, j][:3] == tile[i, j][:3]:
                                same[(c * 16 + i, y * 16 + j)] = drawn[i, j][:3]
                # joined to the stones beside the run
                todo = [p for p in same if p[0] in ((x + 1) * 16, x1 * 16 - 1)]
                seen = set()
                while todo:
                    p = todo.pop()
                    if p in seen:
                        continue
                    seen.add(p)
                    todo += [q for q in ((p[0] + 1, p[1]), (p[0] - 1, p[1]), (p[0], p[1] + 1),
                                         (p[0], p[1] - 1)) if q in same and q not in seen]
                self.curb_px.update({p: same[p] for p in seen})
                self.curbs.append((y, x + 1, x1, t))
                x = x1

    def curb_stones(self, t):
        """Curb tile t's stones alone (16x16, clear where it draws the lawn)."""
        import voxel_building_specs as specs
        spec = next(sp for sp in specs.SPECS if sp.get("name") == "curb")
        L = self.art_layout
        tile, lawn = L.cell_image(t).convert("RGBA"), L.cell_image(spec["ground"][0]).load()
        tpx = tile.load()
        for j in range(16):
            for i in range(16):
                if tpx[i, j][:3] == lawn[i, j][:3]:
                    tpx[i, j] = (0, 0, 0, 0)
        return tile

    def _read(self):
        L = self.art_layout
        W, H = self.w * 16, self.h * 16
        # the ground: the blocks of the plain ground drawn away from the
        # road - open water and the land walked on, nothing on its upper layer
        ground = set()
        for blocks in self.plain_grounds().values():
            ground.update(blocks)
        self.ground_blocks = ground
        # and the colours seen through the rails and round the piers: those
        # of the commonest plain water (open, and shadowed) and plain land
        # (a shore's or a ledge's blocks draw a pier's browns too, a sign's
        # its rails' white)
        freq = {"water": collections.Counter(), "floor": collections.Counter()}
        for y in range(self.h):
            for x in range(self.w):
                m = L.metatile(x, y)
                role = self.cells.role_at(x, y)
                if (x, y) in self.deck_cells or role not in freq \
                        or any(i for blk in L.subtiles(m, 1) for _, i in blk):
                    continue
                freq[role][m] += 1
        ground_colours = set()
        for counter in freq.values():
            for m, _ in counter.most_common(2):
                for blk in L.subtiles(m, 0):
                    ground_colours.update(rgb for rgb, _ in blk)
        zone = self._deck_zone(ZONE)
        tile_pixels = {}
        px = {}
        for (x, y) in zone:
            m = L.metatile(x, y)
            samples = tile_pixels.get(m)
            if samples is None:
                samples = tile_pixels[m] = []
                low, high = L.subtiles(m, 0), L.subtiles(m, 1)
                for q in range(4):
                    is_ground = tuple(rgb for rgb, _ in low[q]) in ground
                    for k, (rgb, index) in enumerate(high[q]):
                        if index or not is_ground:
                            u = (q & 1) * 8 + k % 8
                            v = (q >> 1) * 8 + k // 8
                            samples.append((u, v, rgb if index else low[q][k][0]))
            x0, y0 = x * 16, y * 16
            for u, v, rgb in samples:
                px[(x0 + u, y0 + v)] = rgb
        # a lawn's curb going on under the deck: its stones are not the
        # bridge's (_curbs), whatever their colours
        self._curbs()
        for p in self.curb_px:
            if p in px:
                px[p] = None
        # the road: its light colours flooded from the deck cells' middles
        road = set()
        todo = [(x * 16 + 8, y * 16 + 8) for (x, y) in self.deck_cells
                if px.get((x * 16 + 8, y * 16 + 8)) in ROAD]
        while todo:
            p = todo.pop()
            if p in road:
                continue
            road.add(p)
            u, v = p
            for q in ((u + 1, v), (u - 1, v), (u, v + 1), (u, v - 1)):
                if q not in road and px.get(q) in ROAD:
                    todo.append(q)
        # a signpost standing on the deck (the console stands it there,
        # voxel_mesh_builder.c): the deck goes on under it, laid with the
        # plain road where the sign is drawn
        self.under_sign, self.sign_cells = set(), set()
        for y in range(self.h):
            for x in range(self.w):
                if self.cells.role_at(x, y) != "signpost" or not (
                        {(x - 1, y), (x + 1, y)} <= self.deck_cells
                        or {(x, y - 1), (x, y + 1)} <= self.deck_cells):
                    continue
                self.sign_cells.add((x, y))
                for j in range(16):
                    for i in range(16):
                        p = (x * 16 + i, y * 16 + j)
                        road.add(p)
                        if px.get(p) not in ROAD and px.get(p) != SHADE:
                            self.under_sign.add(p)
                            px[p] = None
        # its shading by the rails, a third colour, in stripes between its own
        # (and between the road and the outline at a rail's foot)
        for _ in range(2):
            more = {(u, v) for (u, v), c in px.items() if c == SHADE and (u, v) not in road
                    and (((u + 1, v) in road and (u - 1, v) in road)
                         or ((u, v + 1) in road and (u, v - 1) in road)
                         or any((u + k, v) in road and px.get((u - k, v)) == LINE for k in (-1, 1)))}
            road |= more
        # a rail's post or bar drawn in the road's colours beside it is not
        # road: a pixel or two across
        road -= {(u, v) for (u, v) in road
                 if sum(1 for k in (-1, 1) if (u + k, v) in road) == 0
                 or sum(1 for k in (-1, 1) if (u, v + k) in road) == 0}
        # a block of the road drawn like the ground is still road: holes
        # the flood went round, not reached from outside the road's box
        u0, u1 = min(u for u, _ in road), max(u for u, _ in road)
        v0, v1 = min(v for _, v in road), max(v for _, v in road)
        outside, todo = set(), [(u0 - 1, v) for v in range(v0 - 1, v1 + 2)]
        while todo:
            p = todo.pop()
            u, v = p
            if p in outside or p in road or not (u0 - 1 <= u <= u1 + 1 and v0 - 1 <= v <= v1 + 1):
                continue
            outside.add(p)
            todo += [(u + 1, v), (u - 1, v), (u, v + 1), (u, v - 1)]
        for v in range(v0, v1 + 1):
            for u in range(u0, u1 + 1):
                if (u, v) not in road and (u, v) not in outside:
                    road.add((u, v))
                    px.setdefault((u, v), None)
        # its colours: those drawn on the deck's cells and on the cells under
        # its south side (its rails, girders and piers) - not a tree's
        # by it, nor a gatehouse's roof (a building of its own)
        palette = set()
        for (x, y) in self.deck_cells:
            for dy in range(0, 5):
                c = (x, y + dy)
                if dy and (c in self.deck_cells or c[1] >= self.h
                           or self.cells.role_at(*c) in ("tree", "wall", "signpost")):
                    continue
                for j in range(16):
                    for i in range(16):
                        rgb = px.get((c[0] * 16 + i, c[1] * 16 + j))
                        if rgb is not None and rgb not in ground_colours and not _green(rgb):
                            palette.add(rgb)
        houses = self.apart
        # and the road's own tileset: a shore or a fence of the primary
        # tileset beside a pier is drawn in the piers' browns and greys
        primary = {(x, y) for (x, y) in zone if L.metatile(x, y) < vb.NUM_PRIMARY}
        for (u, v) in list(px):
            if px[(u, v)] not in palette or (u // 16, v // 16) in houses \
                    or (u // 16, v // 16) in primary:
                if (u, v) not in road:
                    px[(u, v)] = None
        # the bridge: what is drawn touching the road, less the colours of
        # the ground seen through its rails and round its piers
        keep, todo = set(), list(road)
        while todo:
            p = todo.pop()
            if p in keep:
                continue
            keep.add(p)
            u, v = p
            for q in ((u + 1, v), (u - 1, v), (u, v + 1), (u, v - 1)):
                c = px.get(q)
                if q not in keep and c is not None and (q in road or c not in ground_colours):
                    todo.append(q)
        self.road = road
        self.pixels = {p: px[p] for p in keep if px.get(p) is not None}
        self.art = vb.Image.new("RGBA", (W, H), (0, 0, 0, 0))
        apx = self.art.load()
        for (u, v), c in self.pixels.items():
            apx[u, v] = c + (255,)
        self._ramps()
        self._strips()

    def _ramps(self):
        """Runs of road that end on the land, not on a rail: a ramp, from
        the row its stripes turn across to its foot. {column: (top, foot)}."""
        cols = collections.defaultdict(list)
        for (u, v) in self.road:
            cols[u].append(v)
        self.ramp = {}
        apx = self.art.load()
        for u, vs in cols.items():
            for (a, b) in _runs(sorted(vs)):
                below = self.pixels.get((u, b + 1))
                if below is not None and (u, b + 1) not in self.road:
                    continue    # a rail or a front stands under it
                self.ramp[(u, a)] = b
        # where the stripes turn across. A
        # ramp is a road's width, and its foot is on walkable land.
        by_foot = collections.defaultdict(list)
        for (u, a), b in self.ramp.items():
            by_foot[b].append(u)
        self.ramps = []
        for foot, us in by_foot.items():
            for (s0, s1) in _runs(sorted(us)):
                s1 += 1
                fx, fy = (s0 + s1) // 2 // 16, (foot + 1) // 16
                if s1 - s0 < 16 or fy >= self.h or self.cells.blocked(fx, fy) \
                        or self.cells.role_at(fx, fy) == "water":
                    continue
                inner = range(s0 + 2, s1 - 2)

                def across(v):
                    return all((u, v) in self.road for u in inner) and \
                        len({apx[u, v][:3] for u in inner}) == 1

                # the road's stripes run down every row; the ramp's every
                # other row is one colour across
                top = foot
                while all((u, top - 1) in self.road for u in inner) and \
                        (across(top - 1) or across(top - 2)):
                    top -= 1
                self.ramps.append((s0, s1, top, foot + 1))
        # a ramp's own columns only: the rest of the runs ending on nothing
        # are a deck's edges
        self.ramp = {k: b for k, b in self.ramp.items() if self.ramp_at(k[0], b) is not None}

    # -- heights ----------------------------------------------------------------

    def ramp_at(self, u, v):
        """The ramp column u comes down by, near row v."""
        return next((r for r in self.ramps if r[0] <= u < r[1]
                     and self.ramp_head(r) - 4 * 16 <= v < r[3] + 4 * 16), None)

    def _strips(self):
        """The side rails' strips, every pixel of them: beside a road whose
        edge runs straight down, the columns drawn one colour down its
        length (a chamfer's rail and girder cross them diagonally); and up
        each such column to the road over it, where the road widens over
        the strip's head - an inner corner, the strip running on to the
        chamfer that turns the front into it."""
        rows = collections.defaultdict(list)
        for (u, v) in self.road:
            rows[v].append(u)
        straight = collections.defaultdict(list)     # (step, edge): [rows]
        for v, us in rows.items():
            for (l, r) in _runs(sorted(us)):
                straight[(-1, l)].append(v)
                straight[(1, r)].append(v)
        self.strip_px, self.chamfer_cols, self.head_rows = set(), {}, {}
        self.feet = []          # (side, the road's edge, last row, {column: rows under it})
        self.inner_heads = []   # (side, (s0, s1), first straight row, {column: head row})
        self.strip_cols = {}    # (side, the road's edge, row): the strip's columns
        for (step, edge), vs in straight.items():
            for (v0, v1) in _runs(sorted(vs)):
                if v1 - v0 < 7 or any(r[0] - RAMP_SIDE <= edge < r[1] + RAMP_SIDE
                                      and r[2] - 16 <= v1 and v0 < r[3] for r in self.ramps):
                    continue
                cols = self._strip_columns(edge, step, v0, v1)
                if len(cols) < 6 and v1 - v0 >= 16:
                    # a pier's head drawn over the strip's foot, where it
                    # meets a wider deck's north rail, hides its columns
                    # there: read from its first cell's worth of rows
                    first = self._strip_columns(edge, step, v0, v0 + 15)
                    if len(first) >= 6:
                        cols = first
                side = "e" if step > 0 else "w"
                head, outer = {}, {}
                for v in range(v0, v1 + 1):
                    self.strip_cols[(side, edge, v)] = cols
                for c in cols:
                    self.strip_px.update((c, v) for v in range(v0, v1 + 1) if (c, v) in self.pixels)
                    # up to the road over it, when there is one near (an
                    # inner corner), or to the end of the drawing (an outer
                    # one: the chamfer turning it into the north rail)
                    w = v0 - 1
                    while (c, w) in self.pixels and (c, w) not in self.road \
                            and w > v0 - RAIL - GIRDER - 8:
                        w -= 1
                    if (c, w) in self.road:
                        self.strip_px.update((c, v) for v in range(w + 1, v0))
                        head[c] = w + 1
                    elif (c, w) not in self.pixels:
                        outer[c] = w + 1
                # and down from its last row, where the front turns into it
                # at an outer corner: the chamfer's rail and girder, drawn
                # down each of its columns to the end of the drawing
                foot = {}
                for c in cols:
                    w = v1 + 1
                    while (c, w) in self.pixels and (c, w) not in self.road and w < v1 + RAIL + GIRDER + 16:
                        w += 1
                    if (c, w) not in self.pixels and (c, w) not in self.road and w > v1 + 1:
                        foot[c] = (v1 + 1, w)
                if len(cols) >= 6 and len(foot) >= len(cols) - 1:
                    self.feet.append((side, edge, v1, foot))
                if cols and len(head) == len(cols) and min(head.values()) < v0:
                    self._chamfer_head(step, cols, head, v0)
                    if len(cols) >= 6:
                        self.inner_heads.append((side, (min(cols), max(cols) + 1), v0, head))
                elif len(cols) >= 6 and len(outer) == len(cols) and min(outer.values()) < v0:
                    for c, w in outer.items():
                        self.strip_px.update((c, v) for v in range(w, v0))
                    for v in range(min(outer.values()), v0):
                        self.head_rows.setdefault((side, v), []).append([c for c in cols if outer[c] <= v])

    def _strip_columns(self, edge, step, v0, v1):
        """The columns out from a road's edge drawn one colour down rows
        [v0, v1] (most of them), to the strip's outer outline."""
        cols = []
        for k in range(1, SIDE + 1):
            c = edge + step * k
            drawn = [self.pixels.get((c, v)) for v in range(v0, v1 + 1)]
            top = collections.Counter(drawn).most_common(1)[0]
            if top[0] is None or top[1] < 0.6 * len(drawn):
                break
            cols.append(c)
            if k >= 6 and top[0] == LINE:
                break       # its outer outline (a pier's beside it)
        return cols

    def _chamfer_head(self, step, cols, head, v0):
        """An inner corner: the strip's head cut square to the chamfer
        that turns the front into it, at 45 degrees from the strip's outer
        side - the road it is drawn against there laid on the strip's top
        (a pixel or two a column), so its end is one straight diagonal.
        The row the cut crosses each column's sides at, for the road the
        head stands against, in `chamfer_cols`."""
        outer = max(cols) if step > 0 else min(cols)
        r0 = min(head[c] - abs(c - outer) for c in cols)
        for c in cols:
            for v in range(r0 + abs(c - outer), head[c]):
                if (c, v) in self.road and (c, v) in self.pixels:
                    self.road.discard((c, v))
                    self.strip_px.add((c, v))
        # the edge between strip and road: x = outer + 1/2 -+ (t - r0) at
        # row t of the drawing, through no pixel's middle
        for c in cols:
            self.chamfer_cols[(c, r0 + abs(c - outer))] = tuple(
                r0 - step * (x - outer - 0.5) for x in (c, c + 1))

    def beside_road(self, u, v):
        """Is (u, v) a side rail's strip (`_strips`)."""
        return (u, v) in self.strip_px

    def beside_ramp(self, u, v):
        """Is (u, v) drawn beside a ramp, in its rows: its slot's wall stands
        there (_corner)."""
        return any(u0 - RAMP_SIDE <= u < u1 + RAMP_SIDE and top <= v < foot
                   for (u0, u1, top, foot) in self.ramps)

    @staticmethod
    def corner_rows(ramp):
        """The rows of a ramp's side columns built by `_corner`: the cell
        row over its drawn top (where the drawing turns the deck's front
        rail down beside it) and the ramp's own."""
        return ramp[2] - 16, ramp[3]

    @staticmethod
    def ramp_head(ramp):
        """The art row a ramp begins falling at: RAMP_RUN + DECK rows over
        its drawn foot (up in the deck's road, over its drawn top)."""
        return min(ramp[2], ramp[3] - RAMP_RUN - DECK)

    def ramp_point(self, ramp, v):
        """(z, h) of a ramp's surface where its art row v is laid: from its
        head (z its row + DECK, at the deck) to its drawn foot (on the
        ground), every row where the 45-degree view sees it."""
        head, foot = self.ramp_head(ramp), ramp[3]
        s = min(1.0, max(0.0, (v - head) / float(foot - head)))
        return head + DECK + s * (foot - head - DECK), DECK * (1.0 - s)

    def lift_at(self, u, v):
        """The surface's height, pixels, where the GBA draws (u, v): what a
        walker drawn there stands on - the 45-degree rule, so a walker on a
        ramp stays where the GBA draws it."""
        ramp = self.ramp_at(u, v)
        if ramp is not None and v >= self.ramp_head(ramp):
            return self.ramp_point(ramp, v)[1]
        return DECK

    def stretched(self):
        """The pixels the model does not draw as the GBA does: a ramp's drawn
        rows and its corners (`_corner`), the deck's corners on its north
        side (`_north_corner`, `_inner_north_corner`) and its inner ones on
        its south side (`_south_corner`)."""
        out = set()
        for ramp in self.ramps:
            u0, u1, top, foot = ramp
            for v in range(top, foot):
                for u in range(u0, u1):
                    out.add((u, v))
            r0, r1 = self.corner_rows(ramp)
            for v in range(r0, r1):
                for u in list(range(u0 - RAMP_SIDE, u0)) + list(range(u1, u1 + RAMP_SIDE)):
                    out.add((u, v))
        if not hasattr(self, "corner_regions"):
            self.quads()
        for (xa, xb, r0, r1) in self.corner_regions:
            out.update((u, v) for v in range(r0, r1) for u in range(xa, xb))
        return out

    # -- geometry -------------------------------------------------------------

    def quads(self):
        """Every face as (points, uvs, tag, art pixel it is filed under):
        points (x, y, z) in pixels of the map, uvs in pixels of its art."""
        out = []
        road, pix = self.road, self.pixels
        cols = collections.defaultdict(list)
        for (u, v) in road:
            cols[u].append(v)
        claimed = set(road)
        self.patch_px = {}
        darkest_by_cell = {}
        # the curb's stones drawn in the deck's cells: the curb laid under
        # the bridge (block_specs) draws them
        for p, rgb in self.curb_px.items():
            self.art.putpixel(p, tuple(rgb) + (255,))
            claimed.add(p)

        def remove_pixel(p):
            del pix[p]
            # A cached minimum stays valid unless its own pixel was removed.
            cell = (p[0] // 16 * 16, p[1] // 16 * 16)
            if darkest_by_cell.get(cell) == p:
                del darkest_by_cell[cell]

        def lay(p, rgb=None):
            """Pixel p laid on the ground as drawn: a patch of the placement's
            ground (block_specs), lit as the terrain round it is - the
            console casts no shadow on a model's own faces, and the deck's
            shadow falls here."""
            if rgb is None:
                rgb = self.art.getpixel(p)
                remove_pixel(p)
                self.art.putpixel(p, (0, 0, 0, 0))
            self.patch_px[p] = rgb
            claimed.add(p)

        def texel(at):
            """An opaque texel of the cell `at` is in: nearest down its
            column, else anywhere in the cell (alpha-tested, a clear one
            would leave a hole)."""
            u, v = at
            c0, r0 = u // 16 * 16, v // 16 * 16
            for p in [(u, w) for w in range(v, r0 + 16)] + [(u, w) for w in range(r0, v)] + \
                     [(i, j) for j in range(r0, r0 + 16) for i in range(c0, c0 + 16)]:
                if p in pix:
                    return p
            return at

        def darkest(at):
            """The cell's darkest texel: the girder's shade, for what is
            under the deck."""
            cell = c0, r0 = int(at[0]) // 16 * 16, int(at[1]) // 16 * 16
            if cell not in darkest_by_cell:
                best = min(((sum(rgb), (i, j))
                            for j in range(r0, r0 + 16) for i in range(c0, c0 + 16)
                            if (rgb := pix.get((i, j))) is not None), default=None)
                darkest_by_cell[cell] = best[1] if best is not None else None
            chosen = darkest_by_cell[cell]
            return chosen if chosen is not None else texel(at)

        def put(pts, uvs, tag, at):
            if tag.endswith("~clamp"):
                # never seen at 45 degrees: one texel of its own cell
                if tag.startswith(("deck.under", "deck.girder")):
                    t = darkest(at)
                else:
                    t = texel(at)
                uvs = [(t[0] + 0.5, t[1] + 0.5)] * len(pts)
            out.append((pts, uvs, tag, at))

        def upright(u, z, v0, v1, tag):
            """Art rows [v0, v1) of column u on the face standing at z - or,
            z = (left, right), standing from depth `left` at the column's
            west side to `right` at its east side: a diagonal wall, every
            column of it showing its own drawn rows. A row whose middle the
            wall would stand at the ground or under it (a chamfer's foot
            reaching the water) is laid on the ground."""
            zl, zr = z if isinstance(z, tuple) else (z, z)
            ground = [v for v in range(v0, v1) if (zl + zr) / 2.0 - (v + 0.5) <= 1e-6]
            if ground:
                v1 = min(v1, ground[0])
            for v in ground:
                if (u, v) in pix:
                    lay((u, v))
            for (a, b) in _split(v0, v1):
                put([(u, zl - b, zl), (u + 1, zr - b, zr), (u + 1, zr - a, zr), (u, zl - a, zl)],
                    [(u, b), (u + 1, b), (u + 1, a), (u, a)], tag, (u, a))

        # The depth each column's south and north edge stands at, before any
        # face is laid: a chamfer is drawn as a staircase of columns, and its
        # wall is the plane through them (between two neighbours' depths at
        # every column's side, where they are a step of at most 2 apart).
        south, north = collections.defaultdict(list), collections.defaultdict(list)
        for u, vs in cols.items():
            for (a, b) in _runs(sorted(vs)):
                north[u].append(a + DECK)
                kerb = next((v for v in range(b + 1, b + 1 + RAIL + 4)
                             if pix.get((u, v)) == LINE), b + 1 + RAIL)
                south[u].append(kerb + DECK)

        def sides(edges, u, z):
            """(left, right) depths of column u's edge at z, met halfway with
            its neighbours' where they step by at most 2 pixels."""
            out = []
            for nb in (u - 1, u + 1):
                near = [w for w in edges.get(nb, ()) if abs(w - z) <= 2]
                out.append((z + min(near, key=lambda w: abs(w - z))) / 2.0 if near else float(z))
            return tuple(out)

        for u, vs in cols.items():
            for (a, b) in _runs(sorted(vs)):
                b += 1      # [a, b)
                ramp = self.ramp_at(u, b - 1)
                sloped = (u, a) in self.ramp and ramp is not None
                flat = max(a, self.ramp_head(ramp)) if sloped else b
                # the deck's road, flat (the plain road under a signpost)
                for (p0, q0) in _split(a, flat):
                    for (p, q) in _runs([v for v in range(p0, q0) if (u, v) not in self.under_sign]):
                        q += 1
                        put([(u, DECK, p + DECK), (u + 1, DECK, p + DECK),
                             (u + 1, DECK, q + DECK), (u, DECK, q + DECK)],
                            [(u, p), (u + 1, p), (u + 1, q), (u, q)], "deck.top", (u, p))
                    for (p, q) in _runs([v for v in range(p0, q0) if (u, v) in self.under_sign]):
                        pts = [(u, DECK, p + DECK), (u + 1, DECK, p + DECK),
                               (u + 1, DECK, q + 1 + DECK), (u, DECK, q + 1 + DECK)]
                        put(pts, [(x, z - y) for (x, y, z) in pts], "deck.top_under_sign~depth~road", (u, p))
                # north: the rail over the road, standing on its edge
                n0 = a
                while (u, n0 - 1) in pix and (u, n0 - 1) not in claimed and a - n0 < RAIL + SIDE:
                    n0 -= 1
                zn = a + DECK
                znl, znr = sides(north, u, zn)
                if n0 < a:
                    upright(u, (znl, znr), n0, a, "deck.rail_n")
                    claimed.update((u, v) for v in range(n0, a))
                # the girder under it, which the drawing never shows
                put([(u + 1, DECK - GIRDER, znr), (u, DECK - GIRDER, znl), (u, DECK, znl),
                     (u + 1, DECK, znr)], [(u, a)] * 4, "deck.flank_girdern_n~depth", (u, a))
                if sloped:
                    # the ramp: its rows from its head in the deck down to
                    # its foot (the slot's walls either side, _corner)
                    for (p, q) in _split(flat, b):
                        zp, hp = self.ramp_point(ramp, p)
                        zq, hq = self.ramp_point(ramp, q)
                        put([(u, hp, zp), (u + 1, hp, zp), (u + 1, hq, zq), (u, hq, zq)],
                            [(u, p), (u + 1, p), (u + 1, q), (u, q)], "deck.ramp~proj", (u, p))
                    zs = flat + DECK
                    zsl = zsr = float(zs)
                else:
                    # south: the rail, the kerb, the girder and the piers, one
                    # face standing on the deck's edge; the kerb is the outline
                    # the kerb: the outline a rail's height under the road
                    kerb = next((v for v in range(b, b + RAIL + 4)
                                 if pix.get((u, v)) == LINE), b + RAIL)
                    zs = kerb + DECK
                    zsl, zsr = sides(south, u, zs)
                    # down to the piers' feet, through the gaps of a truss;
                    # not a ramp's side rails, which come down beside it, nor
                    # a span's side rail going on south beside its road
                    mine = [v for v in range(b, zs) if (u, v) in pix and (u, v) not in claimed
                            and not self.beside_ramp(u, v) and not self.beside_road(u, v)]
                    if not mine:
                        # no front under this column: the road meets a span's
                        # side rail going on south (an inner corner), whose
                        # strip stands on the deck from a rail's height on
                        kerb = b + RAIL
                        zs = kerb + DECK
                        zsl = zsr = float(zs)
                        if (u, b) in self.chamfer_cols:
                            # the strip's head cut across at 45 degrees
                            zsl, zsr = (t + RAIL + DECK for t in self.chamfer_cols[(u, b)])
                    for (p, q) in _runs(mine):
                        upright(u, (zsl, zsr), p, q + 1, "deck.front")
                    claimed.update((u, v) for v in mine)
                    # the road goes on under the rail to the edge
                    # (laid with the plain road, as if the rail were not there)
                    for (p, q) in _split(b, kerb):
                        # (a hair behind the front, not level with it)
                        far = (q + DECK, q + DECK) if q < kerb else (zsl - BEHIND, zsr - BEHIND)
                        pts = [(u, DECK, p + DECK), (u + 1, DECK, p + DECK),
                               (u + 1, DECK, far[1]), (u, DECK, far[0])]
                        put(pts, [(x, z - y) for (x, y, z) in pts],
                            "deck.top_under_rail~depth~road", (u, p))
                # the slab's underside, cell by cell of the rows over it, from
                # the north edge to the south one
                low = DECK - SLAB
                for (p, q) in _split(a, zs - DECK):
                    near = (znl, znr) if p == a else (p + DECK, p + DECK)
                    far = (zsl - BEHIND, zsr - BEHIND) if q == zs - DECK else (q + DECK, q + DECK)
                    put([(u, low, far[0]), (u + 1, low, far[1]), (u + 1, low, near[1]), (u, low, near[0])],
                        [(u, p), (u + 1, p), (u + 1, p + 1), (u, p + 1)], "deck.under~depth~clamp", (u, p))
        # west and east: the side rails' strips, seen from above at their
        # top; the rail's two faces and the girder under it dressed with the
        # south front's own rail and girder (flank_faces); beside a ramp,
        # nothing: its slot's walls (_corner)
        rows = collections.defaultdict(list)
        for (u, v) in road:
            rows[v].append(u)

        def flank(kind, facing, x, y0, y1, z0, z1, at):
            """A side's face from depth z0 to z1 at x - or x = (at z0, at z1);
            a hair inside what stands over it, so at 45 degrees its top edge
            is under that one's (a strip's sides merge straighter than the
            strip's own rows run)."""
            xa, xb = x if isinstance(x, tuple) else (x, x)
            d = INSET if facing == "w" else -INSET
            xa, xb = xa + d, xb + d
            # and as far under: its top edge is not in the plane of what
            # stands over it (the deck's road at an inner corner's chamfer)
            y0, y1 = y0 - INSET, y1 - INSET
            put([(xa, y0, z0), (xb, y0, z1), (xb, y1, z1), (xa, y1, z0)], [at] * 4,
                "deck.flank_%s_%s~depth" % (kind, facing), at)

        # an outer corner's chamfer: the front turned at 45 degrees from the
        # deck's south edge to the strip's outer side, the strip's last rows
        # cut along it - every column of it showing its own drawn rows from
        # the rail's top on the chamfer down
        cuts = {}       # by foot: what _south_outer_corner leaves chamfered
        for i, (side, edge, last, foot) in enumerate(self.feet):
            fronts = south.get(edge, ())
            if not fronts:
                continue
            zs = min(fronts, key=lambda z: abs(z - (last + 1 + RAIL + DECK)))
            z_edge = sides(south, edge, zs)[0 if side == "w" else 1]
            bottom = 0.0       # the highest the chamfer's drawing ends at
            for c, (a, b) in foot.items():
                k = abs(edge - c)
                zl, zr = (z_edge - k, z_edge - k + 1) if side == "w" else (z_edge - k + 1, z_edge - k)
                first = int(math.ceil((zl + zr) / 2.0 - DECK - RAIL - 0.5))
                rows_ = [v for v in range(min(first, a), b) if v >= first
                         and (c, v) in pix and (c, v) not in claimed]
                for (p, q) in _runs(rows_):
                    upright(c, (zl, zr), p, q + 1, "deck.front")
                claimed.update((c, v) for v in rows_)
                if rows_:
                    bottom = max(bottom, max(zl, zr) - max(rows_) - 1)
            # the plane it stands on (z = x + C west, z = -x + C east), and
            # under the drawing's end nothing of the sides behind it
            if side == "w":
                cuts[i] = (-1.0, z_edge - edge, bottom, min(foot) - SIDE, edge, z_edge)
            else:
                cuts[i] = (1.0, z_edge + edge + 1, bottom, edge + 1, max(foot) + 1 + SIDE, z_edge)
        # the strips row by row, gathered first - (s0, s1, inner side's x,
        # outer side's x, art pixel) by (side, row) - and laid once every
        # row's are known, their sides met with the next rows' (smooth)
        strips = collections.defaultdict(list)
        # the heads of the strips at an outer corner (_strips): drawn up to
        # the chamfer that turns them into the north rail, away from the road
        for (side, v), heads in sorted(self.head_rows.items()):
            for cs in heads:
                strip = [c for c in cs if (c, v) in pix and (c, v) not in claimed]
                if not strip:
                    continue
                claimed.update((c, v) for c in strip)
                s0, s1 = min(strip), max(strip) + 1
                strips[(side, v)].append((s0, s1) + ((s1, s0) if side == "w" else (s0, s1))
                                         + ((strip[0], v),))
        for v, us in rows.items():
            for (l, r) in _runs(sorted(us)):
                r += 1
                for side, edge, step in (("w", l, -1), ("e", r - 1, 1)):
                    out_dir, in_dir = (side, "e" if side == "w" else "w")
                    xi = l if side == "w" else r
                    ramp = next((rp for rp in self.ramps if rp[0] - RAMP_SIDE <= edge < rp[1] + RAMP_SIDE
                                 and rp[2] <= v < rp[3]), None)
                    if ramp is not None:
                        # the rails drawn coming down beside it at 45 degrees
                        # (and the road widened over their head at the corner)
                        # are the slot's walls (_corner): claimed, not drawn
                        claimed.update((edge + step * k, v) for k in range(1, RAMP_SIDE + 1)
                                       if (edge + step * k, v) in pix)
                        continue
                    # the strip, through what of the ground shows in it
                    # (the grass a crossing path draws under the rail) - its
                    # own columns beside a span, not a pier drawn beside them
                    cols = self.strip_cols.get((side, edge, v),
                                               [edge + step * k for k in range(1, SIDE + 1)])
                    strip = [c for c in cols if (c, v) in pix and (c, v) not in claimed]
                    # and on out over the strip's own pixels: a row where the
                    # road widens at an inner corner takes its columns from a
                    # shorter run, and stopped short of its neighbours (a
                    # notch in the rail's side, a crack in the game's view)
                    while strip:
                        c = (min(strip) if side == "w" else max(strip)) + step
                        if abs(c - edge) > SIDE or (c, v) not in self.strip_px \
                                or (c, v) not in pix or (c, v) in claimed:
                            break
                        strip.append(c)
                    if not strip:
                        # no rail drawn here (a chamfer's, standing on the
                        # front or the north edge): the deck's side
                        flank("girder", out_dir, xi, DECK - GIRDER, DECK, v + DECK, v + DECK + 1,
                              (edge, v))
                        continue
                    claimed.update((w, v) for w in strip)
                    s0, s1 = min(strip), max(strip) + 1
                    strips[(side, v)].append((s0, s1, xi, s0 if side == "w" else s1, (strip[0], v)))

        def smooth(side, v, x, k):
            """Side k (2 inner, 3 outer) of a strip's row at x, at the row's
            top and foot: halfway to the rows over and under it where they
            step by a pixel the same way - a corner drawn at 45 degrees, cut
            straight across, through no pixel's middle."""
            near = []
            for nv in (v - 1, v + 1):
                got = [e[k] for e in strips.get((side, nv), ()) if abs(e[k] - x) <= 1]
                near.append(got[0] - x if got else 0)
            up, down = near[0], -near[1]
            if up * down < 0:
                up = down = 0       # a pixel sticking out: as drawn
            return (x + up / 2.0, x - down / 2.0)

        h, top = DECK, DECK + RAIL
        for (side, v), entries in strips.items():
            out_dir, in_dir = (side, "e" if side == "w" else "w")
            zt, zt1 = v + top, v + top + 1
            for (s0, s1, xi, xo, at) in entries:
                inner, outer = smooth(side, v, xi, 2), smooth(side, v, xo, 3)
                touching = xi == (s1 if side == "w" else s0)
                west, east = (outer, inner if touching else (s1, s1)) if side == "w" else \
                    (inner if touching else (s0, s0), outer)
                for (c0, c1) in _split(s0, s1):
                    cell = c0 // 16 * 16
                    xw = [min(cell + 16, max(cell, x)) for x in west] if c0 == s0 else [c0, c0]
                    xe = [min(cell + 16, max(cell, x)) for x in east] if c1 == s1 else [c1, c1]
                    pts = [(xw[0], top, zt), (xe[0], top, zt), (xe[1], top, zt1), (xw[1], top, zt1)]
                    put(pts, [(x, z - y) for (x, y, z) in pts], "deck.rail_side", (c0, v))
                flank("rail", out_dir, outer, h, top, zt, zt1, at)
                flank("rail", in_dir, inner, h, top, zt, zt1, at)
                flank("girder", out_dir, outer, h - GIRDER, h, zt, zt1, at)
                flank("under", in_dir, inner, h - GIRDER, h - SLAB, zt, zt1, at)
                quad = [(inner[0], h - GIRDER, zt), (outer[0], h - GIRDER, zt),
                        (outer[1], h - GIRDER, zt1), (inner[1], h - GIRDER, zt1)]
                put(quad[::-1] if side == "w" else quad, [at] * 4, "deck.under~depth~clamp", at)
        # whatever else of it is drawn, touching what is placed: the piers
        # beside a span or the deck's end - drawn beside the road, as the GBA
        # sees them, and fence cells by the collision (theirs, not a fence's)
        def by_deck(p):
            """A pier: beside the road in its rows, or under it."""
            u, v = p
            return any((u + k, v) in road for k in range(-16, 17)) or \
                any((u, v - k) in road for k in range(1, DECK + RAIL + GIRDER + 8))

        left = {p for p in pix if p not in claimed and by_deck(p)}
        keep, todo = set(), [p for p in left if any(
            (p[0] + i, p[1] + j) in claimed for i in (-1, 0, 1) for j in (-1, 0, 1))]
        while todo:
            p = todo.pop()
            if p in keep:
                continue
            keep.add(p)
            # (and down a pier's pile, however far from the road)
            todo += [(p[0] + i, p[1] + j) for i in (-1, 0, 1) for j in (-1, 0, 1)
                     if ((p[0] + i, p[1] + j) in left or (j == 1 and (p[0] + i, p[1] + j) in pix
                                                           and (p[0] + i, p[1] + j) not in claimed))
                     and (p[0] + i, p[1] + j) not in keep]
        apx = self.art.load()

        def drop(ps):
            for p in ps:
                remove_pixel(p)
                apx[p] = (0, 0, 0, 0)

        # what is not the road's in the cells the road's models stand on
        # (the edge of a shore, a tree's shadow on the water): laid on the
        # ground as drawn - the map paints those cells with their plain
        # ground; elsewhere the map paints it
        owned = {(u // 16, v // 16) for u, v in pix if (u, v) in claimed or (u, v) in keep}
        for p in [p for p in pix if p not in claimed and p not in keep]:
            if (p[0] // 16, p[1] // 16) not in owned:
                drop([p])
                continue
            lay(p)
        # and what those cells draw that is no part of the bridge, in the
        # colours it is not read in (the leaves of a tree beside a span, the
        # long grass by a rail, a pile's edge in the shadowed water's colour):
        # the map lays its plain ground there, so whatever of the drawing
        # that ground does not show is laid on it too
        L = self.art_layout
        crossed = {(x, y) for (y, x0, x1, _) in self.curbs for x in range(x0, x1)}
        for (x, y) in sorted((owned | crossed) - self.sign_cells):
            drawn = L.cell_image(L.metatile(x, y)).load()
            plain = L.cell_image(self.ground_for(x, y)).load()
            for j in range(16):
                for i in range(16):
                    p = (x * 16 + i, y * 16 + j)
                    if p in claimed or p in pix or all(
                            a >> 3 == b >> 3 for a, b in zip(drawn[i, j][:3], plain[i, j][:3])):
                        continue
                    lay(p, drawn[i, j])
        rest = {p for p in pix if p not in claimed}
        for comp in _components(rest):
            us, vs = [p[0] for p in comp], [p[1] for p in comp]
            width, height = max(us) - min(us) + 1, max(vs) - min(vs) + 1
            if 4 <= width <= 12 and height >= 20:
                self._pier(comp, put, upright, texel)
                continue
            cols_ = collections.defaultdict(list)
            for (u, v) in comp:
                cols_[u].append(v)
            for u, vs_ in cols_.items():
                for (a, b) in _runs(sorted(vs_)):
                    if b - a < RAIL - 3 and all(any((u + k, v) in road for k in range(-16, 17))
                                                for v in range(a, b + 1)):
                        # a chamfer's rail, past the strip beside the road:
                        # its top, as the strips are, not a post in the water
                        for v in range(a, b + 1):
                            top = DECK + RAIL
                            put([(u, top, v + top), (u + 1, top, v + top), (u + 1, top, v + top + 1),
                                 (u, top, v + top + 1)], [(u, v), (u + 1, v), (u + 1, v + 1), (u, v + 1)],
                                "deck.rail_side", (u, v))
                        continue
                    # anything else (a truss's foot): upright on the ground
                    # at the foot of its column
                    upright(u, b + 1, a, b + 1, "deck.post")
        self.corner_regions, self._unmerged = [], []
        generic = {id(f): f for f in out}       # (held: an id is not reused)
        for ramp in self.ramps:
            self._corner(ramp, out, put, south)
        for head in self._heads():
            self._north_corner(head, out, put, strips, north, flank)
        for foot in self._feet_north():
            self._inner_north_corner(foot, out, put, north, flank)
        for head in self.inner_heads:
            self._south_corner(head, out, put, south, flank, generic)
        for i, foot in enumerate(self.feet):
            if self._south_outer_corner(foot, out, put, south, flank, generic):
                cuts.pop(i, None)
        faces, self.slanted = _merge(out)
        faces += self._unmerged
        return _cut_corners(_tile_flanks(faces), list(cuts.values()))

    def _corner(self, ramp, out, put, south):
        """A ramp's corners with the deck and its slot through it, square:
        what the generic build laid in its side columns (RAMP_SIDE wide)
        over `corner_rows` - the drawn curve of the front's rail turning
        down beside the ramp, the road's staircase against it - is taken
        out, and each side gets the deck's road over it, the front's rail
        and girder (the flank) straight on to the slot at the depth of the
        front beside it, and under them a wall from the ground to the deck
        down the whole slot: its face on the ramp from the ramp up, its top
        under the road. Past the front the ramp's last pixels fall to the
        ground between two low faces (`ramp_point`)."""
        u0, u1, top, foot = ramp
        r0, r1 = self.corner_rows(ramp)
        sides = ((u0 - RAMP_SIDE, u0, u0 - RAMP_SIDE - 1, "e"), (u1, u1 + RAMP_SIDE, u1 + RAMP_SIDE, "w"))
        cols = {u for xa, xb, _, _ in sides for u in range(xa, xb)}
        out[:] = [f for f in out if not (int(f[3][0]) in cols and r0 <= int(f[3][1]) < r1)]
        # (a corner built beside it stops short of its side columns)
        self.corner_regions += [(xa, xb, r0, r1) for xa, xb, _, _ in sides]
        head = self.ramp_head(ramp)
        for xa, xb, nb, inner in sides:
            # the depth of the deck's front beside the corner
            fronts = south.get(nb, ())
            zf = min(fronts, key=lambda z: abs(z - (top + DECK))) if fronts else top + DECK
            xi = xb if inner == "e" else xa       # the wall's face on the ramp
            xo = xa if inner == "e" else xb
            at = (xa, r0)
            # the deck's road over the corner, and its slab
            self._road(put, xa, xb, r0 + DECK, zf - BEHIND)
            low = DECK - SLAB
            put([(xa, low, zf - BEHIND), (xb, low, zf - BEHIND), (xb, low, r0 + DECK), (xa, low, r0 + DECK)],
                [at] * 4, "deck.under~depth~clamp", at)
            # the front: rail and girder, straight on to the slot
            put([(xa, DECK - GIRDER, zf), (xb, DECK - GIRDER, zf), (xb, DECK + RAIL, zf), (xa, DECK + RAIL, zf)],
                [at] * 4, "deck.flank_rail_s~depth", at)
            # the wall down the slot, under the deck and the front
            d = -INSET if inner == "e" else INSET
            for (p, q) in _split(head, foot):
                (z0, h0), (z1, h1) = self.ramp_point(ramp, p), self.ramp_point(ramp, q)
                if z0 >= zf:
                    break
                if z1 > zf:     # the slot's mouth: up to the front
                    h1 = h0 + (h1 - h0) * (zf - z0) / (z1 - z0)
                    z1 = zf
                wat = (xa, p)
                put([(xi + d, h0, z0), (xi + d, h1, z1), (xi + d, DECK, z1), (xi + d, DECK, z0)],
                    [wat] * 4, "deck.flank_girder_%s~depth" % inner, wat)
                put([(xo, 0.0, z0), (xo, 0.0, z1), (xo, low, z1), (xo, low, z0)],
                    [wat] * 4, "deck.flank_girder_%s~depth" % ("w" if inner == "e" else "e"), wat)
                put([(xa, DECK - INSET, z0), (xb, DECK - INSET, z0), (xb, DECK - INSET, z1),
                     (xa, DECK - INSET, z1)], [wat] * 4, "deck.slot_top~depth~clamp", wat)
            # its end under the front's girder
            put([(xa, 0.0, zf), (xb, 0.0, zf), (xb, DECK - GIRDER, zf), (xa, DECK - GIRDER, zf)],
                [at] * 4, "deck.flank_girder_s~depth", at)
            # past the front, the ramp's last pixels between two low faces
            for (p, q) in _split(head, foot):
                (z0, h0), (z1, h1) = self.ramp_point(ramp, p), self.ramp_point(ramp, q)
                if z1 <= zf:
                    continue
                if z0 < zf:
                    h0 = h0 + (h1 - h0) * (zf - z0) / (z1 - z0)
                    z0 = zf
                if h0 < 1e-3:
                    continue
                put([(xi, 0.0, z0), (xi, 0.0, z1), (xi, h1, z1), (xi, h0, z0)],
                    [(xa, p)] * 4, "deck.flank_girder_%s~depth" % ("w" if inner == "e" else "e"),
                    (xa, p))

    @staticmethod
    def _straight(north, start, step, near):
        """The north rail walked from column `start` by `step`: the depth it
        settles at (the commonest over 48 columns, each column's depth the
        one nearest `near`, within a rail and a strip of it) and how many
        columns in it first stays there
        four columns running - the drawn curve of a corner's rail goes on
        past RAMP_SIDE at some. (None, 0) when there is no rail."""
        ds = []
        for k in range(48):
            vals = [z for z in north.get(start + step * k, ()) if abs(z - near) <= RAIL + SIDE]
            ds.append(min(vals, key=lambda z: abs(z - near)) if vals else None)
        common = collections.Counter(d for d in ds if d is not None).most_common(1)
        if not common:
            return None, 0
        zn = common[0][0]
        k0 = next((k for k in range(len(ds) - 3) if all(d == zn for d in ds[k:k + 4])), RAMP_SIDE)
        return zn, max(k0, RAMP_SIDE)

    @staticmethod
    def _road(put, xa, xb, z0, z1):
        """The deck's road laid over x [xa, xb), z [z0, z1) in pieces a cell
        each (the plain road the model reads it from is a cell of it)."""
        v0, v1 = z0 - DECK, z1 - DECK
        for (a, b) in _split(int(math.floor(xa)), int(math.ceil(xb))):
            a, b = max(a, xa), min(b, xb)
            for (p, q) in _split(int(math.floor(v0)), int(math.ceil(v1))):
                p, q = max(p, v0), min(q, v1)
                if b - a < 1e-6 or q - p < 1e-6:
                    continue
                pts = [(a, DECK, p + DECK), (b, DECK, p + DECK), (b, DECK, q + DECK), (a, DECK, q + DECK)]
                put(pts, [(x, z - y) for (x, y, z) in pts], "deck.top_under_rail~depth~road",
                    (int(a), int(p)))

    def _clear_of(self, xa, xb, r0, r1, s0, s1):
        """A corner's columns [xa, xb) for rows [r0, r1), its strip at
        [s0, s1), cut short of the corners already built beside it (two
        corners a strip's width apart share the rail between them)."""
        for (a, b, q0, q1) in self.corner_regions:
            if q0 < r1 and r0 < q1 and a < xb and xa < b:
                if b <= s0:
                    xa = max(xa, b)
                elif a >= s1:
                    xb = min(xb, a)
        return xa, xb

    def _heads(self):
        """The deck's outer corners on its north side, where a side rail's
        strip comes up to the north rail (_strips' head_rows): (side, the
        strip's first row, the first row it is drawn full width)."""
        out = []
        for side in ("w", "e"):
            for (a, b) in _runs(sorted(v for (sd, v) in self.head_rows if sd == side)):
                out.append((side, a, b + 1))
        return out

    def _north_corner(self, head, out, put, strips, north, flank):
        """An outer corner on the deck's north side, square. The drawing
        rounds it: the north rail's last columns drawn turning down, the side
        strip's head narrowing up to them row by row - built as drawn, a
        column of the rail stood up past it like a post, the strip's head
        rose in a staircase of rail and a crack stood between the two. What
        the generic build laid over the corner (from the strip in to where
        the north rail runs straight, `_straight`; from over the rail's top
        to the end of the cell row under the strip's head) is
        taken out; the north rail runs straight on at its depth to the
        strip's outer side, the strip (its top, its rail's faces, its girder)
        runs on north from its full-width rows to the rail, and the deck's
        road fills the corner."""
        side, v0, v_full = head
        e = next((e for e in strips.get((side, v_full), ())
                  if any(e[0] <= c < e[1] for cs in self.head_rows[(side, v_full - 1)] for c in cs)), None)
        if e is None:
            return
        s0, s1, xi, xo = e[0], e[1], e[2], e[3]
        if side == "e":
            zn, k = self._straight(north, s0 - 1, -1, v_full + DECK)
            xa, xb = s0 - k, s1
        else:
            zn, k = self._straight(north, s1, 1, v_full + DECK)
            xa, xb = s0, s1 + k
        if zn is None:
            return
        # (to the end of a cell row: the generic faces are cut cell by cell)
        r0, r1 = int(zn) - DECK - RAIL - SIDE, ((v_full + 15) // 16 + 1) * 16
        xa, xb = self._clear_of(xa, xb, r0, r1, s0, s1)
        road = (xa, s0) if side == "e" else (s1, xb)
        self.corner_regions.append((xa, xb, r0, r1))
        # the strip's own columns from its full-width row on are the generic
        # build's, as along the rest of it
        out[:] = [f for f in out if not (xa <= int(f[3][0]) < xb and r0 <= int(f[3][1])
                                         < (v_full if s0 <= int(f[3][0]) < s1 else r1))]
        top, low, zr = DECK + RAIL, DECK - SLAB, r1 + DECK
        at = (road[0], r1 - 1)
        # the road over the corner, its slab under it
        self._road(put, road[0], road[1], zn + BEHIND, zr)
        put([(road[0], low, zr), (road[1], low, zr), (road[1], low, zn), (road[0], low, zn)],
            [at] * 4, "deck.under~depth~clamp", at)
        # the north rail straight on to the strip's outer side, its girder under it
        put([(xa, DECK, zn), (xb, DECK, zn), (xb, DECK + NORTH_RAIL, zn), (xa, DECK + NORTH_RAIL, zn)],
            [at] * 4, "deck.flank_railn_s~depth", at)
        put([(xb, DECK - GIRDER, zn), (xa, DECK - GIRDER, zn), (xa, DECK, zn), (xb, DECK, zn)],
            [at] * 4, "deck.flank_girdern_n~depth", at)
        # the strip on north to the rail: its top, column by column in the
        # colours of its first full-width row, and its faces
        # (kept out of _merge, which would lay the whole strip in its first
        # column's colour: the outline's)
        zt = v_full + top
        n = len(out)
        for c in range(s0, s1):
            put([(c, top, zn), (c + 1, top, zn), (c + 1, top, zt), (c, top, zt)],
                [(c, v_full)] * 4, "deck.rail_side~depth~clamp", (c, v_full))
        self._unmerged += out[n:]
        del out[n:]
        out_dir, in_dir = (side, "e" if side == "w" else "w")
        flank("rail", out_dir, xo, DECK, top, zn, zt, at)
        flank("girder", out_dir, xo, DECK - GIRDER, DECK, zn, zt, at)
        flank("rail", in_dir, xi, DECK, top, zn, zt, at)
        flank("under", in_dir, xi, DECK - GIRDER, low, zn, zt, at)

    def _feet_north(self):
        """The deck's inner corners on its north side, where a road coming
        from the north meets a wider deck: a side rail's strip that runs on
        down (16 rows or more) to rows where the road reaches past its outer
        side. (side, the strip's columns (s0, s1), its last row, the road's
        first row past it)."""
        runs = collections.defaultdict(list)
        for (side, edge, v) in self.strip_cols:
            runs[(side, edge)].append(v)
        out = []
        for (side, edge), vs in sorted(runs.items()):
            for (a, b) in _runs(sorted(vs)):
                cols = self.strip_cols[(side, edge, (a + b) // 2)]
                if b - a < 15 or not cols:
                    continue
                outer = min(cols) if side == "w" else max(cols)
                hit = next((v for v in range(b + 1, b + RAIL + SIDE + 8) if (outer, v) in self.road), None)
                if hit is not None:
                    out.append((side, (min(cols), max(cols) + 1), b, hit))
        return out

    def _inner_north_corner(self, foot, out, put, north, flank):
        """An inner corner on the deck's north side, square: the wider
        deck's north rail runs straight on at its depth across the foot of
        the side rail's strip coming down to it, the strip runs on to the
        rail at its full width, and the road fills the corner. The drawing
        turns the rail into the strip in a curve: built as drawn, the rail's
        columns there stood up past it with the strip's pixels on them, a
        thin post (`_north_corner` takes the same region out)."""
        side, (s0, s1), last, hit = foot
        if side == "w":
            zn, k = self._straight(north, s0 - 1, -1, hit + DECK)
            xa, xb = s0 - k, s1
        else:
            zn, k = self._straight(north, s1, 1, hit + DECK)
            xa, xb = s0, s1 + k
        if zn is None:
            return
        # (from a cell over the rail's top: the drawn curve starts the
        # rail's columns that much higher)
        r0, r1 = min(int(zn) - DECK, hit) - RAIL - SIDE - 16, ((hit + 15) // 16 + 1) * 16
        xa, xb = self._clear_of(xa, xb, r0, r1, s0, s1)
        self.corner_regions.append((xa, xb, r0, r1))
        out[:] = [f for f in out if not (xa <= int(f[3][0]) < xb and r0 <= int(f[3][1]) < r1)]
        top, low, zr = DECK + RAIL, DECK - SLAB, r1 + DECK
        at = (xa, r1 - 1)
        # the road over the corner, its slab under it
        self._road(put, xa, xb, zn + BEHIND, zr)
        put([(xa, low, zr), (xb, low, zr), (xb, low, zn), (xa, low, zn)], [at] * 4,
            "deck.under~depth~clamp", at)
        # the north rail straight on across the strip's foot, its girder under it
        put([(xa, DECK, zn), (xb, DECK, zn), (xb, DECK + NORTH_RAIL, zn), (xa, DECK + NORTH_RAIL, zn)],
            [at] * 4, "deck.flank_railn_s~depth", at)
        put([(xb, DECK - GIRDER, zn), (xa, DECK - GIRDER, zn), (xa, DECK, zn), (xb, DECK, zn)],
            [at] * 4, "deck.flank_girdern_n~depth", at)
        # the strip on down to the rail, in the colours of the last row the
        # generic build lays (one texel a column, kept out of _merge)
        z0 = r0 + top
        if zn > z0:
            n = len(out)
            for c in range(s0, s1):
                put([(c, top, z0), (c + 1, top, z0), (c + 1, top, zn), (c, top, zn)],
                    [(c, r0 - 1)] * 4, "deck.rail_side~depth~clamp", (c, r0 - 1))
            self._unmerged += out[n:]
            del out[n:]
            xo, xi = (s0, s1) if side == "w" else (s1, s0)
            out_dir, in_dir = (side, "e" if side == "w" else "w")
            sat = (s0, r0 - 1)
            flank("rail", out_dir, xo, DECK, top, z0, zn, sat)
            flank("girder", out_dir, xo, DECK - GIRDER, DECK, z0, zn, sat)
            flank("rail", in_dir, xi, DECK, top, z0, zn, sat)
            flank("under", in_dir, xi, DECK - GIRDER, low, z0, zn, sat)

    def _south_corner(self, head, out, put, south, flank, generic):
        """An inner corner on the deck's south side, square: where the road
        widens over a side rail's strip (`inner_heads`), the drawing turns
        the front into the strip at 45 degrees, its girder's stripes drawn
        across the turn - built as drawn, a wall of teeth, every column's
        rows a pixel or two off its neighbour's. What the generic build laid
        there but the drawn road (from where the front runs straight,
        `_straight`, to the strip's inner side; from the cell the chamfer
        starts in to the end of the cell row it ends in) is taken out; the
        front's rail and girder run straight on at its depth to the strip's
        outer side and across its head, the strip runs on north from its
        straight rows to the front, and the road under the rail fills the
        corner."""
        side, (s0, s1), v0, heads = head
        near = (heads[s0] if side == "w" else heads[s1 - 1]) + RAIL + DECK
        if side == "w":
            zf, k = self._straight(south, s0 - 1, -1, near)
            xa, xb = s0 - k, s1
        else:
            zf, k = self._straight(south, s1, 1, near)
            xa, xb = s0, s1 + k
        if zf is None:
            return
        # where each column's drawn road ends over the corner
        start = int(zf) - DECK - RAIL - SIDE
        ends = {}
        for u in range(xa, xb):
            v = start
            if (u, v) not in self.road:
                continue
            while (u, v) in self.road:
                v += 1
            ends[u] = v
        if not ends:
            return
        r0 = min(ends.values()) // 16 * 16
        # down to the chamfer's last drawn row (not a pier's, under it), at
        # least to the strip's straight rows; short of a corner built under
        # it on the strip
        drawn = [max(t[1] for t in f[1]) for f in out if id(f) in generic and f[2] == "deck.front"
                 and xa <= int(f[3][0]) < xb and r0 <= int(f[3][1]) < int(zf) - DECK + GIRDER + 8]
        r1 = (max(drawn + [v0 + 1]) + 15) // 16 * 16
        for (a, b, q0, q1) in self.corner_regions:
            if a < s1 and s0 < b and r0 < q0:
                r1 = min(r1, q0)
        xa, xb = self._clear_of(xa, xb, r0, r1, s0, s1)
        self.corner_regions.append((xa, xb, r0, r1))
        out[:] = [f for f in out if not (id(f) in generic and f[2] != "deck.top"
                                         and xa <= int(f[3][0]) < xb and r0 <= int(f[3][1]) < r1)]
        top, low = DECK + RAIL, DECK - SLAB
        fa, fb = (xa, s0) if side == "w" else (s1, xb)      # the front's columns
        at = (fa if side == "w" else fb - 1, r0)
        # the road on from where it is drawn to under the front's rail, and
        # the slab under it all
        for u, b in sorted(ends.items()):
            if xa <= u < xb and b + DECK < zf - BEHIND:
                self._road(put, u, u + 1, b + DECK, zf - BEHIND)
        for (a, b) in _split(xa, xb):
            put([(a, low, zf - BEHIND), (b, low, zf - BEHIND), (b, low, r0 + DECK), (a, low, r0 + DECK)],
                [(a, r0)] * 4, "deck.under~depth~clamp", (a, r0))
        # the front straight on to the strip, and across the strip's head
        self._front(put, fa, fb, zf, at)
        put([(s0, DECK - GIRDER, zf), (s1, DECK - GIRDER, zf), (s1, top, zf), (s0, top, zf)],
            [at] * 4, "deck.flank_rail_s~depth", at)
        # the strip on north to the front: its top, column by column in the
        # colours of its first straight row (kept out of _merge), its faces
        zt = r1 + top
        sat = (s0, v0)
        n = len(out)
        for c in range(s0, s1):
            put([(c, top, zf), (c + 1, top, zf), (c + 1, top, zt), (c, top, zt)],
                [(c, v0)] * 4, "deck.rail_side~depth~clamp", (c, v0))
        self._unmerged += out[n:]
        del out[n:]
        xo, xi = (s0, s1) if side == "w" else (s1, s0)
        out_dir, in_dir = (side, "e" if side == "w" else "w")
        flank("rail", out_dir, xo, DECK, top, zf, zt, sat)
        flank("girder", out_dir, xo, DECK - GIRDER, DECK, zf, zt, sat)
        flank("rail", in_dir, xi, DECK, top, zf, zt, sat)
        flank("under", in_dir, xi, DECK - GIRDER, low, zf, zt, sat)
        quad = [(xi, DECK - GIRDER, zf), (xo, DECK - GIRDER, zf),
                (xo, DECK - GIRDER, zt), (xi, DECK - GIRDER, zt)]
        put(quad[::-1] if side == "w" else quad, [sat] * 4, "deck.under~depth~clamp", sat)

    @staticmethod
    def _front(put, xa, xb, zf, at):
        """The front's rail and girder straight on at depth zf over x [xa,
        xb), dressed with the flank."""
        put([(xa, DECK - GIRDER, zf), (xb, DECK - GIRDER, zf), (xb, DECK + RAIL, zf), (xa, DECK + RAIL, zf)],
            [at] * 4, "deck.flank_rail_s~depth", at)

    def _south_outer_corner(self, foot, out, put, south, flank, generic):
        """An outer corner on the deck's south side, square (`feet`): the
        drawing turns the front into the side rail's strip at 45 degrees,
        and built as drawn its girder was the same wall of teeth an inner
        corner's was. What the generic build laid there but the drawn road
        (from the strip's outer side to where the front runs straight; from
        the cell the strip's last rows are in to the end of the cell row the
        chamfer ends in) is taken out; the front's rail and girder run
        straight on at its depth to the strip's outer side, the strip runs
        on south to the front, and the road under the rail fills the
        corner. False when there is no straight front beside it (its
        chamfer stays)."""
        side, edge, last, rows = foot
        cols = sorted(rows)
        near = last + 1 + RAIL + DECK
        if side == "w":
            s0, s1 = cols[0], edge
            zf, k = self._straight(south, edge, 1, near)
            xa, xb = s0, edge + k
        else:
            s0, s1 = edge + 1, cols[-1] + 1
            zf, k = self._straight(south, edge, -1, near)
            xa, xb = edge + 1 - k, s1
        if zf is None:
            return False
        fa, fb = (s1, xb) if side == "w" else (xa, s0)      # the road's columns
        # where each column's drawn road ends over the corner
        start = min(last, int(zf) - DECK - RAIL - SIDE)
        ends = {}
        for u in range(fa, fb):
            v = start
            if (u, v) not in self.road:
                continue
            while (u, v) in self.road:
                v += 1
            ends[u] = v
        if not ends:
            return False
        r0 = min(ends.values()) // 16 * 16
        drawn = [max(t[1] for t in f[1]) for f in out if id(f) in generic and f[2] == "deck.front"
                 and xa <= int(f[3][0]) < xb and r0 <= int(f[3][1]) < int(zf) - DECK + GIRDER + 8]
        r1 = (max(drawn + [b for (_, b) in rows.values()]) + 15) // 16 * 16
        xa, xb = self._clear_of(xa, xb, r0, r1, s0, s1)
        fa, fb = max(fa, xa), min(fb, xb)
        self.corner_regions.append((xa, xb, r0, r1))
        out[:] = [f for f in out if not (id(f) in generic and f[2] != "deck.top"
                                         and xa <= int(f[3][0]) < xb and r0 <= int(f[3][1]) < r1)]
        top, low = DECK + RAIL, DECK - SLAB
        at = (edge, r0)
        # the road on from where it is drawn to under the front's rail, and
        # the slab under it
        for u, b in sorted(ends.items()):
            if fa <= u < fb and b + DECK < zf - BEHIND:
                self._road(put, u, u + 1, b + DECK, zf - BEHIND)
        for (a, b) in _split(fa, fb):
            put([(a, low, zf - BEHIND), (b, low, zf - BEHIND), (b, low, r0 + DECK), (a, low, r0 + DECK)],
                [(a, r0)] * 4, "deck.under~depth~clamp", (a, r0))
        # the front straight on to the strip's outer side
        self._front(put, xa, xb, zf, at)
        # the strip on south to the front: its top, column by column in the
        # colours of the last row the generic build lays (kept out of
        # _merge), its faces
        z0 = r0 + top
        if zf > z0:
            sat = (s0, r0 - 1)
            n = len(out)
            for c in range(s0, s1):
                put([(c, top, z0), (c + 1, top, z0), (c + 1, top, zf), (c, top, zf)],
                    [(c, r0 - 1)] * 4, "deck.rail_side~depth~clamp", (c, r0 - 1))
            self._unmerged += out[n:]
            del out[n:]
            xo, xi = (s0, s1) if side == "w" else (s1, s0)
            out_dir, in_dir = (side, "e" if side == "w" else "w")
            flank("rail", out_dir, xo, DECK, top, z0, zf, sat)
            flank("girder", out_dir, xo, DECK - GIRDER, DECK, z0, zf, sat)
            flank("rail", in_dir, xi, DECK, top, z0, zf, sat)
            flank("under", in_dir, xi, DECK - GIRDER, low, z0, zf, sat)
            quad = [(xi, DECK - GIRDER, z0), (xo, DECK - GIRDER, z0),
                    (xo, DECK - GIRDER, zf), (xi, DECK - GIRDER, zf)]
            put(quad[::-1] if side == "w" else quad, [sat] * 4, "deck.under~depth~clamp", sat)
        return True

    @staticmethod
    def _pier(comp, put, upright, texel):
        """A pier drawn beside the road: a box standing in the water, as
        deep as it is wide - its top rows the cap, the rest its front, on
        the plane its foot stands on; its sides where the column beside it
        stops short (a head wider than the pile under it)."""
        cols = collections.defaultdict(list)
        for (u, v) in comp:
            cols[u].append(v)
        u0, u1 = min(cols), max(cols) + 1
        zf = max(v for (_, v) in comp) + 1
        depth = u1 - u0

        def span(u):
            """(foot, top) heights of column u's box, or None."""
            vs = cols.get(u)
            if not vs:
                return None
            cap = min(min(vs) + depth, max(vs) + 1)
            return zf - (max(vs) + 1), zf - cap

        for u in range(u0, u1):
            vs = sorted(cols.get(u, ()))
            if not vs:
                continue
            tc = vs[0]
            foot, top = span(u)
            cap = zf - top
            # the cap, its drawn rows from the box's back to its front
            for (p, q) in _split(tc, cap):
                put([(u, top, p + top), (u + 1, top, p + top), (u + 1, top, q + top), (u, top, q + top)],
                    [(u, p), (u + 1, p), (u + 1, q), (u, q)], "deck.pier_top", (u, p))
            # the front: every drawn row under the cap
            for (p, q) in _runs([v for v in vs if v >= cap]):
                upright(u, zf, p, q + 1, "deck.pier")
            # the sides, where the column beside it stops short
            for nb, x, facing in ((u - 1, u, "w"), (u + 1, u + 1, "e")):
                other = span(nb)
                parts = [(foot, top)] if other is None else \
                    [(foot, min(top, other[0])), (max(foot, other[1]), top)]
                for (y0, y1) in parts:
                    if y1 - y0 < 1e-6:
                        continue
                    z0, z1 = zf - depth, zf
                    pts = [(x, y0, z0), (x, y0, z1), (x, y1, z1), (x, y1, z0)]
                    put(pts, [(u, tc)] * 4, "deck.pier_side_%s~depth~clamp" % facing, (u, tc))

    def flank_sample(self):
        """The south front's rail and girder, 16 columns of it with no pier
        (FLANK_ROWS rows from the rail's top): what dresses the deck's other
        sides, which the drawing never shows."""
        if hasattr(self, "_flank"):
            return self._flank
        cols = collections.defaultdict(list)
        for (u, v) in self.road:
            cols[u].append(v)
        clean = {}
        for u, vs in cols.items():
            for (a, b) in _runs(sorted(vs)):
                kerb = next((v for v in range(b + 1, b + 1 + RAIL + 4)
                             if self.pixels.get((u, v)) == LINE), None)
                if kerb is None or kerb - (b + 1) != RAIL:
                    continue
                if all((u, v) in self.pixels for v in range(kerb, kerb + GIRDER)) \
                        and (u, kerb + GIRDER) not in self.pixels:
                    clean[u] = kerb
        found = None
        for u0 in sorted(clean):
            if u0 % 16 == 0 and all(clean.get(u0 + i) == clean[u0] for i in range(16)):
                found = (u0, clean[u0])
                break
        if found is None:
            raise SystemExit("%s: no stretch of the front to dress the deck's sides with" % self.id)
        u0, kerb = found
        self._flank = self.art.crop((u0, kerb - RAIL, u0 + 16, kerb + GIRDER))
        return self._flank

    def road_sample(self):
        """The plain road: the cell drawn most often all road, framed by a
        row of its own repeat above and two below (a merged face's corner
        can lie half a pixel past its cell). What lays the deck under the
        south rail, the road going on as if the rail were not drawn on it."""
        if hasattr(self, "_road_cell"):
            return self._road_cell
        seen = collections.Counter()
        for (x, y) in self.deck_cells:
            if all((x * 16 + i, y * 16 + j) in self.pixels and (x * 16 + i, y * 16 + j) in self.road
                   for j in range(16) for i in range(16)):
                seen[self.art.crop((x * 16, y * 16, x * 16 + 16, y * 16 + 16)).tobytes()] += 1
        if not seen:
            raise SystemExit("%s: no cell of plain road" % self.id)
        cell = vb.Image.frombytes("RGBA", (16, 16), seen.most_common(1)[0][0])
        self._road_cell = vb.Image.new("RGBA", (16, ROAD_ROWS), (0, 0, 0, 0))
        for k in range(-1, ROAD_ROWS - 1, 16):
            self._road_cell.paste(cell, (0, 1 + k))
        return self._road_cell

    # -- what the console needs besides the faces -------------------------------

    def drawn_cells(self):
        """The cells the road is drawn on (once its faces are read), and
        those of the signposts on it."""
        return {(u // 16, v // 16) for (u, v) in self.pixels} | self.sign_cells | \
            {(x, y) for (y, x0, x1, _) in self.curbs for x in range(x0, x1)}

    def decks(self):
        """{(x, y): (lift at the cell's top, at its bottom, elevation)}: the
        surface walkers on the cell stand on, in pixels over the ground, and
        the elevation an object needs to be up there (0: any)."""
        out = {}
        cells = {(u // 16, v // 16) for (u, v) in self.road}
        for ramp in self.ramps:
            u0, u1, top, foot = ramp
            for y in range(self.ramp_head(ramp) // 16, (foot + 15) // 16):
                for x in range(u0 // 16, (u1 - 1) // 16 + 1):
                    cells.add((x, y))
        for (x, y) in sorted(cells):
            mid = x * 16 + 8
            ramp = self.ramp_at(mid, y * 16 + 8)
            elev = self.cells.elevation(x, y)
            on_ramp = ramp is not None and (y + 1) * 16 > ramp[2]
            if on_ramp:
                gate = 0
            elif elev == self.deck_elevation or (x, y) in self.sign_cells:
                gate = 0
            elif elev == self.both:
                gate = self.deck_elevation
            else:
                continue    # a rail's row: nobody walks there
            out[(x, y)] = (int(round(self.lift_at(mid, y * 16))),
                           int(round(self.lift_at(mid, y * 16 + 16))), gate)
        return out

    def ground_for(self, x, y):
        """The metatile laid flat under the model in cell (x, y): what lies
        there with the road taken away - the plain ground whose colours the
        cell's lower layer shows most (the water round a pier, the grass a
        path draws under a rail), or for a cell drawn all road, the open
        water or the path it spans."""
        L = self.art_layout
        cands = self.plain_anywhere()
        subs = L.subtiles(L.metatile(x, y), 0)
        # the lower layer where the road is not drawn
        low = [subs[q][k][0] for q in range(4) for k in range(64)
               if (x * 16 + (q & 1) * 8 + k % 8, y * 16 + (q >> 1) * 8 + k // 8) not in self.pixels]
        colours = {m: {rgb for blk in cands[m] for rgb in blk} for m in cands}
        score = {m: sum(1 for c in low if c in colours[m]) for m in cands}
        best = max(cands, key=lambda m: (score[m], self.counts[m]))
        if score[best] >= 32:
            return best
        # drawn all road: what the row goes on with either side of it, the
        # nearest plain ground (the sea a span crosses, a path under it)
        for d in range(1, self.w):
            for xx in (x - d, x + d):
                if 0 <= xx < self.w and (xx, y) not in self.drawn_cells_cache() \
                        and self.cells.role_at(xx, y) in ("water", "floor"):
                    water = self.cells.role_at(xx, y) == "water"
                    pick = [m for m in cands if (m in self.water) == water]
                    return max(pick or cands, key=lambda m: self.counts[m])
        return max(cands, key=lambda m: self.counts[m])

    def drawn_cells_cache(self):
        if not hasattr(self, "_drawn"):
            self._drawn = self.drawn_cells()
        return self._drawn

    def plain_grounds(self):
        """{metatile: its bottom blocks} of the layout's plain ground: drawn
        away from the road with nothing on its upper layer."""
        if hasattr(self, "_plain"):
            return self._plain
        L = self.art_layout
        self.counts = collections.Counter()
        self.water = set()
        out = {}
        near = self._deck_zone(ZONE + 1)
        for y in range(self.h):
            for x in range(self.w):
                if (x, y) in near:
                    continue
                m = L.metatile(x, y)
                if any(i for blk in L.subtiles(m, 1) for _, i in blk):
                    continue
                if self.cells.blocked(x, y) and self.cells.role_at(x, y) != "water":
                    continue
                out[m] = [tuple(rgb for rgb, _ in blk) for blk in L.subtiles(m, 0)]
                self.counts[m] += 1
                if self.cells.role_at(x, y) == "water":
                    self.water.add(m)
        self._plain = out
        return out

    def plain_anywhere(self):
        """{metatile: its bottom blocks} of the plain ground of the primary
        tileset drawn anywhere (the shadowed water by the road too), for what
        lies under the road's cells; their counts and which are water kept
        with plain_grounds'."""
        if hasattr(self, "_anywhere"):
            return self._anywhere
        self.plain_grounds()
        L = self.art_layout
        out = dict(self._plain)
        for y in range(self.h):
            for x in range(self.w):
                m = L.metatile(x, y)
                if m in out or m >= vb.NUM_PRIMARY or (x, y) in self.deck_cells:
                    continue
                if any(i for blk in L.subtiles(m, 1) for _, i in blk):
                    continue
                role = self.cells.role_at(x, y)
                if role not in ("water", "floor"):
                    continue
                out[m] = [tuple(rgb for rgb, _ in blk) for blk in L.subtiles(m, 0)]
                if role == "water":
                    self.water.add(m)
        self._anywhere = out
        return out


def _merge(faces):
    """Faces that go on from the one beside them, as one.

    A column's face and the next column's in the same cell merge when every
    corner moves by the same step from one to the next: the same heights and
    depths (a straight span), or depths one pixel further each column (a
    chamfered corner, drawn as a staircase of pixels). The merged face is
    the plane through the columns' middles - a diagonal wall, not a
    staircase - and its texture is the projection again (u = x, v = z - y),
    so at 45 degrees every pixel's middle is still the drawing's. Rows along
    a side (a rail's strip, the deck's side) merge the same way."""
    def cell(at):
        return (int(at[0]) // 16, int(at[1]) // 16)

    cols, rows, out = collections.defaultdict(list), collections.defaultdict(list), []
    slanted = set()
    for f in faces:
        pts, uvs, tag, at = f
        xs = sorted({p[0] for p in pts})
        zs = sorted({p[2] for p in pts})
        if len(xs) == 2 and xs[1] - xs[0] == 1:
            u = xs[0]
            edge = tuple(p[0] - u for p in pts)
            cols[(tag, cell(at), edge)].append((u, f))
        elif len(zs) == 2 and not tag.endswith("~proj"):
            edge = tuple(int(p[2] == zs[1]) for p in pts)
            rows[(tag, cell(at), edge, round(zs[1] - zs[0], 6))].append((zs[0], f))
        else:
            out.append(f)

    def fits(run, others):
        """Every corner of the run on a line (one pixel off at most, the
        staircase a chamfer is drawn with), at most a pixel a step."""
        n = len(run)
        if n < 2:
            return True
        first, last = others(run[0]), others(run[-1])
        for i in range(len(first)):
            slope = (last[i] - first[i]) / (n - 1)
            if abs(slope) > 1 + 1e-6:
                return False
            # a pixel off the line on a diagonal (its staircase); none on a
            # straight edge
            slack = 1 + 1e-6 if abs(slope) > 1e-6 else 1e-6
            for k, it in enumerate(run):
                if abs(others(it)[i] - (first[i] + slope * k)) > slack:
                    return False
        return True

    def runs(items, along, others, step):
        """Items (sorted) cut into runs: each next to the last, all of them
        on one plane (fits)."""
        done, run = [], [items[0]]
        for it in items[1:]:
            prev = run[-1]
            if abs(along(it) - along(prev) - step(prev)) < 1e-6 and fits(run + [it], others):
                run.append(it)
            else:
                done.append(run)
                run = [it]
        done.append(run)
        return done

    def projected(tag):
        return "~" not in tag

    def exact_runs(items):
        """Columns of one drawn face side by side: the same rows, each
        starting where the last ends, on one plane (the depth stepping by the
        same amount every column)."""
        done, run, step = [], [items[0]], None
        for it in items[1:]:
            prev = run[-1]
            pp, ip = prev[1][0], it[1][0]
            # the columns' west (dx 0) and east (dx 1) corners, by row
            pw = {round(t[1], 6): p for p, t in zip(pp, prev[1][1]) if abs(p[0] - prev[0]) < 1e-9}
            pe = {round(t[1], 6): p for p, t in zip(pp, prev[1][1]) if abs(p[0] - prev[0] - 1) < 1e-9}
            iw = {round(t[1], 6): p for p, t in zip(ip, it[1][1]) if abs(p[0] - it[0]) < 1e-9}
            ie = {round(t[1], 6): p for p, t in zip(ip, it[1][1]) if abs(p[0] - it[0] - 1) < 1e-9}
            joined = it[0] == prev[0] + 1 and set(pe) == set(iw) and all(
                abs(pe[r][1] - iw[r][1]) < 1e-6 and abs(pe[r][2] - iw[r][2]) < 1e-6 for r in pe)
            d = tuple(round(ie[r][2] - iw[r][2], 6) for r in sorted(ie)) if set(ie) == set(iw) else None
            dp = tuple(round(pe[r][2] - pw[r][2], 6) for r in sorted(pe)) if set(pe) == set(pw) else None
            if joined and d is not None and d == dp and (step is None or d == step):
                run.append(it)
                step = d
            else:
                done.append(run)
                run, step = [it], None
        done.append(run)
        return done

    for (tag, c, edge), items in cols.items():
        items.sort(key=lambda it: it[0])
        if projected(tag):
            # a drawn face: merged only where it stays exact
            by_rows = collections.defaultdict(list)
            for it in items:
                by_rows[tuple(sorted({round(t[1], 6) for t in it[1][1]}))].append(it)
            for rows_, its in by_rows.items():
                for run in exact_runs(its):
                    first, last = run[0][1], run[-1][1]
                    u0, n = run[0][0], len(run)
                    pts = []
                    for i, e in enumerate(edge):
                        src = last if e else first
                        pts.append(src[0][i])
                    pts = [(p[0], p[1], p[2]) for p in pts]
                    uvs = [(p[0], t[1]) for p, t in zip(pts, first[1])]
                    if any(abs(first[0][i][2] - last[0][i][2]) > 1e-6 for i in range(len(edge))) or \
                            any(abs(p[2] - q[2]) > 1e-6 for p, q in zip(first[0], first[0][1:])):
                        slanted.add(c)
                        out.append((pts, uvs, tag + "~proj", first[3]))
                    else:
                        out.append((pts, uvs, tag, first[3]))
            continue
        flat = lambda it: tuple(v for p in it[1][0] for v in p[1:])
        if tag.endswith("~road"):
            # the road under a rail meets the drawn road's staircase edge to
            # edge: merged only where nothing steps
            found = []
            for it in items:
                if found and it[0] == found[-1][-1][0] + 1 and flat(it) == flat(found[-1][-1]):
                    found[-1].append(it)
                else:
                    found.append([it])
        else:
            found = runs(items, lambda it: it[0], flat, lambda it: 1)
        for run in found:
            n = len(run)
            u0 = run[0][0]
            first, last = run[0][1], run[-1][1]
            pts = []
            for i, e in enumerate(edge):
                y0, z0 = first[0][i][1], first[0][i][2]
                dy = (last[0][i][1] - y0) / (n - 1) if n > 1 else 0.0
                dz = (last[0][i][2] - z0) / (n - 1) if n > 1 else 0.0
                k = (n - 0.5) if e else -0.5
                if dy == 0 and dz == 0:
                    k = 0.0
                else:
                    slanted.add(c)
                pts.append((u0 + (n if e else 0), max(0.0, y0 + dy * k), z0 + dz * k))
            slant = any(abs(first[0][i][2] - last[0][i][2]) > 1e-6 for i in range(len(edge)))
            if projected(tag):
                uvs = [(p[0], p[2] - p[1]) for p in pts]
                if slant:
                    # the drawing on a wall across the view: projected as
                    # drawn, sheared as any diagonal wall is at 45 degrees
                    out.append((pts, uvs, tag + "~proj", first[3]))
                    continue
            elif tag.endswith("~clamp"):
                uvs = first[1]
            elif tag.endswith("~road"):
                uvs = [(p[0], p[2] - p[1]) for p in pts]
            else:
                if n > 1 and any(abs(a[1] - b[1]) > 1e-6 for a, b in zip(first[0], last[0])):
                    out += [it[1] for it in run]      # a ramp's columns: as they are
                    continue
                uvs = [(u0 + (n if e else 0), t[1]) for e, t in zip(edge, first[1])]
            out.append((pts, uvs, tag, first[3]))
    for (tag, c, edge, dz), items in rows.items():
        items.sort(key=lambda it: it[0])
        flat = lambda it: tuple(v for p in it[1][0] for v in (p[0], p[1]))
        if projected(tag):
            # a drawn face: rows merged only where they are the same width
            # in the same place, so it stays exact
            done, run = [], [items[0]]
            for it in items[1:]:
                if abs(it[0] - run[-1][0] - dz) < 1e-6 and flat(it) == flat(run[-1]):
                    run.append(it)
                else:
                    done.append(run)
                    run = [it]
            done.append(run)
            for run in done:
                first = run[0][1]
                z0, z1 = run[0][0], run[-1][0] + dz
                pts = [(x, y, z1 if far else z0) for (x, y, far) in
                       [(p[0], p[1], e) for p, e in zip(first[0], edge)]]
                out.append((pts, [(p[0], p[2] - p[1]) for p in pts], tag, first[3]))
            continue
        for run in runs(items, lambda it: it[0], flat, lambda it: dz):
            n = len(run)
            z0 = run[0][0]
            first, last = run[0][1], run[-1][1]
            pts = []
            if {p[0] for p, e in zip(first[0], edge) if e} != {p[0] for p, e in zip(first[0], edge) if not e}:
                # rows already cut across (a strip's head): end to end
                pts = [(last if e else first)[0][i] for i, e in enumerate(edge)]
                out.append((pts, first[1], tag, first[3]))
                continue
            for i, e in enumerate(edge):
                x0, y0 = first[0][i][0], first[0][i][1]
                dx = (last[0][i][0] - x0) / (n - 1) if n > 1 else 0.0
                dy = (last[0][i][1] - y0) / (n - 1) if n > 1 else 0.0
                k = (n - 0.5) if e else -0.5
                if dx == 0 and dy == 0:
                    k = 0.0
                else:
                    slanted.add(c)
                pts.append((x0 + dx * k, y0 + dy * k, z0 + (n * dz if e else 0)))
            name = tag
            if projected(tag):
                uvs = [(p[0], p[2] - p[1]) for p in pts]
                if any(abs(first[0][i][0] - last[0][i][0]) > 1e-6 for i in range(len(edge))):
                    name = tag + "~proj"      # sheared, as above
            else:
                uvs = first[1]
            out.append((pts, uvs, name, first[3]))
    return out, slanted


FLANK_ROWS = RAIL + GIRDER     # the flank's rows: the rail, then the girder
ROAD_ROWS = 19                 # the plain road's: 16, one over them, two under


def _flank_row(d):
    """The flank's row `d` pixels under the rail's top: the rail, then the
    girder repeated down a wall (a ramp's slot's, to the ground). (row,
    band): the band is how many girders down."""
    if d < RAIL:
        return d, -1
    k = int((d - RAIL) // GIRDER)
    return d - k * GIRDER, k


def _cut_corners(faces, cuts):
    """The deck's sides under its rails (the flanks, the girder and the slab
    under a strip), which the drawing never shows, cut at an outer corner by
    the chamfer's plane: running on to the strip's last row they stood out
    in front of it, over the chamfer and the water beside it - and on it,
    the strip's last rows following the chamfer, a second face where the
    chamfer is the one drawn. A cut is (s, C, y, x0, x1, z): what lies
    between x0 and x1 and begins north of z keeps z + s * x <= C - 1, a
    pixel behind the chamfer (its staircase is a pixel deep), and at 45
    degrees no lower than the chamfer's drawing ends, at height y over its
    plane: z + s * x - y' <= C - y."""
    out = []
    for f in faces:
        pts, uvs, tag, at = f
        base = tag.split("~")[0]
        if not base.startswith(("deck.flank_", "deck.under")):
            out.append(f)
            continue
        xs = [p[0] for p in pts]
        for cut in cuts:
            sx, c, y, x0, x1, z = cut
            if min(xs) < x0 - 1e-6 or max(xs) > x1 + 1e-6 or min(p[2] for p in pts) > z + 1e-6:
                continue
            for plane in (lambda p: p[2] + sx * p[0] - (c - 1.0),
                          lambda p: p[2] + sx * p[0] - p[1] - (c - y)):
                if len(pts) >= 3:
                    pts, uvs = _clip_poly(pts, uvs, plane)
            if len(pts) < 3:
                break
        if len(pts) >= 3:
            out.append((pts, uvs, tag, at))
    return out


def _clip_poly(pts, uvs, plane):
    """The polygon (and its uvs) where plane(point) <= 0."""
    side = [plane(p) for p in pts]
    if max(side) <= 1e-6:
        return pts, uvs
    kept_p, kept_t = [], []
    for i in range(len(pts)):
        j = (i + 1) % len(pts)
        if side[i] <= 1e-6:
            kept_p.append(pts[i])
            kept_t.append(uvs[i])
        if (side[i] <= 1e-6) != (side[j] <= 1e-6):
            t = side[i] / (side[i] - side[j])
            kept_p.append(tuple(a + t * (b - a) for a, b in zip(pts[i], pts[j])))
            kept_t.append(tuple(a + t * (b - a) for a, b in zip(uvs[i], uvs[j])))
    return kept_p, kept_t


def _tile_flanks(faces):
    """The deck's sides, laid with the flank (Bridge.flank_sample) one texel a
    pixel: a face is cut where the flank's 16 columns repeat along it and
    where its rows do down it. Their uvs are the flank's own pixels."""
    out = []
    # (the north rail: the front's rail under its highlight, down to the
    # kerb's two rows; and its girder under the kerb the rail is drawn with)
    offset = {"rail": 0.0, "railn": 1.0, "wall": 0.0, "girder": float(RAIL), "girdern": float(RAIL + 2),
              "under": float(RAIL + SLAB)}
    for f in faces:
        pts, uvs, tag, at = f
        if not tag.startswith("deck.flank_") or tag.endswith("~clamp"):
            out.append(f)
            continue
        kind = tag[len("deck.flank_"):].split("_")[0]
        # its two ends: along z, or along x for a face across it (a wall's end)
        k = 2 if len({p[2] for p in pts}) == 2 else 0
        ends = []
        for w in sorted({p[k] for p in pts}):
            here = [p for p in pts if p[k] == w]
            ends.append((here[0][0], here[0][2], min(p[1] for p in here), max(p[1] for p in here)))
        (xa, za, ba, ta), (xb, zb, bb, tb) = ends
        length = ((xb - xa) ** 2 + (zb - za) ** 2) ** 0.5
        # one across z (a rail or front going on straight at a corner) at
        # the map's own phase: its posts stand where the drawn rail's do
        shift = xa - math.floor(xa / 16.0) * 16.0 if k == 0 else 0.0
        ra, rb = ta + offset[kind], tb + offset[kind]
        # the face in (s along it, d under the rail's top)
        poly = [(0.0, ra - ta), (length, rb - tb), (length, rb - bb), (0.0, ra - ba)]
        dmax = max(q[1] for q in poly)
        bands = [(0.0, float(RAIL), 0.0)]
        k = 0
        while RAIL + k * GIRDER < dmax:
            bands.append((RAIL + k * GIRDER, RAIL + (k + 1) * GIRDER, float(k * GIRDER)))
            k += 1
        for (d0, d1, back) in bands:
            for t in range(int((length + shift) // 16) + 1):
                s0, s1 = max(0.0, 16.0 * t - shift), min(length, 16.0 * t + 16 - shift)
                if s1 - s0 < 1e-6:
                    continue
                piece = poly
                for axis, val, keep in ((0, s0, True), (0, s1, False), (1, d0, True), (1, d1, False)):
                    if len(piece) >= 3:
                        piece = vb.clip(piece, axis, val, keep)
                if len(piece) < 3:
                    continue
                world, flank_uv = [], []
                for (ss, dd) in piece:
                    f_ = ss / length if length else 0.0
                    ref = ra + (rb - ra) * f_
                    world.append((xa + (xb - xa) * f_, ref - dd, za + (zb - za) * f_))
                    flank_uv.append((ss - (16.0 * t - shift), dd - back))
                # along a ramp the rail and its girder follow the slope, as a
                # stair's railing does: sheared, not stretched
                out.append((world, flank_uv, tag + ("~proj" if abs(rb - ra) > 1e-6 else ""), at))
    return out


def _green(rgb):
    """A leaf's colour (a tree by the road), never the road's."""
    r, g, b = rgb
    return g > r + 16 and g > b + 16


def _components(pixels):
    """The 8-connected pieces of a set of pixels, each a sorted list."""
    seen, out = set(), []
    for p in sorted(pixels):
        if p in seen:
            continue
        seen.add(p)
        todo, comp = [p], []
        while todo:
            q = todo.pop()
            comp.append(q)
            for i in (-1, 0, 1):
                for j in (-1, 0, 1):
                    r = (q[0] + i, q[1] + j)
                    if r in pixels and r not in seen:
                        seen.add(r)
                        todo.append(r)
        out.append(sorted(comp))
    return out


def _runs(values):
    """Consecutive runs of sorted integers as (first, last)."""
    out = []
    for v in values:
        if out and v == out[-1][1] + 1:
            out[-1] = (out[-1][0], v)
        else:
            out.append((v, v))
    return out


def _split(a, b, size=16):
    """[a, b) cut where a cell of the map ends."""
    out = []
    while a < b:
        e = min(b, (a // size + 1) * size)
        out.append((a, e))
        a = e
    return out


_READ = {}


def bridge(layout_id):
    """The bridge of a layout, read once per run."""
    if layout_id not in _READ:
        _READ[layout_id] = Bridge(layout_id)
    return _READ[layout_id]


def _world_cover(faces, w, h):
    """What the faces stand over, world cell by world cell: {cell: tallest
    point (pixels, at most the deck's road)} and {cell: 16 rows of 16 bits,
    the plan the deck and its rails cover}. What lighting casts shadows from
    (a cell as a box that tall, or its footprint): the deck stands DECK
    pixels south of where it is drawn, so its cells' own rectangles would
    not tell it. A walker up on the deck is over the box, not in it."""
    tops, plan = collections.defaultdict(float), collections.defaultdict(set)
    for pts, uvs, tag, at in faces:
        ys = [p[1] for p in pts]
        if max(ys) < 1.0:
            continue
        x0, x1 = min(p[0] for p in pts), max(p[0] for p in pts)
        z0, z1 = min(p[2] for p in pts), max(p[2] for p in pts)
        for cz in range(int(z0 // 16), int(z1 // 16) + 1):
            for cx in range(int(x0 // 16), int(x1 // 16) + 1):
                if not (0 <= cx < w and 0 <= cz < h):
                    continue
                piece = [(p[0], p[1], p[2]) for p in pts]
                for axis, val, keep in ((0, cx * 16.0, True), (0, cx * 16.0 + 16, False),
                                        (2, cz * 16.0, True), (2, cz * 16.0 + 16, False)):
                    if len(piece) >= 3:
                        piece = vb.clip(piece, axis, val, keep)
                if len(piece) < 3:
                    continue
                tops[(cx, cz)] = max(tops[(cx, cz)], min(DECK, max(p[1] for p in piece)))
                # the plan it covers, pixel by pixel (a face lying flat)
                xs = [p[0] for p in piece]
                zs = [p[2] for p in piece]
                area = sum(piece[i][0] * piece[(i + 1) % len(piece)][2]
                           - piece[(i + 1) % len(piece)][0] * piece[i][2] for i in range(len(piece)))
                if abs(area) < 1e-6:
                    continue    # upright: no plan
                for pz in range(int(min(zs)), int(-(-max(zs) // 1))):
                    for px in range(int(min(xs)), int(-(-max(xs) // 1))):
                        if _inside_xz(piece, px + 0.5, pz + 0.5):
                            plan[(cx, cz)].add((px - cx * 16, pz - cz * 16))
    masks = {}
    for c, got in plan.items():
        if 0 < len(got) < 240:
            masks[c] = tuple(sum(1 << x for x in range(16) if (x, z) in got) for z in range(16))
    return tops, masks


def _inside_xz(poly, x, z):
    """Is (x, z) inside the convex polygon's plan?"""
    sign = 0
    for i in range(len(poly)):
        ax, az = poly[i][0], poly[i][2]
        bx, bz = poly[(i + 1) % len(poly)][0], poly[(i + 1) % len(poly)][2]
        c = (bx - ax) * (z - az) - (bz - az) * (x - ax)
        if abs(c) < 1e-9:
            continue
        if sign == 0:
            sign = 1 if c > 0 else -1
        elif (c > 0) != (sign > 0):
            return False
    return sign != 0


def block_specs(spec):
    """One model per cell of the road and the ground under it, found once:
    a cell drawn and built like another is the same model, placed at both."""
    out = []
    for lid in BRIDGES:
        br = bridge(lid)
        faces = br.quads()
        curb_of = {(x, y): t for (y, x0, x1, t) in br.curbs for x in range(x0, x1)}
        flank = br.flank_sample()
        ground = {c: br.ground_for(*c) for c in br.drawn_cells()}
        tops, masks = _world_cover(faces, br.w, br.h)
        patch_of = collections.defaultdict(dict)
        for (u, v), rgb in br.patch_px.items():
            patch_of[(u // 16, v // 16)][(u, v)] = rgb
        # the plain water or ground the deck stands over, south of where it is
        # drawn: the model's too, so the sun knows the deck is over it
        plain = br.plain_grounds()
        for c in tops:
            if c not in ground and tops[c] > 0 and br.art_layout.metatile(*c) in plain:
                ground[c] = br.art_layout.metatile(*c)
        groups = collections.defaultdict(list)
        for f in faces:
            u, v = f[3]
            cell = (int(u) // 16, int(v) // 16)
            if cell not in ground:
                cell = min(ground, key=lambda c: abs(c[0] - cell[0]) + abs(c[1] - cell[1]))
            groups[(cell[0] // BLOCK, cell[1] // BLOCK, ground[cell])].append(f)
        cells_of = collections.defaultdict(set)
        for c, g in ground.items():
            cells_of[(c[0] // BLOCK, c[1] // BLOCK, g)].add(c)
        # the rails drawn beside every ramp are its slot's walls: judged nowhere
        stretched = br.stretched()
        apx = br.art.load()
        seen = {}
        for key in sorted(cells_of):
            bx, by, g = key
            owned = cells_of[key]
            x0, y0 = bx * BLOCK, by * BLOCK
            w, h = min(BLOCK, br.w - x0), min(BLOCK, br.h - y0)
            mine = groups.get(key, [])
            dressed = any(t.startswith("deck.flank_") and not t.endswith("~clamp")
                          for _, _, t, _ in mine)
            paved = any(t.endswith("~road") for _, _, t, _ in mine)
            # the flank under the cells, for the sides it dresses, and the
            # plain road under it, for the deck under the south rail - each
            # under a row repeating the last one over it (a face ending on
            # the row sampled a hair past it reads its own colour)
            flank_at = h * 16 + 1
            road_at = flank_at + (FLANK_ROWS + 1 if dressed else 0)
            # and a curb going on under the deck: its tile's stones
            curb = [(c, curb_of[c]) for c in sorted(owned) if c in curb_of]
            curb_at = road_at + (ROAD_ROWS if paved else 0)
            art = vb.Image.new("RGBA", (w * 16, curb_at + (16 if curb else 0)), (0, 0, 0, 0))
            bpx = art.load()
            for (cx, cy) in owned:
                for j in range(16):
                    for i in range(16):
                        u, v = cx * 16 + i, cy * 16 + j
                        bpx[u - x0 * 16, v - y0 * 16] = apx[u, v]
            art.paste(art.crop((0, h * 16 - 1, w * 16, h * 16)), (0, h * 16))
            if dressed:
                art.paste(flank, (0, flank_at))
                art.paste(flank.crop((0, FLANK_ROWS - 1, 16, FLANK_ROWS)), (0, flank_at + FLANK_ROWS))
            if paved:
                art.paste(br.road_sample(), (0, road_at))
            for (cx, cy), t in curb:
                art.paste(br.curb_stones(t), ((cx - x0) * 16, curb_at))
            local = []
            for pts, uvs, tag, at in mine:
                if tag.startswith("deck.flank_") and not tag.endswith("~clamp"):
                    uvs = [(t[0], flank_at + t[1]) for t in uvs]
                elif tag.endswith("~road"):
                    cu, cv = int(at[0]) // 16 * 16, int(at[1]) // 16 * 16
                    uvs = [(t[0] - cu, road_at + 1 + t[1] - cv) for t in uvs]
                else:
                    uvs = [(t[0] - x0 * 16, t[1] - y0 * 16) for t in uvs]
                local.append(([(p[0] - x0 * 16, p[1], p[2] - y0 * 16) for p in pts], uvs, tag))
            exact = [(i * 16, j * 16, i * 16 + 16, j * 16 + 16)
                     for (cx, cy) in owned for i, j in [(cx - x0, cy - y0)]
                     if not any((cx * 16 + a, cy * 16 + b) in stretched
                                for a in range(16) for b in range(16))]
            cell_tops = tuple(int(round(tops.get((x0 + i, y0 + j), 0.0)))
                              for j in range(h) for i in range(w))
            cell_masks = [masks.get((x0 + i, y0 + j)) for j in range(h) for i in range(w)]
            # what of the drawing is laid on the ground (Bridge.quads lay):
            # the placement's ground patches, lit as the terrain is
            patches = []
            for (cx, cy) in sorted(owned):
                laid = patch_of.get((cx, cy))
                if laid:
                    img = vb.Image.new("RGBA", (16, 16), (0, 0, 0, 0))
                    for (u, v), rgb in laid.items():
                        img.putpixel((u - cx * 16, v - cy * 16), tuple(rgb[:3]) + (255,))
                    patches.append((cx - x0, cy - y0, img))
            sig = (art.tobytes(), tuple(sorted((c[0] - x0, c[1] - y0) for c in owned)), g,
                   cell_tops, tuple(cell_masks),
                   tuple((i, j, img.tobytes()) for (i, j, img) in patches), tuple(curb),
                   tuple((tuple(tuple(round(c, 3) for c in p) for p in pts),
                          tuple(tuple(round(c, 3) for c in t) for t in uvs), tag)
                         for pts, uvs, tag in local))
            if sig in seen:
                seen[sig]["placed"].append((lid, x0, y0, g))
                continue
            name = "%s_%s_%d_%d" % (spec["name"], lid[len("LAYOUT_"):].lower(), x0, y0)
            sp = {"name": name, "layout": lid, "rect": (x0, y0, w, h), "ground": [g], "gutter": True,
                  "deck_of": lid,
                  "art": art, "owned": {(c[0] - x0, c[1] - y0) for c in owned},
                  "parts": (lambda faces, stones: (lambda: [Faces("deck", faces)] + stones))(
                      local, [_Curb(br.curb_stones(t), curb_at, (cx - x0) * 16, (cy - y0) * 16)
                              for (cx, cy), t in curb]),
                  "exact": exact, "placed": [(lid, x0, y0, g)], "patches": patches,
                  "tops": cell_tops, "footprints": cell_masks}
            seen[sig] = sp
            out.append(sp)
    return out


def deck_table():
    """[(layout id, x, y, top, bottom, elevation)] of every bridge."""
    out = []
    for lid in BRIDGES:
        for (x, y), (top, bottom, gate) in bridge(lid).decks().items():
            out.append((lid, x, y, top, bottom, gate))
    return out


class _Curb:
    """A lawn's curb going on under the deck (Bridge._curbs), as the `curb`
    component builds it beside the bridge: a Relief of the tile's stones as
    high as that one's, here in the cell at (dx, dz) of the model, its
    texture the band of the model's art at row `band`. The drawing's stones
    beside the rails are its own; the rest the bridge stands in front of."""

    def __init__(self, stones, band, dx, dz):
        self.stones, self.band, self.dx, self.dz = stones, band, dx, dz

    def emit(self, mesh):
        import voxel_building_specs as specs
        spec = next(sp for sp in specs.SPECS if sp.get("name") == "curb")
        height = spec["components"]["height"]
        tmp = vb.Mesh()
        vb.Relief("curb", self.stones, height, _front_tile(self.stones, height)).emit(tmp)
        for (tri, shade, tag) in tmp.tris:
            mesh.tri(*[(x + self.dx, y, z + self.dz, u + self.dx, v + self.band)
                       for (x, y, z, u, v) in tri], shade, "deck." + tag)


def _front_tile(art, height):
    """An 8-column tile of the stones' own front for the faces the drawing
    never shows (gen_voxel_buildings pick_side): columns whose runs end on
    the same row with `height` rows of front."""
    W, H = art.size
    px = art.load()
    ends = []
    for u in range(W):
        end = next((v + 1 for v in range(H - 1, -1, -1) if px[u, v][3] >= 128), None)
        full = end is not None and end >= height and all(
            px[u, v][3] >= 128 for v in range(end - height, end))
        ends.append(end if full else None)
    for width in (8, 6, 4):
        best = None
        for u in range(W - width + 1):
            e = ends[u]
            if e is not None and all(ends[u + k] == e for k in range(width)):
                if best is None or e > best[1]:
                    best = (u, e)
        if best is not None:
            u, e = best
            return vb.Tile(u, e - height, u + width, e)
    return vb.Tile(0, 0, 1, 1)


class Faces:
    """Faces already placed: (points, uvs, tag) quads in pixels."""

    def __init__(self, name, faces):
        self.name, self.faces = name, faces

    def emit(self, mesh):
        for pts, uvs, tag in self.faces:
            shade = vb.SHADE_ART
            if tag.startswith("deck.flank_") and not tag.endswith("~clamp"):
                facing = tag[len("deck.flank_"):].split("_")[1][0]
                # (a ramp's slot's wall ends facing south, as the front does;
                # the north girder faces away, in the back's shade)
                shade = {"w": vb.SHADE_WEST, "e": vb.SHADE_EAST, "n": vb.SHADE_BACK}.get(facing, vb.SHADE_ART)
            elif tag.startswith("deck.pier_side_"):
                shade = vb.SHADE_WEST if tag[len("deck.pier_side_")] == "w" else vb.SHADE_EAST
            elif tag.startswith(("deck.girder_n", "deck.under")):
                shade = vb.SHADE_BACK
            mesh.poly([p + t for p, t in zip(pts, uvs)], shade, self.name + "." + tag)


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("layout", nargs="?", default="LAYOUT_ROUTE110")
    ap.add_argument("--png", default=None, help="the classes over the drawing")
    args = ap.parse_args()
    br = Bridge(args.layout)
    faces = br.quads()
    print("%s: %d road px, %d drawn px, %d faces, ramps %s"
          % (args.layout, len(br.road), len(br.pixels), len(faces), br.ramps))
    d = br.decks()
    print("deck cells: %d (%d gated)" % (len(d), sum(1 for v in d.values() if v[2])))
    if args.png:
        img = vb.Image.new("RGB", br.art.size, (0, 0, 0))
        p = img.load()
        tags = {}
        for pts, uvs, tag, at in faces:
            for (u, v) in [(int(x), int(y)) for x, y in uvs]:
                pass
            tags.setdefault(tag.split("~")[0], 0)
        colours = {"deck.top": (200, 200, 200), "deck.front": (0, 120, 255),
                   "deck.rail_n": (255, 200, 0), "deck.rail_side": (0, 255, 0),
                   "deck.post": (255, 0, 255), "deck.ramp": (255, 120, 0)}
        for pts, uvs, tag, at in faces:
            t = tag.split("~")[0]
            if t not in colours:
                continue
            us = [q[0] for q in uvs]
            vs = [q[1] for q in uvs]
            for v in range(int(min(vs)), int(max(vs))):
                for u in range(int(min(us)), int(max(us))):
                    if (u, v) in br.pixels:
                        p[u, v] = colours[t]
        img.save(args.png)


if __name__ == "__main__":
    main()
