#!/usr/bin/env python3
"""Buildings rebuilt in 3D from their own drawing, pixel for pixel.

The premise
-----------
An Emerald building is drawn in a fixed oblique projection: a wall is seen
face on and the ground straight from above, both at one pixel per pixel. That
is exactly what an orthographic camera pitched at 45 degrees sees (scaled by
sqrt 2). A point (X, Y, Z) of the world - X east, Y up, Z south, in pixels -
lands on the art at

    u = X        v = Z - Y

so the drawing is not a texture to be wrapped around a guessed box: it is the
model seen from one direction. Every surface is textured by that fact - one
art texel per GBA pixel seen across the surface at 45 degrees, `density_check`
proves it for every triangle - so nothing is ever stretched or squashed, and
`ortho_check` renders the model in the GBA projection and compares it pixel
for pixel with the art over the regions a spec declares exact.

The drawing is shallower than the building. Emerald draws a four-row house in
five rows of art, which read literally gives a house two tiles deep. A model
keeps the real footprint (the collision) and covers the depth the drawing
lacks the way the building would: more courses of the same roof tiles, more
plaster from the same column (`Strip`, `Tile`). What the drawing never shows -
the sides, the back - is dressed from the same art. No new pixels are painted
and nothing depends on the game's camera, so the model stays right at any
pitch or yaw.

The ground
----------
Emerald paints the ground under a building's edges into the building's own
metatiles. A pixel belongs to the building when its top layer is opaque or
when its bottom-layer 8x8 tile is not one of the ground tiles around it. The
rest is transparent in the model's texture, and the cells the building covers
get the plain ground metatile drawn flat underneath instead.

Modelling vocabulary
--------------------
  Proj     a wall placed at its drawn depth: u = X, v = Z - Y, exactly.
  Strip    courses laid up a face from its eave: the drawn eave rows, then
           repeats of a clean course, at the 45-degree density.
  Tile     an art rectangle repeated 1:1 (side walls, corner posts).
  Prism    a (Z, Y) polygon extruded over X; its edges take Proj/Strip/Tile,
           its two end caps are tiled by Bands.
  HipRoof  a hipped roof with ridge teeth and cap, all four slopes tiled.
  Frustum  walls on any convex plan, a sloped roof band and a flat top
           (the octagonal Pokemon Center and Mart).
  Cylinder a short round thing (a vent), projected where it is drawn.
  Vault    a barrel vault on a flat top (the Pokemon Center's crown).
  Walls    walls along an open plan, projected (a chamfered porch).
  Relief   a solid read column by column off its drawing (hedges, walls).
  Mound    a rounded rock: every drawn pixel lifted onto its own dome.
"""

import math
import os
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dump_region_art import incbin_map, read_u16, Tilesets, ROOT  # noqa: E402

import json  # noqa: E402

NUM_PRIMARY = 512
EPS = 1e-6

# Face shades for the model's faces: the drawing already carries Emerald's lighting, so a face the GBA camera sees
# keeps shade 1 and only the sides the drawing never lit are darkened.
SHADE_ART = 1.0
SHADE_WEST = 0.80
SHADE_EAST = 0.72
SHADE_BACK = 0.66
# A face the drawing never shows whose winding is its outside (a rock's back,
# turned every way): the console lights it as wound, not by a side's rule.
SHADE_WOUND = 0.90


# ── Art ─────────────────────────────────────────────────────────────────────

_LAYOUTS = []
# A metatile draws the same in every layout over the same tileset pair:
# (primary, secondary, metatile, layer(s)) -> its subtiles, its image.
_SUBTILES = {}
_CELL_IMAGES = {}


def _layouts_json():
    """layouts.json, parsed once per process (LayoutArt only reads it)."""
    if not _LAYOUTS:
        _LAYOUTS.append(json.load(open(os.path.join(ROOT, "data", "layouts", "layouts.json"),
                                       encoding="utf-8"))["layouts"])
    return _LAYOUTS[0]


class LayoutArt:
    """A layout's metatiles, composed the way the GBA composes them."""

    def __init__(self, layout_id):
        self.attrs = incbin_map(r"gMetatileAttributes_(\w+)\[\]\s*=\s*INCBIN_U16\(\"([^\"]+)\"\)")
        self.mts = incbin_map(r"gMetatiles_(\w+)\[\]\s*=\s*INCBIN_U16\(\"([^\"]+)\"\)")
        layouts = _layouts_json()
        entry = next(e for e in layouts if e["id"] == layout_id)
        self.layout_index = layouts.index(entry) + 1
        self.id = layout_id
        self.blocks = read_u16(os.path.join(ROOT, entry["blockdata_filepath"]))
        self.w, self.h = entry["width"], entry["height"]
        self.primary, self.secondary = entry["primary_tileset"], entry["secondary_tileset"]
        # A layout can carry no secondary tileset: the source tree writes "0",
        # the builder's layouts.json writes "NULL". Such a layout has nothing to
        # compose and the callers skip it; keep it constructible all the same.
        self.has_secondary = self.secondary not in ("0", "NULL")
        self.ts = Tilesets(self.primary, self.secondary if self.has_secondary else self.primary)
        self.pm = read_u16(self.mts[self.primary])
        self.sm = read_u16(self.mts[self.secondary]) if self.has_secondary else []

    def metatile(self, x, y):
        return self.blocks[y * self.w + x] & 0x3FF

    def entries(self, m):
        if m < NUM_PRIMARY:
            return self.pm[m * 8:m * 8 + 8]
        return self.sm[(m - NUM_PRIMARY) * 8:(m - NUM_PRIMARY) * 8 + 8]

    def subtiles(self, m, layer):
        """Four 8x8 blocks of (rgb, colour index), flips applied, row major.
        Shared between calls: callers only read them."""
        key = (self.primary, self.secondary, m, layer)
        if key not in _SUBTILES:
            _SUBTILES[key] = self._subtiles(m, layer)
        return _SUBTILES[key]

    def _subtiles(self, m, layer):
        out = []
        for entry in self.entries(m)[layer * 4:layer * 4 + 4]:
            data = self.ts.subtile(entry & 0x3FF, (entry >> 12) & 0xF)
            fx, fy = entry & 0x400, entry & 0x800
            out.append([data[(7 - y if fy else y) * 8 + (7 - x if fx else x)]
                        for y in range(8) for x in range(8)])
        return out

    def ground_tiles(self, metatiles):
        """The bottom-layer 8x8 drawings of the given ground metatiles."""
        found = set()
        for m in metatiles:
            for block in self.subtiles(m, 0):
                found.add(tuple(rgb for rgb, _ in block))
        return found

    def ground_pixels(self, metatiles):
        """The bottom layers of the ground metatiles, pixel by pixel."""
        out = []
        for m in metatiles:
            low = self.subtiles(m, 0)
            grid = [[None] * 16 for _ in range(16)]
            for q in range(4):
                for k in range(64):
                    grid[(q >> 1) * 8 + k // 8][(q & 1) * 8 + k % 8] = low[q][k][0]
            out.append(grid)
        return out

    def building_art(self, x0, y0, w, h, ground, cells=None, ground_px=None, upper=False,
                     swap=None):
        """RGBA image of the cells, with every ground pixel transparent.

        A bottom-layer 8x8 block that is one of the ground's is ground whole.
        Where the building's own blocks carry the ground painted in beside it
        (a hedge's flank, a fence post) the test goes down to the pixel: with
        `ground_px`, a bottom-layer pixel equal to a ground metatile's pixel at
        the same place in its cell is ground too. `cells` limits the image to
        the (cx, cy) cells that belong to the object. With `upper`, only the
        upper layer: an object drawn whole on it (a railing) over a bottom
        layer that is the map's ground, edges and all. `swap` draws some
        metatiles as others: an object with something else drawn over it (a
        hedge under a tree's leaves), as it is without.
        """
        img = Image.new("RGBA", (w * 16, h * 16), (0, 0, 0, 0))
        px = img.load()
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
                        X, Y = cx * 16 + lx, cy * 16 + ly
                        rgb1, i1 = high[q][k]
                        rgb0, _ = low[q][k]
                        if i1:
                            px[X, Y] = rgb1 + (255,)
                        elif upper:
                            continue
                        elif not is_ground:
                            if ground_px and any(g[ly][lx] == rgb0 for g in ground_px):
                                continue
                            px[X, Y] = rgb0 + (255,)
        return img

    def cell_image(self, m, layers=(0, 1)):
        """A whole metatile as the ground renderer draws it (with `layers`
        (0,), its lower layer alone)."""
        key = (self.primary, self.secondary, m, tuple(layers))
        if key not in _CELL_IMAGES:
            _CELL_IMAGES[key] = self._cell_image(m, layers)
        return _CELL_IMAGES[key].copy()

    def _cell_image(self, m, layers):
        img = Image.new("RGBA", (16, 16), (0, 0, 0, 255))
        px = img.load()
        low, high = self.subtiles(m, 0), self.subtiles(m, 1)
        for q in range(4):
            for k in range(64):
                X = (q & 1) * 8 + k % 8
                Y = (q >> 1) * 8 + k // 8
                rgb1, i1 = high[q][k]
                px[X, Y] = (rgb1 if i1 and 1 in layers else low[q][k][0]) + (255,)
        return img

    # ── animated tiles ─────────────────────────────────────────────────────

    def upper_source(self, x, y):
        """Where pixel (x, y) of the map's drawing (pixels from its top-left)
        comes from on the upper layer: (tile, pixel, palette) - the tile's
        number in the GBA's BG VRAM (the secondary tileset's from 512), the
        pixel's index in it (row * 8 + column, flips applied), the palette's
        number."""
        q = (y % 16 // 8) * 2 + (x % 16 // 8)
        entry = self.entries(self.metatile(x // 16, y // 16))[4 + q]
        px, py = x % 8, y % 8
        if entry & 0x400:
            px = 7 - px
        if entry & 0x800:
            py = 7 - py
        return entry & 0x3FF, py * 8 + px, (entry >> 12) & 0xF

    def tile_pixel(self, tile, pixel, frame=None):
        """A tile's colour index at `pixel`: the tilesets', or for a tile
        `frame` holds ({tile: its 32 bytes}) an animation frame's."""
        data = (frame or {}).get(tile)
        if data is None:
            tiles, local = ((self.ts.pt, tile) if tile < NUM_PRIMARY
                            else (self.ts.st, tile - NUM_PRIMARY))
            data = tiles[local * 32:local * 32 + 32]
        if len(data) < 32:
            return 0
        b = data[pixel // 8 * 4 + pixel % 8 // 2]
        return (b >> 4) if pixel & 1 else (b & 0xF)

    def palette(self, palette_id):
        """A palette's sixteen colours, as the subtiles read them (Emerald's
        split: 0-5 the primary tileset's, 6-12 the secondary's)."""
        palettes = self.ts.pp if palette_id < 6 else self.ts.sp
        return palettes[palette_id] if palette_id < len(palettes) else [(255, 0, 255)] * 16


def anim_frame(path, first_tile):
    """One frame of a tileset animation (data/tilesets/.../anim/*/N.4bpp,
    whose tiles the game copies over BG VRAM from `first_tile` on, as
    tileset_anims.c does): {tile: its 32 bytes}. The .4bpp, not the .png:
    the release builder rebuilds it from the player's ROM, where it lies as
    is (tools/vtree_manifest.py)."""
    data = open(os.path.join(ROOT, path), "rb").read()
    return {first_tile + t: data[t * 32:t * 32 + 32] for t in range(len(data) // 32)}


# ── Texture mappings ───────────────────────────────────────────────────────

class Proj:
    """The GBA projection: u = X, v = Z - Y, with the rows clamped.

    Rows outside [lo, hi) are drawn by some other surface in the art; where
    this face reaches into them it is hidden in the GBA view (under an eave,
    behind a ridge) and takes the nearest row of its own instead of borrowing
    another surface's pixels.
    """

    def __init__(self, lo=None, hi=None):
        self.lo, self.hi = lo, hi


PROJ = Proj()


class Tile:
    """An art rectangle repeated at one texel per pixel.

    `s` runs along the face's horizontal direction (X for an edge face, Z for
    a cap), `t` runs down from `top`. `period` defaults to the rectangle.
    """

    def __init__(self, u0, v0, u1, v1, s0=0.0, top=None, flip=False):
        self.rect = (u0, v0, u1, v1)
        self.s0, self.top, self.flip = s0, top, flip


class Band:
    """A cap material over the heights [y0, y1).

    `tile` repeats along Z from `z0` (southwards edge first). `front` puts an
    extra art strip at the south end of the band - a facade's corner post.
    """

    def __init__(self, y0, y1, tile, z0, front=None, back=None, z1=None):
        self.y0, self.y1, self.tile, self.z0 = y0, y1, tile, z0
        self.front, self.back = front, back
        self.length = (z0 - z1) if z1 is not None else 1e9


class Prism:
    """A (Z, Y) polygon extruded over [x0, x1].

    `poly` lists (z, y) points counter-clockwise when seen from the west
    (+Z to the right, +Y up): south face first is the natural reading order.
    `edges` maps an edge index (from point i to point i+1) to its material;
    anything absent is PROJ when it faces the GBA camera and skipped when it
    faces straight down. `caps` is the band list for both ends, or None.
    """

    def __init__(self, name, x0, x1, poly, edges=None, caps=None,
                 west=True, east=True, skip=()):
        self.name, self.x0, self.x1 = name, x0, x1
        self.poly = poly
        self.edges = edges or {}
        self.caps = caps
        self.west, self.east = west, east
        self.skip = set(skip)


# ── Geometry kernel ───────────────────────────────────────────────────────

class Mesh:
    def __init__(self):
        self.tris = []  # ((x,y,z,u,v) * 3, shade, tag)

    def tri(self, a, b, c, shade, tag):
        self.tris.append(((a, b, c), shade, tag))

    def poly(self, pts, shade, tag):
        for i in range(1, len(pts) - 1):
            self.tri(pts[0], pts[i], pts[i + 1], shade, tag)


def clip(poly, axis, value, keep_greater):
    """Sutherland-Hodgman against one axis-aligned half plane.

    `poly` is a list of tuples; `axis` indexes the coordinate tested. Every
    other coordinate is interpolated, so texture coordinates carried in the
    tuple stay exact.
    """
    out = []
    n = len(poly)
    for i in range(n):
        a, b = poly[i], poly[(i + 1) % n]
        ina = (a[axis] >= value - EPS) if keep_greater else (a[axis] <= value + EPS)
        inb = (b[axis] >= value - EPS) if keep_greater else (b[axis] <= value + EPS)
        if ina:
            out.append(a)
        if ina != inb:
            t = (value - a[axis]) / (b[axis] - a[axis])
            out.append(tuple(a[k] + (b[k] - a[k]) * t for k in range(len(a))))
    return out


def triangulate(poly):
    """Ear clipping for a simple polygon of (z, y) points, either winding."""
    pts = list(range(len(poly)))
    area = sum(poly[i][0] * poly[(i + 1) % len(poly)][1] -
               poly[(i + 1) % len(poly)][0] * poly[i][1] for i in range(len(poly)))
    sign = 1 if area > 0 else -1
    tris = []

    def cross(o, a, b):
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])

    guard = 0
    while len(pts) > 3 and guard < 10000:
        guard += 1
        for k in range(len(pts)):
            i0, i1, i2 = pts[k - 1], pts[k], pts[(k + 1) % len(pts)]
            a, b, c = poly[i0], poly[i1], poly[i2]
            if cross(a, b, c) * sign <= EPS:
                continue
            inside = False
            for j in pts:
                if j in (i0, i1, i2):
                    continue
                p = poly[j]
                if (cross(a, b, p) * sign > EPS and cross(b, c, p) * sign > EPS
                        and cross(c, a, p) * sign > EPS):
                    inside = True
                    break
            if inside:
                continue
            tris.append((i0, i1, i2))
            del pts[k]
            break
        else:
            break
    if len(pts) == 3:
        tris.append(tuple(pts))
    return tris


def tile_pieces(poly2d, tile, s_of, t_of):
    """Split a planar convex polygon into pieces that each sit in one tile.

    `poly2d` carries (s, t, ...) where s, t are the face coordinates in
    pixels; each returned piece has its (u, v) appended. Within one piece the
    art mapping is a translation, so its UVs are exact.
    """
    u0, v0, u1, v1 = tile.rect
    pw, ph = u1 - u0, v1 - v0
    ss = [p[0] for p in poly2d]
    ts = [p[1] for p in poly2d]
    out = []
    i0 = math.floor((min(ss) - tile.s0) / pw + EPS)
    i1 = math.ceil((max(ss) - tile.s0) / pw - EPS)
    j0 = math.floor(min(ts) / ph + EPS)
    j1 = math.ceil(max(ts) / ph - EPS)
    for i in range(i0, i1):
        for j in range(j0, j1):
            piece = poly2d
            sa, sb = tile.s0 + i * pw, tile.s0 + (i + 1) * pw
            ta, tb = j * ph, (j + 1) * ph
            piece = clip(piece, 0, sa, True)
            if len(piece) >= 3:
                piece = clip(piece, 0, sb, False)
            if len(piece) >= 3:
                piece = clip(piece, 1, ta, True)
            if len(piece) >= 3:
                piece = clip(piece, 1, tb, False)
            if len(piece) < 3:
                continue
            res = []
            for p in piece:
                fs = p[0] - sa
                if tile.flip:
                    fs = pw - fs
                res.append(p + (u0 + fs, v0 + (p[1] - ta)))
            out.append(res)
    return out


def edge_normal(a, b):
    """Outward normal (nz, ny) of a CCW polygon edge a -> b in (z, y)."""
    dz, dy = b[0] - a[0], b[1] - a[1]
    length = math.hypot(dz, dy)
    return (dy / length, -dz / length)


def polygon_ccw(poly):
    area = sum(poly[i][0] * poly[(i + 1) % len(poly)][1] -
               poly[(i + 1) % len(poly)][0] * poly[i][1] for i in range(len(poly)))
    return area > 0


def emit_prism(mesh, pr):
    poly = pr.poly if polygon_ccw(pr.poly) else list(reversed(pr.poly))
    if poly is not pr.poly:
        raise ValueError("%s: polygon must be counter-clockwise (+Z right, +Y up)" % pr.name)
    n = len(poly)
    x0, x1 = pr.x0, pr.x1

    def proj_uv(x, z, y, lo, hi):
        v = z - y
        if lo is not None and v < lo:
            v = lo + 0.5
        if hi is not None and v > hi:
            v = hi - 0.5
        return (x, v)

    for i in range(n):
        if i in pr.skip:
            continue
        a, b = poly[i], poly[(i + 1) % n]
        nz, ny = edge_normal(a, b)
        mat = pr.edges.get(i)
        facing = nz + ny  # dot with the GBA view direction (0, 1, 1)
        if mat is None:
            if ny < -0.5 or facing <= EPS:
                continue  # underside, or edge-on / away and not dressed
            mat = PROJ
        shade = SHADE_ART if facing > EPS else SHADE_BACK
        tag = "%s.e%d" % (pr.name, i)
        if isinstance(mat, Proj):
            # Split the edge where the projected row crosses a clamp, so every
            # piece is either exactly projected or one constant row.
            cuts = [0.0, 1.0]
            va, vb = a[0] - a[1], b[0] - b[1]
            for lim in (mat.lo, mat.hi):
                if lim is not None and abs(vb - va) > EPS:
                    t = (lim - va) / (vb - va)
                    if EPS < t < 1 - EPS:
                        cuts.append(t)
            cuts.sort()
            for k in range(len(cuts) - 1):
                ta, tb = cuts[k], cuts[k + 1]
                pa = (a[0] + (b[0] - a[0]) * ta, a[1] + (b[1] - a[1]) * ta)
                pb = (a[0] + (b[0] - a[0]) * tb, a[1] + (b[1] - a[1]) * tb)
                mid_v = (pa[0] - pa[1] + pb[0] - pb[1]) / 2
                clamp_lo = mat.lo is not None and mid_v < mat.lo
                clamp_hi = mat.hi is not None and mid_v > mat.hi
                quad = []
                for (x, p) in ((x0, pa), (x1, pa), (x1, pb), (x0, pb)):
                    if clamp_lo:
                        uv = (x, mat.lo + 0.5)
                    elif clamp_hi:
                        uv = (x, mat.hi - 0.5)
                    else:
                        uv = proj_uv(x, p[0], p[1], None, None)
                    quad.append((x, p[1], p[0]) + uv)
                mesh.poly(quad, shade, tag + ("~clamp" if clamp_lo or clamp_hi else ""))
        elif isinstance(mat, Strip):
            start, end = (b, a) if mat.from_end else (a, b)
            tdir = _unit((0.0, end[1] - start[1], end[0] - start[0]))
            pts = [(x0, a[1], a[0]), (x1, a[1], a[0]), (x1, b[1], b[0]), (x0, b[1], b[0])]
            strip_face(mesh, pts, (0.0, start[1], start[0]), (1.0, 0.0, 0.0), tdir,
                       mat, shade, tag)
        elif isinstance(mat, Tile):
            # s along X, t measured down the edge from its upper end.
            length = math.hypot(b[0] - a[0], b[1] - a[1])
            upper, lower = (a, b) if a[1] >= b[1] else (b, a)
            face = [(x0, 0.0), (x1, 0.0), (x1, length), (x0, length)]
            for piece in tile_pieces(face, mat, None, None):
                pts = []
                for s, t, u, v in piece:
                    f = t / length if length > EPS else 0.0
                    z = upper[0] + (lower[0] - upper[0]) * f
                    y = upper[1] + (lower[1] - upper[1]) * f
                    pts.append((s, y, z, u, v))
                mesh.poly(pts, shade, tag)

    if pr.caps is None:
        return
    tris = triangulate(poly)
    for side, x, shade in (("w", x0, SHADE_WEST), ("e", x1, SHADE_EAST)):
        if (side == "w" and not pr.west) or (side == "e" and not pr.east):
            continue
        for band in pr.caps:
            for tri in tris:
                piece = [poly[k] for k in tri]
                piece = clip(piece, 1, band.y0, True)
                if len(piece) >= 3:
                    piece = clip(piece, 1, band.y1, False)
                if len(piece) < 3:
                    continue
                emit_cap_piece(mesh, piece, band, x, side, shade,
                               "%s.cap%s" % (pr.name, side))


def emit_cap_piece(mesh, piece, band, x, side, shade, tag):
    """A cap piece tiled by its band: s runs north from the band's z0."""
    top = band.y1 if band.tile.top is None else band.tile.top
    strips = []
    s_front = 0.0
    if band.front is not None:
        fw = band.front.rect[2] - band.front.rect[0]
        strips.append((band.front, 0.0, fw))
        s_front = fw
    s_back = 1e9
    if band.back is not None:
        bw = band.back.rect[2] - band.back.rect[0]
        s_back = band.length - bw
    strips.append((band.tile, s_front, s_back))
    if band.back is not None:
        strips.append((band.back, s_back, s_back + bw))
    for tile, sa, sb in strips:
        pts = [(band.z0 - z, top - y) for z, y in piece]
        pts = clip(pts, 0, sa, True)
        if len(pts) >= 3:
            pts = clip(pts, 0, sb, False)
        if len(pts) < 3:
            continue
        shifted = Tile(*tile.rect, s0=sa, flip=(tile.flip != (side == "w")))
        for sub in tile_pieces(pts, shifted, None, None):
            out = [(x, top - t, band.z0 - s, u, v) for s, t, u, v in sub]
            mesh.poly(out, shade, tag)


# ── Faces laid in courses, and roofs ────────────────────────────────────

class Strip:
    """Tile courses laid up a face from its eave, one texel per GBA pixel.

    A roof is drawn as a few courses of tiles, fewer than a roof of the
    building's real depth needs. Rather than stretch them, the face is built
    the way a roof is: the eave course the art draws (`fixed`, rows [a, b),
    bottom row at the eave) and then as many more courses as the depth takes,
    repeating rows [c, d) of the same art upwards, starting at row `start`
    (default d - 1) so the pattern continues exactly where the fixed rows end.
    With no `repeat` the last fixed row is held (a face the art hides).

    Along the face the art is laid at cos(b) + sin(b) texels per pixel, b the
    face's pitch: the density at which a texel covers exactly one pixel seen
    from 45 degrees across its eave, as the GBA draws every surface. Walls
    and flat tops are then 1:1, and a 45-degree slope is sqrt 2 - which is
    what the drawing's own projection gives it.

    `wrap` = (lo, hi) samples u from that column range only, cyclically, for
    art whose edges are not the material (a verge, a neighbour). Keep hi - lo a
    multiple of the pattern's period so the phase is unchanged. `offset`
    shifts u instead.
    """

    def __init__(self, fixed, repeat=None, start=None, wrap=None, offset=0.0,
                 from_end=False, tail=None, repeat_offset=None):
        self.fixed, self.repeat = fixed, repeat
        self.start = (repeat[1] - 1) if (repeat and start is None) else start
        self.wrap, self.offset, self.from_end = wrap, offset, from_end
        # rows [a, b) laid at the far end of the face - a flat roof's back
        # parapet, which the repeated courses must stop short of
        self.tail = tail
        # u shift for the repeated courses and tail only: columns the drawing
        # fills with something else behind its front rows (a roof unit)
        self.repeat_offset = repeat_offset

    def segments(self, total):
        """(t0, t1, v at t0, constant) runs along the face, in texels."""
        if self.tail is not None:
            ta, tb = self.tail
            length = tb - ta
            if total > length + (self.fixed[1] - self.fixed[0]):
                body = Strip(self.fixed, self.repeat, self.start)
                return body.segments(total - length) + [(total - length, total, float(tb), False)]
        a, b = self.fixed
        out = []
        t = 0.0
        if b > a:
            out.append((0.0, min(total, b - a), float(b), False))
            t = b - a
        if t >= total - EPS:
            return out
        if self.repeat is None:
            out.append((t, total, a + 0.5, True))
            return out
        c, d = self.repeat
        first = self.start + 1 - c
        out.append((t, min(total, t + first), float(self.start + 1), False))
        t += first
        while t < total - EPS:
            out.append((t, min(total, t + (d - c)), float(d), False))
            t += d - c
        return out

    def u_pieces(self, s0, s1):
        """(s0, s1, du) spans: u = s + du inside each."""
        if self.wrap is None:
            return [(s0, s1, self.offset)]
        lo, hi = self.wrap
        period = hi - lo
        out = []
        k = math.floor((s0 - lo) / period + EPS)
        while lo + k * period < s1 - EPS:
            a, b = max(s0, lo + k * period), min(s1, lo + (k + 1) * period)
            if b > a + EPS:
                out.append((a, b, -k * period))
            k += 1
        return out


def strip_face(mesh, pts, origin, sdir, tdir, strip, shade, tag):
    """A planar convex face textured by a Strip.

    `sdir` is the unit direction along the eave (u follows it), `tdir` the unit
    direction up the face from the eave line through `origin` (v runs against
    it). Every piece is affine in its texture, so UVs are exact.
    """
    dens = abs(tdir[1]) + math.hypot(tdir[0], tdir[2])
    loc = []
    for p in pts:
        d = [p[i] - origin[i] for i in range(3)]
        s = sum(d[i] * sdir[i] for i in range(3))
        t = sum(d[i] * tdir[i] for i in range(3)) * dens
        loc.append((s, t) + tuple(p))
    tmax = max(q[1] for q in loc)
    smin, smax = min(q[0] for q in loc), max(q[0] for q in loc)
    fixed_len = strip.fixed[1] - strip.fixed[0]
    for (t0, t1, v0, const) in strip.segments(tmax):
        for (s0, s1, du) in strip.u_pieces(smin, smax):
            if strip.repeat_offset is not None and t0 >= fixed_len - EPS:
                du = strip.repeat_offset
            piece = clip(loc, 1, t0, True)
            for axis, val, keep in ((1, t1, False), (0, s0, True), (0, s1, False)):
                if len(piece) >= 3:
                    piece = clip(piece, axis, val, keep)
            if len(piece) < 3:
                continue
            out = []
            for q in piece:
                v = v0 if const else v0 - (q[1] - t0)
                out.append((q[2], q[3], q[4], q[0] + du, v))
            mesh.poly(out, shade, tag + ("~clamp" if const else ""))


def _unit(v):
    n = math.sqrt(sum(c * c for c in v))
    return tuple(c / n for c in v)


class HipRoof:
    """A hipped roof with its ridge along X, built from its drawing.

    All four sides are tiled slopes, so the roof reads as tiles from every
    side and needs no gable wall. The eave runs round at `y0` plus the fascia,
    the ridge is centred on the eave rectangle, front and back rise at `pitch`
    and the hips come in `run` pixels by the ridge. Front and back lay their
    courses along X; the hips lay the same courses along Z.
    """

    def __init__(self, name, x0, x1, zf, zb, y0, fascia, slope, teeth, cap,
                 pitch, run, ridge_u, end_tile, ridge=True, ridge_wrap=None):
        self.name = name
        self.x0, self.x1, self.zf, self.zb, self.y0 = x0, x1, zf, zb, y0
        self.fascia, self.slope, self.teeth, self.cap = fascia, slope, teeth, cap
        self.pitch, self.run, self.ridge_u, self.end_tile = pitch, run, ridge_u, end_tile
        self.ridge = ridge
        self.ridge_wrap = ridge_wrap
        self.ye = y0 + (fascia.fixed[1] - fascia.fixed[0])
        cd = cap[1] - cap[0]
        self.z_rf = (zf + zb) / 2.0 + cd / 2.0
        self.z_rb = self.z_rf - cd
        self.rise = math.tan(math.radians(pitch)) * (zf - self.z_rf)
        self.yr = self.ye + self.rise
        self.yt = self.yr + (teeth[1] - teeth[0])

    def slope_point(self, texels):
        """(z, y) of the front slope `texels` art rows above its eave."""
        run = self.zf - self.z_rf
        length = math.hypot(run, self.rise)
        t = texels / ((self.rise + run) / length)
        return (self.zf - t * run / length, self.ye + t * self.rise / length)

    def emit(self, mesh):
        x0, x1, zf, zb, y0, ye = self.x0, self.x1, self.zf, self.zb, self.y0, self.ye
        xa, xb = x0 + self.run, x1 - self.run
        zr, zq, yr, yt = self.z_rf, self.z_rb, self.yr, self.yt
        n = self.name
        up = (0.0, 1.0, 0.0)
        fas, sl = self.fascia, self.slope
        # fascia board, round all four sides
        strip_face(mesh, [(x0, y0, zf), (x1, y0, zf), (x1, ye, zf), (x0, ye, zf)],
                   (0, y0, zf), (1, 0, 0), up, fas, SHADE_ART, n + ".fascia_s")
        strip_face(mesh, [(x1, y0, zb), (x0, y0, zb), (x0, ye, zb), (x1, ye, zb)],
                   (0, y0, zb), (1, 0, 0), up, fas, SHADE_BACK, n + ".fascia_n")
        strip_face(mesh, [(x0, y0, zb), (x0, y0, zf), (x0, ye, zf), (x0, ye, zb)],
                   (x0, y0, 0), (0, 0, 1), up, fas, SHADE_WEST, n + ".fascia_w")
        strip_face(mesh, [(x1, y0, zf), (x1, y0, zb), (x1, ye, zb), (x1, ye, zf)],
                   (x1, y0, 0), (0, 0, 1), up, fas, SHADE_EAST, n + ".fascia_e")
        # the four slopes
        t_s = _unit((0, self.rise, -(zf - zr)))
        t_n = _unit((0, self.rise, (zf - zr)))
        t_w = _unit((self.run, self.rise, 0))
        t_e = _unit((-self.run, self.rise, 0))
        strip_face(mesh, [(x0, ye, zf), (x1, ye, zf), (xb, yr, zr), (xa, yr, zr)],
                   (0, ye, zf), (1, 0, 0), t_s, sl, SHADE_ART, n + ".slope_s")
        strip_face(mesh, [(x1, ye, zb), (x0, ye, zb), (xa, yr, zq), (xb, yr, zq)],
                   (0, ye, zb), (1, 0, 0), t_n, sl, SHADE_BACK, n + ".slope_n")
        strip_face(mesh, [(x0, ye, zb), (x0, ye, zf), (xa, yr, zr), (xa, yr, zq)],
                   (x0, ye, 0), (0, 0, 1), t_w, sl, SHADE_WEST, n + ".hip_w")
        strip_face(mesh, [(x1, ye, zf), (x1, ye, zb), (xb, yr, zq), (xb, yr, zr)],
                   (x1, ye, 0), (0, 0, 1), t_e, sl, SHADE_EAST, n + ".hip_e")
        if not self.ridge:
            return
        # Ridge: teeth to the south, cap on top, ends over the hips. Each half
        # takes its own end of the drawn ridge, so both end ornaments stay -
        # unless something stands on the drawn ridge (`ridge_wrap`), and then
        # the whole ridge is laid from the clean columns instead.
        lo, hi = self.ridge_u
        mid = (xa + xb) / 2.0
        if self.ridge_wrap is not None:
            spans = [(xa, xb, None)]
        else:
            spans = [(xa, mid, lo - xa), (mid, xb, hi - xb)]
        for (sa, sb, off) in spans:
            if off is None:
                teeth = Strip(self.teeth, wrap=self.ridge_wrap)
                cap = Strip(self.cap, wrap=self.ridge_wrap)
            else:
                teeth = Strip(self.teeth, offset=off)
                cap = Strip(self.cap, offset=off)
            strip_face(mesh, [(sa, yr, zr), (sb, yr, zr), (sb, yt, zr), (sa, yt, zr)],
                       (0, yr, zr), (1, 0, 0), up, teeth, SHADE_ART, n + ".teeth")
            strip_face(mesh, [(sa, yt, zr), (sb, yt, zr), (sb, yt, zq), (sa, yt, zq)],
                       (0, yt, zr), (1, 0, 0), (0, 0, -1), cap, SHADE_ART, n + ".cap")
        u0, v0, u1, v1 = self.end_tile.rect
        w, h = zr - zq, yt - yr
        for x, shade, flip in ((xa, SHADE_WEST, False), (xb, SHADE_EAST, True)):
            pts = [(x, yr, zq), (x, yr, zr), (x, yt, zr), (x, yt, zq)]
            us = [u0, u0 + w, u0 + w, u0]
            if flip:
                us = [u0 + w - (u - u0) for u in us]
            vs = [v0 + h, v0 + h, v0, v0]
            mesh.poly([p + (u, v) for p, u, v in zip(pts, us, vs)], shade, n + ".ridge_end")


def _plan_normal(p, q):
    """Outward normal of plan edge p -> q for a polygon listed clockwise on
    screen (+X right, +Z down): the south edge runs west to east."""
    dx, dz = q[0] - p[0], q[1] - p[1]
    n = math.hypot(dx, dz)
    return (-dz / n, dx / n)


def inset_plan(plan, d):
    """The convex plan polygon moved inwards by d on every edge."""
    lines = []
    count = len(plan)
    for i in range(count):
        p, q = plan[i], plan[(i + 1) % count]
        nx, nz = _plan_normal(p, q)
        lines.append(((p[0] - nx * d, p[1] - nz * d), (q[0] - p[0], q[1] - p[1])))
    out = []
    for i in range(count):
        (p1, d1), (p2, d2) = lines[i - 1], lines[i]
        den = d1[0] * d2[1] - d1[1] * d2[0]
        t = ((p2[0] - p1[0]) * d2[1] - (p2[1] - p1[1]) * d2[0]) / den
        out.append((p1[0] + d1[0] * t, p1[1] + d1[1] * t))
    return out


class Frustum:
    """Walls on a convex plan, a roof band sloping in, and a flat top.

    The shape of a Pokemon Center or a Mart: octagonal walls, a band of roof
    at `band_pitch` rising `band_rise`, and a plateau. Every face the drawing
    shows (its plan edge faces south or diagonally south) takes the GBA
    projection, so the front, the chamfers and the band reproduce the art
    exactly at 45 degrees; the chamfers are tagged "~proj" because a face
    drawn at 45 degrees in plan has no 1:1 reading. The sides and the back
    take `wall_side` / `band_side` strips, and the plateau is laid from its
    front edge northwards by `top`, so it grows by repeated rows to whatever
    depth the plan has.
    """

    def __init__(self, name, plan, wall_top, wall_side, band_rise, band_side, top):
        self.name, self.plan = name, plan
        self.wall_top, self.wall_side = wall_top, wall_side
        self.band_rise, self.band_side = band_rise, band_side
        self.top = top

    def _proj_face(self, mesh, quad, tag):
        mesh.poly([(x, y, z, x, z - y) for (x, y, z) in quad], SHADE_ART, tag)

    def emit(self, mesh):
        plan, n = self.plan, self.name
        # The walls go a pixel under the ground: the drawing outlines its
        # plinth one pixel outside the footprint, and the ground hides the rest.
        y0, y1 = -1.0, float(self.wall_top)
        y2 = y1 + self.band_rise
        top = inset_plan(plan, self.band_rise)
        count = len(plan)
        for i in range(count):
            p, q = plan[i], plan[(i + 1) % count]
            pi, qi = top[i], top[(i + 1) % count]
            nx, nz = _plan_normal(p, q)
            drawn = nz > 0.1
            diag = abs(nx) > 1e-6
            shade = SHADE_ART if drawn else (SHADE_WEST if nx < -0.3 else
                                             SHADE_EAST if nx > 0.3 else SHADE_BACK)
            wall = [(p[0], y0, p[1]), (q[0], y0, q[1]), (q[0], y1, q[1]), (p[0], y1, p[1])]
            band = [(p[0], y1, p[1]), (q[0], y1, q[1]), (qi[0], y2, qi[1]), (pi[0], y2, pi[1])]
            sdir = _unit((q[0] - p[0], 0.0, q[1] - p[1]))
            if drawn:
                self._proj_face(mesh, wall, n + ".wall" + ("~proj" if diag else ""))
                self._proj_face(mesh, band, n + ".band" + ("~proj" if diag else ""))
            else:
                strip_face(mesh, wall, (p[0], y0, p[1]), sdir, (0.0, 1.0, 0.0),
                           self.wall_side, shade, n + ".wall")
                tdir = _unit((-nx, 1.0, -nz))
                strip_face(mesh, band, (p[0], y1, p[1]), sdir, tdir,
                           self.band_side, shade, n + ".band")
        zfront = max(z for _, z in top)
        strip_face(mesh, [(x, y2, z) for x, z in reversed(top)], (0.0, y2, zfront),
                   (1.0, 0.0, 0.0), (0.0, 0.0, -1.0), self.top, SHADE_ART, n + ".top")


class Vault:
    """A barrel vault standing on a flat top, its axis front to back.

    `profile` is the arch, (x, height) points read off the drawn front arch
    column by column; the vault runs from `zf` back to `zb` at `y0`. Its top
    and front take the GBA projection - at 45 degrees the drawing itself, and
    in any other view a real curved roof - and the back arch repeats the
    front's texels. Faces tagged "~proj" (curved, no single 1:1 reading).
    """

    def __init__(self, name, profile, zf, zb, y0):
        self.name, self.profile = name, profile
        self.zf, self.zb, self.y0 = zf, zb, y0

    def emit(self, mesh):
        zf, zb, y0, n = self.zf, self.zb, self.y0, self.name
        pts = self.profile
        for (xa, ha), (xb, hb) in zip(pts, pts[1:]):
            top = [(xa, y0 + ha, zf), (xb, y0 + hb, zf), (xb, y0 + hb, zb), (xa, y0 + ha, zb)]
            mesh.poly([(x, y, z, x, z - y) for (x, y, z) in top], SHADE_ART, n + ".top~proj")
            if ha <= 0 and hb <= 0:
                continue
            front = [(xa, y0, zf), (xb, y0, zf), (xb, y0 + hb, zf), (xa, y0 + ha, zf)]
            mesh.poly([(x, y, z, x, zf - y) for (x, y, z) in front], SHADE_ART, n + ".front")
            back = [(xb, y0, zb), (xa, y0, zb), (xa, y0 + ha, zb), (xb, y0 + hb, zb)]
            mesh.poly([(x, y, z, x, zf - y) for (x, y, z) in back], SHADE_BACK, n + ".back")


class Walls:
    """Vertical walls along an open plan polyline, drawn where they stand.

    For a front the drawing shows whole - a porch with chamfered corners -
    every wall takes the GBA projection; diagonal ones are tagged "~proj".
    """

    def __init__(self, name, plan, y0, y1):
        self.name, self.plan, self.y0, self.y1 = name, plan, y0, y1

    def emit(self, mesh):
        for (p, q) in zip(self.plan, self.plan[1:]):
            quad = [(p[0], self.y0, p[1]), (q[0], self.y0, q[1]),
                    (q[0], self.y1, q[1]), (p[0], self.y1, p[1])]
            diag = abs(q[1] - p[1]) > 1e-6
            mesh.poly([(x, y, z, x, z - y) for (x, y, z) in quad], SHADE_ART,
                      self.name + (".wall~proj" if diag else ".wall"))


class Relief:
    """A solid read column by column off its own drawing: hedges, walls.

    In every pixel column the drawing shows a run of the object: its top,
    and at the south end its front, `height` rows of it. So the top of each
    run lies at Y = height from Z = start + height to the run's end, and the
    front stands at the end - both projected, so the drawing is reproduced
    exactly. Columns with the same run are merged; where neighbouring
    columns end at different depths, or at the back, the faces the drawing
    never shows take `side`, a tile of the object's own front.
    """

    def __init__(self, name, art, height, side, hull=0, bridge=0, foot=None, solid=False,
                 back=None, top_tile=None, against=False, seam=None, flank_tile=None,
                 seam_x=((), ()), proj_back=False, skirt=1.0, see_through=False):
        self.name, self.art, self.height, self.side = name, art, height, side
        # A frame or a wire the room is seen through (a bed's open head, a
        # tap): its back is its front again, holes and all, so it is seen
        # through from behind as from before, not closed by a back panel.
        self.see_through = see_through
        # Its faces go `skirt` below its foot, into the ground, so no crack
        # shows where they meet it; a part lifted onto another (a table's
        # top on its legs) has nothing under it to hide that, and a row of
        # its side's tile again would hang below it.
        self.skirt = float(skirt)
        # A box whose front has see-through pixels (a dome's rounded rim, a
        # box's corner): its back shows through them at 45 degrees, so it
        # takes the drawing where it stands while its sides are dressed
        # with `side`. With no real depth (`back`) only.
        self.proj_back = proj_back
        # A block of a longer run (a railing cut into blocks): `seam` is
        # (rows, open_s, open_n). The art's rows from `rows` on are the run
        # going on south, which this block's top covers; the columns in
        # open_s carry on past them, so no front stands at the seam, and
        # those in open_n come in from the block north: no back, and a run
        # no longer than `height` there is the north block's.
        self.seam = seam
        # A railing whose run goes down a column is drawn as a line - its
        # top - and never shows its bars: `flank_tile`, the bars of the same
        # railing where it runs along a row, dresses its sides, their gaps
        # kept.
        self.flank_tile = flank_tile
        # the rows (in cells) where the block goes on west and east of its
        # art: a seam, where no flank stands
        self.seam_x = seam_x
        # A piece of furniture stands on the floor along one line, its foot:
        # with `foot`, every column runs from its first drawn row down to it,
        # so a leaf's tip or a machine's rounded corner stands with the rest
        # of it (its own transparency cuts the outline) instead of alone,
        # where its short run would put it - behind the wall.
        self.foot = foot
        # A box of a thing (a shelf, a counter) has sides of its own to show:
        # `solid` dresses them with `side`, where an outline's envelope takes
        # the drawing (projected, so its gaps stay gaps).
        self.solid = solid
        # Real depth: the drawing shows as much of a thing's top as the GBA's
        # 45 degrees leave in view, a few rows of a machine against a wall,
        # but it stands on as many cells as the cartridge says it does. With
        # `back`, the top runs back to that Z, laid with its own drawn rows
        # repeated course by course - never stretched - and the faces it adds
        # are tagged "~depth": at 45 degrees they rise behind the drawing.
        self.back = back
        # What the depth the drawing does not show is laid with: its own top
        # rows repeated, or, with `top_tile`, a stretch of drawing that is a
        # top in full - a glass case's, where the front one is two rows deep.
        self.top_tile = top_tile
        # `against`: the back is a wall. A top drawn deeper than the room in
        # front of it stops there, flat: the rows the drawing puts behind
        # the wall's face are the wall's to hide (seen from the console's
        # camera a desk's top does not bend up the wall). They are the one
        # thing such a piece does not draw at 45 degrees (behind_wall).
        self.against = against
        if back is not None:
            self.solid = True
        # A railing is mostly gaps: `hull` closes gaps down a column up to that
        # many pixels and `bridge` carries a column's run across that many
        # empty columns, so the solid is the railing's envelope and the
        # texture's own transparency cuts the bars out of it.
        self.hull, self.bridge = hull, bridge

    def runs(self):
        W, H = self.art.size
        if self.flank_tile is not None:
            H = self.flank_tile.rect[1]     # the bars' band below is no drawing
        px = self.art.load()
        cols = []
        for u in range(W):
            runs, v = [], 0
            while v < H:
                if px[u, v][3] >= 128:
                    v0 = v
                    while v < H and px[u, v][3] >= 128:
                        v += 1
                    runs.append((v0, v))
                else:
                    v += 1
            if self.hull and runs:
                merged = [runs[0]]
                for (a, b) in runs[1:]:
                    if a - merged[-1][1] <= self.hull:
                        merged[-1] = (merged[-1][0], b)
                    else:
                        merged.append((a, b))
                runs = merged
            cols.append(runs)
        if self.bridge:
            # an empty column between two identical ones takes their run
            for u in range(1, W - 1):
                if cols[u]:
                    continue
                left = next((cols[k] for k in range(u - 1, max(-1, u - 1 - self.bridge), -1) if cols[k]), None)
                right = next((cols[k] for k in range(u + 1, min(W, u + 1 + self.bridge)) if cols[k]), None)
                if left is not None and left == right:
                    cols[u] = list(left)
        if self.foot is not None:
            cols = [[(runs[0][0], max(self.foot, runs[-1][1]))] if runs else [] for runs in cols]
        if self.seam is not None:
            rows, _, open_n = self.seam
            cols = [[(a, b) for (a, b) in runs if a < rows
                     and not (u in open_n and a == 0 and b - a <= self.height)]
                    for u, runs in enumerate(cols)]
        return cols

    def emit(self, mesh):
        H, n = float(self.height), self.name
        cols = self.runs()
        W = len(cols)

        def drawn(runs):
            """(z0, z1, top, va) as the drawing shows each run of a column."""
            out = []
            for (va, vb) in runs:
                depth = vb - va - H
                out.append((va + H, vb, H, va) if depth > 0 else (vb, vb, float(vb - va), va))
            return out

        def solid(runs):
            """(z0, z1, top) footprint pieces of one column, at real depth."""
            out = []
            for (z0, z1, top, va) in drawn(runs):
                if self.back is not None and top == H and self.back < z0:
                    z0 = float(self.back)
                elif self.against and top == H and z0 < self.back < z1:
                    # drawn deeper than the room in front of the wall has:
                    # the top stops at the wall
                    z0 = float(self.back)
                out.append((z0, z1, top))
            return out

        # merged spans of identical columns
        u = 0
        while u < W:
            u1 = u + 1
            while u1 < W and cols[u1] == cols[u]:
                u1 += 1
            for (z0, z1, top), (zd, _, _, va) in zip(solid(cols[u]), drawn(cols[u])):
                x0, x1 = float(u), float(u1)
                zt = max(zd, z0)
                if z1 > zt:
                    quad = [(x0, top, zt), (x1, top, zt), (x1, top, z1), (x0, top, z1)]
                    mesh.poly([(x, y, z, x, z - y) for (x, y, z) in quad], SHADE_ART, n + ".top")
                if zd > z0 and self.top_tile is not None:
                    face = [(x0, 0.0), (x1, 0.0), (x1, zd - z0), (x0, zd - z0)]
                    for piece in tile_pieces(face, self.top_tile, None, None):
                        mesh.poly([(s, top, zd - t, uu, vv) for (s, t, uu, vv) in piece],
                                  SHADE_ART, n + ".top~depth")
                elif zd > z0:
                    # the top's own rows, course after course, back to its
                    # depth: its inner rows, and its back edge - the outline
                    # the drawing puts there - once, at the real back
                    rows = z1 - zd
                    first = va + 1.0 if rows >= 2 else float(va)
                    course = max(1.0, rows - (first - va))
                    edge = 1.0 if rows >= 2 and zd - z0 > 1 else 0.0
                    far = zd
                    while far > z0 + edge + 1e-6:
                        near, far = far, max(z0 + edge, far - course)
                        va0 = first + (course - (near - far))
                        quad = [(x0, top, far), (x1, top, far), (x1, top, near), (x0, top, near)]
                        mesh.poly([(x, y, z, x, va0 + (z - far)) for (x, y, z) in quad],
                                  SHADE_ART, n + ".top~depth")
                    if edge:
                        quad = [(x0, top, z0), (x1, top, z0), (x1, top, z0 + 1), (x0, top, z0 + 1)]
                        mesh.poly([(x, y, z, x, va + (z - z0)) for (x, y, z) in quad],
                                  SHADE_ART, n + ".top~depth")
                seam_s = (self.seam is not None and u in self.seam[1]
                          and z1 >= self.seam[0] + H - 1e-6)
                seam_n = self.seam is not None and u in self.seam[2] and va == 0
                low = -self.skirt
                front = [(x0, low, z1), (x1, low, z1), (x1, top, z1), (x0, top, z1)]
                if not seam_s:
                    mesh.poly([(x, y, z, x, z - y) for (x, y, z) in front], SHADE_ART, n + ".front")
                if z1 > z0 and not seam_n:
                    back = [(x1, low, z0), (x0, low, z0), (x0, top, z0), (x1, top, z0)]
                    if self.see_through:
                        mesh.poly([(x, y, z, x, z1 - y) for (x, y, z) in back], SHADE_BACK,
                                  n + (".back~depth" if self.back is not None else ".back"))
                    elif (self.hull or (self.foot is not None and not self.solid)
                            or (self.proj_back and self.back is None)):
                        # a railing's back takes the drawing where it stands,
                        # so its gaps stay gaps from the front as well
                        mesh.poly([(x, y, z, x, z - y) for (x, y, z) in back],
                                  SHADE_BACK, n + ".back~proj")
                    else:
                        self._tile(mesh, back, (x1, z0), (-1.0, 0.0), top, SHADE_BACK,
                                   n + (".back~depth" if self.back is not None else ".back"))
            u = u1

        # flanks: wherever one column's solid is not matched by its neighbour's
        def covered(pieces):
            return [(a, b, t) for (a, b, t) in pieces if b > a]

        low = -self.skirt
        for u in range(W + 1):
            left = covered(solid(cols[u - 1])) if u > 0 else []
            right = covered(solid(cols[u])) if u < W else []
            for pieces, others, facing in ((left, right, 1.0), (right, left, -1.0)):
                for (a, b, t) in pieces:
                    for (za, zb) in _subtract((a, b), [(c, d) for (c, d, s) in others if s >= t]):
                        x = float(u)
                        edge = self.seam_x[0] if u == 0 else self.seam_x[1] if u == W else None
                        if edge and all(r in edge for r in range(int(za) // 16,
                                                                 int(math.ceil(zb)) // 16 + 1)
                                        if r * 16 < zb):
                            continue    # the run goes on into the next block
                        if self.flank_tile is not None and zb - za >= 16:
                            # a railing running down a column: one plane of
                            # bars down its middle, not one on each side - a
                            # box with bars on both faces read as a wall with
                            # windows. Edge-on at 45 degrees either way.
                            if facing < 0:
                                continue        # the west side: its east one stands for it
                            k = u
                            while k > 0 and any(c <= za + 1e-6 and d >= zb - 1e-6 and s >= t
                                                for (c, d, s) in covered(solid(cols[k - 1]))):
                                k -= 1
                            x = (k + u) / 2.0
                            quad = [(x, low, zb), (x, low, za), (x, t, za), (x, t, zb)]
                            self._tile(mesh, quad, (x, 0.0), (0.0, 1.0), t, SHADE_EAST,
                                       n + ".flank", self.flank_tile)
                            continue
                        if self.hull or (self.foot is not None and not self.solid):
                            # the end of a railing: the drawn column there, projected
                            col = min(max(u - (1 if facing > 0 else 0), 0), W - 1) + 0.5
                            quad = [(x, low, za), (x, low, zb), (x, t, zb), (x, t, za)]
                            mesh.poly([(q[0], q[1], q[2], col, q[2] - q[1]) for q in quad],
                                      SHADE_EAST if facing > 0 else SHADE_WEST, n + ".flank~proj")
                            continue
                        tag = n + (".flank~depth" if self.back is not None else ".flank")
                        if facing > 0:
                            quad = [(x, low, zb), (x, low, za), (x, t, za), (x, t, zb)]
                            self._tile(mesh, quad, (x, zb), (0.0, -1.0), t, SHADE_EAST, tag)
                        else:
                            quad = [(x, low, za), (x, low, zb), (x, t, zb), (x, t, za)]
                            self._tile(mesh, quad, (x, za), (0.0, 1.0), t, SHADE_WEST, tag)

    def _tile(self, mesh, quad, origin, sdir, top, shade, tag, tile=None):
        """1:1 side tile laid along the face from `origin`, top row at `top`."""
        u0, v0, u1, v1 = (tile or self.side).rect
        tile = Tile(u0, v0, u1, v1)
        pts = []
        for (x, y, z) in quad:
            s = (x - origin[0]) * sdir[0] + (z - origin[1]) * sdir[1]
            pts.append((s, top - y, x, y, z))
        for piece in tile_pieces(pts, tile, None, None):
            mesh.poly([(q[2], q[3], q[4], q[5], q[6]) for q in piece], shade, tag)


def _subtract(span, others):
    """Parts of span (a, b) not covered by any of `others`."""
    out = [span]
    for (c, d) in others:
        nxt = []
        for (a, b) in out:
            if d <= a or c >= b:
                nxt.append((a, b))
                continue
            if c > a:
                nxt.append((a, c))
            if d < b:
                nxt.append((d, b))
        out = nxt
    return [(a, b) for (a, b) in out if b - a > 1e-6]


class Moved:
    """A part moved along Y and Z, its texture with it, its faces tagged
    `tag`: a table's back legs, which its drawing never shows, as copies of
    its front ones ("~depth": hidden behind its top at 45 degrees)."""

    def __init__(self, part, dy=0.0, dz=0.0, tag=""):
        self.part, self.dy, self.dz, self.tag = part, float(dy), float(dz), tag

    def emit(self, mesh):
        inner = Mesh()
        if isinstance(self.part, Prism):
            emit_prism(inner, self.part)
        else:
            self.part.emit(inner)
        for (tri, shade, tag) in inner.tris:
            mesh.tri(*[(x, y + self.dy, z + self.dz, u, v) for (x, y, z, u, v) in tri],
                     shade=shade, tag=tag if self.tag in tag else tag + self.tag)


class Lifted:
    """A part standing on another: everything moved by (0, base, base).

    A point of the drawing lifted as the terrain relief lifts it keeps its
    place in the GBA view (v = Z - Y does not change), so a machine drawn on
    a counter stands on the counter's top and is still drawn exactly.
    """

    def __init__(self, part, base):
        self.part, self.base = part, float(base)

    def emit(self, mesh):
        inner = Mesh()
        if isinstance(self.part, Prism):
            emit_prism(inner, self.part)
        else:
            self.part.emit(inner)
        b = self.base
        for (tri, shade, tag) in inner.tris:
            mesh.tri(*[(x, y + b, z + b, u, v) for (x, y, z, u, v) in tri],
                     shade=shade, tag=tag)


class Mound:
    """A rounded thing - a rock in the sea - lifted pixel by pixel off its
    own drawing.

    Every point (u, v) of the drawing goes to (u, h, v + h), which the GBA's
    45 degrees see as that same point whatever h is: the drawing is the
    rock seen from there, exactly, and h is only how round it is. Each
    column of the drawing is a cut through a dome: an ellipse half `rise`
    times as tall as it is deep, standing on the water, its foot where the
    column's drawing ends and its top as far up as the column is drawn (the
    ray of the 45-degree view grazes it there). The point of the drawing is
    the point of that ellipse the view meets first. Behind the grazing point
    the ellipse goes on down to the water: the back no drawing shows, laid
    with the texels in front of it, and closed at both ends.

    `step` is the lattice in pixels across the columns; down a column the
    dome is cut as finely. Faces are projected ("~proj"): curved, not 1:1
    anywhere off the axis, and the texture's own transparency cuts the
    outline.

    `ring`: the colours of what the drawing puts round the rock's foot on
    the water (foam, its grey shadow). They are the sea's, not the rock's:
    laid flat on the water, textured from the band of the art below `rows`
    that holds them alone (Mound.with_ring), never lifted with the dome.
    """

    def __init__(self, name, art, rise=1.0, step=4, back_steps=3, ring=(), rows=None):
        self.name, self.rise, self.step, self.back_steps = name, rise, step, back_steps
        self.rows = rows if rows is not None else art.size[1]
        self.ring = {tuple(c) for c in ring}
        body = art.crop((0, 0, art.size[0], self.rows))
        bpx = body.load()
        for (x, y) in Mound.ring_pixels(body, self.ring):
            bpx[x, y] = (0, 0, 0, 0)
        self.art = body
        self.full = art
        # the band the faces no drawing shows are laid with (with_ring)
        self.back_band = 2 * self.rows if art.size[1] >= 3 * self.rows else 0

    @staticmethod
    def ring_pixels(art, ring):
        """The pixels of the ring's colours at the rock's foot: in each
        column, below the lowest pixel of the rock itself (or all of them,
        in a column with none). The same colour higher up is the rock's."""
        W, H = art.size
        px = art.load()
        out = set()
        for x in range(W):
            body = [y for y in range(H) if px[x, y][3] >= 128 and px[x, y][:3] not in ring]
            foot = body[-1] if body else -1
            out |= {(x, y) for y in range(foot + 1, H)
                    if px[x, y][3] >= 128 and px[x, y][:3] in ring}
        return out

    @staticmethod
    def with_ring(art, ring):
        """The texture, three bands of the drawing's size: the drawing
        without its ring; its ring alone; and the rock again, grown past its
        outline with its own edge colours - what the faces no drawing shows
        (the back, the ends) are laid with, so that no texel they sample is
        a hole."""
        ring = {tuple(c) for c in ring}
        W, H = art.size
        out = Image.new("RGBA", (W, H * 3), (0, 0, 0, 0))
        apx, opx = art.load(), out.load()
        foam = Mound.ring_pixels(art, ring)
        grown = {}
        for y in range(H):
            for x in range(W):
                if apx[x, y][3] >= 128:
                    opx[x, y + (H if (x, y) in foam else 0)] = apx[x, y]
                    if (x, y) not in foam:
                        grown[(x, y)] = apx[x, y]
        edge = list(grown)
        while edge and len(grown) < W * H:
            nxt = []
            for (x, y) in edge:
                for (i, j) in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
                    if 0 <= i < W and 0 <= j < H and (i, j) not in grown:
                        grown[(i, j)] = grown[(x, y)]
                        nxt.append((i, j))
            edge = nxt
        for (x, y), c in grown.items():
            opx[x, y + 2 * H] = c[:3] + (255,)
        return out

    def _spans(self):
        """(x, top, foot) at the lattice's lines across the columns: through
        the middle of a pixel column, its own drawn run, so the strips
        between them meet the outline at every pixel's centre and stand
        nowhere outside it; the two ends at the drawing's own edges. A line
        is left out where the outline between its neighbours is straight to
        within half a pixel, `step` columns at most."""
        W, H = self.art.size
        px = self.art.load()
        runs = []
        for u in range(W):
            v = [y for y in range(H) if px[u, y][3] >= 128]
            runs.append((v[0], v[-1] + 1) if v else None)
        cols = [u for u in range(W) if runs[u]]
        u0, u1 = cols[0], cols[-1] + 1
        for u in range(u0, u1):
            if runs[u] is None:     # a gap down the middle: bridged
                runs[u] = runs[u - 1]
        lines = [(float(u0),) + runs[u0]] + [(u + 0.5,) + runs[u] for u in range(u0, u1)]             + [(float(u1),) + runs[u1 - 1]]

        def straight(i, j):
            (xa, ta, fa), (xb, tb, fb) = lines[i], lines[j]
            for k in range(i + 1, j):
                x, t, f = lines[k]
                g = (x - xa) / (xb - xa)
                if abs(ta + (tb - ta) * g - t) > 0.5 or abs(fa + (fb - fa) * g - f) > 0.5:
                    return False
            return True

        keep, i = [0], 0
        while i < len(lines) - 1:
            j = i + 1
            while (j + 1 < len(lines) and lines[j + 1][0] - lines[i][0] <= self.step
                   and straight(i, j + 1)):
                j += 1
            keep.append(j)
            i = j
        return [lines[k] for k in keep]

    def emit(self, mesh):
        k = self.rise
        spans = self._spans()
        n = max(4, int(math.ceil(max(b - a for _, a, b in spans) / float(self.step))))
        cols = []
        for (x, vt, vb) in spans:
            L = float(vb - vt)
            a = L / (1.0 + math.sqrt(1.0 + k * k))
            b = k * a
            zc = vb - a
            front = []
            for i in range(n + 1):
                v = vt + L * i / n
                # ((v + t - zc) / a)^2 + (t / b)^2 = 1, the root nearest the eye
                A = 1.0 / (a * a) + 1.0 / (b * b)
                B = 2.0 * (v - zc) / (a * a)
                C = (v - zc) ** 2 / (a * a) - 1.0
                disc = max(0.0, B * B - 4 * A * C)
                t = max(0.0, (-B + math.sqrt(disc)) / (2 * A))
                front.append((x, t, v + t, x, v))
            back = []
            th0 = math.pi - math.atan2(b, a)
            for i in range(1, self.back_steps + 1):
                th = th0 + (math.pi - th0) * i / self.back_steps
                z, y = zc + a * math.cos(th), b * math.sin(th)
                if i == self.back_steps:
                    y = 0.0
                back.append((x, y, z, x, min(vb - 1e-3, max(vt, z - y)) + self.back_band))
            cols.append((front, back, zc))
        for c in range(len(cols) - 1):
            (fa, ba, za), (fb, bb, zb) = cols[c], cols[c + 1]
            for i in range(n):
                p, q, r, s_ = fa[i], fb[i], fb[i + 1], fa[i + 1]
                self._quad(mesh, p, q, r, s_, SHADE_ART, self.name + "~proj")
            ra, rb = [self._behind(fa[0])] + ba, [self._behind(fb[0])] + bb
            centre = ((fa[0][0] + fb[0][0]) / 2.0, 0.0, (za + zb) / 2.0)
            for i in range(len(ra) - 1):
                self._quad(mesh, ra[i], rb[i], rb[i + 1], ra[i + 1], SHADE_WOUND,
                           self.name + ".back~behind~proj", outward=centre)
        # the ring round its foot, flat on the water
        if self.ring:
            W, H = self.full.size
            box = self.full.crop((0, self.rows, W, min(H, 2 * self.rows))).getbbox()
            if box:
                x0, y0, x1, y1 = box
                e = 0.05
                q = [(x, e, v + e, x, v + self.rows) for (x, v) in
                     ((x0, y0), (x1, y0), (x1, y1), (x0, y1))]
                self._quad(mesh, q[0], q[1], q[2], q[3], SHADE_ART, self.name + ".ring~proj")
        # the two ends, closed by the column's own cut
        for (front, back, zc), side in ((cols[0], -1), (cols[-1], 1)):
            ring = [self._behind(p) for p in reversed(front)] + back
            if len(ring) < 3:
                continue
            x = ring[0][0]
            centre = (x - side, 0.0, zc)
            for i in range(1, len(ring) - 1):
                self._tri(mesh, ring[0], ring[i], ring[i + 1], SHADE_WOUND,
                          self.name + ".end~behind~proj", outward=centre)

    def _behind(self, p):
        """A point of the drawn front, textured from the band the faces no
        drawing shows are laid with."""
        return p[:4] + (p[4] + self.back_band,)

    @staticmethod
    def _area(a, b, c):
        e1 = [b[i] - a[i] for i in range(3)]
        e2 = [c[i] - a[i] for i in range(3)]
        n = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
             e1[0] * e2[1] - e1[1] * e2[0])
        return n

    def _tri(self, mesh, a, b, c, shade, tag, outward=None):
        n = self._area(a, b, c)
        if sum(v * v for v in n) < 1e-12:
            return
        if outward is None:
            # drawn: it faces the 45-degree view
            if n[1] + n[2] < 0:
                b, c = c, b
        else:
            g = [(a[i] + b[i] + c[i]) / 3.0 - outward[i] for i in range(3)]
            if n[0] * g[0] + n[1] * g[1] + n[2] * g[2] < 0:
                b, c = c, b
        mesh.tri(a, b, c, shade, tag)

    def _quad(self, mesh, p, q, r, s, shade, tag, outward=None):
        self._tri(mesh, p, q, r, shade, tag, outward)
        self._tri(mesh, p, r, s, shade, tag, outward)


def catmull(pts, n):
    """n + 1 points along a Catmull-Rom curve through pts (any dimension)."""
    P = [pts[0]] + list(pts) + [pts[-1]]
    out = []
    for k in range(n + 1):
        s = (len(pts) - 1) * k / float(n)
        i = min(int(s), len(pts) - 2)
        t = s - i
        p0, p1, p2, p3 = P[i], P[i + 1], P[i + 2], P[i + 3]
        out.append(tuple(0.5 * ((2 * p1[d]) + (-p0[d] + p2[d]) * t
                                + (2 * p0[d] - 5 * p1[d] + 4 * p2[d] - p3[d]) * t * t
                                + (-p0[d] + 3 * p1[d] - 3 * p2[d] + p3[d]) * t ** 3)
                         for d in range(len(p1))))
    return out


class ProjDome:
    """A small round thing (a bush) as a smooth dome over an ellipse,
    textured with its drawing projected (u = x, v = z - y).

    Any surface laid with the projection is the drawing at 45 degrees
    wherever it stands, so the dome only has to cover the drawing's outline
    there; the texture's own transparency cuts the rest (the console tests
    alpha), and from the game's camera the cut is the outline drawn out
    along the 45-degree view - clean, where a height field cut column by
    column left fins and steps on the flanks. `seg` points round, `rings`
    up; the base ring at `y0`, the crown `h` above it.
    """

    def __init__(self, name, cx, cz, rx, rz, h, y0=0.0, seg=12, rings=3):
        self.name, self.cx, self.cz, self.rx, self.rz = name, cx, cz, rx, rz
        self.h, self.y0, self.seg, self.rings = h, y0, seg, rings

    def emit(self, mesh):
        def pt(k, i):
            phi = (math.pi / 2) * k / self.rings
            a = 2 * math.pi * i / self.seg
            r = math.cos(phi)
            x = self.cx + self.rx * r * math.cos(a)
            z = self.cz + self.rz * r * math.sin(a)
            y = self.y0 + self.h * math.sin(phi)
            return (x, y, z, x, z - y)
        tag = self.name + "~proj"
        for k in range(self.rings):
            for i in range(self.seg):
                a, b = pt(k, i), pt(k, i + 1)
                c, d = pt(k + 1, i + 1), pt(k + 1, i)
                if k == self.rings - 1:
                    mesh.tri(a, b, c, 1.0, tag)
                else:
                    mesh.poly([a, b, c, d], 1.0, tag)


class ProjLathe:
    """A small turned thing (a bollard) as a smooth surface of revolution on
    an elliptical plan, its drawing projected (see ProjDome). `profile`
    lists (y, radius) from the foot up; the plan is `squash` as deep as
    wide; a lid closes the top ring (a radius of 0 at either end: a pole,
    a ball's). `seg` points round. `offset`: the texture's rows read that
    far down (a band of the part's own pixels, own_bands)."""

    def __init__(self, name, cx, cz, profile, squash=1.0, seg=12, offset=0.0):
        self.name, self.cx, self.cz, self.profile = name, cx, cz, profile
        self.squash, self.seg, self.offset = squash, seg, float(offset)

    def emit(self, mesh):
        # the polygon round each ring circumscribes it: the drawing's outline
        # stays covered
        grow = 1.0 / math.cos(math.pi / self.seg)

        def ring(y, r):
            r *= grow
            out = []
            for i in range(self.seg):
                a = 2 * math.pi * i / self.seg
                x = self.cx + r * math.cos(a)
                z = self.cz + r * self.squash * math.sin(a)
                out.append((x, y, z, x, z - y + self.offset))
            return out
        tag = self.name + "~proj"
        rings = [ring(y, r) for (y, r) in self.profile]
        n = self.seg
        for j, (lo, hi) in enumerate(zip(rings, rings[1:])):
            for i in range(n):
                quad = [lo[i], lo[(i + 1) % n], hi[(i + 1) % n], hi[i]]
                if self.profile[j + 1][1] < 1e-6:
                    mesh.tri(quad[0], quad[1], quad[2], 1.0, tag)
                elif self.profile[j][1] < 1e-6:
                    # a pole at the foot (a ball's): one triangle a side
                    mesh.tri(quad[0], quad[2], quad[3], 1.0, tag)
                else:
                    mesh.poly(quad, 1.0, tag)
        if self.profile[-1][1] > 1e-6:
            mesh.poly(rings[-1], 1.0, tag)


class ProjCanopy:
    """A parasol's canopy: a shallow cone on a round plan, from its rim
    (radius `r` at `y_rim`) up to its crown (`y_top` over `cx`, `cz`), its
    drawing projected (see ProjDome) from `offset` rows down - the canopy's
    own pixels grown past their outline (own_bands), so that no face of it
    samples a hole. Shallower than the view, it shows only its top at 45
    degrees, and there its outline is the rim's.

    The drawing's rings are flatter than deep (as a bollard's are), a round
    rim stood past the drawing at the front: the rim's front half is lifted
    `front` (by the square of its angle's sine), which ends it where the
    drawing does and leaves the plan round. Underneath, a flatter cone up
    to the pole at `y_under`, which the 45-degree view never sees (the
    console's camera does, low down), darker. `seg` points round."""

    def __init__(self, name, cx, cz, r, y_rim, y_top, front=0.0, y_under=None, offset=0.0,
                 seg=16):
        self.name, self.cx, self.cz, self.r = name, cx, cz, r
        self.y_rim, self.y_top, self.front = y_rim, y_top, front
        self.y_under = y_rim + 1.0 if y_under is None else y_under
        self.offset, self.seg = float(offset), seg

    def emit(self, mesh):
        # the polygon circumscribes the circle: the drawing's outline stays
        # covered
        r = self.r / math.cos(math.pi / self.seg)

        def P(x, y, z):
            return (x, y, z, x, z - y + self.offset)
        rim = []
        for i in range(self.seg):
            a = 2 * math.pi * i / self.seg
            s = math.sin(a)
            rim.append(P(self.cx + r * math.cos(a), self.y_rim + self.front * max(0.0, s) ** 2,
                         self.cz + r * s))
        top, under = P(self.cx, self.y_top, self.cz), P(self.cx, self.y_under, self.cz)
        n = self.seg
        for i in range(n):
            a, b = rim[i], rim[(i + 1) % n]
            mesh.tri(b, a, top, SHADE_ART, self.name + "~proj")
            mesh.tri(a, b, under, SHADE_BACK, self.name + ".under~behind~proj")


class ProjSection:
    """A thing whose side outline (`section`, (z, y) points) runs across
    from x0 to x1 - a sun lounger's frame, a mat - every face with its
    drawing projected (see ProjDome), its transparency cutting the outline:
    the faces the 45-degree view looks at and the undersides and backs
    behind them alike (those darker), or the console would show a
    back through a front's cut corner. The ends, edge on to that view,
    read it `grown` rows down (the drawing grown past its outline,
    Mound.with_ring), where no texel is a hole. With `uv`, every face takes
    that one texel instead (a strut the drawing never shows, in its frame's
    colour). A face the view grazes - the cosine to it under `graze` (a
    pillow's back, round) - is a sliver at 45 degrees and wide from
    anywhere else: it reads the grown band too, or the drawing's rounded
    corners there would be holes in it."""

    def __init__(self, name, section, x0, x1, grown=0.0, uv=None, graze=0.0):
        self.name, self.sec = name, [tuple(map(float, p)) for p in section]
        self.x0, self.x1, self.grown, self.uv = float(x0), float(x1), float(grown), uv
        self.graze = graze

    def emit(self, mesh):
        sec, x0, x1 = self.sec, self.x0, self.x1
        n = len(sec)
        area = sum(sec[i][0] * sec[(i + 1) % n][1] - sec[(i + 1) % n][0] * sec[i][1] for i in range(n))

        def P(x, y, z, cut):
            if self.uv is not None:
                return (x, y, z) + tuple(self.uv)
            return (x, y, z, x, z - y + (0.0 if cut else self.grown))
        for i in range(n):
            (za, ya), (zb, yb) = sec[i], sec[(i + 1) % n]
            L = math.hypot(zb - za, yb - ya)
            if L < 1e-6:
                continue
            nz, ny = (yb - ya) / L, -(zb - za) / L
            if area < 0:
                nz, ny = -nz, -ny
            seen = ny + nz > 1e-6
            cut = not seen or (ny + nz) / math.sqrt(2) >= self.graze
            q = [P(x0, ya, za, cut), P(x1, ya, za, cut), P(x1, yb, zb, cut), P(x0, yb, zb, cut)]
            shade = SHADE_ART if seen else SHADE_BACK
            tag = self.name + ("~proj" if seen else ".back~behind~proj")
            PlanPrism._tri(mesh, (q[0], q[1], q[2]), (0.0, ny, nz), shade, tag)
            PlanPrism._tri(mesh, (q[0], q[2], q[3]), (0.0, ny, nz), shade, tag)
        # the ends, edge on at 45 degrees: the drawing's own edge columns
        # (just inside them), grown
        for x, nx, xs in ((x0, -1.0, x0 + 0.01), (x1, 1.0, x1 - 0.01)):
            for (a, b, c) in triangulate(sec):
                tri = []
                for (z, y) in (sec[a], sec[b], sec[c]):
                    pt = P(xs, y, z, False)
                    tri.append((x,) + pt[1:])
                PlanPrism._tri(mesh, tri, (nx, 0.0, 0.0), SHADE_EAST if nx > 0 else SHADE_WEST,
                               self.name + ".end~behind~proj")


class ProjRun:
    """A thing whose outline seen from the side (`section`, (x, y) points:
    x across, y up) runs back from z0 to z1 - an escalator's ramp and the
    balustrades along it, rising as it does - every face with its drawing
    projected (see ProjDome), its transparency cutting the outline. The
    front (z1) and what faces up are the drawing at 45 degrees; the back,
    the underside and the ends, edge on to that view, read it too (an end
    the column just inside it), darker: from the console's camera they
    are the same glass and steps, not a hole. A face lying on the floor
    is left out: nothing sees it."""

    def __init__(self, name, section, z0, z1):
        self.name, self.sec = name, [tuple(map(float, p)) for p in section]
        self.z0, self.z1 = float(z0), float(z1)

    def emit(self, mesh):
        sec, z0, z1 = self.sec, self.z0, self.z1
        n = len(sec)
        area = sum(sec[i][0] * sec[(i + 1) % n][1] - sec[(i + 1) % n][0] * sec[i][1]
                   for i in range(n))

        def P(x, y, z, u=None):
            return (x, y, z, x if u is None else u, z - y)
        for i in range(n):
            (xa, ya), (xb, yb) = sec[i], sec[(i + 1) % n]
            L = math.hypot(xb - xa, yb - ya)
            if L < 1e-6:
                continue
            # the outward normal: right of a->b for a section wound
            # counter-clockwise (x right, y up), left otherwise
            nx, ny = (yb - ya) / L, -(xb - xa) / L
            if area < 0:
                nx, ny = -nx, -ny
            if ny < -0.5 and max(ya, yb) <= 1e-6:
                continue        # its foot, on the floor
            if ny > 1e-6:
                shade, tag, u = SHADE_ART, self.name + "~proj", None
            elif ny < -1e-6:
                shade, tag, u = SHADE_BACK, self.name + ".under~behind~proj", None
            else:
                # an end, edge on at 45 degrees: the drawing's column just
                # inside it
                shade = SHADE_EAST if nx > 0 else SHADE_WEST
                tag, u = self.name + ".end~behind~proj", xa - 0.5 * (1 if nx > 0 else -1)
            q = [P(xa, ya, z0, u), P(xb, yb, z0, u), P(xb, yb, z1, u), P(xa, ya, z1, u)]
            PlanPrism._tri(mesh, (q[0], q[1], q[2]), (nx, ny, 0.0), shade, tag)
            PlanPrism._tri(mesh, (q[0], q[2], q[3]), (nx, ny, 0.0), shade, tag)
        for z, nz in ((z1, 1.0), (z0, -1.0)):
            shade = SHADE_ART if nz > 0 else SHADE_BACK
            tag = self.name + ("~proj" if nz > 0 else ".back~behind~proj")
            for (a, b, c) in triangulate(sec):
                PlanPrism._tri(mesh, tuple(P(sec[k][0], sec[k][1], z) for k in (a, b, c)),
                               (0.0, 0.0, nz), shade, tag)


class ProjHull:
    """An open boat as smooth surfaces, its drawing projected on all of
    them (see ProjDome: any surface so laid is the drawing at 45 degrees,
    so only the outline has to be covered; the texture's transparency cuts
    the rest).

    The outside upright on a superellipse plan (`cx`, `cz`, `rx`, `rz`,
    `p`) from `y0` (rising `keel` to the ends) to the gunwale at `y1`,
    rising `sheer` to the ends; open, the rim across to the inside
    (`inner` = (cx, cz, rx, rz, floor)) and its wall down to the floor, or
    with no `inner` a deck along the sheer; `boxes` (x0, x1, y0, y1, z0, z1)
    standing in it (a thwart). A height field cut column by column stood it
    in steps.
    """

    def __init__(self, name, cx, cz, rx, rz, y0, y1, p, sheer, inner=None, boxes=(), seg=32,
                 keel=0.0):
        self.name, self.cx, self.cz, self.rx, self.rz = name, cx, cz, rx, rz
        self.y0, self.y1, self.p, self.sheer = y0, y1, p, sheer
        self.inner, self.boxes, self.seg = inner, boxes, seg
        # the bottom rising `keel` to the ends (a stem out of the water)
        self.keel = keel

    def _ring(self, cx, cz, rx, rz):
        out = []
        for i in range(self.seg):
            a = 2 * math.pi * i / self.seg
            c, s = math.cos(a), math.sin(a)
            k = 2.0 / self.p
            out.append((cx + rx * math.copysign(abs(c) ** k, c),
                        cz + rz * math.copysign(abs(s) ** k, s)))
        return out

    def _gunwale(self, x):
        k = (x - self.cx) / self.rx
        return self.y1 + self.sheer * k * k

    def _bottom(self, x):
        k = (x - self.cx) / self.rx
        return self.y0 + self.keel * k ** 4

    def emit(self, mesh):
        tag = self.name + "~proj"

        def P(x, y, z):
            return (x, y, z, x, z - y)
        outer = self._ring(self.cx, self.cz, self.rx, self.rz)
        n = self.seg
        for i in range(n):
            (xa, za), (xb, zb) = outer[i], outer[(i + 1) % n]
            mesh.poly([P(xa, self._bottom(xa), za), P(xb, self._bottom(xb), zb),
                       P(xb, self._gunwale(xb), zb), P(xa, self._gunwale(xa), za)], 1.0, tag)
        mesh.poly([P(x, self._bottom(x), z) for (x, z) in outer], 1.0, tag)
        if self.inner is None:
            # decked: the deck follows the sheer, a fan from its middle
            mid = P(self.cx, self.y1, self.cz)
            for i in range(n):
                (xa, za), (xb, zb) = outer[i], outer[(i + 1) % n]
                mesh.tri(mid, P(xb, self._gunwale(xb), zb), P(xa, self._gunwale(xa), za),
                         1.0, tag)
        else:
            icx, icz, irx, irz, floor = self.inner
            inner = self._ring(icx, icz, irx, irz)
            for i in range(n):
                (xa, za), (xb, zb) = outer[i], outer[(i + 1) % n]
                (ia, ka), (ib, kb) = inner[i], inner[(i + 1) % n]
                ga, gb = self._gunwale(xa), self._gunwale(xb)
                ha, hb = self._gunwale(ia), self._gunwale(ib)
                mesh.poly([P(xa, ga, za), P(xb, gb, zb), P(ib, hb, kb), P(ia, ha, ka)], 1.0,
                          tag)
                mesh.poly([P(ia, ha, ka), P(ib, hb, kb), P(ib, floor, kb), P(ia, floor, ka)],
                          1.0, tag)
            mesh.poly([P(x, floor, z) for (x, z) in inner], 1.0, tag)
        for (x0, x1, y0, y1, z0, z1) in self.boxes:
            for quad in ([(x0, y1, z1), (x1, y1, z1), (x1, y1, z0), (x0, y1, z0)],
                         [(x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)],
                         [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0)],
                         [(x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0)],
                         [(x1, y0, z0), (x1, y0, z1), (x1, y1, z1), (x1, y1, z0)]):
                mesh.poly([P(*q) for q in quad], 1.0, tag)


class Volume:
    """A rough solid a drawing is laid on (HeightSolid): a union of shapes,
    each a test of whether a point (x, y, z) in pixels is inside it."""

    def __init__(self):
        self.shapes = []
        self.holes = []

    def dome(self, cx, cz, rx, rz, h, p=2.0, y0=0.0):
        """A dome on a superellipse plan, its sections half ellipses."""
        def f(x, y, z):
            if y < y0:
                return False
            q = (abs(x - cx) / rx) ** p + (abs(z - cz) / rz) ** p
            return q ** (2.0 / p) + ((y - y0) / h) ** 2 <= 1.0
        self.shapes.append(f)
        return self

    def drum(self, cx, cz, rx0, rz0, rx1, rz1, y0, y1):
        """An upright drum, a frustum on ellipses."""
        def f(x, y, z):
            if not y0 <= y <= y1:
                return False
            k = (y - y0) / (y1 - y0)
            rx, rz = rx0 + (rx1 - rx0) * k, rz0 + (rz1 - rz0) * k
            return ((x - cx) / rx) ** 2 + ((z - cz) / rz) ** 2 <= 1.0
        self.shapes.append(f)
        return self

    def tube(self, pts, radius=None, n=48):
        """A tube along a curve through pts: (x, y, z) with `radius`, or
        (x, y, z, radius)."""
        path = catmull([tuple(p) + (radius,) if len(p) == 3 else tuple(p) for p in pts], n)
        segs = [(path[i], path[i + 1]) for i in range(n)]
        lo = [min(min(a[d], b[d]) - max(a[3], b[3]) for a, b in segs) for d in range(3)]
        hi = [max(max(a[d], b[d]) + max(a[3], b[3]) for a, b in segs) for d in range(3)]

        def f(x, y, z):
            q = (x, y, z)
            if any(q[d] < lo[d] or q[d] > hi[d] for d in range(3)):
                return False
            for a, b in segs:
                d = [b[k] - a[k] for k in range(3)]
                dd = sum(c * c for c in d) or 1e-9
                t = max(0.0, min(1.0, sum((q[k] - a[k]) * d[k] for k in range(3)) / dd))
                r = a[3] + (b[3] - a[3]) * t
                if sum((q[k] - a[k] - d[k] * t) ** 2 for k in range(3)) <= r * r:
                    return True
            return False
        self.shapes.append(f)
        return self

    def slab(self, cx, cz, rx, rz, y0, y1, p=2.0, sheer=0.0, keel=0.0):
        """An upright prism on a superellipse plan (a hull, a plinth); its
        top rises `sheer` towards the ends along x (a boat's bow and stern)
        and its bottom `keel` (the stem standing out of the water)."""
        def f(x, y, z):
            k = (x - cx) / rx
            return (y0 + keel * k ** 4 <= y <= y1 + sheer * k * k
                    and abs(k) ** p + (abs(z - cz) / rz) ** p <= 1.0)
        self.shapes.append(f)
        return self

    def plate(self, poly, z0, z1):
        """An upright plate between z0 and z1, its outline `poly` in (x, y)
        (a sail)."""
        def f(x, y, z):
            if not z0 <= z <= z1:
                return False
            inside = False
            n = len(poly)
            for i in range(n):
                (xa, ya), (xb, yb) = poly[i], poly[(i + 1) % n]
                if (ya > y) != (yb > y) and x < xa + (y - ya) * (xb - xa) / (yb - ya):
                    inside = not inside
            return inside
        self.shapes.append(f)
        return self

    def lathe(self, cx, cz, profile, squash=1.0):
        """A turned thing (a stone lantern): `profile` lists (y, radius) up
        it; its plan an ellipse `squash` times as deep as wide, the way the
        GBA draws round things."""
        ys = [p[0] for p in profile]

        def f(x, y, z):
            if not ys[0] <= y <= ys[-1]:
                return False
            k = max(i for i in range(len(ys)) if ys[i] <= y)
            if k == len(ys) - 1:
                r = profile[k][1]
            else:
                (ya, ra), (yb, rb) = profile[k], profile[k + 1]
                r = ra + (rb - ra) * (y - ya) / (yb - ya)
            if r <= 0:
                return False
            return ((x - cx) / r) ** 2 + ((z - cz) / (squash * r)) ** 2 <= 1.0
        self.shapes.append(f)
        return self

    def profile(self, poly, x0, x1):
        """A prism across x from x0 to x1, its section `poly` in (z, y) (a
        sun lounger's back, seat and footrest)."""
        def f(x, y, z):
            if not x0 <= x <= x1:
                return False
            inside = False
            n = len(poly)
            for i in range(n):
                (za, ya), (zb, yb) = poly[i], poly[(i + 1) % n]
                if (ya > y) != (yb > y) and z < za + (y - ya) * (zb - za) / (yb - ya):
                    inside = not inside
            return inside
        self.shapes.append(f)
        return self

    def pyramid(self, cx, cz, rx, rz, y0, h, sides=6):
        """A pyramid on a regular polygon (a hexagonal roof lantern), its
        plan squashed to rx by rz, standing at y0, `h` high."""
        def f(x, y, z):
            if not y0 <= y <= y0 + h:
                return False
            k = 1.0 - (y - y0) / h
            dx, dz = (x - cx) / (rx * k + 1e-9), (z - cz) / (rz * k + 1e-9)
            ang = math.atan2(dz, dx)
            sector = 2 * math.pi / sides
            a = (ang % sector) - sector / 2
            return math.hypot(dx, dz) * math.cos(a) <= math.cos(sector / 2)
        self.shapes.append(f)
        return self

    def prism(self, plan, y0, y1):
        """An upright prism on a plan polygon `plan` in (x, z) (a hall with
        its corners cut off)."""
        def f(x, y, z):
            if not y0 <= y <= y1:
                return False
            inside = False
            n = len(plan)
            for i in range(n):
                (xa, za), (xb, zb) = plan[i], plan[(i + 1) % n]
                if (za > z) != (zb > z) and x < xa + (z - za) * (xb - xa) / (zb - za):
                    inside = not inside
            return inside
        self.shapes.append(f)
        return self

    def hip(self, x0, x1, z0, z1, y0, rise, ymax=None):
        """A hipped roof over the rectangle, from its eaves at y0 rising
        `rise` per pixel in from every side (to `ymax` at most: the ridge)."""
        def f(x, y, z):
            if not (x0 <= x <= x1 and z0 <= z <= z1) or y < y0:
                return False
            top = y0 + rise * min(x - x0, x1 - x, z - z0, z1 - z)
            if ymax is not None:
                top = min(top, ymax)
            return y <= top
        self.shapes.append(f)
        return self

    def box(self, x0, x1, y0, y1, z0, z1):
        self.shapes.append(lambda x, y, z: x0 <= x <= x1 and y0 <= y <= y1 and z0 <= z <= z1)
        return self

    def hollow(self, maker, *args, **kw):
        """Takes a shape out (a boat's inside): `maker` is one of these
        methods, given the shape's own arguments."""
        n = len(self.shapes)
        maker(*args, **kw)
        self.holes += self.shapes[n:]
        del self.shapes[n:]
        return self

    def inside(self, x, y, z):
        return any(f(x, y, z) for f in self.shapes) and not any(f(x, y, z) for f in self.holes)


def own_bands(tex, rows, parts):
    """A texture of Mound.with_ring with one more band per part: the
    drawing grown out of that part's own pixels only (`parts`: name ->
    test (u, v)), so a part's hidden faces never take another part's
    colours (a parasol's pole, the canopy). Returns the texture and each
    part's band index."""
    W = tex.size[0]
    out = Image.new("RGBA", (W, rows * (3 + len(parts))), (0, 0, 0, 0))
    out.paste(tex, (0, 0))
    src = tex.load()
    index = {}
    for k, (name, mine) in enumerate(parts.items()):
        art = Image.new("RGBA", (W, rows), (0, 0, 0, 0))
        apx = art.load()
        for v in range(rows):
            for u in range(W):
                if src[u, v][3] >= 128 and mine(u, v):
                    apx[u, v] = src[u, v]
        grown = Mound.with_ring(art, ()).crop((0, 2 * rows, W, 3 * rows))
        out.paste(grown, (0, rows * (3 + k)))
        index[name] = 3 + k
    return out, index


class _RayLifted:
    """A mesh whose points are all moved `lift` up and `lift` south: along
    the 45-degree ray, so the drawing stays where it is."""

    def __init__(self, mesh, lift):
        self.mesh, self.lift = mesh, lift

    def tri(self, a, b, c, shade, tag):
        L = self.lift
        self.mesh.tri(*[(p[0], p[1] + L, p[2] + L) + tuple(p[3:]) for p in (a, b, c)],
                      shade, tag)


# A spec's level of detail (step, cell, tol), set by gen_voxel_buildings while
# its parts are built: every HeightSolid made meanwhile takes at least those.
HEIGHTSOLID_LOD = None


class HeightSolid:
    """A drawn round thing as a height field over its plan, exact at 45
    degrees and closed.

    `volume` gives the thing's shape (a dome, a crown on it, cloth arms in
    relief...): over the plan, the height of its top. The drawing then
    cuts it, column by column of pixels: the 45-degree ray of a column
    stays in it, and grazes it at min(z - y), which must be where the
    drawing's column starts; so no point of the column stands past the ray
    through its first drawn pixel (the volume is cut there by a slope of 45
    degrees, edge-on to the view), and its plan ends where the drawing's
    column ends on the ground. The outline is the drawing's, pixel for
    pixel; the volume gives only the shape inside it.

    The plan is cut in cells 2 pixels wide, 1 where the outline jumps from
    one pixel column to the next (a side drawn upright). Each cell is cut by
    its own columns; where two cells' heights part, an upright wall closes
    the step between them (edge-on to the view too), and walls close the
    ends.

    Faces the 45-degree view sees take the drawing projected, (x, z - y):
    exact. Faces it does not (the back past the grazing ray) take the
    drawing's grown band (Mound.with_ring), mirrored in the plan about
    z = `mirror_z`: the back carries the front opposite it.

    `mask(u, v)`: pixels that are someone else's (the porch, modelled on
    its own): they neither start nor end a column, and the back is never
    laid with them.
    """

    def __init__(self, name, art, rows, volume, step=2, depth=(0.0, 80.0), mask=None,
                 tmax=96.0, mirror_z=None, lift=0.0, band=2, cell=2, tol=1, behind=None,
                 hang=(), side=None, open_top=False, lean=None):
        self.lift, self.cell, self.tol = lift, cell, tol
        if HEIGHTSOLID_LOD is not None:
            # a coarser grid (the spec's "lod"): the drawing still projected
            # on every face the view sees, a fraction of the triangles
            step = max(step, HEIGHTSOLID_LOD[0])
            self.cell, self.tol = max(cell, HEIGHTSOLID_LOD[1]), max(tol, HEIGHTSOLID_LOD[2])
        # the plan's west and east ends leaned: heights fall to the ground
        # there, `lean` px per px (a cloth flap draped down a tent's flank,
        # not the volume cut upright at the drawing's last column)
        self.lean = lean
        # a drawing cut by the map's top edge (Lilycove's department store):
        # what starts on its first row goes on above it, uncut
        self.open_top = open_top
        self.side = side
        # pixel columns drawn up in the air only (a tower's arm): their
        # lowest pixel is no foot on the ground
        self.hang = set(hang)
        # pixels of something standing behind this solid: not its own, but
        # they hide none of its ends (the rocket over the Space Center's roof)
        self.behind = behind or (lambda u, v: False)
        self.name, self.art, self.rows, self.vol = name, art, rows, volume
        self.step, self.depth, self.tmax, self.mirror_z = step, depth, tmax, mirror_z
        self.mask = mask or (lambda u, v: False)
        # the band of the texture the faces no drawing shows are laid with
        # (Mound.with_ring's grown drawing; own_bands for a thing of parts)
        self.band = band * rows
        self.short = {}     # pixel columns the volume does not reach the drawn top of

    def _top(self, x, z):
        ins = self.vol.inside
        y = self.tmax
        while y >= 0 and not ins(x, y, z):
            y -= 1.0
        if y < 0:
            return 0.0
        lo, hi = y, y + 1.0
        for _ in range(7):
            m = (lo + hi) / 2
            if ins(x, m, z):
                lo = m
            else:
                hi = m
        return lo

    def _runs(self):
        """Per pixel column: (first drawn row, row its drawing ends on the
        ground or None where someone else's pixels hide its foot)."""
        W, H = self.art.size[0], self.rows
        px = self.art.load()
        runs = {}
        for u in range(W):
            v = [y for y in range(H) if px[u, y][3] >= 128]
            own = [y for y in v if not self.mask(u, y) and not self.behind(u, y)]
            if own:
                # someone else's pixels just above the first or below the
                # last own one: that end of the column is behind them
                top = own[0] - 1 >= 0 and px[u, own[0] - 1][3] >= 128 and self.mask(u, own[0] - 1)
                foot = u in self.hang or (own[-1] + 1 < H and px[u, own[-1] + 1][3] >= 128
                                          and self.mask(u, own[-1] + 1))
                if self.open_top and own[0] == 0:
                    top = True
                runs[u] = (-1e9 if top else own[0], None if foot else own[-1] + 1, own[0])
        return runs

    def cells(self):
        """The plan's cells [x0, x1), each with its two edge profiles."""
        W, st = self.art.size[0], self.step
        runs = self._runs()
        z0, z1 = self.depth
        # the plan's own two edges closed by a sample of nothing just past them
        inner = [z0 + j * st for j in range(int((z1 - z0) / st) + 1)]
        zs = [z0 - 1e-3] + inner + [z1 + 1e-3]
        cache = {}

        def profile(x):
            if x not in cache:
                cache[x] = [0.0] + [self._top(x, z) for z in inner] + [0.0]
            return cache[x]
        out, spans = [], {}
        u = 0
        while u < W:
            w = 1
            if u + 1 < W and u in runs and u + 1 in runs:
                a, b = runs[u], runs[u + 1]
                tol = self.tol
                if abs(a[0] - b[0]) <= tol and (a[1] is None) == (b[1] is None) and \
                        (a[1] is None or abs(a[1] - b[1]) <= tol):
                    w = 2
                    # wider over columns drawn alike (a flat front), up to
                    # `cell`, their tops `tol` apart at most (a corrugated edge)
                    while w < self.cell and u + w in runs and \
                            abs(runs[u + w][0] - a[0]) <= tol and \
                            (runs[u + w][1] is None) == (a[1] is None) and \
                            (a[1] is None or abs(runs[u + w][1] - a[1]) <= tol):
                        w += 1
            cols = [runs[k] for k in range(u, u + w) if k in runs]
            if cols:
                vt = min(c[0] for c in cols)
                feet = [c[1] for c in cols if c[1] is not None]
                vf = max(feet) if len(feet) == len(cols) else None
                edges = [self._edge(profile(x), zs, vt, vf) for x in (u, u + w)]
                if edges[0] is None and edges[1] is None:
                    # drawn, but the volume is not there (a bow out of the
                    # water): a sliver a pixel thick standing at the foot,
                    # as tall as the column is drawn
                    first = min(c[2] for c in cols)
                    end = vf if vf is not None else max(c[1] or c[2] + 1 for c in cols)
                    edges = [[end - first if end - 1.0 <= z <= end else 0.0 for z in zs]] * 2
                    self.short[u] = float("inf")
                # an edge the volume misses takes the other one's
                edges = [e if e is not None else edges[1 - k] for k, e in enumerate(edges)]
                if vt > -1e8:
                    # the middle of the cell must reach the drawn top too: two
                    # edges grazing it at different depths sag between them
                    lo = vt - 0.5
                    a, b = list(edges[0]), list(edges[1])
                    mid = [(z - (p + q) / 2.0, k) for k, (z, p, q) in enumerate(zip(zs, a, b))
                           if p > 0 or q > 0]
                    if mid and min(mid)[0] > lo + 0.01:
                        k = min(mid)[1]
                        # a few pixels at most: a sag, not a top the volume
                        # never reaches (a tail drawn high over the body)
                        if zs[k] - lo - max(a[k], b[k]) <= 6.0:
                            a[k] = max(a[k], zs[k] - lo)
                            b[k] = max(b[k], zs[k] - lo)
                            edges = [a, b]
                for e in edges:
                    short = min([z - h for z, h in zip(zs, e) if h > 0] or [0.0]) - vt
                    if short > 0.01 and vt > -1e8:
                        self.short[u] = max(self.short.get(u, 0), short)
                out.append((u, u + w, edges[0], edges[1]))
                spans[u] = (vt, vf)
            u += w
        if self.lean:
            out = self._leaned(zs, out, spans)
        return zs, out

    def _leaned(self, zs, cells, spans):
        """The cells with their heights capped by the lean from the plan's
        west and east ends, row by row (lower only: the drawing stays exact,
        every ray that met the solid still meets it, on its footprint)."""
        s = float(self.lean)
        out, ends = [], []
        for j in range(len(zs)):
            xs = [x for (x0, x1, a, b) in cells for x, e in ((x0, a), (x1, b)) if e[j] > 0]
            ends.append((min(xs), max(xs)) if xs else None)
        for (x0, x1, a, b) in cells:
            na, nb = [], []
            vt, vf = spans[x0]
            for col, new, x in ((a, na, x0), (b, nb, x1)):
                leaned = False
                for j, h in enumerate(col):
                    if h > 0 and ends[j]:
                        lo, hi = ends[j]
                        cap = min(max(0.25, s * (x - lo)), max(0.25, s * (hi - x)))
                        if cap < h:
                            h, leaned = cap, True
                    new.append(h)
                if leaned and vt > -1e8:
                    # lowered, the flap must still lie under every drawn
                    # pixel's ray: on the ground back to where it starts
                    foot = vf if vf is not None else max(z for z, h in zip(zs, new) if h > 0)
                    for j, z in enumerate(zs):
                        if vt - 0.5 <= z <= foot and new[j] <= 0:
                            new[j] = 0.25
            out.append((x0, x1, na, nb))
        return out

    def _theirs(self, u, v):
        """Whether the drawing's pixel at (u, v) is someone else's."""
        u = min(self.art.size[0] - 1, max(0, int(u)))
        v = min(self.rows - 1, max(0, int(v)))
        return self.art.getpixel((u, v))[3] >= 128 and self.mask(u, v)

    def _edge(self, col, zs, vt, vf):
        """One edge of a cell, the volume cut to its columns' drawing: None
        where the volume is not there at all."""
        if vf is not None:
            col = [h if z <= vf else 0.0 for h, z in zip(col, zs)]
        # nothing stands past the ray through the first drawn pixel (half a
        # pixel above it, so that its whole row is covered: what stands in
        # the clear row above takes clear texels)
        vt = vt - 0.5
        col = [max(0.0, min(h, z - vt)) if h > 0 else 0.0 for h, z in zip(col, zs)]
        if not any(h > 0 for h in col):
            return None
        if vf is not None:
            # short of the drawn foot: its front carried on to it
            last = max(j for j, h in enumerate(col) if h > 0)
            col = [col[last] if zs[last] < z <= vf else h for h, z in zip(col, zs)]
        short = min(z - h for z, h in zip(zs, col) if h > 0) - vt
        if short > 0.01 and vt > -1e8:
            # short of the drawn top: its back carried on north at the height
            # of its first point, to where the ray of the first drawn pixel
            # meets it (a few pixels at most: further, a shelf would stand out)
            first = min(j for j, h in enumerate(col) if h > 0)
            h0 = col[first]
            if zs[first] - (vt + h0) <= 6.0:
                col = [h0 if vt + h0 - 1e-6 <= z < zs[first] else h for h, z in zip(col, zs)]
            short = min(z - h for z, h in zip(zs, col) if h > 0) - vt
            if short > 0.01:
                # still short (the plan's edge): raised by what it lacks
                col = [min(z - vt, h + short) if h > 0 else 0.0 for h, z in zip(col, zs)]
        return col

    def emit(self, mesh):
        if self.lift:
            # lifted along the 45-degree ray (up and south alike): the same
            # drawing, standing in the air (a parasol's canopy); its
            # underside closed
            mesh = _RayLifted(mesh, self.lift)
        zs, cells = self.cells()
        self.grid = (zs, cells)
        if self.lift:
            self._floor(mesh, zs, cells)
        n = len(zs)
        W = self.art.size[0]
        zc = self.mirror_z
        # each column's own drawn rows (the back's texture stays in them)
        _px = self.art.load()
        spans = {}
        for _u in range(W):
            _v = [r for r in range(self.rows) if _px[_u, r][3] >= 128 and not self.mask(_u, r)
                  and not self.behind(_u, r)]
            if _v:
                spans[_u] = (_v[0], _v[-1] + 1)
        self._spans = spans

        def visible(col, j):
            h, z = col[j], zs[j]
            if h <= 0:
                return False
            return all(col[k] < h + (zs[k] - z) - 1e-6 for k in range(j + 1, n))

        def mirrored(col, x, h, z):
            if zc is None:
                return z - h
            zm = min(zs[-1], max(zs[0], 2 * zc - z))
            j = min(n - 2, int((zm - zs[0]) / (zs[1] - zs[0])))
            g = (zm - zs[j]) / (zs[1] - zs[0])
            v = zm - (col[j] * (1 - g) + col[j + 1] * g)
            u = min(W - 1, max(0, int(x)))
            v = min(v, self.rows - 1.0)
            while v > 0 and self.mask(u, int(v)):
                v -= 1.0
            # within the column's own drawn rows: past them the grown band
            # runs out and the back would show holes
            span = spans.get(u)
            if span is not None:
                v = min(max(v, span[0] + 0.5), span[1] - 0.5)
            return v

        def tex(x, u_in, v):
            return (min(W - 1e-3, max(0.0, u_in)), min(self.rows - 1e-3, max(0.0, v)) + self.band)

        for (x0, x1, ca, cb) in cells:
            va = [visible(ca, j) for j in range(n)]
            vb = [visible(cb, j) for j in range(n)]
            for j in range(n - 1):
                hs = (ca[j], cb[j], cb[j + 1], ca[j + 1])
                if all(h <= 0 for h in hs):
                    continue
                pts = [(x0, ca[j], zs[j]), (x1, cb[j], zs[j]), (x1, cb[j + 1], zs[j + 1]),
                       (x0, ca[j + 1], zs[j + 1])]
                cols = (ca, cb, cb, ca)
                front = va[j] or vb[j] or va[j + 1] or vb[j + 1]
                qb = [p + tex(p[0], p[0], mirrored(c, p[0], p[1], p[2]))
                      for p, c in zip(pts, cols)]
                if front:
                    q = [p + (p[0], p[2] - p[1]) for p in pts]
                    # under another part's pixels (the shadow under a
                    # parasol's canopy): that part hides it at 45 degrees,
                    # and elsewhere it shows its own colours
                    uc = sum(p[0] for p in pts) / 4.0
                    vc = sum(p[2] - p[1] for p in pts) / 4.0
                    probe = [(0.75 * p[0] + 0.25 * uc, 0.75 * (p[2] - p[1]) + 0.25 * vc) for p in pts]
                    if all(self._theirs(u, v) for u, v in probe + [(uc, vc)]):
                        q = [p + tex(p[0], p[0], p[2] - p[1]) for p in pts]
                else:
                    q = qb
                for k3 in ((0, 1, 2), (0, 2, 3)):
                    tri = tuple(q[k] for k in k3)
                    edge_on = False
                    if front and max(pts[k][2] - pts[k][1] for k in k3) -                             min(pts[k][2] - pts[k][1] for k in k3) < 0.25:
                        # the cut along the ray of the first drawn pixel:
                        # edge-on at 45 degrees, where it would take the clear
                        # row above the drawing - from the side a hole.
                        # Dressed as the back.
                        tri, edge_on = tuple(qb[k] for k in k3), True
                    nrm = Mound._area(*tri)
                    L = math.sqrt(sum(c * c for c in nrm))
                    if L < 1e-9:
                        continue
                    if nrm[1] < 0:          # wound to face up
                        tri = (tri[0], tri[2], tri[1])
                        nrm = tuple(-c for c in nrm)
                    if front and not edge_on:
                        mesh.tri(tri[0], tri[1], tri[2], SHADE_ART, self.name + "~proj")
                    else:
                        f = (nrm[1] + nrm[2]) / (L * math.sqrt(2.0))
                        s = SHADE_BACK + (SHADE_ART - SHADE_BACK) * max(0.0, min(1.0, 0.5 + 0.5 * f))
                        mesh.tri(tri[0], tri[1], tri[2], s, self.name + ".back~behind~proj")
        # upright walls: between cells whose heights part, and at the ends
        flat = [0.0] * n
        for k in range(len(cells) + 1):
            left = cells[k - 1] if k > 0 else None
            right = cells[k] if k < len(cells) else None
            if left and right and left[1] != right[0]:
                self._wall(mesh, zs, left[1], left[3], flat, tex, +1)
                self._wall(mesh, zs, right[0], right[2], flat, tex, -1)
                continue
            if left is None and right is not None:
                self._wall(mesh, zs, right[0], right[2], flat, tex, -1)
            elif right is None and left is not None:
                self._wall(mesh, zs, left[1], left[3], flat, tex, +1)
            elif left and right:
                # the taller side shows its step, facing the lower one
                lo = [min(a, b) for a, b in zip(left[3], right[2])]
                self._wall(mesh, zs, left[1], left[3], lo, tex, +1, outer=False)
                self._wall(mesh, zs, right[0], right[2], lo, tex, -1, outer=False)

    def _held(self, u, v, x):
        """A texel for a step's wall: in a drawn column (the nearest one
        towards the wall's own x), within its drawn rows - past them the
        drawing is clear and the wall a hole from the side."""
        spans = getattr(self, "_spans", None)
        if not spans:
            return u, v
        W = self.art.size[0]
        k = int(min(W - 1, max(0, u)))
        if k not in spans:
            way = 1 if x > u else -1
            while 0 <= k < W and k not in spans:
                k += way
            if k not in spans:
                return u, v
            u = k + 0.5
        a, b = spans[k]
        return u, min(max(v, a + 0.5), b - 0.5)

    def _floor(self, mesh, zs, cells):
        """The underside of a lifted solid, laid with the grown drawing."""
        W = self.art.size[0]
        for (x0, x1, ca, cb) in cells:
            for j in range(len(zs) - 1):
                if not (ca[j] > 0 or cb[j] > 0 or ca[j + 1] > 0 or cb[j + 1] > 0):
                    continue
                q = [(x, 0.0, z, min(W - 1e-3, max(0.0, x)),
                      min(self.rows - 1e-3, max(0.0, z)) + self.band)
                     for (x, z) in ((x0, zs[j]), (x0, zs[j + 1]), (x1, zs[j + 1]), (x1, zs[j]))]
                for tri in ((q[0], q[1], q[2]), (q[0], q[2], q[3])):
                    nrm = Mound._area(*tri)
                    if nrm[1] > 0:
                        tri = (tri[0], tri[2], tri[1])
                    mesh.tri(tri[0], tri[1], tri[2], SHADE_BACK, self.name + ".under~behind~proj")

    def _wall(self, mesh, zs, x, top, bottom, tex, side, outer=True):
        """An upright wall at x from `bottom` up to `top`, facing `side`
        (+1 east, -1 west). With `side`, dressed as Oldale's sides are: a strip of the
        facade's own columns (`side`: offsets from the edge, inwards) laid
        over and over along its depth, each height taking the facade's row
        at that height, so courses, plinths and eaves run round the corner
        (one column smeared back along the ray only made stripes)."""
        shade = SHADE_EAST if side > 0 else SHADE_WEST
        live = [j for j in range(len(zs)) if top[j] - bottom[j] >= 0.05]
        if not live:
            return
        # a step a few pixels deep (a cut corner's stair, a ledge) is part
        # of the drawing round it: only a true side takes the strip
        # an inner step (a roof's foot over a terrace) is drawing too
        if self.side is None or not outer or zs[max(live)] - zs[min(live)] <= 6.0:
            if zs[max(live)] - zs[min(live)] <= 6.0:
                # one step of a stair: lit as the faces beside it, so the
                # stair reads as the one slanting face it stands for
                shade = SHADE_ART
            # a step of a curve or a cut corner: unfolded onto the drawing as
            # the slanting face the stair stands for - each pixel of depth the
            # next column out - so it goes on from the faces beside it (one
            # column stretched over the depth made stripes)
            zf = zs[max(live)]
            W = self.art.size[0]
            # at the solid's own end there is no column further out (it
            # would hold the outline, stretched): the next columns in, the
            # end's own drawing folded back round it
            way = side
            if outer and not 0.0 <= x + side * (zs[max(live)] - zs[min(live)]) <= W:
                way = -side
            for j in range(len(zs) - 1):
                if top[j] - bottom[j] < 0.05 and top[j + 1] - bottom[j + 1] < 0.05:
                    continue
                pts = [(x, bottom[j], zs[j]), (x, bottom[j + 1], zs[j + 1]),
                       (x, top[j + 1], zs[j + 1]), (x, top[j], zs[j])]
                q = [p + tex(x, *self._held(x + way * (zf - p[2]) - 0.5 * way, p[2] - p[1], x))
                     for p in pts]
                for tri in ((q[0], q[1], q[2]), (q[0], q[2], q[3])):
                    nrm = Mound._area(*tri)
                    if sum(c * c for c in nrm) < 1e-12:
                        continue
                    if nrm[0] * side < 0:
                        tri = (tri[0], tri[2], tri[1])
                    mesh.tri(tri[0], tri[1], tri[2], shade, self.name + ".wall~behind~proj")
            return
        zf = zs[max(live)]
        if len(self.side) == 3:
            # columns u0-u1 of the facade standing on row `foot`: a straight
            # stretch of it, where the wall's own edge is drawn slanting
            u0, u1, foot = self.side
            P = float(u1 - u0)
            vfoot = float(foot)

            def u_at(k):
                return u0 + k if side < 0 else u1 - 1e-3 - k
        else:
            a, b = self.side
            P = float(b - a)
            vfoot = zf

            def u_at(k):
                return x + a + k if side < 0 else x - 1e-3 - a - k

        def lerp(arr, j, g):
            return arr[j] + (arr[j + 1] - arr[j]) * g
        for j in range(len(zs) - 1):
            if top[j] - bottom[j] < 0.05 and top[j + 1] - bottom[j + 1] < 0.05:
                continue
            # cut the step where the strip starts over
            za, zb = zs[j], zs[j + 1]
            cuts = [za, zb]
            k = math.floor((zf - zb) / P) + 1
            while k * P < zf - za - 1e-6:
                cuts.append(zf - k * P)
                k += 1
            cuts = sorted(set(cuts))
            for c0, c1 in zip(cuts, cuts[1:]):
                g0, g1 = (c0 - za) / (zb - za), (c1 - za) / (zb - za)
                base = math.floor((zf - (c0 + c1) / 2.0) / P) * P
                pts = [(x, lerp(bottom, j, g0), c0), (x, lerp(bottom, j, g1), c1),
                       (x, lerp(top, j, g1), c1), (x, lerp(top, j, g0), c0)]
                q = [p + tex(x, u_at(min(P - 1e-3, max(0.0, zf - p[2] - base))), vfoot - p[1])
                     for p in pts]
                for tri in ((q[0], q[1], q[2]), (q[0], q[2], q[3])):
                    nrm = Mound._area(*tri)
                    if sum(c * c for c in nrm) < 1e-12:
                        continue
                    if nrm[0] * side < 0:
                        tri = (tri[0], tri[2], tri[1])
                    mesh.tri(tri[0], tri[1], tri[2], shade, self.name + ".wall~behind~proj")


class PlanPrism:
    """An upright prism on a plan polygon (x, z) - a block with its corners
    cut, a rounded one - from y0 to y1, its faces flat whatever their
    direction (a stair of cells made a cut corner a sheaf of plates).

    A face the 45-degree view sees (turned south, not edge on) is one quad
    laid with the drawing projected onto it: exact, and a slanting face
    shows its drawn pixels at their own width. The top is projected too. A
    side or back face the drawing never shows is dressed as Oldale's sides
    are: `side` = (u0, u1, foot), columns u0-u1 of a straight stretch of the
    facade standing on row `foot`, repeated along the face, each height
    taking the facade's row at that height."""

    def __init__(self, name, plan, y0, y1, side, top=None, shrink=1.0, open_top=False,
                 inward=False, art=None):
        self.name, self.plan = name, [tuple(map(float, p)) for p in plan]
        self.y0, self.y1, self.side = float(y0), float(y1), side
        # `shrink`: the top's plan is the foot's scaled about its middle (a
        # frustum: a tower's tapering crown)
        self.shrink = shrink
        # `top`: a Tile its roof is laid with when the drawing does not show
        # it (a block cut by the map's top edge)
        self.top = top
        # `open_top`: no top at all (another part lays it: a basin's rim);
        # `inward`: the walls of a hollow, facing into it (a pool's sides)
        self.open_top, self.inward = open_top, inward
        # `art`: the texture the strip is in. Where every texel of the strip
        # a side face reads is one opaque colour, the face is one polygon
        # reading one of them (~clamp) instead of a piece per repeat of the
        # strip: the same colour on the same pixels, a few triangles instead
        # of dozens (a fence rail's back, 2 px of strip repeated along it,
        # was a pair of triangles every 2 px - and every triangle a chunk
        # holds is built, lit and packed on the console)
        self.art = art

    def _plain_texel(self, tile):
        """The (u, v) of a texel of the tile when all of its rectangle is one
        opaque colour in `art`, else None."""
        if self.art is None:
            return None
        u0, v0, u1, v1 = (int(math.floor(c + EPS)) if k < 2 else int(math.ceil(c - EPS))
                          for k, c in enumerate(tile.rect))
        W, H = self.art.size
        if u1 <= u0 or v1 <= v0 or u0 < 0 or v0 < 0 or u1 > W or v1 > H:
            return None
        px = self.art.load()
        first = px[u0, v0]
        if len(first) < 4 or first[3] < 128:
            return None
        for v in range(v0, v1):
            for u in range(u0, u1):
                c = px[u, v]
                if c[3] < 128 or c[:3] != first[:3]:
                    return None
        return (u0 + 0.5, v0 + 0.5)

    def emit(self, mesh):
        pl = self.plan
        n = len(pl)
        area = sum(pl[i][0] * pl[(i + 1) % n][1] - pl[(i + 1) % n][0] * pl[i][1] for i in range(n))
        y0, y1 = self.y0, self.y1
        u0, u1, foot = self.side
        # a wall taller than the drawing above its foot: the strip's rows
        # repeat up it (a tower whose top is off the map)
        tile = Tile(u0, max(0.0, foot - (y1 - y0)), u1, foot - 0.0)
        plain = self._plain_texel(tile)
        cx = sum(p[0] for p in pl) / n
        cz = sum(p[1] for p in pl) / n
        k_ = self.shrink
        up = [(cx + (x - cx) * k_, cz + (z - cz) * k_) for (x, z) in pl]

        def at(i, s_, y):
            """The point of face i at distance s_ along its foot edge, height y."""
            (xa, za), (xb, zb) = pl[i], pl[(i + 1) % n]
            (ta, sa), (tb, sb) = up[i], up[(i + 1) % n]
            L = math.hypot(xb - xa, zb - za)
            g, f = s_ / L, (y - y0) / (y1 - y0)
            fx, fz = xa + (xb - xa) * g, za + (zb - za) * g
            hx, hz = ta + (tb - ta) * g, sa + (sb - sa) * g
            return (fx + (hx - fx) * f, y, fz + (hz - fz) * f)
        for i in range(n):
            (xa, za), (xb, zb) = pl[i], pl[(i + 1) % n]
            L = math.hypot(xb - xa, zb - za)
            if L < 1e-6:
                continue
            # the outward normal: right of a->b for a plan wound with
            # positive area in (x, z), left otherwise
            nx, nz = (zb - za) / L, -(xb - xa) / L
            if (area < 0) != self.inward:
                nx, nz = -nx, -nz
            if nz > 0.02 and abs(nx) < 0.995:
                pts = [(xa, y0, za), (xb, y0, zb), (up[(i + 1) % n][0], y1, up[(i + 1) % n][1]),
                       (up[i][0], y1, up[i][1])]
                q = [p + (p[0], p[2] - p[1]) for p in pts]
                self._quad(mesh, q, (nx, 0.0, nz), SHADE_ART, self.name + "~proj")
                continue
            # dressed with the strip, laid from the end it faces left of
            a, b = ((xa, za), (xb, zb))
            # rows counted so that the foot of the wall takes the strip's foot
            ph = tile.rect[3] - tile.rect[1]
            off = (-(y1 - y0)) % ph
            face = [(0.0, off), (L, off), (L, off + y1 - y0), (0.0, off + y1 - y0)]
            shade = SHADE_EAST if nx > 0.3 else SHADE_WEST if nx < -0.3 else SHADE_BACK
            if plain is not None:
                # one colour wherever the pieces would read: the face whole
                pts = [at(i, s, y1 - (t - off)) + plain for s, t in face]
                for k in range(1, len(pts) - 1):
                    self._tri(mesh, (pts[0], pts[k], pts[k + 1]), (nx, 0.0, nz), shade,
                              self.name + ".side~behind~clamp")
                continue
            for piece in tile_pieces(face, tile, None, None):
                pts = [at(i, s, y1 - (t - off)) + (u, v) for s, t, u, v in piece]
                for k in range(1, len(pts) - 1):
                    self._tri(mesh, (pts[0], pts[k], pts[k + 1]), (nx, 0.0, nz), shade,
                              self.name + ".side~behind")
        if self.open_top:
            return
        # a convex top is a fan; a U's (a counter's arms and bar) is cut
        # into ears, or the fan would roof the yard between the arms
        turns = [(up[(i + 1) % n][0] - up[i][0]) * (up[(i + 2) % n][1] - up[(i + 1) % n][1])
                 - (up[(i + 1) % n][1] - up[i][1]) * (up[(i + 2) % n][0] - up[(i + 1) % n][0])
                 for i in range(n)]
        if all(t >= -EPS for t in turns) or all(t <= EPS for t in turns):
            fan = [(0, k, k + 1) for k in range(1, n - 1)]
        else:
            fan = triangulate(up)
        if self.top is not None:
            for (a, b, c) in fan:
                tri = [up[a], up[b], up[c]]
                for piece in tile_pieces([(x, z) for (x, z) in tri], self.top, None, None):
                    pts = [(s_, y1, t, u, v) for s_, t, u, v in piece]
                    for j in range(1, len(pts) - 1):
                        self._tri(mesh, (pts[0], pts[j], pts[j + 1]), (0.0, 1.0, 0.0), SHADE_ART,
                                  self.name + ".roof~behind")
            return
        top = [(x, y1, z, x, z - y1) for (x, z) in up]
        for (a, b, c) in fan:
            self._tri(mesh, (top[a], top[b], top[c]), (0.0, 1.0, 0.0), SHADE_ART, self.name + "~proj")

    @staticmethod
    def _tri(mesh, tri, normal, shade, tag):
        a, b, c = tri
        e1 = [b[k] - a[k] for k in range(3)]
        e2 = [c[k] - a[k] for k in range(3)]
        nrm = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
        if sum(nrm[k] * normal[k] for k in range(3)) < 0:
            b, c = c, b
        mesh.tri(a, b, c, shade, tag)

    def _quad(self, mesh, q, normal, shade, tag):
        self._tri(mesh, (q[0], q[1], q[2]), normal, shade, tag)
        self._tri(mesh, (q[0], q[2], q[3]), normal, shade, tag)


class PlanPyramid:
    """A pyramid of flat faces over a plan polygon (x, z) at y0, its apex
    (x, y, z): a faceted roof (Sootopolis's spires). A face the 45-degree
    view sees takes the drawing projected onto it - exact; the others the
    drawing mirrored about the apex's z (the roof's own colours)."""

    def __init__(self, name, plan, y0, apex):
        self.name, self.plan = name, [tuple(map(float, p)) for p in plan]
        self.y0, self.apex = float(y0), tuple(map(float, apex))

    def emit(self, mesh):
        ax, ay, az = self.apex
        pl, y0 = self.plan, self.y0
        n = len(pl)
        cx = sum(p[0] for p in pl) / n
        cz = sum(p[1] for p in pl) / n
        for i in range(n):
            (xa, za), (xb, zb) = pl[i], pl[(i + 1) % n]
            a, b, c = (xa, y0, za), (xb, y0, zb), (ax, ay, az)
            e1 = [b[k] - a[k] for k in range(3)]
            e2 = [c[k] - a[k] for k in range(3)]
            nrm = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]]
            # outward: away from the plan's middle
            mx, mz = (xa + xb) / 2 - cx, (za + zb) / 2 - cz
            if nrm[0] * mx + nrm[2] * mz < 0:
                b, c = c, b
                nrm = [-k for k in nrm]
            if nrm[1] + nrm[2] > 1e-6:
                q = [p + (p[0], p[2] - p[1]) for p in (a, b, c)]
                mesh.tri(q[0], q[1], q[2], SHADE_ART, self.name + "~proj")
            else:
                q = [p + (p[0], 2 * az - p[2] - p[1]) for p in (a, b, c)]
                mesh.tri(q[0], q[1], q[2], SHADE_BACK, self.name + ".back~behind")


class PlatePrism:
    """A block whose front is a polygon (x, y) - a flat roof falling to a
    slope at one end (a cable car station) - run back from z1 to z0. The
    front, the roof and the slopes (every face turned up or south) take the
    drawing projected onto them; an upright end, the back and the bottom
    are dressed with `side` = (u0, u1, foot) as Oldale's sides are."""

    def __init__(self, name, poly, z0, z1, side):
        self.name, self.poly = name, [tuple(map(float, p)) for p in poly]
        self.z0, self.z1, self.side = float(z0), float(z1), side

    def emit(self, mesh):
        pl, z0, z1 = self.poly, self.z0, self.z1
        n = len(pl)
        area = sum(pl[i][0] * pl[(i + 1) % n][1] - pl[(i + 1) % n][0] * pl[i][1] for i in range(n))
        u0, u1, foot = self.side
        ytop = max(p[1] for p in pl)
        front = [(x, y, z1, x, z1 - y) for (x, y) in pl]
        for k in range(1, n - 1):
            PlanPrism._tri(mesh, (front[0], front[k], front[k + 1]), (0.0, 0.0, 1.0), SHADE_ART,
                           self.name + "~proj")
        for i in range(n):
            (xa, ya), (xb, yb) = pl[i], pl[(i + 1) % n]
            L = math.hypot(xb - xa, yb - ya)
            if L < 1e-6:
                continue
            nx, ny = (yb - ya) / L, -(xb - xa) / L
            if area < 0:
                nx, ny = -nx, -ny
            if ny > 0.05:
                q = [(xa, ya, z1), (xb, yb, z1), (xb, yb, z0), (xa, ya, z0)]
                q = [p + (p[0], p[2] - p[1]) for p in q]
                PlanPrism._tri(mesh, (q[0], q[1], q[2]), (nx, ny, 0.0), SHADE_ART, self.name + "~proj")
                PlanPrism._tri(mesh, (q[0], q[2], q[3]), (nx, ny, 0.0), SHADE_ART, self.name + "~proj")
                continue
            if ny < -0.5:
                continue        # the bottom, on the ground
            # an upright end: the strip along its depth, each height its row
            tile = Tile(u0, foot - ytop, u1, foot)
            shade = SHADE_EAST if nx > 0 else SHADE_WEST
            ylo, yhi = min(ya, yb), max(ya, yb)
            face = [(0.0, ytop - yhi), (z1 - z0, ytop - yhi), (z1 - z0, ytop - ylo), (0.0, ytop - ylo)]
            x = xa
            for piece in tile_pieces(face, tile, None, None):
                pts = [(x, ytop - t, z1 - s, u, v) for s, t, u, v in piece]
                for k in range(1, len(pts) - 1):
                    PlanPrism._tri(mesh, (pts[0], pts[k], pts[k + 1]), (nx, 0.0, 0.0), shade,
                                   self.name + ".side~behind")
        # the back: the strip across it
        tile = Tile(u0, foot - ytop, u1, foot)
        back = pl
        for k in range(1, n - 1):
            tri = [back[0], back[k], back[k + 1]]
            for piece in tile_pieces([(x, ytop - y) for (x, y) in tri], tile, None, None):
                pts = [(s, ytop - t, z0, u, v) for s, t, u, v in piece]
                for j in range(1, len(pts) - 1):
                    PlanPrism._tri(mesh, (pts[0], pts[j], pts[j + 1]), (0.0, 0.0, -1.0), SHADE_BACK,
                                   self.name + ".back~behind")


class SectionPrism:
    """A block whose section (z, y) - a roof's gable outline, an eave - runs
    across from x0 to x1 (Lilycove's harbour). Every face turned up or south
    takes the drawing projected onto it; the ends, the undersides and what
    faces north are dressed with `side` = (u0, u1, foot)."""

    def __init__(self, name, section, x0, x1, side):
        self.name, self.sec = name, [tuple(map(float, p)) for p in section]
        self.x0, self.x1, self.side = float(x0), float(x1), side

    def emit(self, mesh):
        sec, x0, x1 = self.sec, self.x0, self.x1
        n = len(sec)
        area = sum(sec[i][0] * sec[(i + 1) % n][1] - sec[(i + 1) % n][0] * sec[i][1] for i in range(n))
        u0, u1, foot = self.side
        ytop = max(p[1] for p in sec)
        zf = max(p[0] for p in sec)
        tile = Tile(u0, foot - ytop, u1, foot)
        for i in range(n):
            (za, ya), (zb, yb) = sec[i], sec[(i + 1) % n]
            L = math.hypot(zb - za, yb - ya)
            if L < 1e-6:
                continue
            nz, ny = (yb - ya) / L, -(zb - za) / L
            if area < 0:
                nz, ny = -nz, -ny
            q = [(x0, ya, za), (x1, ya, za), (x1, yb, zb), (x0, yb, zb)]
            if nz < -0.05 and ny > 0.05:
                # a slope turned north, behind the ridge at 45 degrees: the
                # front slope mirrored about the ridge
                zr = max(sec, key=lambda p: p[1])[0]
                q = [p + (p[0], 2 * zr - p[2] - p[1]) for p in q]
                PlanPrism._tri(mesh, (q[0], q[1], q[2]), (0.0, ny, nz), SHADE_BACK, self.name + ".back~behind")
                PlanPrism._tri(mesh, (q[0], q[2], q[3]), (0.0, ny, nz), SHADE_BACK, self.name + ".back~behind")
            elif ny + nz > 0.05:
                q = [p + (p[0], p[2] - p[1]) for p in q]
                PlanPrism._tri(mesh, (q[0], q[1], q[2]), (0.0, ny, nz), SHADE_ART, self.name + "~proj")
                PlanPrism._tri(mesh, (q[0], q[2], q[3]), (0.0, ny, nz), SHADE_ART, self.name + "~proj")
            elif ny > -0.5:
                # a face turned north: the strip along x, each height its row
                for piece in tile_pieces([(x0, ytop - ya), (x1, ytop - ya), (x1, ytop - yb), (x0, ytop - yb)],
                                         tile, None, None):
                    zz = lambda t: za + (zb - za) * ((ytop - t) - ya) / (yb - ya) if abs(yb - ya) > 1e-6 else za
                    pts = [(s, ytop - t, zz(t), u, v) for s, t, u, v in piece]
                    for j in range(1, len(pts) - 1):
                        PlanPrism._tri(mesh, (pts[0], pts[j], pts[j + 1]), (0.0, ny, nz), SHADE_BACK,
                                       self.name + ".back~behind")
        # the two ends: the section, the strip across its depth
        for x, nx in ((x0, -1.0), (x1, 1.0)):
            for k in range(1, n - 1):
                tri = [sec[0], sec[k], sec[k + 1]]
                for piece in tile_pieces([(zf - z, ytop - y) for (z, y) in tri], tile, None, None):
                    pts = [(x, ytop - t, zf - s, u, v) for s, t, u, v in piece]
                    for j in range(1, len(pts) - 1):
                        PlanPrism._tri(mesh, (pts[0], pts[j], pts[j + 1]), (nx, 0.0, 0.0),
                                       SHADE_EAST if nx > 0 else SHADE_WEST, self.name + ".side~behind")


class Grown:
    """A flat-faced part whose hidden faces (sides, backs, a roof the
    drawing does not show - its '~behind' faces) read the texture `offset`
    rows down: the drawing's grown band (Mound.with_ring), with no clear
    texel to make a hole."""

    def __init__(self, part, offset):
        self.part, self.offset = part, float(offset)
        self.name = getattr(part, "name", "grown")

    def emit(self, mesh):
        tmp = Mesh()
        self.part.emit(tmp)
        for (tri, shade, tag) in tmp.tris:
            if "~behind" in tag:
                tri = tuple((x, y, z, u, v + self.offset) for (x, y, z, u, v) in tri)
            mesh.tri(tri[0], tri[1], tri[2], shade, tag)


class PlanHip:
    """A hipped roof of four flat faces over the eave rectangle x0-x1,
    z0-z1 at height ye, its ridge from rx0 to rx1 at (rz, ry). A face the
    45-degree view sees takes the drawing projected onto it - exact; the
    others the drawing mirrored about the ridge (the roof's own colours)."""

    def __init__(self, name, x0, x1, z0, z1, ye, rx0, rx1, rz, ry):
        self.name = name
        self.box = (float(x0), float(x1), float(z0), float(z1), float(ye))
        self.ridge = (float(rx0), float(rx1), float(rz), float(ry))

    def emit(self, mesh):
        x0, x1, z0, z1, ye = self.box
        rx0, rx1, rz, ry = self.ridge
        a, b, c, d = (x0, ye, z1), (x1, ye, z1), (x1, ye, z0), (x0, ye, z0)
        r0, r1 = (rx0, ry, rz), (rx1, ry, rz)
        faces = [(a, b, r1, r0), (c, d, r0, r1), (d, a, r0), (b, c, r1)]
        cx, cz = (x0 + x1) / 2, (z0 + z1) / 2
        for f in faces:
            e1 = [f[1][k] - f[0][k] for k in range(3)]
            e2 = [f[-1][k] - f[0][k] for k in range(3)]
            nrm = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]]
            mx = sum(p[0] for p in f) / len(f) - cx
            mz = sum(p[2] for p in f) / len(f) - cz
            if nrm[1] < 0 or (nrm[1] == 0 and nrm[0] * mx + nrm[2] * mz < 0):
                nrm = [-k for k in nrm]
            if nrm[1] + nrm[2] > 1e-6:
                q = [p + (p[0], p[2] - p[1]) for p in f]
                tag, shade = self.name + "~proj", SHADE_ART
            else:
                q = [p + (p[0], 2 * rz - p[2] - p[1]) for p in f]
                tag, shade = self.name + ".back~behind", SHADE_BACK
            for k in range(1, len(q) - 1):
                PlanPrism._tri(mesh, (q[0], q[k], q[k + 1]), nrm, shade, tag)


class Card:
    """A thing of leaves - a potted plant - as a card at its foot, facing the
    GBA camera, its drawing projected: seen from the console's camera it
    stands like the characters do. (A second card across it showed from
    above as a flame of leaf tips.)

    `art` is the piece's own image; `foot` the row it stands on; the columns
    it spans are read off the art. `voff`: the rows below the model's drawing
    where the card's own pixels are, so that it shows nothing else. `base`:
    lifted that high onto what it stands on (pots on a stand), as far back
    as it is lifted - its foot drawn where it was.
    """

    def __init__(self, name, art, foot, voff=0, base=0, span=None):
        self.name, self.art, self.foot, self.voff = name, art, float(foot), voff
        self.base = float(base)
        # (u0, u1, v0): its extent, where the art's own pixels are not all of
        # it - a beam whose other animation frame reaches further
        self.span = span

    def emit(self, mesh):
        W, H = self.art.size
        px = self.art.load()
        cols = [u for u in range(W) if any(px[u, v][3] >= 128 for v in range(H))]
        rows = [v for v in range(H) if any(px[u, v][3] >= 128 for u in range(W))]
        if self.span is not None:
            cols, rows = [self.span[0], self.span[1] - 1], [self.span[2]]
        if not cols:
            return
        u0, u1 = float(min(cols)), float(max(cols) + 1)
        v0, f, b = float(min(rows)), self.foot, self.base
        h = f - v0
        front = [(u0, b - 1.0, f + b), (u1, b - 1.0, f + b), (u1, b + h, f + b), (u0, b + h, f + b)]
        mesh.poly([(x, y, z, x, z - y + self.voff) for (x, y, z) in front], SHADE_ART,
                  self.name + ".card")


class Ribbon:
    """A strip running back across the plan, from (za, ya) to (zb, yb), u0
    to u1 wide, its drawing projected (v = z - y): a beam of light from one
    post to the next, exact at 45 degrees and a strip in the air from any
    other angle. `voff`: the rows below the model's drawing where its own
    pixels are."""

    def __init__(self, name, u0, u1, za, ya, zb, yb, voff=0):
        self.name, self.voff = name, voff
        self.u0, self.u1 = float(u0), float(u1)
        self.a, self.b = (float(za), float(ya)), (float(zb), float(yb))

    def emit(self, mesh):
        (za, ya), (zb, yb) = self.a, self.b
        # wound as a card is: its lower edge on the screen first
        lo, hi = ((zb, yb), (za, ya)) if zb - yb > za - ya else ((za, ya), (zb, yb))
        quad = [(self.u0, lo[1], lo[0]), (self.u1, lo[1], lo[0]),
                (self.u1, hi[1], hi[0]), (self.u0, hi[1], hi[0])]
        mesh.poly([(x, y, z, x, z - y + self.voff) for (x, y, z) in quad], SHADE_ART,
                  self.name + ".ribbon~proj")


class Facet:
    """A vertical wall along a line of the plan, its drawing projected: a
    room's chamfered corner, drawn as a band between two parallel diagonals.

    `a` and `b` are its foot's ends (x, z) and `ha`, `hb` its heights there.
    """

    def __init__(self, name, a, b, ha, hb):
        self.name, self.a, self.b, self.ha, self.hb = name, a, b, float(ha), float(hb)

    def emit(self, mesh):
        (xa, za), (xb, zb) = self.a, self.b
        quad = [(xa, -1.0, za), (xb, -1.0, zb), (xb, self.hb, zb), (xa, self.ha, za)]
        mesh.poly([(x, y, z, x, z - y) for (x, y, z) in quad], SHADE_ART,
                  self.name + ".facet~proj")


class PlainWall:
    """A wall the drawing never shows - a room's side wall, edge on to the GBA
    camera - dressed with `tile` at one texel per pixel, laid from `a` to `b`
    (x, z) with its top row at `y1`. It faces left of the direction a->b."""

    def __init__(self, name, a, b, y0, y1, tile):
        self.name, self.a, self.b = name, a, b
        self.y0, self.y1, self.tile = float(y0), float(y1), tile

    def emit(self, mesh):
        (xa, za), (xb, zb) = self.a, self.b
        length = math.hypot(xb - xa, zb - za)
        dx, dz = (xb - xa) / length, (zb - za) / length
        face = [(0.0, 0.0), (length, 0.0), (length, self.y1 - self.y0), (0.0, self.y1 - self.y0)]
        for piece in tile_pieces(face, self.tile, None, None):
            pts = []
            for s, t, u, v in piece:
                pts.append((xa + dx * s, self.y1 - t, za + dz * s, u, v))
            mesh.poly(pts, SHADE_WEST, self.name + ".plain")


class Decal:
    """What a room paints on its floor in the cells a model covers: a flat
    quad per cell, drawn from the floor-only copy of the drawing that the
    model's art carries `v_offset` rows below its own. With `boxes` ({cell:
    (u0, v0, u1, v1)}, pixels of the cell) a cell's quad covers only that
    box - what it paints: the export crops a texture to what its faces
    read, and a cell's quad over a few marks would keep a cell of clear
    texels on the page."""

    def __init__(self, name, cells, v_offset, lift=0.5, boxes=None):
        self.name, self.cells, self.v_offset, self.lift = name, cells, v_offset, lift
        self.boxes = boxes or {}

    def emit(self, mesh):
        y, o = self.lift, self.v_offset
        for (i, j) in self.cells:
            u0, v0, u1, v1 = self.boxes.get((i, j), (0, 0, 16, 16))
            x0, z0, x1, z1 = i * 16.0 + u0, j * 16.0 + v0, i * 16.0 + u1, j * 16.0 + v1
            quad = [(x0, y, z0), (x1, y, z0), (x1, y, z1), (x0, y, z1)]
            # Half a pixel above the floor drawn under it - any less and the
            # console's depth buffer cannot tell them apart up close - and
            # lifted along the GBA's line of sight (as the terrain's relief
            # lifts a cell), so at 45 degrees it covers its own rows exactly.
            mesh.poly([(x, yy, z + y, x, z + o) for (x, yy, z) in quad], SHADE_ART,
                      self.name + ".decal")


class Cylinder:
    """A short upright cylinder of elliptical footprint - a vent, a pot.

    A curved thing in the art has no eave to lay courses from, so its top and
    the side facing the camera take the GBA projection itself (u = X,
    v = Z - Y), which reproduces its drawing exactly at 45 degrees; the far
    side and the flanks take a 1:1 tile of the drawn near side. `density_check` exempts the
    projected faces (tag "~proj"): off-axis, a curved face is not 1:1 anywhere.
    """

    def __init__(self, name, cx, cz, rx, rz, y0, y1, back_tile, sides=16, proj=True):
        self.name, self.cx, self.cz, self.rx, self.rz = name, cx, cz, rx, rz
        self.y0, self.y1, self.back_tile, self.sides = y0, y1, back_tile, sides
        # `proj` False: every side takes the tile, the near one too (a stem
        # the drawing hides under what it carries)
        self.proj = proj

    def emit(self, mesh):
        ring = [(self.cx + self.rx * math.cos(2 * math.pi * k / self.sides),
                 self.cz + self.rz * math.sin(2 * math.pi * k / self.sides))
                for k in range(self.sides)]
        y0, y1 = self.y0, self.y1
        top = [(x, y1, z, x, z - y1) for (x, z) in ring]
        mesh.poly(top, SHADE_ART, self.name + ".top~proj")
        u0, v0, u1, v1 = self.back_tile.rect
        for k in range(self.sides):
            (xa, za), (xb, zb) = ring[k], ring[(k + 1) % self.sides]
            # outward normal of the segment on the ellipse
            nx, nz = (zb - za), -(xb - xa)
            if nx * ((xa + xb) / 2 - self.cx) + nz * ((za + zb) / 2 - self.cz) < 0:
                nx, nz = -nx, -nz
            nz /= math.hypot(nx, nz)
            quad = [(xa, y0, za), (xb, y0, zb), (xb, y1, zb), (xa, y1, za)]
            # Only a face turned to the camera is drawn in the art; one seen
            # edge on would sample the pixels just outside the drawn outline.
            if nz > 0.2 and self.proj:
                mesh.poly([p + (p[0], p[2] - p[1]) for p in quad],
                          SHADE_ART, self.name + ".side~proj")
            else:
                w = math.hypot(xb - xa, zb - za)
                ua = u0 + (k * 3) % max(1, int(u1 - u0 - w))
                vs = [v1, v1, v1 - (y1 - y0), v1 - (y1 - y0)]
                us = [ua, ua + w, ua + w, ua]
                # the lighting turns a face to its shade's side (nx points
                # out): a flank turned east is lit as one, not as a west face
                mesh.poly([p + (u, v) for p, u, v in zip(quad, us, vs)],
                          SHADE_BACK if nz < 0 else SHADE_EAST if nx > 0 else SHADE_WEST,
                          self.name + ".side")


# ── Model ─────────────────────────────────────────────────────────────────

class Model:
    def __init__(self, name, art, prisms, cells, ground_metatile):
        self.name, self.art, self.prisms = name, art, prisms
        self.cells = cells  # (w, h) in metatiles
        self.ground_metatile = ground_metatile
        self.mesh = Mesh()
        for part in prisms:
            if isinstance(part, Prism):
                emit_prism(self.mesh, part)
            else:
                part.emit(self.mesh)

    def triangle_count(self):
        return len(self.mesh.tris)


# ── Software rasterizer (preview and proof) ────────────────────────────────

class Raster:
    def __init__(self, w, h, bg=(40, 44, 60)):
        self.w, self.h = w, h
        self.color = [bg] * (w * h)
        self.depth = [-1e30] * (w * h)
        self.owner = [None] * (w * h)

    def draw(self, verts, tex, shade, owner=None):
        """verts: three (sx, sy, depth, invw, u, v); larger depth is nearer.

        u, v are perspective-divided by invw already (u*invw, v*invw), so the
        interpolation is perspective correct.
        """
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

    def image(self):
        img = Image.new("RGB", (self.w, self.h))
        img.putdata(self.color)
        return img


def ortho_check(model, out_png=None, exact=None, margin=32, reference=None):
    """Render the model in the GBA projection and compare it with the art.

    `exact` lists art rectangles (x0, y0, x1, y1[, behind]) the model must
    reproduce pixel for pixel (with `behind` true, geometry may show where the
    art has ground there: the real depth rising behind a drawn outline) - the facade and whatever else faces the camera at its
    drawn depth. Outside them the model may differ from the drawing, because
    it is deeper than the drawing. None means the whole art.

    Returns (wrong, missing, extra): pixels drawn with the wrong colour, art
    pixels nothing drew, and pixels drawn where the art has ground.

    With `reference`, the render (textured with the model's own art) is
    judged against that drawing instead: a model whose texture is laid out
    otherwise than the drawing (a rock's foam on a band of its own).
    """
    art = model.art
    W, H = art.size
    M = margin
    ras = Raster(W + 2 * M, H + M, bg=(0, 0, 0))
    for (tri, shade, tag) in model.mesh.tris:
        if "~depth" in tag or "~behind" in tag:
            continue  # real depth behind the drawing: not the drawing's to judge
        vs = []
        for (x, y, z, u, v) in tri:
            vs.append((x + M, z - y + M, y + z, 1.0, u, v))
        ras.draw(vs, art, shade, tag)
    regions = exact if exact is not None else [(0, 0, W, H)]
    apx = (reference if reference is not None else art).load()
    wrong = missing = extra = 0
    img = ras.image()
    diff = img.copy()
    dpx = diff.load()
    for y in range(H + M):
        for x in range(W + 2 * M):
            c = dpx[x, y]
            dpx[x, y] = (c[0] // 3, c[1] // 3, c[2] // 3)
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
                        dpx[x + M, y + M] = (255, 0, 255)
                    elif col != (r, g, b):
                        wrong += 1
                        dpx[x + M, y + M] = (255, 0, 0)
                    else:
                        dpx[x + M, y + M] = (r, g, b)
                elif got is not None and not behind:
                    extra += 1
                    dpx[x + M, y + M] = (0, 255, 255)
    if out_png:
        sheet = Image.new("RGB", ((W + 2 * M) * 3 + 8, H + M), (0, 0, 0))
        bgart = Image.new("RGB", (W, H), (0, 0, 0))
        bgart.paste(art, (0, 0), art)
        sheet.paste(bgart, (M, M))
        sheet.paste(img, (W + 2 * M + 4, 0))
        sheet.paste(diff, (2 * (W + 2 * M) + 8, 0))
        sheet = sheet.resize((sheet.width * 3, sheet.height * 3), Image.NEAREST)
        sheet.save(out_png)
    return wrong, missing, extra


# --- true voxels -------------------------------------------------------------

VX_SPREAD = 0.5
VX_TOP, VX_SOUTH, VX_NORTH, VX_WEST, VX_EAST, VX_BOTTOM = range(6)
VX_NORMALS = {VX_TOP: (0, 1, 0), VX_SOUTH: (0, 0, 1), VX_NORTH: (0, 0, -1), VX_WEST: (-1, 0, 0),
           VX_EAST: (1, 0, 0), VX_BOTTOM: (0, -1, 0)}


class VoxelLathe:
    """A drawn round thing (a stone lantern) as true 1-pixel cubes.

    The volume is a lathe (`profile` of (y, radius), its plan an ellipse
    `squash` times as deep as wide, `power` by height for a squarer plan),
    voxelized pixel by pixel, then carved until the GBA's 45-degree view of
    its cubes is the drawing exactly: a cube showing an undrawn pixel goes,
    a drawn pixel nothing shows gets a cube resting on the solid. Faces the
    view sees take their pixel's colour; the others are painted from the
    drawing, never stretched - the back the front opposite it cube for
    cube, the sides the ring of their height turned round the axis, a
    step's side its own face's colour. A speck drawn over a clear pixel
    (no cube shows one without the other) is a one-pixel card. Faces of
    one colour merge into rectangles; the texture is the drawing with a
    band of colour swatches under it (`texture()`, before emitting).
    """

    def __init__(self, name, art, drawn, cx, cz, profile, squash, ymax=64, power=None):
        """art: RGBA drawing; drawn(u, v) -> bool: the pixel is this thing's."""
        self.name, self.art, self.drawn = name, art, drawn
        self.cx, self.cz, self.profile, self.squash = cx, cz, profile, squash
        self.W, self.H = art.size
        self.ymax = ymax
        # the plan's roundness by height: 2 an ellipse, more a squarer one
        # (a cap drawn with a straight back)
        self.power = power or (lambda y: 2.0)
        self.build()

    # --- shape ------------------------------------------------------------
    def radius(self, y):
        p = self.profile
        if y < p[0][0] or y > p[-1][0]:
            return 0.0
        for (ya, ra), (yb, rb) in zip(p, p[1:]):
            if ya <= y <= yb:
                return ra if yb == ya else ra + (rb - ra) * (y - ya) / (yb - ya)
        return 0.0

    def inside(self, x, y, z):
        r = self.radius(y)
        p = self.power(y)
        return r > 0 and abs((x - self.cx) / r) ** p + abs((z - self.cz) / (self.squash * r)) ** p <= 1.0

    def faces_of(self, occ):
        """pixel -> (key, (i, j, k), face) the 45-degree view sees there."""
        seen = {}
        for (i, j, k) in occ:
            if (i, j + 1, k) not in occ:
                p, key = (i, k - j - 1), j + 1.0
                if p not in seen or seen[p][0] < key:
                    seen[p] = (key, (i, j, k), VX_TOP)
            if (i, j, k + 1) not in occ:
                p, key = (i, k - j), j + 0.5
                if p not in seen or seen[p][0] < key:
                    seen[p] = (key, (i, j, k), VX_SOUTH)
        return seen

    def build(self):
        D = {(u, v) for v in range(self.H) for u in range(self.W) if self.drawn(u, v)}
        zmax = self.H + self.ymax
        occ = set()
        for j in range(self.ymax):
            for k in range(zmax):
                for i in range(self.W):
                    if self.inside(i + 0.5, j + 0.5, k + 0.5):
                        occ.add((i, j, k))
        # carve: a cube showing an undrawn pixel goes; new ones show behind
        while True:
            seen = self.faces_of(occ)
            bad = {c for p, (key, c, f) in seen.items() if p not in D}
            if not bad:
                break
            occ -= bad
        # fill: a drawn pixel nothing shows gets a cube, as high as it can
        # stand touching the solid, showing nothing undrawn
        for _ in range(8):
            seen = self.faces_of(occ)
            missing = sorted(D - set(seen), key=lambda p: (p[1], p[0]))
            if not missing:
                break
            for (u, v) in missing:
                if (u, v) in self.faces_of(occ):
                    continue
                # every cube that would show it, touching the solid and
                # showing nothing undrawn; the one resting on the solid
                # first, else the lowest (never a bar hung in the air)
                best = None
                for j in range(self.ymax):
                    for c in ((u, j, v + j), (u, j, v + j + 1)):
                        if c in occ or c[2] < 0:
                            continue
                        i, jj, k = c
                        nb = ((i - 1, jj, k), (i + 1, jj, k), (i, jj - 1, k), (i, jj + 1, k),
                              (i, jj, k - 1), (i, jj, k + 1))
                        if not any(n in occ for n in nb):
                            continue
                        trial = occ | {c}
                        s2 = self.faces_of(trial)
                        if (u, v) in s2 and all(p in D for p in s2):
                            score = (0 if (i, jj - 1, k) in occ or jj == 0 else 1, jj)
                            if best is None or score < best[0]:
                                best = (score, c)
                if best:
                    occ = occ | {best[1]}
        self.occ = occ
        seen = self.faces_of(occ)
        self.missing_px = sorted(D - set(seen))
        # a speck drawn over a clear pixel (no cube can show it without
        # showing that one too): a one-pixel card facing the view, at the
        # depth of the surface beside it
        self.cards = []
        for (u, v) in self.missing_px:
            # standing on the solid's top where the ray of that pixel grazes
            # it: a card at z covers row v from y = z - v - 1 up
            top = {}
            for (i, j, k) in occ:
                if i == u:
                    top[k] = max(top.get(k, 0), j + 1)
            best = None
            for k, t in top.items():
                for z in (k, k + 1):
                    gap = abs((z - v - 1) - t)
                    if best is None or gap < best[0]:
                        best = (gap, z)
            z = best[1] if best else v + 1
            self.cards.append((u, v, z))
        self.missing = 0
        self.extra = len([p for p in seen if p not in D])
        self.visible = {(c, f): p for p, (key, c, f) in seen.items()}

    # --- colour -----------------------------------------------------------
    def px(self, u, v):
        u = min(self.W - 1, max(0, int(u)))
        v = min(self.H - 1, max(0, int(v)))
        return self.art.getpixel((u, v))

    def nearest_drawn(self, u, v):
        """The drawn pixel nearest (u, v) along its row, then its column."""
        u, v = int(round(u)), int(round(v))
        for d in range(0, 40):
            for (a, b) in ((u - d, v), (u + d, v), (u, v - d), (u, v + d)):
                if 0 <= a < self.W and 0 <= b < self.H and self.drawn(a, b):
                    return self.art.getpixel((a, b))
        return (128, 128, 128, 255)

    def colour(self, c, f):
        if (c, f) in self.visible:
            return self.px(*self.visible[(c, f)])
        if f in (VX_WEST, VX_EAST) and (c, VX_SOUTH) in self.visible:
            # a step of the round front: its side as its face, so the stair
            # reads as the one curved surface the drawing shades
            return self.px(*self.visible[(c, VX_SOUTH)])
        i, j, k = c
        x, y, z = i + 0.5, j + 0.5, k + 0.5
        if f in (VX_TOP, VX_NORTH, VX_SOUTH):
            # the back carries the front opposite it, cube for cube
            zm = 2 * self.cz - z
            km = int(math.floor(zm))
            if f == VX_TOP:
                v = km - j - 1
            else:
                v = km - j
            if self.drawn(i, v):
                return self.px(i, v)
        # turned round the axis: the angle on the ellipse, folded onto the
        # drawn front half, laid evenly over the front's columns (inside its
        # outline)
        th = math.atan2(x - self.cx, (z - self.cz) / self.squash)
        if abs(th) > math.pi / 2:
            th = math.copysign(math.pi - abs(th), th)
        r = self.radius(y) or 1.0
        v = self.cz + self.squash * r - y
        row = [u for u in range(self.W) if self.drawn(u, int(v))]
        if row:
            # the middle of the front: its edges are the drawing's shading
            # and outline, not the stone's colour
            lo, hi = row[0] + 1.5, row[-1] - 0.5
            u = (lo + hi) / 2 + (hi - lo) / 2 * VX_SPREAD * th / (math.pi / 2)
            return self.nearest_drawn(u, v)
        return self.nearest_drawn(x, v)

    # --- mesh -------------------------------------------------------------
    def quads(self):
        """(face, slice, a, b, w, h, colours) - the faces of each slice merged
        into rectangles whatever their colours (a texel each, laid in the
        texture by texture()). Faces turned down are left out: the camera is
        always above."""
        occ = self.occ
        out = []
        dirs = {VX_TOP: (0, 1, 0), VX_SOUTH: (0, 0, 1), VX_NORTH: (0, 0, -1),
                VX_EAST: (1, 0, 0), VX_WEST: (-1, 0, 0)}
        for f, (dx, dy, dz) in dirs.items():
            cells = {}
            for (i, j, k) in occ:
                if (i + dx, j + dy, k + dz) in occ:
                    continue
                col = self.colour((i, j, k), f)[:3]
                if f == VX_TOP:
                    key, a, b = j, i, k
                elif f in (VX_SOUTH, VX_NORTH):
                    key, a, b = k, i, j
                else:
                    key, a, b = i, k, j
                cells[(key, a, b)] = col
            used = set()
            for (key, a, b) in sorted(cells, key=lambda t: (t[0], t[2], t[1])):
                if (key, a, b) in used:
                    continue
                w = 1
                while (key, a + w, b) in cells and (key, a + w, b) not in used:
                    w += 1
                h = 1
                while all((key, a + t, b + h) in cells and (key, a + t, b + h) not in used
                          for t in range(w)):
                    h += 1
                cols = [[cells[(key, a + t, b + s_)] for t in range(w)] for s_ in range(h)]
                for t in range(w):
                    for s_ in range(h):
                        used.add((key, a + t, b + s_))
                out.append((f, key, a, b, w, h, cols))
        return out

    def texture(self):
        """The drawing, and under it the faces' texels: each rectangle's
        block, one texel a cube face (shelf-packed in the drawing's width)."""
        qs = self.quads()
        blocks = [(q[4], q[5], q) for q in qs] + [(1, 1, ("card", c)) for c in self.cards]
        order = sorted(range(len(blocks)), key=lambda i: -blocks[i][1])
        place, x, y, shelf = {}, 0, 0, 0
        for i in order:
            w, h, _ = blocks[i]
            if x + w > self.W:
                x, y, shelf = 0, y + shelf, 0
            place[i] = (x, y)
            x += w
            shelf = max(shelf, h)
        band = y + shelf
        tex = Image.new("RGBA", (self.W, self.H + band), (0, 0, 0, 0))
        tex.paste(self.art, (0, 0))
        self._quads, self._cards = [], []
        for i, (w, h, q) in enumerate(blocks):
            bx, by = place[i]
            by += self.H
            if q[0] == "card":
                u, v, z = q[1]
                tex.putpixel((bx, by), self.px(u, v)[:3] + (255,))
                self._cards.append((u, v, z, bx, by))
                continue
            for s_ in range(h):
                for t in range(w):
                    tex.putpixel((bx + t, by + s_), q[6][s_][t] + (255,))
            self._quads.append(q[:6] + ((bx, by),))
        return tex

    def emit(self, mesh):
        shades = {VX_TOP: SHADE_ART, VX_SOUTH: SHADE_ART, VX_NORTH: SHADE_BACK,
                  VX_WEST: SHADE_WEST, VX_EAST: SHADE_EAST}
        for (f, key, a, b, w, h, (bx, by)) in self._quads:
            if f == VX_TOP:
                y = key + 1
                pts = [(a, y, b), (a + w, y, b), (a + w, y, b + h), (a, y, b + h)]
            elif f in (VX_SOUTH, VX_NORTH):
                z = key + (1 if f == VX_SOUTH else 0)
                pts = [(a, b, z), (a + w, b, z), (a + w, b + h, z), (a, b + h, z)]
            else:
                x = key + (1 if f == VX_EAST else 0)
                pts = [(x, b, a), (x, b, a + w), (x, b + h, a + w), (x, b + h, a)]
            uvs = [(bx, by), (bx + w, by), (bx + w, by + h), (bx, by + h)]
            q = [p + uv for p, uv in zip(pts, uvs)]
            self._quad(mesh, q, VX_NORMALS[f], shades[f])
        for (u, v, z, bx, by) in self._cards:
            y = z - v - 1
            uv = (bx + 0.5, by + 0.5)
            q = [p + uv for p in [(u, y, z), (u + 1, y, z), (u + 1, y + 1, z), (u, y + 1, z)]]
            self._quad(mesh, q, (0, 0, 1), SHADE_ART)
            self._quad(mesh, [q[1], q[0], q[3], q[2]], (0, 0, -1), SHADE_BACK)

    def _quad(self, mesh, q, nrm, shade):
        if True:
            for tri in ((q[0], q[1], q[2]), (q[0], q[2], q[3])):
                ar = Mound._area(*tri)
                if sum(ar[t] * nrm[t] for t in range(3)) < 0:
                    tri = (tri[0], tri[2], tri[1])
                mesh.tri(tri[0], tri[1], tri[2], shade, self.name + ".voxel~clamp")


class BakedLathe:
    """A turned thing as a game draws a prop: a low-poly lathe (`sides`
    around, a ring at each point of the profile) with its texture baked off
    the true-cube model (VoxelLathe) - every texel the colour of the cube
    face under it, the drawing's where the GBA's view sees it. The cubes'
    look at a few hundred triangles: no stair of geometry, the pixels in the
    texture at one texel a pixel.

    The plan is the cubes' (`squash`, `power` by height); u runs round it by
    angle (the back is the seam), v up the profile by its slant length.
    """

    # a face turned at least this much to the view reads the drawing itself
    FACING = 0.45

    def __init__(self, name, cubes, sides=24, shift_z=0.0):
        self.name, self.cubes, self.sides = name, cubes, sides
        # stood this far north of where it is drawn (its texture as drawn)
        self.shift_z = shift_z
        vl = cubes
        self.rings = []
        for (y, r) in vl.profile:
            self.rings.append((float(y), float(r)))
        # v: slant length up the profile
        self.vs = [0.0]
        for (y0, r0), (y1, r1) in zip(self.rings, self.rings[1:]):
            self.vs.append(self.vs[-1] + math.hypot(y1 - y0, (r1 - r0)))
        rmax = max(r for y, r in self.rings)
        # u: round the widest ring
        a, b = rmax, rmax * vl.squash
        self.ulen = int(math.ceil(math.pi * (3 * (a + b) - math.sqrt((3 * a + b) * (a + 3 * b)))))

    def point(self, ring, th):
        """The ring's point at angle th (0 the front, south), and the plan's
        outward normal there."""
        vl = self.cubes
        y, r = self.rings[ring]
        p = vl.power(y)
        c, s_ = math.cos(th), math.sin(th)
        e = 2.0 / p
        cx = math.copysign(abs(s_) ** e, s_)
        cz = math.copysign(abs(c) ** e, c)
        x = vl.cx + r * cx
        z = vl.cz + vl.squash * r * cz
        return (x, y, z)

    def theta(self, k):
        return -math.pi + 2.0 * math.pi * k / self.sides

    def seen(self, pt):
        """Whether the GBA's 45-degree view sees the surface at pt: nothing
        of the solid on the way up its ray, (0, t, t)."""
        vl = self.cubes
        x, y, z = pt
        t = 0.75
        while t < 80.0:
            if vl.inside(x, y + t, z + t):
                return False
            t += 0.5
        return True

    def colour_at(self, pt, nrm):
        """Where the view sees it, the drawing's pixel there (projected,
        (x, z - y)); elsewhere what the cubes paint: the back the front
        opposite it, the sides the ring turned round."""
        vl = self.cubes
        x, y, z = pt
        if nrm[1] > 0.7 and z < vl.cz:
            # a top's back half: the GBA draws its far edge as a thick outline,
            # which laid on a flat top reads as a stain - the front half
            # opposite it instead
            zm = 2 * vl.cz - z
            if self.seen((x, y, zm)):
                u, v = int(math.floor(x)), int(math.floor(zm - y))
                if vl.drawn(u, v):
                    return vl.px(u, v)[:3]
        if self.seen(pt):
            u, v = int(math.floor(x)), int(math.floor(z - y))
            if vl.drawn(u, v):
                return vl.px(u, v)[:3]
        zm = 2 * vl.cz - z
        if self.seen((x, y, zm)):
            u, v = int(math.floor(x)), int(math.floor(zm - y))
            if vl.drawn(u, v):
                return vl.px(u, v)[:3]
        c = (int(math.floor(x - nrm[0] * 0.5)), int(math.floor(y - nrm[1] * 0.5)),
             int(math.floor(z - nrm[2] * 0.5)))
        face = VX_EAST if nrm[0] > 0 else VX_WEST
        if abs(nrm[1]) > max(abs(nrm[0]), abs(nrm[2])):
            face = VX_TOP
        return vl.colour(c, face)[:3]

    def texture(self):
        vl = self.cubes
        W = max(vl.W, self.ulen)
        V = int(math.ceil(self.vs[-1])) + 1
        tex = Image.new("RGBA", (W, vl.H + V), (0, 0, 0, 0))
        tex.paste(vl.art, (0, 0))
        self.v0 = vl.H
        px = tex.load()
        n = len(self.rings)
        last = None
        for iv in range(V):
            sv = iv + 0.5
            j = 0
            while j < n - 2 and self.vs[j + 1] < sv:
                j += 1
            g = min(1.0, max(0.0, (sv - self.vs[j]) / max(1e-6, self.vs[j + 1] - self.vs[j])))
            for iu in range(self.ulen):
                th = -math.pi + 2.0 * math.pi * (iu + 0.5) / self.ulen
                a, b = self.point(j, th), self.point(j + 1, th)
                pt = tuple(a[t] + (b[t] - a[t]) * g for t in range(3))
                # the surface's normal: across the ring and up the profile
                e = 1e-3
                t1 = self.point(j, th + e)
                t2 = self.point(j + 1, th + e)
                tang = tuple((t1[t] - a[t]) * (1 - g) + (t2[t] - b[t]) * g for t in range(3))
                up = tuple(b[t] - a[t] for t in range(3))
                nn = (tang[1] * up[2] - tang[2] * up[1], tang[2] * up[0] - tang[0] * up[2],
                      tang[0] * up[1] - tang[1] * up[0])
                L = math.sqrt(sum(c * c for c in nn)) or 1.0
                nn = tuple(c / L for c in nn)
                # outward: away from the axis
                out = (pt[0] - vl.cx, 0.0, pt[2] - vl.cz)
                if nn[0] * out[0] + nn[2] * out[2] < 0 or (abs(out[0]) + abs(out[2]) < 1e-6 and nn[1] < 0):
                    nn = tuple(-c for c in nn)
                col = self.colour_at(pt, nn)
                if col is None:
                    col = last or (200, 200, 200)
                last = col
                px[iu, self.v0 + iv] = col + (255,)
        return tex

    def emit(self, mesh):
        n = len(self.rings)
        for j in range(n - 1):
            for k in range(self.sides):
                t0, t1 = self.theta(k), self.theta(k + 1)
                u0 = self.ulen * k / float(self.sides)
                u1 = self.ulen * (k + 1) / float(self.sides)
                va, vb_ = self.v0 + self.vs[j], self.v0 + self.vs[j + 1]
                q = [self.point(j, t0) + (u0, va), self.point(j, t1) + (u1, va),
                     self.point(j + 1, t1) + (u1, vb_), self.point(j + 1, t0) + (u0, vb_)]
                for tri in ((q[0], q[1], q[2]), (q[0], q[2], q[3])):
                    ar = Mound._area(*tri)
                    if sum(c * c for c in ar) < 1e-9:
                        continue
                    mid = [sum(p[t] for p in tri) / 3.0 for t in range(3)]
                    out = (mid[0] - self.cubes.cx, 0.0, mid[2] - self.cubes.cz)
                    if ar[0] * out[0] + ar[2] * out[2] < 0 or (ar[1] < 0 and abs(ar[0]) + abs(ar[2]) < 1e-9):
                        tri = (tri[0], tri[2], tri[1])
                        ar = tuple(-c for c in ar)
                    # lit as the drawn faces are: facing the view, or the side
                    shade = SHADE_ART if ar[1] + ar[2] >= 0 else SHADE_BACK
                    L = math.sqrt(sum(c * c for c in ar))
                    facing = (ar[1] + ar[2]) / (L * math.sqrt(2.0))
                    top_back = ar[1] / L > 0.7 and sum(p[2] for p in tri) / 3.0 < self.cubes.cz
                    dz = self.shift_z
                    if facing > self.FACING and not top_back and all(self.seen(p[:3]) for p in tri):
                        # turned to the GBA's view: the drawing itself, projected
                        tri = tuple((p[0], p[1], p[2] + dz, p[0], p[2] - p[1]) for p in tri)
                        mesh.tri(tri[0], tri[1], tri[2], shade, self.name + ".front~proj")
                    else:
                        tri = tuple((p[0], p[1], p[2] + dz) + p[3:] for p in tri)
                        mesh.tri(tri[0], tri[1], tri[2], shade, self.name + ".baked~clamp")


def density_check(model, tolerance=1e-3):
    """Every art texel must cover one pixel seen across its face at 45 degrees.

    Along the horizontal direction of a face the art must run one texel per
    pixel; along its fall line cos(b) + sin(b) texels per pixel, b its pitch;
    and the two must not shear. That is the property that keeps a stretched
    or squashed texel from ever reaching the screen, and it holds for every
    face - the ones the drawing shows and the ones rebuilt from it. Faces
    that hold one row (hidden under an eave) are exempt.

    Returns a list of (tag, along, down, shear) for faces that break it.
    """
    bad = []
    for (tri, shade, tag) in model.mesh.tris:
        if tag.endswith("~clamp") or tag.endswith("~proj"):
            continue
        p = [tri[i][:3] for i in range(3)]
        q = [tri[i][3:] for i in range(3)]
        e1 = [p[1][k] - p[0][k] for k in range(3)]
        e2 = [p[2][k] - p[0][k] for k in range(3)]
        nrm = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
               e1[0] * e2[1] - e1[1] * e2[0])
        nl = math.sqrt(sum(c * c for c in nrm))
        if nl < 1e-9:
            continue
        nrm = tuple(c / nl for c in nrm)
        # horizontal in-plane direction h, fall line f
        if abs(nrm[1]) > 1 - 1e-9:
            h, f = (1.0, 0.0, 0.0), (0.0, 0.0, 1.0)
        else:
            h = _unit((nrm[2], 0.0, -nrm[0]))
            f = (h[1] * nrm[2] - h[2] * nrm[1], h[2] * nrm[0] - h[0] * nrm[2],
                 h[0] * nrm[1] - h[1] * nrm[0])
        beta = math.asin(min(1.0, math.sqrt(nrm[0] ** 2 + nrm[2] ** 2)))
        expect = math.cos(beta) + math.sin(beta)
        # solve uv = A [a b]^T for the triangle's local coordinates
        a1, b1 = sum(e1[k] * h[k] for k in range(3)), sum(e1[k] * f[k] for k in range(3))
        a2, b2 = sum(e2[k] * h[k] for k in range(3)), sum(e2[k] * f[k] for k in range(3))
        det = a1 * b2 - a2 * b1
        if abs(det) < 1e-9:
            continue
        du1, dv1 = q[1][0] - q[0][0], q[1][1] - q[0][1]
        du2, dv2 = q[2][0] - q[0][0], q[2][1] - q[0][1]
        dh = ((du1 * b2 - du2 * b1) / det, (dv1 * b2 - dv2 * b1) / det)
        df = ((du2 * a1 - du1 * a2) / det, (dv2 * a1 - dv1 * a2) / det)
        along = math.hypot(*dh)
        down = math.hypot(*df)
        shear = abs(dh[0] * df[0] + dh[1] * df[1])
        if (abs(along - 1.0) > tolerance or abs(down - expect) > tolerance
                or shear > tolerance):
            bad.append((tag, along, down / expect, shear))
    return bad


class Camera:
    """The voxel renderer's follow camera, in world units (16 px)."""

    def __init__(self, target, pitch=40.0, yaw=0.0, distance=12.0, fov=35.0,
                 width=400, height=240):
        self.target = target
        self.pitch, self.yaw, self.distance, self.fov = pitch, yaw, distance, fov
        self.width, self.height = width, height
        p, yw = math.radians(pitch), math.radians(yaw)
        tx, ty, tz = target
        self.eye = (tx + math.sin(yw) * distance, ty + math.tan(p) * distance,
                    tz + math.cos(yw) * distance)
        f = [target[i] - self.eye[i] for i in range(3)]
        fl = math.sqrt(sum(c * c for c in f))
        self.f = [c / fl for c in f]
        up = (0.0, 1.0, 0.0)
        s = [self.f[1] * up[2] - self.f[2] * up[1],
             self.f[2] * up[0] - self.f[0] * up[2],
             self.f[0] * up[1] - self.f[1] * up[0]]
        sl = math.sqrt(sum(c * c for c in s))
        self.s = [c / sl for c in s]
        self.u = [self.s[1] * self.f[2] - self.s[2] * self.f[1],
                  self.s[2] * self.f[0] - self.s[0] * self.f[2],
                  self.s[0] * self.f[1] - self.s[1] * self.f[0]]
        self.t = 1.0 / math.tan(math.radians(fov) / 2)
        self.aspect = width / height

    def project(self, p):
        d = [p[i] - self.eye[i] for i in range(3)]
        x = sum(d[i] * self.s[i] for i in range(3))
        y = sum(d[i] * self.u[i] for i in range(3))
        z = sum(d[i] * self.f[i] for i in range(3))
        if z < 0.05:
            return None
        sx = (x * self.t / self.aspect / z * 0.5 + 0.5) * self.width
        sy = (0.5 - y * self.t / z * 0.5) * self.height
        return sx, sy, 1.0 / z, 1.0 / z


def render_scene(cam, items, scale=2):
    """items: list of (triangles, texture) where triangles are in world units
    as ((x,y,z,u,v)*3, shade)."""
    ras = Raster(cam.width * scale, cam.height * scale)
    cam2 = Camera(cam.target, cam.pitch, cam.yaw, cam.distance, cam.fov,
                  cam.width * scale, cam.height * scale)
    for tris, tex in items:
        for (tri, shade) in tris:
            vs = []
            ok = True
            for (x, y, z, u, v) in tri:
                pr = cam2.project((x, y, z))
                if pr is None:
                    ok = False
                    break
                sx, sy, d, iw = pr
                vs.append((sx, sy, d, iw, u * iw, v * iw))
            if ok:
                ras.draw(vs, tex, shade)
    return ras.image()


def model_world_tris(model, cell_x, cell_y):
    out = []
    for (tri, shade, tag) in model.mesh.tris:
        out.append((tuple((cell_x + x / 16.0, y / 16.0, cell_y + z / 16.0, u, v)
                          for (x, y, z, u, v) in tri), shade))
    return out


def ground_tris(layout, x0, y0, x1, y1, override, atlas_cache):
    """Flat ground for context; cells in `override` draw that metatile.

    Returns (tris, texture) using a small atlas of the metatiles involved.
    """
    ids = {}
    for y in range(y0, y1):
        for x in range(x0, x1):
            if 0 <= x < layout.w and 0 <= y < layout.h:
                m = override.get((x, y), layout.metatile(x, y))
                ids.setdefault(m, len(ids))
    cols = 16
    tex = Image.new("RGBA", (cols * 16, ((len(ids) + cols - 1) // cols) * 16), (0, 0, 0, 255))
    for m, i in ids.items():
        tex.paste(layout.cell_image(m), ((i % cols) * 16, (i // cols) * 16))
    tris = []
    for y in range(y0, y1):
        for x in range(x0, x1):
            if not (0 <= x < layout.w and 0 <= y < layout.h):
                continue
            m = override.get((x, y), layout.metatile(x, y))
            i = ids[m]
            u0, v0 = (i % cols) * 16, (i // cols) * 16
            a = (x, 0, y, u0, v0)
            b = (x + 1, 0, y, u0 + 16, v0)
            c = (x + 1, 0, y + 1, u0 + 16, v0 + 16)
            d = (x, 0, y + 1, u0, v0 + 16)
            tris.append(((a, b, c), 1.0))
            tris.append(((a, c, d), 1.0))
    return tris, tex
