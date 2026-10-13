#!/usr/bin/env python3
"""Build the voxel building models, prove them, and preview them.

    python gen_voxel_buildings.py [--verify] [--output BIN] [--preview DIR] [--only NAME]

For every spec in voxel_building_specs.py:
  * extracts the art with the ground made transparent;
  * builds the mesh;
  * with --verify (and when there is nothing to export), renders it in the
    GBA projection and compares every pixel with the art, and every modelled
    room with its drawing (the run fails unless they are reproduced exactly);
    the diagnostic images go to build/buildings or the --preview folder;
  * with --preview, renders the reference layout from several cameras.

The port's own build (full.mk) always verifies. The release builder only
exports: its output must match the CRC of the verified build, which is the
stronger check there, and the proofs would double its time.
"""

import argparse
import copy
from PIL import Image, ImageChops, ImageFilter
import json
import math
import os
import struct
import sys
import types

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import voxel_building as vb  # noqa: E402
import voxel_building_specs as specs  # noqa: E402
import voxel_cells as vc  # noqa: E402
import voxel_props  # noqa: E402
import voxel_relief_fixes  # noqa: E402

PORT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
# A placement whose cells keep their own metatile under the model: the
# console draws each cell's own drawing there, from the map's atlas, so its
# animation goes on (the water round a rock) - less the quarters of its upper
# layer the model stands for, a variant of the metatile the atlas composes
# in a slot of its own (`ground_variants`).
OWN_GROUND = 0xFFFF
MAX_VARIANTS = 128      # voxel_atlas.h VOXEL_VARIANTS


def component_specs(spec, layouts):
    """Expand a `components` spec - hedges, walls: objects with no fixed
    shape - into one ordinary spec per connected run of its metatiles, in
    every layout whose secondary tileset is the one named (or, for
    metatiles of a primary tileset, every layout on that primary)."""
    comp = spec["components"]
    tiles = comp["tiles"]
    layouts_json = json.load(open(os.path.join(vb.ROOT, "data", "layouts", "layouts.json"),
                                  encoding="utf-8"))["layouts"]
    out = []
    for entry in layouts_json:
        if "blockdata_filepath" not in entry or entry.get("id") in comp.get("skip", ()):
            continue
        if ("primary" in comp and entry.get("primary_tileset") != comp["primary"]) or                 ("secondary" in comp and entry.get("secondary_tileset") != comp["secondary"]):
            continue
        blocks = vb.read_u16(os.path.join(vb.ROOT, entry["blockdata_filepath"]))
        w, h = entry["width"], entry["height"]
        seen = set()
        for y in range(h):
            for x in range(w):
                if (x, y) in seen or (blocks[y * w + x] & 0x3FF) not in tiles:
                    continue
                todo, cells = [(x, y)], []
                seen.add((x, y))
                while todo:
                    cx, cy = todo.pop()
                    cells.append((cx, cy))
                    for nx, ny in ((cx + 1, cy), (cx - 1, cy), (cx, cy + 1), (cx, cy - 1)):
                        if (0 <= nx < w and 0 <= ny < h and (nx, ny) not in seen
                                and (blocks[ny * w + nx] & 0x3FF) in tiles):
                            seen.add((nx, ny))
                            todo.append((nx, ny))
                pieces = [cells]
                size = comp.get("block")
                if size:
                    # A thin object of long runs (a railing) is cut into
                    # blocks aligned on the map, so a stretch that repeats is
                    # one model placed many times.
                    by = {}
                    for (cx, cy) in cells:
                        by.setdefault((cx // size, cy // size), []).append((cx, cy))
                    pieces = list(by.values())
                for piece in pieces:
                    out.append(piece_spec(spec, comp, entry, blocks, w, h, piece, tiles, cells))
    # identical blocks: one model, placed at each
    if comp.get("block"):
        merged = {}
        for sp in out:
            key = (sp["layout"], sp["pattern"])
            if key in merged:
                merged[key]["repeat_at"].append(sp["rect"][:2])
            else:
                sp["repeat_at"] = [sp["rect"][:2]]
                merged[key] = sp
        out = list(merged.values())
    return out


def prop_specs(spec):
    """Expand a `props` spec - an object drawn as one block of subtiles,
    found by its tiles wherever it lands (voxel_props.py) - into one model
    per offset in the cells and per variant of what is drawn of it. Each
    stands at every copy, on the cells' own ground."""
    name = spec["props"]
    obj = voxel_props.OBJECTS[name]
    grid = obj["grid"]
    groups = {}
    for lid, found in sorted(voxel_props.everywhere([name]).items()):
        for (_, sx, sy, present) in found:
            key = (sx % 2, sy % 2, present)
            groups.setdefault(key, []).append((lid, sx, sy))
    out = []
    for (ox, oy, present), where in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        lid, sx, sy = where[0]
        layout = vb.LayoutArt(lid)
        cells = voxel_props.cells_of(name, sx, sy)
        cx0, cy0 = min(c[0] for c in cells), min(c[1] for c in cells)
        w = max(c[0] for c in cells) - cx0 + 1
        h = max(c[1] for c in cells) - cy0 + 1
        art = vb.Image.new("RGBA", (w * 16, h * 16), (0, 0, 0, 0))
        apx = art.load()
        # which quarter of which cell each drawn subtile is: what the atlas
        # leaves out under the model
        quads = {}
        for (i, j) in present:
            t = grid[j][i]
            tile, pal = (t[0], t[2]) if isinstance(t, tuple) else (t, obj["palette"])
            data = layout.ts.subtile(tile, pal)
            X, Y = ox * 8 + i * 8, oy * 8 + j * 8
            for k, (rgb, index) in enumerate(data):
                if index:
                    apx[X + k % 8, Y + k // 8] = rgb + (255,)
            ci, cj = X // 16, Y // 16
            quads[(ci, cj)] = quads.get((ci, cj), 0) | 1 << ((Y % 16) // 8 * 2 + (X % 16) // 8)
        drawing = art
        art = vb.Mound.with_ring(art, spec.get("ring", ()))
        # the commonest copy is the object's plain name, its variants numbered
        variant = spec["name"] if not out else "%s_%d" % (spec["name"], len(out))
        at = [(l, min(c[0] for c in voxel_props.cells_of(name, x, y)),
               min(c[1] for c in voxel_props.cells_of(name, x, y))) for (l, x, y) in where]
        shape = spec.get("shape", "mound")
        if shape == "card":
            # a thing of leaves or fruit: it stands at its foot, facing the
            # camera, like a potted plant
            parts = (lambda a, f: (lambda: [vb.Card("card", a, f)]))(
                art, max(y for (x, y) in [(u, v) for v in range(art.size[1]) for u in range(art.size[0])
                                           if art.getpixel((u, v))[3] >= 128]) + 1)
        elif shape == "cylinder":
            # a round basket: its drawn top ellipse and a short side
            c = spec["cylinder"]
            parts = (lambda c: (lambda: [vb.Cylinder("basket", c["cx"], c["cz"], c["rx"], c["rz"],
                                                     -1, c["h"], back_tile=vb.Tile(*c["side"]),
                                                     sides=24)]))(c)
        elif shape == "lathe":
            # a turned thing (a pot): smooth, its drawing projected, where
            # this variant's half-cell offset puts it
            c = spec["lathe"]
            parts = (lambda c, dx, dz: (lambda: [vb.ProjLathe(
                spec["name"], c["cx"] + dx, c["cz"] + dz, c["profile"],
                squash=c.get("squash", 1.0), seg=c.get("seg", 10))]))(c, ox * 8, oy * 8)
        elif shape == "dome":
            # a heap (fruit): a smooth dome, its drawing projected
            c = spec["dome"]
            parts = (lambda c, dx, dz: (lambda: [vb.ProjDome(
                spec["name"], c["cx"] + dx, c["cz"] + dz, c["rx"], c["rz"], c["h"],
                seg=c.get("seg", 12), rings=c.get("rings", 3))]))(c, ox * 8, oy * 8)
        else:
            parts = (lambda a, k, st, rg, r: (lambda: [vb.Mound("mound", a, rise=k, step=st,
                                                              ring=rg, rows=r)]))(
                art, spec["rise"], spec["step"], spec.get("ring", ()), h * 16)
        out.append({"name": variant, "layout": lid, "rect": (cx0, cy0, w, h),
                    "ground": spec["ground"], "art": art,
                    "parts": parts,
                    # a polygon round a drawn oval misses its outline by a pixel
                    "exact": spec.get("exact", [(0, 0, w * 16, h * 16)]), "at": at, "quads": quads,
                    "drawing": drawing, "static": spec.get("static", False),
                    "clip_art": spec.get("clip_art", False)})
    return out


def piece_spec(spec, comp, entry, blocks, w, h, cells, tiles, run=None):
    x0, y0 = min(c[0] for c in cells), min(c[1] for c in cells)
    x1, y1 = max(c[0] for c in cells) + 1, max(c[1] for c in cells) + 1
    owned = {(cx - x0, cy - y0) for cx, cy in cells}
    # A block cut from a longer run (see component_specs) goes on into the
    # block south or north of it: the cells whose neighbour there is the
    # run's and not the block's. The seam is no end: nothing stands up there.
    run = set(run or cells)
    south = frozenset((i, j) for (i, j) in owned
                      if (x0 + i, y0 + j + 1) in run and (i, j + 1) not in owned)
    north = frozenset((i, j) for (i, j) in owned
                      if (x0 + i, y0 + j - 1) in run and (i, j - 1) not in owned)
    # and east or west of it: the rows (in cells) of its edge columns that go on
    east = frozenset(j for (i, j) in owned
                     if (x0 + i + 1, y0 + j) in run and (i + 1, j) not in owned)
    west = frozenset(j for (i, j) in owned
                     if (x0 + i - 1, y0 + j) in run and (i - 1, j) not in owned)
    below = tuple(blocks[(y0 + j + 1) * w + x0 + i] & 0x3FF for (i, j) in sorted(south))
    above = tuple(blocks[(y0 + j - 1) * w + x0 + i] & 0x3FF for (i, j) in sorted(north))
    pattern = tuple((i, j, blocks[(y0 + j) * w + x0 + i] & 0x3FF)
                    for j in range(y1 - y0) for i in range(x1 - x0) if (i, j) in owned)
    pattern += (tuple(sorted(south)), below, tuple(sorted(north)), above,
                tuple(sorted(east)), tuple(sorted(west)))
    name = "%s_%s_%d_%d" % (spec["name"], entry["id"][len("LAYOUT_"):].lower(), x0, y0)
    return {"name": name, "layout": entry["id"], "rect": (x0, y0, x1 - x0, y1 - y0),
            "ground": spec["ground"], "owned": owned, "relief": comp, "pattern": pattern,
            "south": south, "north": north, "east": east, "west": west,
            "patch_own": spec.get("patch_own", False),
            **{k: spec[k] for k in ("ground_rgb", "ground_fixed") if k in spec}}


def clip_to_art(model):
    """Cut every triangle to its texture's rectangle (spec "clip_art"). A
    shape laid with its drawing projected (ProjDome, a card) may stand past
    the drawing; the previews read nothing there, but on the console the
    texel beside a model's art on its page is another model's (a grey
    flag over a mast, a speck over a bollard). A triangle's uv is affine
    in its position, so the cut is exact."""
    # to its opaque pixels' box, a hair inside: the pages keep no more of a
    # drawing than that, and a face on its very edge reads the next texel
    # (a prop's texture carries a grown band under its drawing: the box is
    # its drawing's)
    src = (getattr(model, "spec", None) or {}).get("drawing") or model.art
    alpha = src.getchannel("A").point(lambda a: 255 if a >= 128 else 0)
    box = alpha.getbbox()
    if box is None:
        return
    e = 1.0 / 64
    u0, v0, u1, v1 = box[0] + e, box[1] + e, box[2] - e, box[3] - e
    out = []
    for (tri, shade, tag) in model.mesh.tris:
        u = [p[3] for p in tri]
        v = [p[4] for p in tri]
        if min(u) >= u0 and min(v) >= v0 and max(u) <= u1 and max(v) <= v1:
            out.append((tri, shade, tag))
            continue
        poly = list(tri)
        for axis, value, keep in ((3, u0, True), (3, u1, False), (4, v0, True), (4, v1, False)):
            if len(poly) >= 3:
                poly = vb.clip(poly, axis, value, keep)
        for k in range(1, len(poly) - 1):
            out.append(((poly[0], poly[k], poly[k + 1]), shade, tag))
    model.mesh.tris = out


def clear_rgb(art, colours):
    """Make every pixel of these colours transparent."""
    apx = art.load()
    for v in range(art.size[1]):
        for u in range(art.size[0]):
            if apx[u, v][3] and apx[u, v][:3] in colours:
                apx[u, v] = (0, 0, 0, 0)


def seam_art(layout, spec, art, height, ground, ground_px):
    """A block's drawing with the run it goes on into: `height` rows of the
    cell south of each seam below it, so its top runs on to where the next
    block's own starts (at 45 degrees a top lies `height` rows south of the
    rows it shows). Returns the art, the pixel columns that carry on past
    those rows south (no front at the seam) and those that come in from the
    block north (no back; a run shorter than `height` there is the north
    block's to draw)."""
    x, y, w, h = spec["rect"]
    hull = spec["relief"].get("hull", 0)
    south, north = spec.get("south", ()), spec.get("north", ())
    open_s, open_n = set(), set()
    if not south and not north:
        return art, open_s, open_n
    px = art.load()

    def carries(img, u, rows):
        """The column's drawn run from row 0, gaps of `hull` closed, reaches
        `rows` rows."""
        p, end, gap = img.load(), 0, 0
        for v in range(img.height):
            if p[u, v][3] >= 128:
                end, gap = v + 1, 0
            else:
                gap += 1
                if gap > hull:
                    break
        return p[u, 0][3] >= 128 and end >= rows

    out = art
    if south:
        out = Image.new("RGBA", (w * 16, h * 16 + height), (0, 0, 0, 0))
        out.paste(art, (0, 0))
        opx = out.load()
        for (i, j) in south:
            cell = layout.building_art(x + i, y + j + 1, 1, 1, ground, ground_px=ground_px,
                                       upper=spec["relief"].get("upper", False))
            cpx = cell.load()
            for k in range(16):
                u = i * 16 + k
                if px[u, h * 16 - 1][3] < 128 or cpx[k, 0][3] < 128:
                    continue
                for v in range(height):
                    opx[u, h * 16 + v] = cpx[k, v]
                if carries(cell, k, height + 1):
                    open_s.add(u)
    for (i, j) in north:
        cell = layout.building_art(x + i, y + j - 1, 1, 1, ground, ground_px=ground_px,
                                   upper=spec["relief"].get("upper", False))
        cpx = cell.load()
        for k in range(16):
            u = i * 16 + k
            if px[u, 0][3] >= 128 and cpx[k, 15][3] >= 128:
                open_n.add(u)
    return out, open_s, open_n


def pick_side(art, height):
    """An 8-column tile of the object's own drawn front, for the faces the
    drawing never shows: the longest stretch of columns whose runs end on the
    same row with a full front."""
    W, Hh = art.size
    px = art.load()
    ends = []
    for u in range(W):
        end = None
        for v in range(Hh - 1, -1, -1):
            if px[u, v][3] >= 128:
                end = v + 1
                break
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
    # No straight front at all (a railing is mostly gaps): the block of the
    # object's own drawing with the most drawn pixels; its gaps stay gaps.
    best, score = None, -1
    for v in range(0, max(1, Hh - height + 1)):
        for u in range(0, max(1, W - 4 + 1)):
            n = sum(px[u + i, v + j][3] >= 128 for i in range(min(4, W))
                    for j in range(min(height, Hh)))
            if n > score:
                best, score = (u, v), n
    u, v = best
    return vb.Tile(u, v, u + min(4, W), v + min(height, Hh))


def flank_band(layout, art, relief, height):
    """With `flank` (a metatile: the railing along a row), its drawing on the
    upper layer appended below the art, the rows its front shows: what the
    sides of a railing down a column are dressed with. Returns the art and
    the tile, or None."""
    m = relief.get("flank")
    if m is None:
        return art, None
    rows = height + 2
    cell = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
    cpx = cell.load()
    for q, sub in enumerate(layout.subtiles(m, 1)):
        for k, (rgb, index) in enumerate(sub):
            if index:
                cpx[(q & 1) * 8 + k % 8, (q >> 1) * 8 + k // 8] = rgb + (255,)
    out = Image.new("RGBA", (art.width, art.height + rows), (0, 0, 0, 0))
    out.paste(art, (0, 0))
    out.paste(cell.crop((0, 16 - rows, 16, 16)), (0, art.height))
    return out, vb.Tile(0, art.height, 16, art.height + rows)


def kit_specs(spec):
    """Expand a `kit` spec: every building of one kit in its layout, found by
    its top-left corner metatile, its top row and its bottom-left foot, each
    modelled from its own art. Identical drawings are one model."""
    kit = spec["kit"]
    layout = vb.LayoutArt(kit["layout"])
    out, grids = [], set()
    for y in range(layout.h):
        for x in range(layout.w):
            if layout.metatile(x, y) != kit["corner"]:
                continue
            x2 = x + 1
            while x2 < layout.w and layout.metatile(x2, y) in kit["top"]:
                x2 += 1
            if x2 >= layout.w or layout.metatile(x2, y) not in kit["end"]:
                continue
            y2 = y + 1
            while y2 < layout.h and layout.metatile(x, y2) != kit["foot"]:
                y2 += 1
            if y2 >= layout.h:
                continue
            w, h = x2 - x + 1, y2 - y + 1
            grid = tuple(layout.metatile(x + i, y + j) for j in range(h) for i in range(w))
            if grid in grids:
                continue
            grids.add(grid)
            top_row = [layout.metatile(x + i, y) for i in range(w)]
            meta = {"unit": 0x222 in top_row and top_row[-1] == 0x223}
            out.append({"name": "%s_%d_%d" % (spec["name"], x, y), "layout": kit["layout"],
                        "rect": (x, y, w, h), "ground": spec["ground"],
                        "parts": (lambda f, W, H, M: (lambda: f(W, H, M)))(
                            spec["parts"], w * 16, h * 16, meta),
                        "exact": spec["exact"](w * 16, h * 16, meta)})
    return out


def _inside(shape, x, y):
    """Is pixel (x, y) in a piece's shape: rectangles (x0, y0, x1, y1),
    ellipses ("ellipse", cx, cy, rx, ry) and polygons (a list of points), any
    number of each."""
    cx, cy = x + 0.5, y + 0.5
    for part in shape:
        if part[0] == "ellipse":
            _, ex, ey, rx, ry = part
            if ((cx - ex) / rx) ** 2 + ((cy - ey) / ry) ** 2 <= 1.0:
                return True
            continue
        if len(part) == 4 and not isinstance(part[0], (tuple, list)):
            x0, y0, x1, y1 = part
            if x0 <= cx < x1 and y0 <= cy < y1:
                return True
            continue
        hit = False
        n = len(part)
        for k in range(n):
            (ax, ay), (bx, by) = part[k], part[(k + 1) % n]
            if (ay > cy) != (by > cy) and cx < ax + (bx - ax) * (cy - ay) / (by - ay):
                hit = not hit
        if hit:
            return True
    return False


def _inside_grid(shape, W, H):
    """[[_inside(shape, x, y) for x in range(W)] for y in range(H)], asked only
    within the shape's bounds (a pixel beyond them by more than one is out)."""
    out = [[False] * W for _ in range(H)]
    xs, ys = [], []
    for part in shape:
        if part[0] == "ellipse":
            _, ex, ey, rx, ry = part
            xs += [ex - abs(rx), ex + abs(rx)]
            ys += [ey - abs(ry), ey + abs(ry)]
        elif len(part) == 4 and not isinstance(part[0], (tuple, list)):
            xs += [part[0], part[2]]
            ys += [part[1], part[3]]
        else:
            xs += [p[0] for p in part]
            ys += [p[1] for p in part]
    if not xs:
        return out
    x0, x1 = max(0, int(math.floor(min(xs))) - 2), min(W, int(math.ceil(max(xs))) + 2)
    y0, y1 = max(0, int(math.floor(min(ys))) - 2), min(H, int(math.ceil(max(ys))) + 2)
    for y in range(y0, y1):
        row = out[y]
        for x in range(x0, x1):
            row[x] = _inside(shape, x, y)
    return out


# ── Reuse: a tile modelled once is modelled everywhere ───────────────────
#
# A piece of furniture is modelled in the first room (in SPECS order) that
# has it. Any other room that repeats its tiles exactly - every cell of the
# piece's rectangle drawing the same pixels, whatever tileset or metatile
# number they come from - gets the same model, as it is: a room modelled
# later only models what is new in it, and a room not modelled at all gets
# its known furniture standing, the rest of it still flat.

_PIECES = []        # furniture modelled so far, in the order it was
_CELL_KEYS = {}


def cell_keys(layout):
    """Each cell's drawing as bytes, [y][x]: two cells are the same tile when
    they draw the same pixels."""
    out = []
    for y in range(layout.h):
        row = []
        for x in range(layout.w):
            key = (layout.primary, layout.secondary, layout.metatile(x, y))
            if key not in _CELL_KEYS:
                _CELL_KEYS[key] = layout.cell_image(key[2]).tobytes()
            row.append(_CELL_KEYS[key])
        out.append(row)
    return out


def same_room(a, b):
    return (a.w, a.h) == (b.w, b.h) and all(
        (p & 0x3FF) == (q & 0x3FF) for p, q in zip(a.blocks, b.blocks))


def reuse_pieces(layout):
    """[(piece, x, y, pixels, exact)]: the furniture already modelled that
    `layout` repeats, at the cell its rectangle starts, and the room's pixels
    it draws. A piece repeats where every cell of its rectangle is the same
    tile (`exact`) or - a thing standing in the room, not a wall - where its
    own pixels are drawn again, pixel for pixel, whatever floor is round them:
    the gyms' statue stands in every gym. A pixel is drawn once: of two
    pieces drawn alike (two stools), the first placed takes it."""
    keys = cell_keys(layout)
    drawn = {k for row in keys for k in row}
    placed, taken = [], set()
    rooms = {}
    candidates = []
    for p in _PIECES:
        src = p["layout"]
        if src == layout.id:
            continue
        if src not in rooms:
            rooms[src] = same_room(vb.LayoutArt(src), layout)
        if not rooms[src]:      # the same room: its own pieces stand in it already
            candidates.append(p)
    # the same tiles first: two pieces drawn alike may stand at different
    # depths, and where the tiles are the same, so is the piece
    for exact in (True, False):
        for p in candidates:
            if not exact and not p["loose"]:
                continue
            w, h = p["size"]
            first = p["keys"][0][0]
            if exact and first not in drawn:
                continue    # its first cell is drawn nowhere here
            loose = set() if exact else _loose_starts(p, layout, keys)
            for y in range(layout.h - h + 1):
                for x in range(layout.w - w + 1):
                    if exact:
                        if keys[y][x] != first or any(keys[y + j][x:x + w] != p["keys"][j]
                                                      for j in range(h)):
                            continue
                    elif (x, y) not in loose or not _drawn_at(p, keys, x, y):
                        continue
                    pts = {(x * 16 + u, y * 16 + v) for (u, v) in p["pixels"]}
                    if pts & taken:
                        continue
                    taken |= pts
                    placed.append((p, x, y, pts, exact))
    return placed


# A piece is found by its own pixels alone only when it has this many:
# fewer could be drawn anywhere by chance.
LOOSE_PIXELS = 64


def _cell_drawn(key, want):
    return all(key[o:o + 3] == c for (o, c) in want)


def _loose_starts(p, layout, keys):
    """The cells a loose piece's rectangle may start at: wherever its
    anchor cell's pixels are drawn."""
    ai, aj = p["anchor"]
    want = p["cells"][(ai, aj)]
    seen, out = {}, set()
    for y in range(layout.h):
        for x in range(layout.w):
            key = keys[y][x]
            if key not in seen:
                seen[key] = _cell_drawn(key, want)
            if seen[key]:
                out.add((x - ai, y - aj))
    return out


def _drawn_at(p, keys, x, y):
    return all(_cell_drawn(keys[y + j][x + i], want) for (i, j), want in p["cells"].items())


def register_piece(piece_spec, layout, art, hidden=(), loose=False, own=None):
    """Remember a room's piece for the rooms that repeat it. Walls and
    corners too: their rectangle runs as far as they do, so only a room with
    the same wall, cell for cell, repeats them. A piece drawn nowhere (a
    doorway's sides) has no tiles to be found by. `hidden`: what it fills in
    behind the pieces in front of it, which is theirs to draw. `loose`: a
    thing standing in the room, found by its own pixels too (reuse_pieces).
    `own`: the pixels it claimed, when its drawing also carries the floor's
    marks round it - the room's, not the thing's, and another room's floor
    has its own."""
    x0, y0, w, h = piece_spec["rect"]
    keys = cell_keys(layout)
    apx = art.load()
    pixels = {(u, v) for v in range(h * 16) for u in range(w * 16)
              if apx[u, v][3] >= 128 and (u, v) not in hidden
              and (own is None or not loose or (u, v) in own)}
    if not pixels:
        return
    grid = [[keys[y0 + j][x0 + i] for i in range(w)] for j in range(h)]
    # its own pixels cell by cell: (byte offset in the cell's key, rgb)
    cells = {}
    for (u, v) in pixels:
        o = ((v % 16) * 16 + u % 16) * 4
        cells.setdefault((u // 16, v // 16), []).append((o, grid[v // 16][u // 16][o:o + 3]))
    _PIECES.append({"spec": piece_spec, "layout": layout.id, "size": (w, h), "pixels": pixels,
                    "keys": grid, "cells": cells,
                    "anchor": max(cells, key=lambda c: len(cells[c])),
                    "loose": loose and len(pixels) >= LOOSE_PIXELS})


def place_reused(layout, ground):
    """Stand the known furniture in `layout`; returns the pixels it draws and
    the cells it covers. `ground` None: a room nobody modelled, whose every
    other pixel the placements lay flat as ground patches."""
    pixels, cells = set(), set()
    for p, x, y, pts, exact in reuse_pieces(layout):
        if exact:
            p["spec"].setdefault("reused_at", []).append((layout.id, x, y, ground))
        else:
            # found by its own pixels on another floor: it stands there bare,
            # without the marks of its own room's floor, and the room's own
            # paint round it is laid over the placement's floor, cell by cell
            p["spec"].setdefault("bare_at", []).append((layout.id, x, y))
            p["spec"]["own"] = p["pixels"]
        pixels |= pts
        w, h = p["size"]
        cells |= {(x + i, y + j) for j in range(h) for i in range(w)}
        print("  reuse: %s stands in %s at %d,%d" % (p["spec"]["name"], layout.id, x, y))
    return pixels, cells


def reuse_everywhere():
    """The known furniture in every indoor room no spec models."""
    import voxel_cells as vc
    modelled = {s["interior"]["layout"] for s in specs.SPECS if "interior" in s}
    outdoor = vc.MapEvents().outdoor
    entries = json.load(open(os.path.join(vb.ROOT, "data", "layouts", "layouts.json"),
                             encoding="utf-8"))["layouts"]
    for entry in entries:
        lid = entry.get("id")
        if (not lid or lid in modelled or lid in outdoor or "blockdata_filepath" not in entry
                # No tileset: "0" in the source tree, "NULL" in the builder's.
                or {"0", "NULL"} & {entry.get("primary_tileset"), entry.get("secondary_tileset")}
                or not os.path.exists(os.path.join(vb.ROOT, entry["blockdata_filepath"]))):
            continue
        place_reused(vb.LayoutArt(lid), None)


# Where the diagnostic images go (a room's claims): main sets it when it
# verifies or previews; a plain export draws none.
DIAGNOSTICS = None


def interior_specs(spec):
    """Expand an `interior` spec: a room cut into pieces, one model each.

    A room's drawing is walls, furniture and floor. Every pixel equal to the
    floor metatile's own at that place in its cell is floor, left to the
    terrain; the rest is claimed by the first piece (front first) whose shape
    holds it. A piece is read column by column off the pixels it claimed
    (vb.Relief: its top, and its front `height` rows at its foot), standing on
    `base` - a machine on a counter. A piece does not claim the colours it
    `leave`s where they run on out of its shape: the wall seen between a
    plant's leaves or round a machine's corners. With `fill` = P, what the pieces in front
    of it hide of it is drawn from its own pixels P columns away, so the wall
    behind a bookcase is a wall and not a hole in the shape of one. Pixels no
    piece claims are the floor's own marks (an emblem, a mat, shadows), laid
    flat by the pieces whose cells hold them.

    Each piece is its own model, of the cells it covers: a room would not fit
    one chunk's vertices whole, and apart the pieces spread over the chunks
    they stand in.
    """
    room = spec["interior"]
    layout = vb.LayoutArt(room["layout"])
    W, H = layout.w * 16, layout.h * 16
    full = Image.new("RGBA", (W, H))
    for y in range(layout.h):
        for x in range(layout.w):
            full.paste(layout.cell_image(layout.metatile(x, y)), (x * 16, y * 16))
    fpx = full.load()
    floors = [layout.cell_image(m).load() for m in room["ground"]]
    pieces = voxel_relief_fixes.furniture_pieces(spec["name"], room["pieces"])
    # Floor is a pixel the floor metatile has there too - and, inside a
    # piece's shape, one that the open floor reaches through such pixels: a
    # counter top can be painted in the floor's own cream, but its outline
    # closes it off.
    match = [[any(f[x % 16, y % 16] == fpx[x, y] for f in floors) for x in range(W)]
             for y in range(H)]
    grids = [_inside_grid(pc["shape"], W, H) for pc in pieces]
    shaped = [[any(g[y][x] for g in grids) for x in range(W)] for y in range(H)]
    ground = [[match[y][x] and not shaped[y][x] for x in range(W)] for y in range(H)]
    # The room's front corners: black in the drawing, the outside of a room
    # whose front wall the GBA never draws. Seen from the console's camera, a
    # black triangle on the floor is a hole; they are floor.
    opened = _inside_grid(room.get("open", ()), W, H)
    todo = [(x, y) for y in range(H) for x in range(W) if ground[y][x]]
    while todo:
        x, y = todo.pop()
        for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if 0 <= nx < W and 0 <= ny < H and match[ny][nx] and not ground[ny][nx]:
                ground[ny][nx] = True
                todo.append((nx, ny))
    # the floor's shaded variants: their colours are floor wherever they run
    # on from the open floor, never a piece's
    shade = set()
    for m in room.get("shade", ()):
        shade.update("%02x%02x%02x" % c[:3] for c in layout.cell_image(m).getdata())
    owner = [[None] * W for _ in range(H)]
    # the furniture an earlier room has modelled, standing here as it is:
    # its pixels are no piece's of this room's
    reused, reused_cells = place_reused(layout, room["ground"][0])
    for (x, y) in reused:
        owner[y][x] = -1
    hexes = None    # each pixel's colour as `leave` names it, read once a room
    for k, pc in enumerate(pieces):
        inside = _inside_grid(pc["shape"], W, H)
        # what the piece leaves: its `leave` colours where they run on out of
        # its shape - the wall round a machine's corners, not the machine's
        # own orange lamps inside its outline - and, but for a wall, the
        # floor's shadows
        leave = set(pc.get("leave", ())) | (set() if pc.get("fill") or pc.get("facet") else shade)
        left = set()
        if leave:
            if hexes is None:
                hexes = [["%02x%02x%02x" % fpx[x, y][:3] for x in range(W)] for y in range(H)]
            todo = [(x, y) for y in range(H) for x in range(W)
                    if not inside[y][x] and hexes[y][x] in leave]
            seen = set(todo)
            while todo:
                x, y = todo.pop()
                for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
                    if (0 <= nx < W and 0 <= ny < H and (nx, ny) not in seen
                            and hexes[ny][nx] in leave):
                        seen.add((nx, ny))
                        todo.append((nx, ny))
                        if inside[ny][nx]:
                            left.add((nx, ny))
        for y in range(H):
            if not any(inside[y]):
                continue    # (a pixel outside the shape is never the piece's)
            for x in range(W):
                if not inside[y][x]:
                    continue
                # a wall (a filled piece, a corner) is solid in its whole shape: its
                # baseboard may be drawn in the floor's own colours
                # `claim`: where an outline happens to be drawn in the floor's
                # own colour at the floor's own place - a bed's corner on a
                # nail head - and so was taken for floor
                if (owner[y][x] is None and (not ground[y][x] or pc.get("fill") or pc.get("facet")
                                             or _inside(pc.get("claim", ()), x, y))
                        and inside[y][x]
                        and (x, y) not in left):
                    owner[y][x] = k
    if DIAGNOSTICS is not None:
        # the claims, for the eye: each piece a colour, floor dark, marks white
        claims = Image.new("RGB", (W, H))
        cpx = claims.load()
        for y in range(H):
            for x in range(W):
                o = owner[y][x]
                if o is not None:
                    cpx[x, y] = ((o * 97) % 200 + 55, (o * 57) % 200 + 55, (o * 151) % 200 + 55)
                else:
                    cpx[x, y] = (30, 30, 36) if ground[y][x] else (255, 255, 255)
        os.makedirs(DIAGNOSTICS, exist_ok=True)
        claims.resize((W * 3, H * 3), Image.NEAREST).save(
            os.path.join(DIAGNOSTICS, spec["name"] + "_claims.png"))
    # each piece's pixels (row by row, as one pass over the room finds
    # them), and what of it is hidden behind earlier pieces
    owned_by = [set() for _ in pieces]
    for y in range(H):
        for x in range(W):
            o = owner[y][x]
            if o is not None and o >= 0:
                owned_by[o].add((x, y))
    walls = {k for k, pc in enumerate(pieces) if pc.get("fill")}
    drawn = []
    for k, pc in enumerate(pieces):
        mine = owned_by[k]
        fill = {}
        period = pc.get("fill")
        if period:
            for y in range(H):
                for x in range(W):
                    o = owner[y][x]
                    if o is None or o >= k or not _inside(pc["shape"], x, y):
                        continue
                    for d in range(period, W, period):
                        # from any wall's own pixel, this piece's or the
                        # other half's: a row's every panel may be hidden
                        src = next(((xx, y) for xx in (x - d, x + d)
                                    if 0 <= xx < W and owner[y][xx] in walls), None)
                        if src:
                            fill[(x, y)] = fpx[src]
                            break
        if not mine and not pc.get("walls"):
            raise SystemExit("%s: piece %s claims no pixel" % (spec["name"], pc["name"]))
        drawn.append((mine, fill))
    # the room with what the pieces hide filled in: where hidden faces are
    # dressed from
    filled = full.copy()
    lpx = filled.load()
    for mine, fill in drawn:
        for (x, y), c in fill.items():
            lpx[x, y] = c
    rects = []
    for k, (mine, fill) in enumerate(drawn):
        pts = list(mine) + list(fill)
        if pieces[k].get("facet"):
            (xa, _, fa), (xb, _, fb) = pieces[k]["facet"]
            pts += [(int(xa), int(fa) - 1), (int(xb) - 1, int(fb) - 1)]
        # cells a piece answers for beyond its pixels: where its side walls
        # run, the black corners it closes the room at
        for wall in pieces[k].get("walls", ()):
            wa, wb = wall[0], wall[1]
            pts += [(min(int(wa[0]), W - 1), min(int(wa[1]), H - 1)),
                    (min(int(wb[0]), W - 1), min(int(wb[1]), H - 1))]
        pts += [(cx * 16, cy * 16) for (cx, cy) in pieces[k].get("cells", ())]
        xs = [x for x, _ in pts]
        ys = [y for _, y in pts]
        rects.append((min(xs) // 16, min(ys) // 16, max(xs) // 16 + 1, max(ys) // 16 + 1))
    def mark_under(x, y):
        """The floor mark a piece hides at (x, y): the same mark where the
        row runs out of the piece both ways - a rug under a chair, a stripe
        running on under it. Anything else (the shade down a room's edge on
        one side) is left to the floor."""
        found = []
        for step in (-1, 1):
            xx = x
            while 0 <= xx < W and owner[y][xx] is not None:
                xx += step
            if not (0 <= xx < W) or ground[y][xx] or opened[y][xx]:
                return None
            found.append(fpx[xx, y])
        return found[0] if found[0] == found[1] else None

    decal_of = {}
    for k, (x0, y0, x1, y1) in enumerate(rects):
        for cy in range(y0, y1):
            for cx in range(x0, x1):
                decal_of.setdefault((cx, cy), k)
    for y in range(layout.h):
        for x in range(layout.w):
            if ((x, y) not in decal_of and (x, y) not in reused_cells
                    and (layout.blocks[y * layout.w + x] >> 10) & 3):
                print("  %s: blocked cell %d,%d is in no piece" % (spec["name"], x, y))
    out = []
    for k, pc in enumerate(pieces):
        mine, fill = drawn[k]
        x0, y0, x1, y1 = rects[k]
        w, h = x1 - x0, y1 - y0
        obj = Image.new("RGBA", (w * 16, h * 16), (0, 0, 0, 0))
        # the art: the piece and its floor marks; the marks alone below it;
        # below those, the stretch of the room its hidden faces are dressed
        # from, which need not be its own (a side wall's inside is the back
        # wall's orange, not the line its top is drawn with)
        sample = pc.get("side")
        sh = (sample[3] - sample[1]) if sample else 0
        sw = (sample[2] - sample[0]) if sample else 0
        lid = pc.get("top")
        th = (lid[3] - lid[1]) if lid else 0
        tw = (lid[2] - lid[0]) if lid else 0
        # a card (a plant) is a rectangle standing up: it is textured with its
        # own pixels alone, stored below the rest, or the floor marks behind
        # its pot would stand up with it
        ch = h * 16 if pc.get("card") else 0
        # and last, what the pieces hide of the room's floor marks (a rug
        # under a chair), laid under everything
        uo = 2 * h * 16 + sh + th + ch
        art = Image.new("RGBA", (max(w * 16, sw, tw), uo + h * 16), (0, 0, 0, 0))
        if sample:
            # a piece's own pixels stay its own: the wall behind a television
            # fills in what the set hides of it, not the set's casing
            crop = filled.crop(sample)
            cpx = crop.load()
            for (x, y) in mine:
                if sample[0] <= x < sample[2] and sample[1] <= y < sample[3]:
                    cpx[x - sample[0], y - sample[1]] = fpx[x, y]
            art.paste(crop, (0, 2 * h * 16))
        if lid:
            # and the stretch its hidden top is laid with
            art.paste(filled.crop(lid), (0, 2 * h * 16 + sh))
        opx, apx = obj.load(), art.load()
        for (x, y) in mine:
            opx[x - x0 * 16, y - y0 * 16] = fpx[x, y]
        for (x, y), c in fill.items():
            opx[x - x0 * 16, y - y0 * 16] = c
        art.paste(obj, (0, 0))
        cells = sorted(c for c, o in decal_of.items() if o == k)
        for (cx, cy) in cells:
            for j in range(16):
                for i in range(16):
                    x, y = cx * 16 + i, cy * 16 + j
                    lx, ly = x - x0 * 16, y - y0 * 16
                    if not ground[y][x] and owner[y][x] is None and not opened[y][x]:
                        apx[lx, ly] = fpx[x, y]
                        apx[lx, ly + h * 16] = fpx[x, y]
                    elif owner[y][x] is not None and owner[y][x] >= 0 and not pieces[
                            owner[y][x]].get("card"):
                        # under a piece: what the drawing hides there. On a
                        # rug (a mark either side of it) it is the rug, not
                        # the room's bare floor showing round a chair's legs
                        # from any other angle. Not behind a card (a plant):
                        # that floor is in view, and its drawing never was
                        under = mark_under(x, y)
                        if under is not None:
                            apx[lx, ly + uo] = under
        if sample:
            side = vb.Tile(0, 2 * h * 16, sw, 2 * h * 16 + sh)
        else:
            side = pick_side(obj, int(pc["height"]))
        if pc.get("facet"):
            # a chamfered corner: one wall along its foot, not a staircase of
            # columns whose steps show from any other angle. It stands a
            # pixel south of its drawn foot, a pixel taller - the same pixels
            # at 45 degrees - so its foot row is not level with the floor.
            (xa, ta, fa), (xb, tb, fb) = pc["facet"]
            ox, oz = x0 * 16, y0 * 16
            parts = [vb.Facet(pc["name"], (xa - ox, fa + 1 - oz), (xb - ox, fb + 1 - oz),
                              fa + 1 - ta, fb + 1 - tb)]
        elif not mine:
            parts = []  # a stairwell's sides: walls only, drawn nowhere
        else:
            # furniture stands on its foot, the lowest row it is drawn to; a
            # filled piece (a wall) is whole in every column already - but a
            # wall with a doorway in it names its foot, or the lintel would
            # stand at the doorway's top
            if pc.get("foot") is not None:
                foot = pc["foot"] - y0 * 16
            else:
                foot = None if pc.get("fill") else max(y for _, y in mine) + 1 - y0 * 16
            back = pc.get("back") if pc.get("against") is None else pc["against"]
            if back is not None:
                back = back - y0 * 16 - pc.get("base", 0)
            if pc.get("card"):
                art.paste(obj, (0, 2 * h * 16 + sh + th))
                parts = [vb.Card(pc["name"], obj, foot, voff=2 * h * 16 + sh + th)]
            else:
                top_tile = (vb.Tile(0, 2 * h * 16 + sh, tw, 2 * h * 16 + sh + th)
                            if lid else None)
                relief = vb.Relief(pc["name"], obj, pc["height"], side, foot=foot,
                                   solid=pc.get("solid", False), back=back, top_tile=top_tile,
                                   against=pc.get("against") is not None)
                parts = [vb.Lifted(relief, pc.get("base", 0)) if pc.get("base") else relief]
        for n, wall in enumerate(pc.get("walls", ())):
            # the room's side walls: edge on to the GBA camera, so drawn
            # nowhere, and dressed with the stretch of wall `side` names;
            # a third member is a wall's own height (a stairwell's sides)
            wa, wb = wall[0], wall[1]
            tall = wall[2] if len(wall) > 2 else pc["height"]
            ox, oz = x0 * 16, y0 * 16
            # a stairwell's back shows at 45 degrees only where the wall in
            # front of it has its doorway: the room's check judges it, not
            # its own model's
            behind = "" if mine else "~behind"
            parts.append(vb.PlainWall("%s_side%d%s" % (pc["name"], n, behind),
                                      (wa[0] - ox, wa[1] - oz),
                                      (wb[0] - ox, wb[1] - oz), -1, tall, side))
        # only the cells with a mark: a quad over nothing but floor would sample
        # outside the drawing once the export crops it to what is opaque
        local = [(cx - x0, cy - y0) for (cx, cy) in cells
                 if art.crop(((cx - x0) * 16, (cy - y0) * 16 + h * 16,
                              (cx - x0) * 16 + 16, (cy - y0) * 16 + h * 16 + 16)).getbbox()]
        if local:
            parts.append(vb.Decal(pc["name"] + "_floor", local, h * 16))
        hidden = [(cx - x0, cy - y0) for (cx, cy) in cells
                  if art.crop(((cx - x0) * 16, (cy - y0) * 16 + uo,
                               (cx - x0) * 16 + 16, (cy - y0) * 16 + uo + 16)).getbbox()]
        if hidden:
            # "~behind": at 45 degrees the piece stands over it; the room's
            # check, with every piece in place, judges it
            parts.append(vb.Decal(pc["name"] + "_under~behind", hidden, uo, lift=0.25))
        out.append({"name": "%s_%s" % (spec["name"], pc["name"]), "layout": room["layout"],
                    "rect": (x0, y0, w, h), "ground": room["ground"], "art": art,
                    "parts": (lambda ps: (lambda: ps))(parts),
                    "exact": [(0, 0, w * 16, h * 16)], "interior": spec["name"]})
        # `alone`: a room's own shell - its walls, a doorway's recess - which
        # another room does not repeat by having the same floor along an edge
        if not pc.get("alone"):
            register_piece(out[-1], layout, art, {(x - x0 * 16, y - y0 * 16) for (x, y) in fill},
                           loose=not (pc.get("fill") or pc.get("facet") or pc.get("walls")),
                           own={(x - x0 * 16, y - y0 * 16) for (x, y) in mine})
    return out


def build_models(only=None):
    models = []
    layouts = {}
    expanded = []
    for spec in specs.SPECS:
        if "components" in spec:
            expanded += component_specs(spec, layouts)
        elif "kit" in spec:
            expanded += kit_specs(spec)
        elif "props" in spec:
            expanded += prop_specs(spec)
        elif "interior" in spec:
            expanded += interior_specs(spec)
        else:
            expanded.append(spec)
    reuse_everywhere()
    for spec in expanded:
        if only and not spec["name"].startswith(only):
            continue
        lid = spec["layout"]
        if lid not in layouts:
            layouts[lid] = vb.LayoutArt(lid)
        layout = layouts[lid]
        x, y, w, h = spec["rect"]
        owned = spec.get("owned")
        if "art" in spec:
            art = spec["art"]  # composed by its expander (a room's piece)
        else:
            art = layout.building_art(x, y, w, h, layout.ground_tiles(spec["ground"]), cells=owned,
                                      ground_px=layout.ground_pixels(spec["ground"])
                                      if owned or spec.get("ground_px") else None,
                                      upper=spec.get("relief", {}).get("upper", False))
        if "ground_rgb" in spec:
            # colours the object never has: the ground painted in its own
            # cells wherever it is (a curb's paving and lawn beside it, which
            # the ground metatiles do not draw at the same place)
            clear_rgb(art, spec["ground_rgb"])
        if "keep" in spec:
            # only these rectangles of the drawing are the model's: what
            # stands under a stall is not
            apx = art.load()
            for v in range(h * 16):
                for u in range(w * 16):
                    if not any(a <= u < c and b <= v < d for (a, b, c, d) in spec["keep"]):
                        apx[u, v] = (0, 0, 0, 0)
        if "relief" in spec:
            height = spec["relief"]["height"]
            relief = spec["relief"]
            side = pick_side(art, height)
            art, open_s, open_n = seam_art(layout, spec, art, height,
                                           layout.ground_tiles(spec["ground"]),
                                           layout.ground_pixels(spec["ground"]))
            if "ground_rgb" in spec:
                clear_rgb(art, spec["ground_rgb"])     # and the run's next rows
            art, flank = flank_band(layout, art, relief, height)
            parts = [vb.Relief("relief", art, height, side,
                               hull=relief.get("hull", 0), bridge=relief.get("bridge", 0),
                               seam=(h * 16, open_s, open_n), flank_tile=flank,
                               seam_x=(spec.get("west", ()), spec.get("east", ())))]
            spec["exact"] = [(0, 0, w * 16, h * 16)]
        elif "parts_art" in spec:
            # parts read off the drawing itself (a battle tent's dome), at
            # the spec's level of detail ("lod": HeightSolid step, cell, tol)
            vb.HEIGHTSOLID_LOD = spec.get("lod")
            try:
                parts = spec["parts_art"](art)
            finally:
                vb.HEIGHTSOLID_LOD = None
            if isinstance(parts, tuple):
                parts, art = parts      # with a texture of its own
        else:
            parts = spec["parts"]()
        model = vb.Model(spec["name"], art, parts, (w, h), spec["ground"][0])
        if spec.get("clip_art"):
            model.spec = spec
            clip_to_art(model)
        model.owned = owned if owned is not None else {(i, j) for i in range(w) for j in range(h)}
        model.spec = spec
        model.layout = layout
        models.append(model)
        if spec.get("bare_at"):
            # the piece as it stands on another room's floor: without its
            # own room's floor marks (reuse_pieces)
            bare = dict(spec, name=spec["name"] + "_bare", bare=True,
                        reused_at=[(l, bx, by, None) for (l, bx, by) in spec["bare_at"]])
            # and with nothing of its own room in its art: what is not its
            # own (a statue's pedestal corners, the floor at its foot) is
            # clear, so the room's paint laid under it shows (ground_patch)
            own = set(spec["own"])
            bare_art = art.copy()
            bpx = bare_art.load()
            for v in range(h * 16):
                for u in range(w * 16):
                    if (u, v) not in own:
                        bpx[u, v] = (0, 0, 0, 0)
            twin = vb.Model(bare["name"], bare_art, [pt for pt in parts if not (
                isinstance(pt, vb.Decal) and pt.name.endswith("_floor"))],
                (w, h), spec["ground"][0])
            twin.owned, twin.spec, twin.layout = model.owned, bare, layout
            twin.own = set(spec["own"])
            models.append(twin)
    return models


CAMERAS = [
    ("game", dict(pitch=40.0, yaw=0.0, distance=12.0)),
    ("left", dict(pitch=40.0, yaw=0.0, distance=12.0, shift=(4.5, 0))),
    ("right", dict(pitch=40.0, yaw=0.0, distance=12.0, shift=(-4.5, 0))),
    ("yaw35", dict(pitch=35.0, yaw=35.0, distance=9.0)),
    ("yaw-50", dict(pitch=30.0, yaw=-50.0, distance=8.0)),
    ("high", dict(pitch=62.0, yaw=15.0, distance=8.0)),
]


def preview(model, out_dir, cams=None):
    layout = model.layout
    x, y, w, h = model.spec["rect"]
    override = {(x + i, y + j): model.ground_metatile for i in range(w) for j in range(h)}
    ground, gtex = vb.ground_tris(layout, x - 8, y - 6, x + w + 8, y + h + 8, override, None)
    house = vb.model_world_tris(model, x, y)
    art = model.art
    paths = []
    for name, c in CAMERAS:
        if cams and name not in cams:
            continue
        sx, sz = c.get("shift", (0, 0))
        target = (x + w / 2.0 + sx, 0.0, y + h - 1.0 + sz)
        cam = vb.Camera(target, pitch=c["pitch"], yaw=c["yaw"], distance=c["distance"])
        img = vb.render_scene(cam, [(ground, gtex), (house, art)], scale=2)
        path = os.path.join(out_dir, "%s_%s.png" % (model.name, name))
        img.save(path)
        paths.append(path)
    return paths


# ── Export ───────────────────────────────────────────────────────────────

MAGIC = b"VXB7"
# A model drawn from its own mesh (static: ctr_voxel.c StaticMeshPrepare), not
# copied into the chunk it stands in - a chunk's buffer holds ~9300 vertices
# with its ground and trees (voxel_engine_view.py check):
#   one of more than STATIC_TRIANGLES triangles;
#   one of STATIC_REPEATED or more that a map places STATIC_TIMES times or more
#   (sea rocks, the market's urns): one mesh, drawn at each place.
STATIC_TRIANGLES = 1000
STATIC_REPEATED = 200
STATIC_TIMES = 3


def cell_heights(model):
    """Tallest point of the model over each cell, in pixels.

    The lighting pass casts a building's shadow from one height per cell;
    this gives it the model's own instead of a guessed box. Worked out once
    per model of this run (every placement and the export ask again).
    """
    tops = getattr(model, "_cell_heights", None)
    if tops is None:
        tops = model._cell_heights = _cell_heights(model)
    return tops


def _cell_heights(model):
    w, h = model.cells
    tops = [0.0] * (w * h)
    for (tri, shade, tag) in model.mesh.tris:
        pts = [p[:3] for p in tri]
        # a cell the triangle lies wholly outside (by more than clip's EPS)
        # clips to nothing
        x0, x1 = min(p[0] for p in pts) - 2 * vb.EPS, max(p[0] for p in pts) + 2 * vb.EPS
        z0, z1 = min(p[2] for p in pts) - 2 * vb.EPS, max(p[2] for p in pts) + 2 * vb.EPS
        for cy in range(h):
            if z1 < cy * 16.0 or z0 > cy * 16.0 + 16:
                continue
            for cx in range(w):
                if x1 < cx * 16.0 or x0 > cx * 16.0 + 16:
                    continue
                piece = vb.clip(pts, 0, cx * 16.0, True)
                for axis, val, keep in ((0, cx * 16.0 + 16, False), (2, cy * 16.0, True),
                                        (2, cy * 16.0 + 16, False)):
                    if len(piece) >= 3:
                        piece = vb.clip(piece, axis, val, keep)
                if len(piece) >= 3 and _area_xz(piece) > 1e-6:
                    i = cy * w + cx
                    tops[i] = max(tops[i], max(p[1] for p in piece))
    return tuple(min(255, int(round(t))) for t in tops)


# A cell whose solid covers less of it than this casts from its own
# footprint, pixel by pixel (a railing's line, a hedge's end), not a box.
FOOTPRINT_FULL = 240


def cell_footprints(model):
    """Each cell's footprint as the sun sees it: 16 rows of 16 bits, bit x of
    row z set where the model stands over that pixel of the cell's plan (any
    face with plan area, a pixel above the ground). None for a cell it covers
    whole or not at all - a box, as cell_heights gives it - and for a room's
    piece, which no sun reaches."""
    w, h = model.cells
    out = [None] * (w * h)
    if "interior" in model.spec:
        return out
    grid = [[False] * (w * 16) for _ in range(h * 16)]
    for (tri, shade, tag) in model.mesh.tris:
        if max(p[1] for p in tri) < 1.0:
            continue    # a decal or a patch on the ground casts nothing
        (ax, _, az), (bx, _, bz), (cx, _, cz) = (p[:3] for p in tri)
        area = (bx - ax) * (cz - az) - (bz - az) * (cx - ax)
        if abs(area) < 1e-6:
            continue
        x0 = max(0, int(math.floor(min(ax, bx, cx))))
        x1 = min(w * 16, int(math.ceil(max(ax, bx, cx))))
        z0 = max(0, int(math.floor(min(az, bz, cz))))
        z1 = min(h * 16, int(math.ceil(max(az, bz, cz))))
        for pz in range(z0, z1):
            for px in range(x0, x1):
                qx, qz = px + 0.5, pz + 0.5
                d1 = (bx - ax) * (qz - az) - (bz - az) * (qx - ax)
                d2 = (cx - bx) * (qz - bz) - (cz - bz) * (qx - bx)
                d3 = (ax - cx) * (qz - cz) - (az - cz) * (qx - cx)
                if (d1 >= 0 and d2 >= 0 and d3 >= 0) or (d1 <= 0 and d2 <= 0 and d3 <= 0):
                    grid[pz][px] = True
    for cy in range(h):
        for cx in range(w):
            rows = [sum(1 << x for x in range(16) if grid[cy * 16 + z][cx * 16 + x])
                    for z in range(16)]
            n = sum(bin(r).count("1") for r in rows)
            if 0 < n < FOOTPRINT_FULL:
                out[cy * w + cx] = tuple(rows)
    return out


def _area_xz(poly):
    """Plan area of a clipped piece; a face lying on the cell's edge has none."""
    return abs(sum(poly[i][0] * poly[(i + 1) % len(poly)][2] -
                   poly[(i + 1) % len(poly)][0] * poly[i][2] for i in range(len(poly)))) / 2


_EXISTS = {}    # blockdata path -> on disk: asked for every model
_WATER = {}     # (primary, secondary) -> their metatiles that are water
_PAINTED = {}   # blockdata path -> the metatiles the map paints
_SEAMLESS = {}  # blockdata path -> those it paints beside themselves (_seamless)


def _painted(path, blocks):
    if path not in _PAINTED:
        _PAINTED[path] = {b & 0x3FF for b in blocks}
    return _PAINTED[path]


def _seamless(path, blocks, w):
    """The metatiles a map paints beside themselves both across and down: a
    material that tiles (paving, grass, the sea), not the edge of one (a
    path's grassy border), which laid under every cell of a model would
    stripe the floor."""
    if path not in _SEAMLESS:
        across, down = set(), set()
        m = [b & 0x3FF for b in blocks]
        for k in range(len(m)):
            if (k + 1) % w and k + 1 < len(m) and m[k + 1] == m[k]:
                across.add(m[k])
            if k + w < len(m) and m[k + w] == m[k]:
                down.add(m[k])
        _SEAMLESS[path] = across & down
    return _SEAMLESS[path]


def water_metatiles(primary, secondary):
    """The metatiles of a pair of tilesets whose behaviour is water (the
    sea, a pond, a sea's rocky edge drawn on it)."""
    if (primary, secondary) not in _WATER:
        out = set()
        for which, ts in enumerate((primary, secondary)):
            if ts in vc.ATTRIBUTES:
                for k, a in enumerate(vc.read_u16(vc.ATTRIBUTES[ts])):
                    if (a & 0xFF) in vc.WATER:
                        out.add(k + which * vb.NUM_PRIMARY)
        _WATER[(primary, secondary)] = out
    return _WATER[(primary, secondary)]


# The ground under a placement shows whole where a model's cell has no patch
# and the model draws nothing, and where the GBA's view sees none of it: a
# patch lies a hair over it, so a line of it shows along the patch's edges in
# the game's view, and round a model that is not its drawing in depth. What
# the map draws in a model's own cells where the model draws nothing is what
# should be there; `placement_ground` picks the metatile that draws most of
# it, and the cells its ground still is not are reported (report_grounds).
GROUND_REPORT = []   # (model, layout, x, y, ground, [(i, j, kind, pixels, metatile)])
GROUND_REPORT_PIXELS = 24   # a cell's pixels its ground is not, to report it
_CELL_RGB = {}
_EXPOSED = {}


def _cell_rgb(layout_id, metatile):
    art = _layout_art(layout_id)
    key = (art.primary, art.secondary, metatile)
    if key not in _CELL_RGB:
        _CELL_RGB[key] = art.cell_image(metatile).convert("RGB")
    return _CELL_RGB[key]


def _exposed(model, i, j):
    """Cell (i, j) of a model as a mask: 255 where the model draws nothing,
    which the map's own drawing shows there - by a patch (ground_patch) or
    by the bare ground."""
    key = (id(model), i, j)
    if key not in _EXPOSED:
        apx = model.art.load()
        W, H = model.art.size
        own = getattr(model, "own", None)   # a bare piece: its own pixels only
        mask = Image.new("L", (16, 16), 0)
        mpx = mask.load()
        for y in range(16):
            for x in range(16):
                X, Y = i * 16 + x, j * 16 + y
                drawn = ((X, Y) in own if own is not None
                         else X < W and Y < H and apx[X, Y][3] >= 128)
                if not drawn:
                    mpx[x, y] = 255
        _EXPOSED[key] = mask if mask.getbbox() else None
    return _EXPOSED[key]


_COVERED = {}


def _covered(model):
    """The model in the GBA's projection, as a mask the size of its art: 255
    where its faces draw (an opaque texel), whatever the drawing has there -
    its real depth rises past the drawing (a roof behind the drawn outline
    hides the ground at its corners)."""
    key = id(model)
    if key not in _COVERED:
        W, H = model.art.size
        ras = vb.Raster(W, H, bg=(0, 0, 0))
        for (tri, shade, tag) in model.mesh.tris:
            ras.draw([(x, z - y, y + z, 1.0, u, v) for (x, y, z, u, v) in tri], model.art, 1.0, 1)
        mask = Image.new("L", (W, H), 0)
        mask.putdata([0 if o is None else 255 for o in ras.owner])
        _COVERED[key] = mask
    return _COVERED[key]


def _bare(model, i, j):
    """What shows of the ground in cell (i, j) of a model with no patch over
    it, in the GBA's view: where the model draws nothing (_exposed) and no
    face of it stands in front (_covered). None if nothing."""
    mask = _exposed(model, i, j)
    if mask is None:
        return None
    faces = _covered(model).crop((i * 16, j * 16, i * 16 + 16, j * 16 + 16))
    mask = ImageChops.multiply(mask, ImageChops.invert(faces))
    return mask if mask.getbbox() else None


def _drawn_alike(a, b, mask):
    """How many of the mask's pixels two cells draw the same."""
    r, g, bl = ImageChops.difference(a, b).split()
    same = ImageChops.lighter(ImageChops.lighter(r, g), bl).point(lambda v: 255 if v == 0 else 0)
    return ImageChops.multiply(same, mask).histogram()[255]


def _cell_grounds(model, layout_id, px, py):
    """The cells a spec lays a ground of its choosing on (cell_ground,
    cell_ground_at): {(i, j): metatile}."""
    chosen = dict(model.spec.get("cell_ground", {}))
    chosen.update(model.spec.get("cell_ground_at", {}).get((layout_id, px, py), {}))
    return chosen


def patched_cells(model, layout_id, px, py, odd):
    """The cells of `odd` a placement lays the map's own paint over
    (placement_patches), but for those of a ground the spec chose: where
    the model stands the map's paint is its own drawing, lost."""
    w, h = model.cells
    tops = cell_heights(model)
    chosen = _cell_grounds(model, layout_id, px, py)
    return {(i, j) for (i, j) in odd if (i, j) not in chosen
            and (tops[j * w + i] == 0 or layout_id in model.spec.get("patch_all", ()))}


def _edge(mask):
    """A mask's pixels on its edge: next to one it leaves out, or on the
    cell's border."""
    pad = Image.new("L", (18, 18), 0)
    pad.paste(mask, (1, 1))
    inner = pad.filter(ImageFilter.MinFilter(3)).crop((1, 1, 17, 17))
    return ImageChops.subtract(mask, inner)


def placement_ground(model, entry, blocks, path, px, py, ring, patched):
    """The ground under one placement, of the metatiles the map paints round
    it and the spec's own grounds the map paints (those of them that tile,
    `_seamless`, if any). First the one that draws most of what the map
    shows in the model's cells where the model draws nothing and no patch
    covers it (the grass under a fence's bars): there the ground shows
    whole, in the GBA's view too. Then what the game's camera sees of it, a
    line along the edges of the patches and of the model: the patches'
    edge pixels it draws as the map does (the paving between a curb's
    slabs), and the ground round the model, 16 pixels of edge a cell."""
    lw = entry["width"]
    candidates = set(ring) | (set(model.spec["ground"]) & _painted(path, blocks))
    candidates = (candidates & _seamless(path, blocks, lw)) or candidates
    if len(candidates) < 2:
        return min(candidates) if candidates else model.ground_metatile
    chosen = _cell_grounds(model, entry["id"], px, py)
    bare, edges = [], []
    for (i, j) in sorted(model.owned):
        if (i, j) in chosen or _exposed(model, i, j) is None:
            continue
        rgb = _cell_rgb(entry["id"], blocks[(py + j) * lw + px + i] & 0x3FF)
        if (i, j) in patched:
            edges.append((rgb, _edge(_exposed(model, i, j))))
        else:
            mask = _bare(model, i, j)
            if mask is not None:
                bare.append((rgb, mask))
    score = {}
    for c in candidates:
        tile = _cell_rgb(entry["id"], c)
        score[c] = (sum(_drawn_alike(rgb, tile, mask) for rgb, mask in bare),
                    sum(_drawn_alike(rgb, tile, mask) for rgb, mask in edges) + 16 * ring.get(c, 0))
    return max(sorted(candidates), key=score.get)


def report_grounds(models, found):
    """The cells where a placement's ground is not what the map shows, into
    GROUND_REPORT. In each map the first placement that owns a cell lays its
    ground there (voxel_building.c ModelCell), and what shows of it is what
    no model there draws, no face of one stands in front of (_bare) and no
    patch covers (a table's top over a room's floor, a tree's crown).
    bare: GROUND_REPORT_PIXELS of those pixels or more drawn otherwise,
    as the GBA's view sees them too (gba); patched: most of the patch's
    pixels, and as many, colours the ground has none of - a line of it
    along the patch's edges in the game's view (under): the sea under a
    curb's paving."""
    del GROUND_REPORT[:]
    full = Image.new("L", (16, 16), 255)
    by_layout = {}
    for rec in found:
        by_layout.setdefault(rec[0], []).append(rec)
    for recs in by_layout.values():
        name = recs[0][6]
        art = _layout_art(name)
        drawn, laid, owner = {}, {}, {}
        for k, (_, index, px, py, ground, cells, _) in enumerate(recs):
            m = models[index]
            for (i, j) in m.owned:
                c = (px + i, py + j)
                owner.setdefault(c, k)
                mask = _bare(m, i, j)
                d = full if mask is None else ImageChops.invert(mask)
                drawn[c] = ImageChops.lighter(drawn[c], d) if c in drawn else d
            for (i, j, img) in cells:
                c = (px + i, py + j)
                a = img.getchannel("A").point(lambda v: 255 if v >= 128 else 0)
                laid[c] = ImageChops.lighter(laid[c], a) if c in laid else a
        bad = {}
        for (X, Y), k in sorted(owner.items()):
            _, index, px, py, ground, cells, _ = recs[k]
            m = models[index]
            if ground == OWN_GROUND or not (0 <= X < art.w and 0 <= Y < art.h):
                continue
            if (X - px, Y - py) in _cell_grounds(m, name, px, py):
                continue    # a ground of the spec's choosing, whole
            own = art.metatile(X, Y)
            rgb, tile = _cell_rgb(name, own), _cell_rgb(name, ground)
            patch = laid.get((X, Y))
            cover = drawn[(X, Y)] if patch is None else ImageChops.lighter(drawn[(X, Y)], patch)
            shown = ImageChops.invert(cover)
            n = shown.histogram()[255]
            off = n - _drawn_alike(rgb, tile, shown)
            if off >= GROUND_REPORT_PIXELS:
                bad.setdefault(k, []).append((X - px, Y - py, "bare", off, own))
            if patch is not None:
                # the patch where no model draws over it: the map's paint
                shows = ImageChops.multiply(patch, ImageChops.invert(drawn[(X, Y)]))
                palette = set(tile.getdata())
                seen = [c for c, v in zip(rgb.getdata(), shows.getdata()) if v]
                off = sum(1 for c in seen if c not in palette)
                if off >= GROUND_REPORT_PIXELS and off * 2 > len(seen):
                    bad.setdefault(k, []).append((X - px, Y - py, "patched", off, own))
        for k, cells in sorted(bad.items()):
            _, index, px, py, ground, _, _ = recs[k]
            GROUND_REPORT.append((models[index].name, name, px, py, ground, cells))


def find_placements(model, layouts_json):
    """Every place in Hoenn the building stands, with the ground around it.

    A spec may name `match_rows` (first, end): only those rows of the
    reference must repeat cell for cell. The rest belong to the building's
    surroundings as much as to it - Oldale paints its path and a tree's crown
    into the Pokemon Center's and the Mart's top row - and are drawn by the
    model regardless. When every matched metatile is in the primary tileset
    the building is the same in every town that shares it, so any secondary
    tileset is accepted.

    A core cell the map paints with another metatile still matches when it
    draws the building's own pixels: Rustboro paints the Center's and the
    Mart's corners with its paving round them, not Petalburg's grass.

    Each placement carries its own ground metatile, one the map paints (its
    atlas holds no other): of those round it (not water, for what stands on
    land) and the spec's own, the one that draws most of what the map shows
    in the model's cells where the model draws nothing (placement_ground).
    """
    ref = model.layout
    x, y, w, h = model.spec["rect"]
    r0, r1 = model.spec.get("match_rows", (0, h))
    template = [ref.metatile(x + i, y + j) for j in range(h) for i in range(w)]
    # a cell the drawing gives none of the model's pixels (under a stall)
    # may be anything
    apx = model.art.load()
    core = [(i, j) for j in range(r0, r1) for i in range(w) if (i, j) in model.owned
            and any(apx[i * 16 + x, j * 16 + y][3] >= 128 for y in range(16) for x in range(16))]
    if not core:
        core = [(i, j) for j in range(r0, r1) for i in range(w) if (i, j) in model.owned]
    core.sort(key=lambda c: (c[1], c[0]))
    primary_only = all(template[j * w + i] < vb.NUM_PRIMARY for i, j in core)
    found = []
    if "at" in model.spec:
        # found by its tiles (prop_specs): it stands where it was found, on
        # each cell's own ground
        index_of = {e.get("id"): k for k, e in enumerate(layouts_json)}
        return [(index_of[lid] + 1, px, py, OWN_GROUND, lid, [])
                for (lid, px, py) in model.spec["at"]]
    for index, entry in enumerate(layouts_json):
        if model.spec.get("bare"):
            break       # it stands only where reuse_pieces found it
        if entry.get("primary_tileset") != ref.primary:
            continue
        # An object of free shape (a hedge run) stands only where it was
        # found: a short run also repeats inside every longer one. A block of
        # one stands wherever the generator found the same block.
        if "owned" in model.spec and entry["id"] != ref.id:
            continue
        # a building drawn alike in other towns' own tiles (the battle
        # tents): `anywhere` looks for its pixels in every secondary tileset
        if not primary_only and entry.get("secondary_tileset") != ref.secondary                 and not model.spec.get("anywhere"):
            continue
        path = os.path.join(vb.ROOT, entry["blockdata_filepath"])
        if path not in _EXISTS:
            _EXISTS[path] = os.path.exists(path)
        if not _EXISTS[path]:
            continue
        blocks = vb.read_u16(path)
        lw, lh = entry["width"], entry["height"]
        i0, j0 = core[0]
        # the first cell may be another metatile drawing the same building
        # (Dewford paints the gym with its own tiles, sand where Petalburg
        # has grass): asked once per metatile of the layout
        starts = {}
        for py in range(lh - h + 1):
            for px in range(lw - w + 1):
                first = blocks[(py + j0) * lw + px + i0] & 0x3FF
                if first not in starts:
                    starts[first] = first == template[j0 * w + i0] or (
                        "interior" not in model.spec and "owned" not in model.spec
                        and entry.get("secondary_tileset") not in ("0", "NULL")
                        and same_building_pixels(model, entry["id"], first, i0, j0))
                if not starts[first]:
                    continue
                if first != template[j0 * w + i0] and (entry["id"], px, py) in _canonical_at(model):
                    continue    # another spec models this copy (Rustboro's gym)
                if not all((blocks[(py + j) * lw + px + i] & 0x3FF) == template[j * w + i]
                           or same_building_pixels(model, entry["id"],
                                                   blocks[(py + j) * lw + px + i] & 0x3FF, i, j)
                           for i, j in core):
                    continue
                if "owned" in model.spec and (px, py) not in model.spec.get("repeat_at", [(x, y)]):
                    continue
                if "interior" in model.spec:
                    # a room's piece belongs to its place in the room: a stool
                    # drawn like another stands only where it was drawn, and
                    # on the room's floor, whatever the cells round it are;
                    # and in another layout only if the room is the same room,
                    # or it would lend half a room to a different one
                    if (px, py) != (x, y):
                        continue
                    if entry["id"] != ref.id and (
                            (lw, lh) != (ref.w, ref.h)
                            or any((blocks[k] & 0x3FF) != (ref.blocks[k] & 0x3FF)
                                   for k in range(lw * lh))):
                        continue
                    odd = [(i, j) for j in range(h) for i in range(w)
                           if (blocks[(py + j) * lw + px + i] & 0x3FF) != template[j * w + i]]
                    found.append((index + 1, px, py, model.ground_metatile, entry["id"], odd))
                    continue
                ring = {}
                for yy in range(py - 1, py + h + 1):
                    for xx in range(px - 1, px + w + 1):
                        inside = px <= xx < px + w and py <= yy < py + h
                        if inside or not (0 <= xx < lw and 0 <= yy < lh):
                            continue
                        cell = blocks[yy * lw + xx]
                        if cell & 0xC00:
                            continue
                        ring[cell & 0x3FF] = ring.get(cell & 0x3FF, 0) + 1
                # what stands on land stands on land: the sea along a pier's
                # edge or under a fence by the shore can be the commonest
                # cell round it, and shows in a line wherever the model or
                # its patches leave the ground under them uncovered. Not for
                # a spec that names water among its grounds (a boat, the
                # Trick House by its pond).
                if not water_metatiles(ref.primary, ref.secondary) & set(model.spec["ground"]):
                    water = water_metatiles(entry["primary_tileset"], entry["secondary_tileset"])
                    land = {k: n for k, n in ring.items() if k not in water}
                    if land:
                        ring = land
                odd = [(i, j) for j in range(h) for i in range(w) if (i, j) in model.owned
                       and (blocks[(py + j) * lw + px + i] & 0x3FF) != template[j * w + i]]
                if model.spec.get("patch_cells"):
                    # cells whose map paint shows round the model (the sea
                    # behind the Space Center's rocket): laid flat on them
                    odd = sorted(set(odd) | set(model.spec["patch_cells"]))
                    model.spec.setdefault("patch_all", set()).add(entry["id"])
                if model.spec.get("patch_own"):
                    odd = sorted(model.owned)
                    model.spec.setdefault("patch_all", set()).add(entry["id"])
                if model.spec.get("cell_ground"):
                    odd = sorted(set(odd) | set(model.spec["cell_ground"]))
                if (entry["id"], px, py) in model.spec.get("cell_ground_at", {}):
                    odd = sorted(set(odd) | set(model.spec["cell_ground_at"][(entry["id"], px, py)]))
                if model.spec.get("relief", {}).get("upper"):
                    # only the upper layer is modelled: the map paints the
                    # rest of every cell, its ground's edges as drawn
                    odd = sorted(model.owned)
                    model.spec.setdefault("patch_all", set()).add(entry["id"])
                ground = placement_ground(model, entry, blocks, path, px, py, ring,
                                          patched_cells(model, entry["id"], px, py, odd))
                fixed = model.spec.get("ground_fixed")
                if isinstance(fixed, dict):
                    fixed = fixed.get(entry["id"])
                elif fixed is True:
                    fixed = model.ground_metatile
                # the spec's own ground whatever is round it (a boat moored by
                # a pier floats on the sea, not on the pier), or one of its
                # own in one map (a tent on its lawn, the path in front of it
                # more common round it) - if the map paints it somewhere: the
                # console's atlas holds only the metatiles its maps use
                if fixed is not None and fixed is not False and fixed in _painted(path, blocks):
                    ground = fixed
                found.append((index + 1, px, py, ground, entry["id"], odd))
    # and the rooms that repeat a piece of furniture's tiles (reuse_pieces):
    # on a modelled room's floor, or, in a room nobody modelled, on its
    # commonest floor with the rest of its cells laid over it as patches
    ids = [e.get("id") for e in layouts_json]
    for (lid, px, py, ground) in model.spec.get("reused_at", ()):
        if ground is None:
            blocks = _layout_art(lid).blocks
            floor = {}
            for cell in blocks:
                if not cell & 0xC00:
                    floor[cell & 0x3FF] = floor.get(cell & 0x3FF, 0) + 1
            ground = max(floor, key=floor.get) if floor else model.ground_metatile
            odd = [(i, j) for j in range(h) for i in range(w)]
            model.spec.setdefault("patch_all", set()).add(lid)
        else:
            odd = []
        found.append((ids.index(lid) + 1, px, py, ground, lid, odd))
    return found


def _canonical_at(model):
    """Where the other specs' own copies stand: (layout, x, y)."""
    return {(s["layout"], s["rect"][0], s["rect"][1]) for s in specs.SPECS
            if "rect" in s and "layout" in s and s["name"] != model.spec["name"]}


def _layout_art(layout_id):
    if layout_id not in _LAYOUT_ART:
        _LAYOUT_ART[layout_id] = vb.LayoutArt(layout_id)
    return _LAYOUT_ART[layout_id]


_LAYOUT_ART = {}


def same_building_pixels(model, layout_id, metatile, i, j):
    """Does `metatile` draw every pixel the model owns in its cell (i, j)?"""
    if layout_id not in _LAYOUT_ART:
        _LAYOUT_ART[layout_id] = vb.LayoutArt(layout_id)
    ipx = _LAYOUT_ART[layout_id].cell_image(metatile).load()
    apx = model.art.load()
    owned = 0
    for y in range(16):
        for x in range(16):
            a = apx[i * 16 + x, j * 16 + y]
            if a[3] < 128:
                continue
            owned += 1
            if ipx[x, y][:3] != a[:3]:
                return False
    return owned > 0


def pack_atlas(images):
    """Pack (key, image) pairs into the smallest page: each goes where its
    top edge lands lowest on the skyline of what is placed (bottom-left). A
    shelf packer left the space beside a tall, narrow drawing - a room's
    side wall - empty, and a small room took a page of half a megabyte.

    A page is a power of two wide and stores only the rows it uses, in whole
    tiles of 8. The console's texture is those rows rounded up to a power of
    two (texture_rows) and is backed for that height: the rows past the
    stored ones are never written, and the console sizes the piece for the
    texture it reads. Rounded up, the Battle Frontier's two halves take
    512 KiB each, over what the page arena holds with both maps in view -
    that has to be answered by the packing. Every width is tried, tallest
    first and largest first, and the fewest texels win.
    Returns ((width, rows), {key: (ox, oy)})."""
    orders = [sorted(images, key=lambda kv: (-kv[1].size[1], -kv[1].size[0])),
              sorted(images, key=lambda kv: (-kv[1].size[0] * kv[1].size[1], -kv[1].size[1]))]
    best = None
    for tw in (64, 128, 256, 512, 1024):
        for order in orders:
            spots = _skyline(order, tw, MAX_TEXTURE_SIDE)
            if spots is None:
                continue
            rows = max(spots[key][1] + img.size[1] for key, img in order)
            rows = max(8, (rows + 7) // 8 * 8)
            th = texture_rows(rows)
            if tw > 8 * th or th > 8 * tw:
                continue
            if best is None or (tw * rows, tw * th) < best[0]:
                best = ((tw * rows, tw * th), (tw, rows), spots)
    if best is None:
        raise SystemExit("building art does not fit a 1024x1024 texture")
    return best[1], best[2]


def texture_rows(rows):
    """The height of the texture that samples a page of `rows` rows: the
    power of two the GPU needs (voxel_building.c TakePage)."""
    th = 8
    while th < rows:
        th *= 2
    return th


def _skyline(order, tw, th):
    sky = [0] * tw          # the lowest free row of every column
    spots = {}
    for key, img in order:
        aw, ah = img.size
        if aw > tw:
            return None
        best = None
        for x in range(tw - aw + 1):
            y = max(sky[x:x + aw])
            if y + ah <= th and (best is None or y < best[0]):
                best = (y, x)
        if best is None:
            return None
        y, x = best
        spots[key] = (x, y)
        for k in range(x, x + aw):
            sky[k] = y + ah
    return spots


MAX_TEXTURE = (512, 512)   # one page in VRAM; ctr_voxel.c caches four
MAX_TEXTURE_SIDE = 1024    # the GPU's largest texture


def texel_offset(x, y, width):
    """8x8 Morton tiles, the PICA200 order (as gen_voxel_trees.py)."""
    morton = 0
    for bit in range(3):
        morton |= ((x >> bit) & 1) << (2 * bit) | ((y >> bit) & 1) << (2 * bit + 1)
    return ((y // 8) * (width // 8) + x // 8) * 64 + morton


def ground_patch(model, layout, px, py, i, j):
    """What the map paints in cell (i, j) of a placement besides the building.

    The map's own metatile with every pixel the model's drawing owns made
    transparent: Oldale's path beside the Center's roof, a tree's crown
    behind the Mart, a cliff's edge. Laid flat over the placement's ground.
    """
    img = layout.cell_image(layout.metatile(px + i, py + j)).convert("RGBA")
    ipx, apx = img.load(), model.art.load()
    own = getattr(model, "own", None)   # a bare piece: its own pixels only
    for y in range(16):
        for x in range(16):
            if (apx[i * 16 + x, j * 16 + y][3] >= 128 if own is None
                    else (i * 16 + x, j * 16 + y) in own):
                ipx[x, y] = (0, 0, 0, 0)
    return img


def placement_patches(model, layout_json_entry_name, layouts, px, py, odd):
    """The ground patches of one placement: cells the model leaves empty
    where the map paints something the reference does not."""
    if not odd:
        return []
    cells = []
    # and those of one placement (`cell_ground_at`, by layout and position)
    chosen = _cell_grounds(model, layout_json_entry_name, px, py)
    patched = patched_cells(model, layout_json_entry_name, px, py, odd)
    for (i, j) in odd:
        if (i, j) in chosen:
            # a ground of the spec's choosing, whole (what the drawing hid)
            if layout_json_entry_name not in layouts:
                layouts[layout_json_entry_name] = vb.LayoutArt(layout_json_entry_name)
            cells.append((i, j, layouts[layout_json_entry_name].cell_image(chosen[(i, j)])
                          .convert("RGBA")))
            continue
        if (i, j) not in patched:
            continue  # the model stands there; the map's own paint is lost
        if layout_json_entry_name not in layouts:
            layouts[layout_json_entry_name] = vb.LayoutArt(layout_json_entry_name)
        img = ground_patch(model, layouts[layout_json_entry_name], px, py, i, j)
        if img.getbbox() is not None:
            cells.append((i, j, img))
    return cells


def sampled_texels(uv, W, H):
    """The texels a triangle with these uv corners can read with nearest
    sampling: every texel whose square its uv triangle overlaps (separating
    axes: the square's two and the triangle's three edge normals), within
    the rows and columns its corners span. A triangle with no area in uv
    (a face edge-on to the drawing) reads along its longest edge."""
    (u0, v0), (u1, v1), (u2, v2) = uv
    area = (u1 - u0) * (v2 - v0) - (u2 - u0) * (v1 - v0)
    eps = 1e-4
    lo_u = max(0, int(math.floor(min(u0, u1, u2) + eps)))
    hi_u = min(W - 1, max(lo_u, int(math.ceil(max(u0, u1, u2) - eps)) - 1))
    lo_v = max(0, int(math.floor(min(v0, v1, v2) + eps)))
    hi_v = min(H - 1, max(lo_v, int(math.ceil(max(v0, v1, v2) - eps)) - 1))
    out = set()
    if abs(area) < 1e-6:
        pts = sorted(uv)
        a, c = pts[0], pts[-1]
        n = int(max(abs(c[0] - a[0]), abs(c[1] - a[1])) * 4) + 1
        for k in range(n + 1):
            u = a[0] + (c[0] - a[0]) * k / n
            v = a[1] + (c[1] - a[1]) * k / n
            out.add((min(hi_u, max(lo_u, int(u))), min(hi_v, max(lo_v, int(v)))))
        return out
    sign = 1.0 if area > 0 else -1.0
    edges = [((ax, ay), (bx - ax, by - ay)) for (ax, ay), (bx, by) in
             (((u0, v0), (u1, v1)), ((u1, v1), (u2, v2)), ((u2, v2), (u0, v0)))]
    for v in range(lo_v, hi_v + 1):
        for u in range(lo_u, hi_u + 1):
            # overlapping unless some edge has the whole square outside it
            if all(max(sign * (ex * (cv - ay) - ey * (cu - ax))
                       for cu in (u, u + 1) for cv in (v, v + 1)) > eps * math.hypot(ex, ey)
                   for (ax, ay), (ex, ey) in edges):
                out.add((u, v))
    return out


def pack_band_pieces(model, top, left, texel_sets, band_art):
    """The faces still reading a band (what they read past the outline the
    drawing's own faces read clear) keep only what they read: the texels of
    each, a texel round them, in pieces of those that overlap, packed in
    shelves under the rows the drawing's faces read (`top`). Each face moves with its piece by whole
    texels, so what it shows is unchanged; the band's rows then go."""
    W, H = model.art.size
    rects = []
    for texels in texel_sets:
        us = [u for (u, v) in texels]
        vs = [v for (u, v) in texels]
        rects.append([max(0, min(us) - 1), max(0, min(vs) - 1),
                      min(W, max(us) + 2), min(H, max(vs) + 2)])
    # pieces: the faces whose rectangles overlap, joined
    parent = list(range(len(rects)))

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    grid = {}
    for i, (u0, v0, u1, v1) in enumerate(rects):
        for gx in range(u0 // 16, (u1 - 1) // 16 + 1):
            for gy in range(v0 // 16, (v1 - 1) // 16 + 1):
                grid.setdefault((gx, gy), []).append(i)
    for members in grid.values():
        for a in range(len(members)):
            for b in range(a + 1, len(members)):
                i, j = members[a], members[b]
                ri, rj = rects[i], rects[j]
                if ri[0] < rj[2] and rj[0] < ri[2] and ri[1] < rj[3] and rj[1] < ri[3]:
                    parent[find(i)] = find(j)
    pieces = {}
    for i, r in enumerate(rects):
        p = pieces.setdefault(find(i), [r[0], r[1], r[2], r[3], []])
        p[0], p[1] = min(p[0], r[0]), min(p[1], r[1])
        p[2], p[3] = max(p[2], r[2]), max(p[3], r[3])
        p[4].append(i)
    # merging may make pieces overlap again: join until none do
    merged = True
    pieces = list(pieces.values())
    while merged:
        merged = False
        for a in range(len(pieces)):
            for b in range(a + 1, len(pieces)):
                pa, pb = pieces[a], pieces[b]
                if pa[0] < pb[2] and pb[0] < pa[2] and pa[1] < pb[3] and pb[1] < pa[3]:
                    pa[0], pa[1] = min(pa[0], pb[0]), min(pa[1], pb[1])
                    pa[2], pa[3] = max(pa[2], pb[2]), max(pa[3], pb[3])
                    pa[4] += pb[4]
                    del pieces[b]
                    merged = True
                    break
            if merged:
                break
    # shelves under the drawing, tallest first
    width = max(W, max(p[2] - p[0] for p in pieces))
    x = y = shelf = 0
    spots = []
    for p in sorted(pieces, key=lambda p: -(p[3] - p[1])):
        w, h = p[2] - p[0], p[3] - p[1]
        if x + w > width:
            x, y, shelf = 0, y + shelf, 0
        spots.append((p, x, y))
        x += w
        shelf = max(shelf, h)
    out = Image.new("RGBA", (width, top + y + shelf), (0, 0, 0, 0))
    out.paste(model.art.crop((0, 0, W, top)), (0, 0))
    tris = list(model.mesh.tris)
    for p, px, py in spots:
        out.paste(band_art.crop((p[0], p[1], p[2], p[3])), (px, top + py))
        du, dv = px - p[0], top + py - p[1]
        for i in p[4]:
            k = left[i]
            tri, shade, tag = tris[k]
            tris[k] = (tuple((x_, y_, z_, u + du, v + dv) for (x_, y_, z_, u, v) in tri), shade, tag)
    model.mesh.tris = tris
    model.art = out


def same_reads(model, art, tris):
    """Proof of the compactions: every face reads, texel for texel, the
    colours it read before (its texels moved by the whole texels its
    corners moved by). A face reading a texel that differs is an error."""
    W, H = art.size
    new = model.art.convert("RGBA")
    NW, NH = new.size
    apx, npx = art.load(), new.load()
    for (tri, _, _), (moved, _, _) in zip(tris, model.mesh.tris):
        du = moved[0][3] - tri[0][3]
        dv = moved[0][4] - tri[0][4]
        if abs(du - round(du)) > 1e-6 or abs(dv - round(dv)) > 1e-6:
            raise SystemExit("%s: a face moved by part of a texel" % model.name)
        du, dv = int(round(du)), int(round(dv))
        for (u, v) in sampled_texels([(p[3], p[4]) for p in tri], W, H):
            a = apx[u, v]
            b = npx[u + du, v + dv] if 0 <= u + du < NW and 0 <= v + dv < NH else (0, 0, 0, 0)
            if (a[3] >= 128) != (b[3] >= 128) or (a[3] >= 128 and a[:3] != b[:3]):
                raise SystemExit("%s: a face reads %s at %d,%d where it read %s"
                                 % (model.name, b, u + du, v + dv, a))


def compact_rows(model):
    """Drop the rows of a model's texture no triangle samples, moving every
    triangle's v down by the rows dropped above it. Each triangle's whole v
    range (and a row either side) is kept, so its shift is one number and
    what it shows is unchanged."""
    # the band's faces moved onto the drawing, or (compact_rows_plain)
    # only those whose rectangle reads it as it is: whichever leaves the
    # smaller texture - never more than before
    trial = copy.copy(model)
    trial.mesh = copy.copy(model.mesh)
    trial.mesh.tris = list(model.mesh.tris)
    compact_rows_plain(trial)
    compact_rows_moved(model)
    if folded_area(trial.art, trial.mesh.tris) < folded_area(model.art, model.mesh.tris):
        model.art, model.mesh.tris = trial.art, trial.mesh.tris


def compact_rows_plain(model):
    """compact_rows as it was: a band face moves onto the drawing when the
    rectangle of texels its corners span is the same there."""
    H = model.art.size[1]
    rows = model.cells[1] * 16
    if H > rows:
        art = model.art.convert("RGBA")
        W = art.size[0]
        tris = []
        for (tri, shade, tag) in model.mesh.tris:
            vs = [p[4] for p in tri]
            us = [p[3] for p in tri]
            b = int(min(vs) // rows)
            if b >= 1 and min(vs) < H and int((max(vs) - 1e-6) // rows) == b:
                v0, v1 = int(math.floor(min(vs))), int(math.ceil(max(vs)))
                u0, u1 = int(math.floor(min(us))), int(math.ceil(max(us)))
                v1, u1 = max(v1, v0 + 1), max(u1, u0 + 1)
                u0, u1 = max(0, u0), min(W, u1)
                v1 = min(H, v1)
                if (u1 > u0 and v1 > v0 and art.crop((u0, v0 - b * rows, u1, v1 - b * rows)).tobytes()
                        == art.crop((u0, v0, u1, v1)).tobytes()):
                    tri = tuple((x, y, z, u, v - b * rows) for (x, y, z, u, v) in tri)
            tris.append((tri, shade, tag))
        model.mesh.tris = tris
    drop_rows(model)


def compact_rows_moved(model):
    """compact_rows with the band's faces moved onto the drawing wherever
    what they read can be made the same there."""
    H = model.art.size[1]
    rows = model.cells[1] * 16
    if H > rows:
        # A face laid with a grown band (the back of a solid) reads the
        # drawing instead: inside the outline the bands are copies of it,
        # and what it reads past the outline - the grown colours - is
        # written into the drawing's clear texels that no face of the
        # drawing reads. What every face shows is unchanged, and the
        # band's rows go (the dome of the Battle Frontier was two pages'
        # worth of the same drawing).
        art = model.art.convert("RGBA")
        W = art.size[0]
        apx = art.load()
        band = []
        front = set()
        top = rows          # the rows the drawing's own faces read
        for (tri, shade, tag) in model.mesh.tris:
            vs = [p[4] for p in tri]
            texels = sampled_texels([(p[3], p[4]) for p in tri], W, H)
            b = int(min(vs) // rows)
            if min(vs) >= H:
                band.append(None)   # past the texture: reads nothing of it
            elif b >= 1 and int((max(vs) - 1e-6) // rows) == b:
                band.append((texels, b))
            else:
                band.append(None)
                top = max(top, min(H, int(math.ceil(max(vs))) + 1))
                # and a texel round it: a face's edge on a texel's edge may
                # read the next one by rounding
                front |= {(u + du, v + dv) for (u, v) in texels if v < rows
                          for du in (-1, 0, 1) for dv in (-1, 0, 1)}
        filled = {}         # clear texels given a band's colour
        held = set()        # texels a moved face reads as they are
        moved, left = {}, []

        def exact(k):
            texels, b = band[k]
            return all(apx[u, v - b * rows] == apx[u, v] for (u, v) in texels)

        # the faces that read the drawing as it is first: a fill made for
        # another must never take one of those from them
        # and no fill outside the drawing's own box: it would widen the crop
        box = art.crop((0, 0, W, rows)).getbbox() or (0, 0, 0, 0)
        order = [k for k, seen in enumerate(band) if seen is not None and seen[0]]
        order.sort(key=lambda k: 0 if exact(k) else 1)
        for k in order:
            texels, b = band[k]
            fills, holds = {}, set()
            for (u, v) in texels:
                at = (u, v - b * rows)
                want = apx[u, v]
                now = fills.get(at, filled.get(at, apx[at]))
                if now == want:
                    holds.add(at)
                    continue
                if (at in filled or at in fills or at in front or at in held
                        or at in holds or apx[at][3] >= 128 or want[3] < 128
                        or not (box[0] <= at[0] < box[2] and box[1] <= at[1] < box[3])):
                    fills = None
                    break
                fills[at] = want
            if fills is not None and not (holds & set(fills)):
                filled.update(fills)
                held |= holds
                moved[k] = b
            else:
                left.append(k)
        left.sort()
        tris = []
        for k, (tri, shade, tag) in enumerate(model.mesh.tris):
            if k in moved:
                tri = tuple((x, y, z, u, v - moved[k] * rows) for (x, y, z, u, v) in tri)
            tris.append((tri, shade, tag))
        band_art = art.copy()
        for (u, v), c in filled.items():
            apx[u, v] = c
        model.art = art
        model.mesh.tris = tris
        if left:
            # the pieces, or the band's rows as they are: whichever leaves
            # the smaller texture (many small pieces with their margins can
            # take more than the few rows they come from)
            plain = (model.art, list(model.mesh.tris))
            drop_rows(model)
            kept = (model.art, model.mesh.tris)
            model.art, model.mesh.tris = plain[0], list(plain[1])
            pack_band_pieces(model, top, left, [band[k][0] for k in left], band_art)
            drop_rows(model)
            if folded_area(*kept) <= folded_area(model.art, model.mesh.tris):
                model.art, model.mesh.tris = kept
            return
    drop_rows(model)


def folded_area(art, tris):
    """The texels a model's texture takes on its page once folded
    (compact_islands) and cropped."""
    trial = type("Trial", (), {})()
    trial.art, trial.mesh = art, type("Mesh", (), {})()
    trial.mesh.tris = list(tris)
    compact_islands(trial)
    b = trial.art.getbbox() or (0, 0, 1, 1)
    return (b[2] - b[0]) * (b[3] - b[1])


def drop_rows(model):
    """compact_rows' last step: the rows no face reads go."""
    H = model.art.size[1]
    keep = [False] * H
    spans = []
    for (tri, shade, tag) in model.mesh.tris:
        vs = [p[4] for p in tri]
        lo = min(H - 1, max(0, int(math.floor(min(vs))) - 1))
        hi = min(H - 1, int(math.ceil(max(vs))) + 1)
        spans.append(lo)
        for r in range(lo, hi + 1):
            keep[r] = True
    if all(keep):
        return
    dropped, before = 0, []
    for r in range(H):
        before.append(dropped)
        if not keep[r]:
            dropped += 1
    rows = [r for r in range(H) if keep[r]]
    W = model.art.size[0]
    out = Image.new("RGBA", (W, max(1, len(rows))), (0, 0, 0, 0))
    for k, r in enumerate(rows):
        out.paste(model.art.crop((0, r, W, r + 1)), (0, k))
    tris = []
    for (tri, shade, tag), lo in zip(model.mesh.tris, spans):
        off = before[lo]
        tris.append((tuple((x, y, z, u, v - off) for (x, y, z, u, v) in tri), shade, tag))
    model.mesh.tris = tris
    model.art = out


def compact_islands(model):
    """Fold a tall model texture (a big solid's drawing over its bands) into
    strips laid side by side, cut at rows no face reads across: a page packs
    a squarer block. Each face moves with its strip, by whole texels, so
    what it shows is unchanged."""
    W, H = model.art.size
    if H <= 256 or not model.mesh.tris:
        return
    spans = []
    crossed = [0] * (H + 1)
    for (tri, shade, tag) in model.mesh.tris:
        vs = [p[4] for p in tri]
        lo = max(0, int(math.floor(min(vs))) - 1)
        hi = min(H, int(math.ceil(max(vs))) + 1)
        spans.append(lo)
        crossed[lo + 1] += 1
        crossed[hi] -= 1
    inside, cuts = 0, []
    for r in range(1, H):
        inside += crossed[r]
        if inside == 0:
            cuts.append(r)
    # as many strips as the page's width allows, cut at the free rows
    # nearest to even shares
    k = min((H + 255) // 256, 512 // max(1, W))
    if k < 2:
        return
    segs, start = [], 0
    for j in range(1, k):
        want = H * j / k
        ok = [c for c in cuts if c > start]
        if not ok:
            break
        end = min(ok, key=lambda c: abs(c - want))
        segs.append((start, end))
        start = end
    segs.append((start, H))
    if len(segs) < 2:
        return
    # not, where the page can take it whole, when the cuts are so uneven
    # the strips take far more room than the texture did
    if H <= 512 and W * len(segs) * max(e - s_ for s_, e in segs) > 1.15 * W * H:
        return
    out = Image.new("RGBA", (W * len(segs), max(e - s_ for s_, e in segs)), (0, 0, 0, 0))
    for j, (s_, e) in enumerate(segs):
        out.paste(model.art.crop((0, s_, W, e)), (j * W, 0))
    tris = []
    for (tri, shade, tag), lo in zip(model.mesh.tris, spans):
        j = max(i for i, (s_, e) in enumerate(segs) if s_ <= lo)
        s_ = segs[j][0]
        tris.append((tuple((x, y, z, u + j * W, v - s_) for (x, y, z, u, v) in tri), shade, tag))
    model.mesh.tris = tris
    model.art = out


def export(models, path):
    """Write buildings.bin (VXB7).

    Geometry is stored once per model, texture coordinates in pixels of the
    model's own drawing. Textures are paged by map: each layout that places
    anything gets one page holding just the drawings it uses (and its ground
    patches), and a page-model record tells where on that page a model's
    drawing went. The console loads the pages of the maps on screen only.

      "VXB7", u16 pages, models, pageModels, placements, heightBytes, masks,
      u32 vertices, u16 variants, u16 0
      pages       x 8:  u16 w, h; u32 file offset of its RGBA5551 texels (h the
                        rows it holds, a multiple of 8; its texture is h
                        rounded up to a power of two - texture_rows)
      models      x 16: u8 w, h; u16 ground; u32 firstVertex, vertexCount, heights
                        (vertexCount's top bit: a static model)
      pageModels  x 8:  u16 model, page; i16 ox, oy (pixels)
      placements  x 16: u16 layout, pageModel, x, y, ground, extraCount;
                        u32 extraFirst (ground patches, uv in page pixels)
      heightBytes, padding to 2, u16 footprint per height byte (a mask's
      index, 0xFFFF for a box), masks x 32 (16 u16 rows, bit x of row z: the
      solid stands over that pixel of the cell), u8 quarters per height
      byte (of its own metatile's upper layer, bit 2 * row + column, the
      model stands for; 0 for none), padding to 2, variants x 6 (u16
      layout, metatile; u8 quarters, 0: a metatile of the tileset that
      layout draws it from, less those quarters - see ground_variants),
      padding to 4,
      vertices x 24 (x, y, z, u, v, shade), then the pages' texels.
    """
    layouts_json = json.load(open(os.path.join(vb.ROOT, "data", "layouts", "layouts.json"),
                                  encoding="utf-8"))["layouts"]
    layouts = {}
    found = []
    per_map = {i: {} for i in range(len(models))}   # placements of each model in each map
    for index, m in enumerate(models):
        for lid, px, py, ground, name, odd in find_placements(m, layouts_json):
            cells = placement_patches(m, name, layouts, px, py, odd)
            found.append((lid, index, px, py, ground, cells, name))
            per_map.setdefault(index, {})
            per_map[index][lid] = per_map[index].get(lid, 0) + 1
            if odd:
                print("  %s: %s at %d,%d differs in %d cell(s), %d patched from the map"
                      % (m.name, name, px, py, len(odd), len(cells)))
    report_grounds(models, found)
    if GROUND_REPORT:
        # what to fix, and with which spec key: README, the ground chosen
        # and what the generator reports
        print("voxel buildings: %d placement(s) on a ground not what their cells show - "
              "cell i,j, bare (pixels drawn otherwise) or patched (pixels of colours the "
              "ground has none of), and the cell's own metatile" % len(GROUND_REPORT))
        for name, lid, px, py, ground, bad in GROUND_REPORT:
            print("  ground: %s in %s at %d,%d on 0x%03x: %s" % (
                name, lid, px, py, ground, ", ".join("%d,%d %s %d (0x%03x)" % c for c in bad)))
    # only the rows of a drawing some face samples go to the page: a solid's
    # grown bands (Mound.with_ring, own_bands) are mostly never read, and
    # the ring band of a thing without one is empty
    for m in models:
        before = (m.art.convert("RGBA"), list(m.mesh.tris))
        compact_rows(m)
        compact_islands(m)
        same_reads(m, *before)
    crops = {m.name: (m.art.getbbox() or (0, 0, 1, 1)) for m in models}

    # models: geometry once, uv in the model's own art pixels
    records, verts, heights, quarters = [], [], bytearray(), bytearray()
    # a footprint per model cell (0xFFFF: a box), the masks shared
    footprint_of, masks, mask_index = [], [], {}
    for index, m in enumerate(models):
        first = len(verts) // 6
        for (tri, shade, tag) in m.mesh.tris:
            for (x, y, z, u, v) in tri:
                verts += [x / 16.0, y / 16.0, z / 16.0, u, v, shade]
        count = len(verts) // 6 - first
        w, h = m.cells
        # a static model (spec "static"): drawn from its own mesh, never
        # copied into a chunk (ctr_voxel.c StaticMeshPrepare) - vertexCount's top bit
        # and the heavy and the repeated (STATIC_TRIANGLES)
        tris = len(m.mesh.tris)
        static = 0x80000000 if ((getattr(m, "spec", None) or {}).get("static")
                                or tris > STATIC_TRIANGLES
                                or (tris >= STATIC_REPEATED
                                    and max(per_map[index].values() or [0]) >= STATIC_TIMES)) else 0
        records.append(struct.pack("<BBHIII", w, h, m.ground_metatile, first, count | static,
                                   len(heights)))
        # 255: a cell of the rectangle the object does not own (a hedge's
        # rectangle holds the house it runs round)
        heights += bytes(t if (i % w, i // w) in m.owned else 255
                         for i, t in enumerate(cell_heights(m)))
        quads = m.spec.get("quads", {})
        quarters += bytes(quads.get((i % w, i // w), 0) for i in range(w * h))
        for fp in cell_footprints(m):
            if fp is None:
                footprint_of.append(0xFFFF)
            else:
                if fp not in mask_index:
                    mask_index[fp] = len(masks)
                    masks.append(fp)
                footprint_of.append(mask_index[fp])

    # pages: one per layout
    by_layout = {}
    for rec in found:
        by_layout.setdefault(rec[0], []).append(rec)
    pages, page_models, placements, texels = [], [], [], []
    for lid in sorted(by_layout):
        recs = by_layout[lid]
        used = sorted({r[1] for r in recs})
        patches = {}
        for r in recs:
            for (i, j, img) in r[5]:
                patches.setdefault(img.tobytes(), img)
        images = [(("m", k), models[k].art.crop(crops[models[k].name])) for k in used]
        images += [(("p", key), img) for key, img in patches.items()]
        (tw, rows), spots = pack_atlas(images)
        if tw * rows > MAX_TEXTURE[0] * MAX_TEXTURE[1]:
            big = sorted(images, key=lambda kv: -kv[1].size[0] * kv[1].size[1])[:12]
            print("  page of %s: %d texels in %d images; largest: %s" % (
                recs[0][6], sum(i.size[0] * i.size[1] for _, i in images), len(images),
                ", ".join("%s %dx%d" % (models[k[1]].name if k[0] == "m" else "patch", i.size[0], i.size[1])
                          for k, i in big)))
            raise SystemExit("%s: building page %dx%d exceeds the console's %dx%d"
                             % (recs[0][6], tw, rows, MAX_TEXTURE[0], MAX_TEXTURE[1]))
        page = len(pages)
        tex = bytearray(tw * rows * 2)
        for key, img in images:
            ox, oy = spots[key]
            apx = img.load()
            for y in range(img.size[1]):
                for x in range(img.size[0]):
                    r_, g_, b_, a_ = apx[x, y]
                    value = (r_ >> 3) << 11 | (g_ >> 3) << 6 | (b_ >> 3) << 1 | int(a_ >= 128)
                    struct.pack_into("<H", tex, 2 * texel_offset(ox + x, oy + y, tw), value)
        pages.append((tw, rows))
        texels.append(tex)
        pm_of = {}
        for k in used:
            ox, oy = spots[("m", k)]
            ox -= crops[models[k].name][0]
            oy -= crops[models[k].name][1]
            pm_of[k] = len(page_models)
            page_models.append((k, page, ox, oy))
        for (lid_, index, px, py, ground, cells, name) in recs:
            first = len(verts) // 6
            for (i, j, img) in cells:
                ox, oy = spots[("p", img.tobytes())]
                a = [i, 0.01, j, ox, oy, 1.0]
                b = [i + 1, 0.01, j, ox + 16, oy, 1.0]
                c = [i + 1, 0.01, j + 1, ox + 16, oy + 16, 1.0]
                d = [i, 0.01, j + 1, ox, oy + 16, 1.0]
                verts += a + b + c + a + c + d
            placements.append((lid, pm_of[index], px, py, ground, len(verts) // 6 - first, first))
    placements.sort()
    nverts = len(verts) // 6
    # ctr_voxel.c keeps a flag a page (VOXEL_MAX_PAGES), and a page larger
    # than half its VRAM block cannot share it with the next map's
    if len(pages) > 256:
        raise SystemExit("%d texture pages: the console knows 256" % len(pages))
    variants = ground_variants(models, layouts_json)
    head = MAGIC + struct.pack("<HHHHHHIHH", len(pages), len(models), len(page_models),
                               len(placements), len(heights), len(masks), nverts,
                               len(variants), 0)
    body = bytearray()
    for r in records:
        body += r
    for pm in page_models:
        body += struct.pack("<HHhh", *pm)
    for p in placements:
        body += struct.pack("<HHHHHHI", *p)
    body += heights
    if len(heights) % 2:
        body += bytes(1)
    body += struct.pack("<%dH" % len(footprint_of), *footprint_of)
    for fp in masks:
        body += struct.pack("<16H", *fp)
    body += quarters
    if len(quarters) % 2:
        body += bytes(1)
    for (lid, m, q) in variants:
        body += struct.pack("<HHBB", lid, m, q, 0)
    table_size = 8 * len(pages)
    fixed = len(head) + table_size + len(body)
    pad = (-fixed) % 4
    body += bytes(pad)
    body += struct.pack("<%df" % len(verts), *verts)
    offset = len(head) + table_size + len(body)
    table = bytearray()
    for (tw, rows), tex in zip(pages, texels):
        table += struct.pack("<HHI", tw, rows, offset)
        offset += len(tex)
    blob = head + table + body + b"".join(texels)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, "wb").write(blob)
    print("voxel buildings: %d models, %d pages, %d placements, %d vertices, %d footprints, "
          "%.1f KiB -> %s" % (len(models), len(pages), len(placements), nverts, len(masks),
                              len(blob) / 1024.0, path))
    for (tw, rows), lid in zip(pages, sorted(by_layout)):
        print("  page for layout %3d: %dx%d" % (lid, tw, rows))


def ground_variants(models, layouts_json):
    """[(layout, metatile, quarters)]: every metatile a model found by its
    tiles (prop_specs) stands on, less the quarters of its upper layer the
    model stands for. The console composes each in an atlas slot of its own
    and draws it under the model, so that the cell's drawing - its animated
    water - lies there without the rock painted flat on it. Any other copy
    of the metatile keeps its whole drawing."""
    index_of = {e.get("id"): k + 1 for k, e in enumerate(layouts_json)}
    entry_of = {e.get("id"): e for e in layouts_json}
    blocks_of = {}
    out = {}
    for m in models:
        for (lid, px, py) in m.spec.get("at", ()):
            e = entry_of[lid]
            if lid not in blocks_of:
                blocks_of[lid] = vb.read_u16(os.path.join(vb.ROOT, e["blockdata_filepath"]))
            for (i, j), q in m.spec["quads"].items():
                x, y = px + i, py + j
                if not (0 <= x < e["width"] and 0 <= y < e["height"]):
                    continue    # across a seam: the map next door draws it
                mt = blocks_of[lid][y * e["width"] + x] & 0x3FF
                ts = e["primary_tileset"] if mt < vb.NUM_PRIMARY else e["secondary_tileset"]
                out.setdefault((ts, mt, q), index_of[lid])
    variants = sorted((lid, mt, q) for (ts, mt, q), lid in out.items())
    if len(variants) > MAX_VARIANTS:
        raise SystemExit("%d ground variants: the atlas holds %d" % (len(variants), MAX_VARIANTS))
    print("voxel buildings: %d ground variants (a metatile less what a model stands for)"
          % len(variants))
    return variants


def town_preview(models, layout_id, out_dir):
    """A whole layout with every model placed in it, by the console's rules."""
    layouts_json = json.load(open(os.path.join(vb.ROOT, "data", "layouts", "layouts.json"),
                                  encoding="utf-8"))["layouts"]
    layout = vb.LayoutArt(layout_id)
    override, items = {}, []
    for m in models:
        w, h = m.cells
        tops = cell_heights(m)
        for lid, px, py, ground, name, odd in find_placements(m, layouts_json):
            if name != layout_id:
                continue
            for j in range(h):
                for i in range(w):
                    if ground != OWN_GROUND:    # a prop keeps each cell's own
                        override[(px + i, py + j)] = ground
            for (i, j, patch) in placement_patches(m, layout_id, {layout_id: layout}, px, py, odd):
                q = [(px + i, 0.01, py + j, 0, 0), (px + i + 1, 0.01, py + j, 16, 0),
                     (px + i + 1, 0.01, py + j + 1, 16, 16), (px + i, 0.01, py + j + 1, 0, 16)]
                items.append(([((q[0], q[1], q[2]), 1.0), ((q[0], q[2], q[3]), 1.0)], patch))
            items.append((vb.model_world_tris(m, px, py), m.art))
    ground, gtex = vb.ground_tris(layout, 0, 0, layout.w, layout.h, override, None)
    shots = []
    for name, (tx, tz, pitch, yaw, dist) in (
            ("overview", (layout.w / 2.0, layout.h / 2.0 + 2, 45.0, 0.0, 18.0)),
            ("yaw30", (layout.w / 2.0, layout.h / 2.0, 35.0, 30.0, 16.0))):
        cam = vb.Camera((tx, 0.0, tz), pitch=pitch, yaw=yaw, distance=dist)
        img = vb.render_scene(cam, [(ground, gtex)] + items, scale=2)
        path = os.path.join(out_dir, "%s_%s.png" % (layout_id.lower(), name))
        img.save(path)
        shots.append(path)
    return shots


def room_check(models, layout_id, out_dir):
    """A whole room as the console composes it, against its drawing.

    The terrain draws each cell a model covers with the placement's floor
    and every other cell with its own metatile; the pieces stand over them.
    Rendered in the GBA projection, the room must be its drawing again,
    pixel for pixel. Returns the pixels that differ.
    """
    layouts_json = json.load(open(os.path.join(vb.ROOT, "data", "layouts", "layouts.json"),
                                  encoding="utf-8"))["layouts"]
    layout = vb.LayoutArt(layout_id)
    W, H = layout.w * 16, layout.h * 16
    ras = vb.Raster(W, H, bg=(0, 0, 0))
    covered, items, patches = {}, [], []
    for m in models:
        w, h = m.cells
        for lid, px, py, ground, name, odd in find_placements(m, layouts_json):
            if name != layout_id:
                continue
            for j in range(h):
                for i in range(w):
                    covered[(px + i, py + j)] = ground
            items.append((m, px, py))
            patches += [(px + i, py + j, img) for (i, j, img)
                        in placement_patches(m, layout_id, {layout_id: layout}, px, py, odd)]
    for y in range(layout.h):
        for x in range(layout.w):
            img = layout.cell_image(covered.get((x, y), layout.metatile(x, y)))
            q = [(x * 16, 0, y * 16, 0, 0), (x * 16 + 16, 0, y * 16, 16, 0),
                 (x * 16 + 16, 0, y * 16 + 16, 16, 16), (x * 16, 0, y * 16 + 16, 0, 16)]
            for tri in ((q[0], q[1], q[2]), (q[0], q[2], q[3])):
                ras.draw([(X, Z - Y, Y + Z, 1.0, U, V) for (X, Y, Z, U, V) in tri], img, 1.0)
    # the map's own paint round a piece found on another floor, laid flat
    # over the placement's floor as the console lays it
    for (x, y, img) in patches:
        q = [(x * 16, 0, y * 16, 0, 0), (x * 16 + 16, 0, y * 16, 16, 0),
             (x * 16 + 16, 0, y * 16 + 16, 16, 16), (x * 16, 0, y * 16 + 16, 0, 16)]
        for tri in ((q[0], q[1], q[2]), (q[0], q[2], q[3])):
            ras.draw([(X, Z - Y, Y + Z + 0.01, 1.0, U, V) for (X, Y, Z, U, V) in tri], img, 1.0,
                     "patch")
    for (m, px, py) in items:
        for (tri, shade, tag) in m.mesh.tris:
            if "~depth" in tag:
                continue  # real depth rising behind the drawing
            vs = [(x + px * 16, z + py * 16 - y, y + z + py * 16, 1.0, u, v)
                  for (x, y, z, u, v) in tri]
            ras.draw(vs, m.art, shade, tag)
    full = Image.new("RGB", (W, H))
    for y in range(layout.h):
        for x in range(layout.w):
            full.paste(layout.cell_image(layout.metatile(x, y)).convert("RGB"), (x * 16, y * 16))
    got = ras.image()
    fpx, gpx = full.load(), got.load()
    bad = 0
    diff = got.copy()
    dpx = diff.load()
    culprits = {}
    room = next((s["interior"] for s in specs.SPECS
                 if s.get("interior", {}).get("layout") == layout_id), {})
    for y in range(H):
        for x in range(W):
            if _inside(room.get("open", ()), x, y):
                continue  # floor in the round where the drawing is black
            if fpx[x, y] != gpx[x, y]:
                bad += 1
                dpx[x, y] = (255, 0, 255)
                tag = ras.owner[y * W + x] or "terrain"
                culprits.setdefault(tag, []).append((x, y))
    for tag, pts in sorted(culprits.items(), key=lambda kv: -len(kv[1])):
        print("    %-34s %4d px, e.g. %s" % (tag, len(pts), pts[:3]))
    sheet = Image.new("RGB", (W * 2 + 4, H))
    sheet.paste(full, (0, 0))
    sheet.paste(diff, (W + 4, 0))
    sheet.resize((sheet.width * 3, sheet.height * 3), Image.NEAREST).save(
        os.path.join(out_dir, layout_id.lower() + "_room_check.png"))
    return bad


def verify(models, out):
    """The proofs: each model against its drawing, each room as composed.
    Writes the diagnostic images to `out`; True when all of them pass."""
    failed = False
    for model in models:
        judged = model
        if getattr(model, "own", None) is not None:
            # a bare piece: its own pixels, without its room's floor marks
            art = model.art.copy()
            apx = art.load()
            for v in range(model.cells[1] * 16):
                for u in range(model.cells[0] * 16):
                    if (u, v) not in model.own:
                        apx[u, v] = (0, 0, 0, 0)
            judged = types.SimpleNamespace(art=art, mesh=model.mesh, name=model.name)
        reference = model.spec.get("drawing")
        if callable(reference):
            reference = reference(model)
        wrong, missing, extra = vb.ortho_check(judged, os.path.join(out, model.name + "_ortho.png"),
                                               exact=model.spec.get("exact"),
                                               reference=reference)
        bad = vb.density_check(model)
        print("%-22s %4d tris  exact: wrong=%d missing=%d extra=%d  texel density: %d bad"
              % (model.name, model.triangle_count(), wrong, missing, extra, len(bad)))
        for tag, along, down, shear in bad[:8]:
            print("    %-28s along=%.3f down=%.3f shear=%.3f" % (tag, along, down, shear))
        # a model whose outline is a polygon (BakedLathe) declares how many
        # pixels of it may differ; any more is a failure
        if wrong + missing + extra > model.spec.get("tolerance", 0) or bad:
            failed = True
    rooms = sorted({m.spec["layout"] for m in models if "interior" in m.spec})
    for lid in rooms:
        bad = room_check(models, lid, out)
        print("room %-28s composed with the terrain: %d pixel(s) differ from the drawing"
              % (lid, bad))
        failed = failed or bad != 0
    return not failed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preview", default=None)
    ap.add_argument("--only", default=None)
    ap.add_argument("--cams", default=None)
    ap.add_argument("--output", default=None)
    ap.add_argument("--verify", action="store_true",
                    help="prove every model and room against its drawing (the default without --output)")
    ap.add_argument("--town", default=None, help="render a whole layout, e.g. LAYOUT_OLDALE_TOWN")
    args = ap.parse_args()
    out = args.preview or os.path.join(PORT, "build", "buildings")
    checks = args.verify or not (args.output or args.town)
    if checks or args.preview or args.town:
        os.makedirs(out, exist_ok=True)
        global DIAGNOSTICS
        DIAGNOSTICS = out
    models = build_models(args.only)
    if args.preview:
        for model in models:
            preview(model, out, args.cams.split(",") if args.cams else None)
    if checks and not verify(models, out):
        raise SystemExit("a model does not reproduce its drawing")
    if args.output:
        export(models, args.output)
    if args.town:
        for path in town_preview(models, args.town, out):
            print("town preview:", path)


if __name__ == "__main__":
    main()
