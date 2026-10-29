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
import bisect
import collections
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
import voxel_bridge  # noqa: E402
import voxel_fence  # noqa: E402
import gen_voxel_sign_masks  # noqa: E402
import voxel_fortree  # noqa: E402
import voxel_props  # noqa: E402
import voxel_relief_fixes  # noqa: E402
import voxel_night

PORT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
# A placement whose cells keep their own metatile under the model: the
# console draws each cell's own drawing there, from the map's atlas, so its
# animation goes on (the water round a rock) - less the quarters of its upper
# layer the model stands for, a variant of the metatile the atlas composes
# in a slot of its own (`ground_variants`).
OWN_GROUND = 0xFFFF
MAX_VARIANTS = 160      # voxel_atlas.h VOXEL_VARIANTS


def component_specs(spec, layouts):
    """Expand a `components` spec - hedges, walls: objects with no fixed
    shape - into one ordinary spec per connected run of its metatiles, in
    every layout whose secondary tileset is the one named (or, for
    metatiles of a primary tileset, every layout on that primary)."""
    comp = spec["components"]
    layouts_json = json.load(open(os.path.join(vb.ROOT, "data", "layouts", "layouts.json"),
                                  encoding="utf-8"))["layouts"]
    out = []
    for entry in layouts_json:
        if "blockdata_filepath" not in entry or entry.get("id") in comp.get("skip", ()):
            continue
        if ("primary" in comp and entry.get("primary_tileset") != comp["primary"]) or                 ("secondary" in comp and entry.get("secondary_tileset") != comp["secondary"]):
            continue
        # and the metatiles of the secondary tileset that draw the same object
        # (`also`: the General's fence in the Battle Frontier's own tiles)
        tiles = set(comp["tiles"]) | set(comp.get("also", {}).get(entry.get("secondary_tileset"), ()))
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
                # `fill`: tiles that are the object only inside a run of its
                # own drawn tiles (a curb's plain middle); a run of nothing
                # else is the ground they also pave
                if all((blocks[cy * w + cx] & 0x3FF) in comp.get("fill", ()) for (cx, cy) in cells):
                    continue
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


# a colour no live texel of a flag draws: its live texels are clear only
# where the tile's own pixel is (colour 0), never for a colour behind them
FLAG_BG = (255, 0, 255)


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
        elif shape == "flag":
            # a flag on its pole: a card standing at the pole's foot, over
            # the whole grid (a frame of the cloth's animation reaches where
            # the first does not draw), the cloth's texels following the
            # game's animation of its tiles ("live": every pixel of them)
            gw, gh = len(grid[0]), len(grid)
            foot = max(oy * 8 + j * 8 + 8 for (i, j) in present)
            span = (ox * 8, ox * 8 + gw * 8, oy * 8)
            parts = (lambda d, f, sp: (lambda: [vb.Card("flag~live", d, f, span=sp)]))(
                drawing, foot, span)
            live = {}
            lo, hi = spec["live_tiles"]
            for (i, j) in present:
                t = grid[j][i]
                if lo <= (t[0] if isinstance(t, tuple) else t) < hi:
                    for k in range(64):
                        px_ = (ox * 8 + i * 8 + k % 8, oy * 8 + j * 8 + k // 8)
                        live[px_] = px_ + (FLAG_BG,)
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
        if shape == "flag":
            out[-1]["live_map"] = live
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
    out = {"name": name, "layout": entry["id"], "rect": (x0, y0, x1 - x0, y1 - y0),
           "ground": spec["ground"], "owned": owned, "relief": comp, "pattern": pattern,
           "south": south, "north": north, "east": east, "west": west,
           "patch_own": spec.get("patch_own", False),
           **{k: spec[k] for k in ("ground_rgb", "ground_fixed") if k in spec}}
    if out.get("ground_fixed") is True and any(m >= vb.NUM_PRIMARY for (_, _, m) in pattern[:len(owned)]):
        # a piece with a secondary tileset's metatiles (`also`) stands on
        # that tileset's own ground, not on the one the spec fixes
        out["ground_fixed"] = False
    return out


def kept_by_others(layout_id):
    """The map's pixels (X, Y) a building of its own keeps (its cells, or
    with `keep` only those rectangles of them): a fence sharing a cell with
    a building's wall (the Battle Pyramid's, the Battle Dome's) leaves the
    wall to it."""
    if layout_id not in _KEPT:
        out = set()
        for s in specs.SPECS:
            if s.get("layout") != layout_id or "rect" not in s:
                continue
            x, y, w, h = s["rect"]
            owned = s.get("owned") or {(i, j) for i in range(w) for j in range(h)}
            keep = s.get("keep") or [(0, 0, w * 16, h * 16)]
            out |= {(x * 16 + u, y * 16 + v) for (a, b, c, d) in keep
                    for v in range(b, d) for u in range(a, c) if (u // 16, v // 16) in owned}
        _KEPT[layout_id] = out
    return _KEPT[layout_id]


_KEPT = {}


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


def close_gaps(art, drawn, gap):
    """Give back the object's pixels that the ground test took inside it: a
    shaded band of a hedge has texels of the lawn's own colours, and a
    column broken by them is read as two runs (a step down its side). A
    pixel goes back when its column is the object's within `gap` rows above
    and below it."""
    W, H = art.size
    px, dp = art.load(), drawn.load()
    holes = []
    for u in range(W):
        for v in range(H):
            if px[u, v][3] >= 128 or dp[u, v][3] < 128:
                continue
            up = any(px[u, v - k][3] >= 128 for k in range(1, gap + 1) if v - k >= 0)
            down = any(px[u, v + k][3] >= 128 for k in range(1, gap + 1) if v + k < H)
            if up and down:
                holes.append((u, v))
    for (u, v) in holes:
        px[u, v] = dp[u, v]


def trim_outline(art, colour, reach=2):
    """Take off what lies past an object's own dark outline: a hedge's shadow
    on the lawn, a pixel column against the house beside it and a row or two
    under its front, drawn in green the ground test does not know. Per row
    and per column, up to `reach` pixels beyond the outermost outline pixel,
    where the drawing ends there."""
    W, H = art.size
    px = art.load()
    out = set()

    def line(points):
        # each drawn run of the line on its own (a row crosses both arms)
        k = 0
        while k < len(points):
            if px[points[k]][3] < 128:
                k += 1
                continue
            run = []
            while k < len(points) and px[points[k]][3] >= 128:
                run.append(points[k])
                k += 1
            marks = [i for i, p in enumerate(run) if px[p][:3] == colour]
            if not marks:
                continue
            for tail in (run[marks[-1] + 1:], run[:marks[0]]):
                if 0 < len(tail) <= reach:
                    out.update(tail)

    for v in range(H):
        line([(u, v) for u in range(W)])
    for u in range(W):
        line([(u, v) for v in range(H)])
    for p in out:
        px[p] = (0, 0, 0, 0)


def join_corners(layout, spec, art):
    """An arm's inner corner drawn as the straight arm (`clean`), the run
    turning into it from the side (`joins`: the corner's metatile -> -1 for
    a run coming from the west, 1 from the east): that run goes on into the
    gap the straight piece leaves along the arm's side (no slit between
    them), and its top over the arm's outline - the straight piece has the
    outline down its whole side - for its plain rows, not the light edge
    above its front."""
    x, y, w, h = spec["rect"]
    relief = spec["relief"]
    owned = spec.get("owned") or {(i, j) for i in range(w) for j in range(h)}
    px = art.load()
    for (i, j) in owned:
        side = relief["joins"].get(layout.metatile(x + i, y + j))
        if side is None or not 0 <= i + side < w:
            continue
        # the run's column against the arm, in the cell beside it
        edge = (i + side) * 16 + (15 if side < 0 else 0)
        end = 0
        while end < 16 and px[edge, j * 16 + end][3] >= 128:
            end += 1
        for r in range(end):
            for k in range(1, 4):
                u, v = edge - side * k, j * 16 + r
                if px[u, v][3] < 128 or r < end - relief["height"] - 1:
                    px[u, v] = px[edge, v]


def fill_behind(layout, spec, art, drawn):
    """A run of an object (a hedge) that goes on behind a building: its
    columns reach the last row of a cell whose south neighbour is no part of
    it and no ground - a roof drawn over the rest - with fewer rows than a
    finished front. The run is drawn on under the roof as the object draws
    it in the open (`under`: the metatile under that one where nothing hides
    it), so it is read as one hedge round the house, its line and corners
    the drawn ones, its front against the house's back wall. The cell south
    stays the building's: only the model's drawing reaches into it."""
    x, y, w, h = spec["rect"]
    relief = spec["relief"]
    owned = spec.get("owned") or {(i, j) for i in range(w) for j in range(h)}
    tiles, under = relief["tiles"], relief.get("under", {})
    swap = relief.get("clean") or {}
    ground = layout.ground_tiles(spec["ground"])
    ground_px = layout.ground_pixels(spec["ground"])
    px = art.load()
    for (i, j) in sorted(owned):
        if (i, j + 1) in owned or j + 1 >= h or y + j + 1 >= layout.h:
            continue
        south = layout.metatile(x + i, y + j + 1)
        if south in tiles or south in set(spec["ground"]):
            continue
        m = layout.metatile(x + i, y + j)
        below = under.get(swap.get(m, m))
        if below is None:
            continue
        v = j * 16 + 15
        cut = False
        for u in range(i * 16, i * 16 + 16):
            n = 0
            while v - n >= 0 and px[u, v - n][3] >= 128:
                n += 1
            # an end the object finishes itself has its whole front drawn
            if 0 < n < relief["height"]:
                cut = True
        if not cut:
            continue
        for img, gpx in ((art, ground_px), (drawn, None)):
            cell = layout.building_art(x + i, y + j + 1, 1, 1, ground, ground_px=gpx,
                                       swap={south: below})
            img.paste(cell, (i * 16, (j + 1) * 16), cell)


def pick_side(art, height, longest=False):
    """An 8-column tile of the object's own drawn front, for the faces the
    drawing never shows: the longest stretch of columns whose runs end on the
    same row with a full front. With `longest`, the middle of the widest such
    stretch: a lowest one may be a narrow end, its 8 columns taking in its
    outline and the gap beside it (a bar and a slit on every side face)."""
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
    if longest:
        best = None
        u = 0
        while u < W:
            k = u
            while k < W and ends[u] is not None and ends[k] == ends[u]:
                k += 1
            if ends[u] is not None and (best is None or k - u > best[1] - best[0]):
                best = (u, k)
            u = max(k, u + 1)
        if best is not None and best[1] - best[0] >= 8:
            # clear of the stretch's end columns (its outline, the light
            # edge beside it): a hedge one cell wide has only a few between
            a, b = best[0] + 2, best[1] - 2
            width = min(8, b - a)
            u = (a + b - width) // 2
            e = ends[u]
            return vb.Tile(u, e - height, u + width, e)
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


def fill_hidden(pc, k, owner, fpx, walls):
    """What the pieces listed before piece k (`fill` given) hide of it, as
    {(x, y): colour}: `owner` the room's pixels' pieces, `walls` the pieces
    with a `fill`. A period (a wall's panels): the pixel that many columns
    along its row, from any wall's own. "column" (a top that runs back
    under what stands on it - a counter's arm under its Poke Ball): its
    own nearest pixel up or down its column, so that its rim stays a rim
    and its plain top plain, not a row's outline brought in from its
    side."""
    period = pc["fill"]
    H, W = len(owner), len(owner[0])
    fill = {}
    for y in range(H):
        for x in range(W):
            o = owner[y][x]
            if o is None or o >= k or not _inside(pc["shape"], x, y):
                continue
            if period == "column":
                src = next(((x, yy) for d in range(1, H) for yy in (y - d, y + d)
                            if 0 <= yy < H and owner[yy][x] == k), None)
                if src:
                    fill[(x, y)] = fpx[src]
                continue
            for d in range(period, W, period):
                # from any wall's own pixel, this piece's or the other
                # half's: a row's every panel may be hidden
                src = next(((xx, y) for xx in (x - d, x + d)
                            if 0 <= xx < W and owner[y][xx] in walls), None)
                if src:
                    fill[(x, y)] = fpx[src]
                    break
    return fill


def _around(px):
    """`px` and every pixel beside one of them."""
    return px | {(x + dx, y + dy) for (x, y) in px for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))}


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
                    "loose": loose and (len(pixels) >= LOOSE_PIXELS or piece_spec.get("found"))})


def place_reused(layout, ground, pit=()):
    """Stand the known furniture in `layout`; returns the pixels it draws and
    the cells it covers. `ground` None: a room nobody modelled, whose every
    other pixel the placements lay flat as ground patches. `pit`: the room's
    rectangles (x0, y0, x1, y1) that draw the floor below it (a balcony's
    well): what the room below has there is not stood at this floor's level,
    it stays drawn."""
    pixels, cells = set(), set()
    painted = {cell & 0x3FF for cell in layout.blocks}
    for p, x, y, pts, exact in reuse_pieces(layout):
        if any(x0 <= u < x1 and y0 <= v < y1 for (u, v) in pts for (x0, y0, x1, y1) in pit):
            print("  reuse: %s not stood in %s at %d,%d: in its pit, the floor below"
                  % (p["spec"]["name"], layout.id, x, y))
            continue
        if exact:
            # every cell of it drawn as in its own room: the floor under it is
            # its own room's, and it stands on that wherever this room paints
            # it too (the contest stage's cushions on the stages' green, in a
            # hall whose floor is a checker); where it does not, on this room's
            # floor with each of its cells' own paint laid over it, as in a room
            # nobody modelled (the Department Store's 4F shelves on 5F, their
            # tiles drawn on 4F's floor)
            own = p["spec"]["ground"][0]
            on = None if ground is None or own not in painted else own
            p["spec"].setdefault("reused_at", []).append((layout.id, x, y, on))
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
    _REUSED_PIXELS[layout.id] = _REUSED_PIXELS.get(layout.id, set()) | pixels
    return pixels, cells


# layout -> the room's pixels the pieces found again in it draw: none of them
# is laid flat round another as the map's own paint (ground_patch)
_REUSED_PIXELS = {}


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


# ── What the 45-degree proof cannot judge (voxel_defect_scan.py) ─────────
#
# A room's piece is proved at 45 degrees pixel for pixel, and its flanks,
# its back and the depth it is given behind its drawing are seen there by
# no pixel at all. What the scan needs to judge those - which pixels are the
# piece's, what it stands on, what its sides are dressed with - is known
# only here; it goes in the spec's "lint", never in the export.

SHELL_PIECES = ("wall", "side", "stairwell", "corner", "edge", "block", "partition", "divider")


def _hex_lum(c):
    return 0.299 * int(c[0:2], 16) + 0.587 * int(c[2:4], 16) + 0.114 * int(c[4:6], 16)


def _flood(seeds, allowed):
    """`seeds` and every pixel of `allowed` reached from them, 4-connected."""
    out = set(s for s in seeds if s in allowed)
    todo = list(out)
    while todo:
        x, y = todo.pop()
        for n in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if n in allowed and n not in out:
                out.add(n)
                todo.append(n)
    return out


def room_facts(spec, layout, pieces, drawn, owner, shade, fpx, legs_of=()):
    """Per piece of a room, what it shares with the others: the kind of
    piece, what it stands on, what it takes of it, what of the room's floor
    and walls it carries."""
    hexes = lambda x, y: "%02x%02x%02x" % fpx[x, y][:3]  # noqa: E731
    floorish = set(shade)
    for m in spec["interior"]["ground"]:
        floorish.update("%02x%02x%02x" % c[:3] for c in layout.cell_image(m).getdata())
    # the floor's dark grid lines are every piece's outline colour too
    floorish = {c for c in floorish if _hex_lum(c) >= 90}
    names = ["%s_%s" % (spec["name"], pc["name"]) for pc in pieces]

    def kind(pc, mine):
        if (pc.get("facet") or pc.get("walls") or not mine
                or pc["name"].split("_")[0] in SHELL_PIECES):
            return "shell"
        return "card" if pc.get("card") else "furniture"

    kinds = [kind(pc, drawn[k][0]) for k, pc in enumerate(pieces)]
    # the room round the pieces: its walls' pixels and every pixel no piece
    # takes (the floor and its marks), colour by colour
    backdrop = collections.Counter(hexes(x, y) for k in range(len(pieces))
                                   if kinds[k] == "shell" for (x, y) in drawn[k][0])
    backdrop.update(hexes(x, y) for y in range(len(owner)) for x in range(len(owner[0]))
                    if owner[y][x] is None)
    main = commonest(backdrop)
    out = [{"kind": kinds[k], "cut": {}} for k in range(len(pieces))]
    H = len(owner)
    for k, pc in enumerate(pieces):
        mine = drawn[k][0]
        f = out[k]
        if not mine or kinds[k] == "shell":
            continue
        border = {(x, y) for (x, y) in mine
                  if any(n not in mine for n in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)))}
        if pc.get("base"):
            # what it stands on: the piece drawn under its foot, whose top is
            # at the height it is lifted to
            fy = max(y for _, y in mine)
            votes = collections.Counter()
            for (x, y) in mine:
                if y == fy and fy + 1 < H:
                    # past what stands on the same top in front of it: a pot
                    # behind a row of plants on a stand, its foot hidden
                    yy = fy + 1
                    while (yy + 1 < H and owner[yy][x] is not None and owner[yy][x] >= 0
                           and owner[yy][x] != k
                           and pieces[owner[yy][x]].get("base", 0) == pc["base"]):
                        yy += 1
                    o = owner[yy][x]
                    top = pieces[o].get("base", 0) + pieces[o]["height"] if o is not None else 0
                    if o is not None and o >= 0 and o != k and abs(top - pc["base"]) <= 2:
                        votes[o] += 1
            if not votes:
                f["floating"] = True
                continue
            s = votes.most_common(1)[0][0]
            f["support"] = names[s]
            # its pixels in its support's colours, reached from its outline:
            # the desk's top taken for the book's front. Only the colours
            # that are more the support's than its own (a red dome on a
            # machine with a few red pixels round it keeps its red)
            theirs = collections.Counter(hexes(x, y) for (x, y) in drawn[s][0])
            ours = collections.Counter(hexes(x, y) for (x, y) in mine)
            palette = {c for c, n in theirs.items()
                       if c in commonest(theirs) and n > ours[c] and _hex_lum(c) >= 90}
            # but not its own outline's colours, dark beside the stuff it is
            # made of: a computer's grey line that its desk's edge shares
            stuff = max(sum(_hex_lum(hexes(x, y)) for (x, y) in mine) / len(mine),
                        _hex_lum(ours.most_common(1)[0][0]))
            same = {(x, y) for (x, y) in mine if hexes(x, y) in palette
                    and _hex_lum(hexes(x, y)) >= 0.6 * stuff}
            f["bleed"] = _flood(border, same)
            # and what it takes of its support, which has no pixel there: a
            # hole in its top, unless the support fills in what it hides
            if not pieces[s].get("fill"):
                taken = sum(1 for (x, y) in mine if _inside(pieces[s]["shape"], x, y)
                            and (x, y) not in drawn[s][1])
                if taken:
                    out[s]["cut"][names[k]] = taken
        elif kinds[k] == "furniture" and k not in legs_of:
            # the lowest rows of its front: floor, shadow or what it leaves
            # round it there is the space under a table stood up as a wall
            foot = (pc["foot"] if pc.get("foot") is not None else max(y for _, y in mine) + 1)
            low = [(x, y) for (x, y) in mine if foot - 4 <= y < foot]
            leave = set(pc.get("leave", ())) | floorish
            f["under"] = (sum(1 for (x, y) in low if hexes(x, y) in leave), len(low))
        # what it carries of the room round it - the wall seen behind a
        # computer's corners, the floor round a plant's pot - reached from
        # its edge: the colours that are more the room's walls and floor
        # than its own. At 45 degrees they lie on the wall they were drawn
        # on; anywhere else they stand on the piece (a computer's top laid
        # with the wall, a plant's card with a panel of it)
        if kinds[k] == "card":
            # a card's leaves have the wall between them: any of the colours
            # of the room's plaster and floor - not the ink every outline
            # shares, nor a picture's on the wall (the leaves' green is the
            # Center's map's)
            bg = {(x, y) for (x, y) in mine if hexes(x, y) in main | floorish
                  and _hex_lum(hexes(x, y)) >= 90}
            f["card_bg"] = (len(_flood(border, bg)), len(mine))
        else:
            # not the colours its body is made of, two pixels in from its
            # outline: a white cupboard is not the wall's white trim
            ours = collections.Counter(hexes(x, y) for (x, y) in mine)
            body = {hexes(x, y) for (x, y) in mine
                    if all((x + i, y + j) in mine for i in range(-2, 3) for j in range(-2, 3))}
            bg = {(x, y) for (x, y) in mine
                  if hexes(x, y) in main and backdrop[hexes(x, y)] > ours[hexes(x, y)]
                  and hexes(x, y) not in body and _hex_lum(hexes(x, y)) >= 90}
            # A piece seen through (`through`) has left the room inside it
            # already: what is left is its own (a tap's white spokes beside
            # the baseboard seen between them)
            f["backdrop"] = set() if pc.get("through") else _flood(border, bg)
    return out


def piece_facts(f, pc, mine, fill, origin, h, side_rect, side_foreign=None, sh=0):
    """A piece's own pixels in its art, and what of the stretches its hidden
    faces are dressed with is not its own (`side_foreign`: of the stretch
    laid under the drawing, side_image's; None: its side is read off its
    front)."""
    ox, oy = origin[0] * 16, origin[1] * 16
    own = frozenset((x - ox, y - oy) for (x, y) in mine)
    filled = frozenset((x - ox, y - oy) for (x, y) in fill)
    f = dict(f, own=own, fill=filled, height=pc["height"], base=pc.get("base", 0),
             side=side_rect, top=None, through=bool(pc.get("through")))
    for key in ("bleed", "backdrop"):
        if key in f:
            f[key] = frozenset((x - ox, y - oy) for (x, y) in f[key])
    lid = pc.get("top")
    v0 = 2 * h * 16
    if side_foreign is not None:
        f["side_foreign"] = frozenset((u, v + v0) for (u, v) in side_foreign)
    elif side_rect is not None:
        u0, t0, u1, t1 = side_rect
        f["side_foreign"] = frozenset((u, v) for v in range(t0, t1) for u in range(u0, u1)
                                      if (u, v) not in own and (u, v) not in filled)
    if lid:
        f["top"] = (0, v0 + sh, lid[2] - lid[0], v0 + sh + lid[3] - lid[1])
        f["top_foreign"] = frozenset(
            (x - lid[0], y - lid[1] + v0 + sh)
            for y in range(lid[1], lid[3]) for x in range(lid[0], lid[2]) if (x, y) not in mine)
    return f


def _is_shell(pc, mine):
    """A room's shell - its walls, a doorway's recess, a maze's blocks - not
    a thing standing in it."""
    return bool(pc.get("facet") or pc.get("walls") or not mine
                or pc["name"].split("_")[0] in SHELL_PIECES)


def commonest(counter, share=0.85):
    """The colours that make up `share` of a count: a room's plaster and
    floor, not the greys of an outline that happen to be drawn round it."""
    total, keep, acc = sum(counter.values()), set(), 0
    for c, n in counter.most_common():
        if acc >= share * total:
            break
        keep.add(c)
        acc += n
    return keep


def find_supports(pieces, grids, W, H):
    """What each lifted piece (`base`) stands on: the piece whose shape is
    drawn under its shape's foot and whose top is at the height it is lifted
    to (a book on a desk, a dome on its machine). {piece: support}."""
    out = {}
    for k, pc in enumerate(pieces):
        if not pc.get("base"):
            continue
        rows = [y for y in range(H) if any(grids[k][y])]
        if not rows or rows[-1] + 1 >= H:
            continue
        b = rows[-1]
        votes = collections.Counter()
        for x in range(W):
            if not grids[k][b][x]:
                continue
            for j, other in enumerate(pieces):
                if (j != k and grids[j][b + 1][x]
                        and abs(other.get("base", 0) + other["height"] - pc["base"]) <= 2):
                    votes[j] += 1
                    break
        if votes:
            out[k] = votes.most_common(1)[0][0]
    return out


def auto_leave(pieces, grids, ground, hexes, supports, W, H):
    """The colours each piece leaves where they run on into its shape from
    round it, beyond its own `leave`: the room round it - the colours that
    make the room's walls and floor and are more theirs than its own, not
    those its body is made of two pixels in (the wall round a computer's
    corners; a white cupboard keeps its white) - and, for a card, any of
    those, but for one lifted onto a stand. What it stands on is
    support_left's. Walls, filled pieces and corners leave nothing."""
    lum = lambda c: _hex_lum(c)  # noqa: E731
    shell = [k for k, pc in enumerate(pieces)
             if pc.get("facet") or pc.get("walls") or pc["name"].split("_")[0] in SHELL_PIECES]
    # the room round the pieces: everything outside the furniture's shapes
    # (the walls, the floor, its shades and marks)
    things = [k for k in range(len(pieces)) if k not in shell]
    roomy = [[not any(grids[k][y][x] for k in things) for x in range(W)] for y in range(H)]
    room = collections.Counter(hexes[y][x] for y in range(H) for x in range(W) if roomy[y][x])
    main = commonest(room)
    out = []
    for k, pc in enumerate(pieces):
        if k in shell or pc.get("fill") or pc.get("facet"):
            out.append(set())
            continue
        inside = {(x, y) for y in range(H) for x in range(W) if grids[k][y][x]}
        ours = collections.Counter(hexes[y][x] for (x, y) in inside)
        # the room but what is drawn in this piece's own shape (a plant
        # drawn over the wall it stands against)
        theirs = room - collections.Counter(hexes[y][x] for (x, y) in inside if roomy[y][x])
        if pc.get("card"):
            # a card's leaves have the wall between them, deep in its shape,
            # and every stuff of the room round it: its band, its shadows.
            # Lifted onto a stand, what is round it is the stand's top
            # (support_left), not the room's - whose counter may be drawn
            # in its pot's orange
            out.append(set() if k in supports else
                       {c for c in ours if theirs[c] > 2 * ours[c] and lum(c) >= 90})
            continue
        body = {hexes[y][x] for (x, y) in inside
                if all((x + i, y + j) in inside for i in range(-2, 3) for j in range(-2, 3))}
        leave = {c for c in ours if c in main and theirs[c] > ours[c] and c not in body
                 and lum(c) >= 90}
        out.append(leave)
    return out


def support_left(k, s, grids, hexes, W, H, sibs=()):
    """The pixels of a lifted piece's shape that are its support's top going
    on under it (the desk's squares in a book's rectangle), reached from the
    piece's outline: the colours that are more the support's than its own;
    or, where those would take half the piece (an open book as grey as its desk),
    only what has the support's colour two to eight pixels along the row
    outside the piece. Never half the piece. The support's top is what of
    its shape the things standing on it (`sibs`, besides this one) do not
    hold: pots in a row on a stand are not the stand's colours."""
    inside = {(x, y) for y in range(H) for x in range(W) if grids[k][y][x]}
    edge = {(x, y) for (x, y) in inside
            if any(n not in inside for n in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)))}
    ours = collections.Counter(hexes[y][x] for (x, y) in inside)
    top = [[grids[s][y][x] and not grids[k][y][x] and not any(grids[j][y][x] for j in sibs)
            for x in range(W)] for y in range(H)]
    theirs = collections.Counter(hexes[y][x] for y in range(H) for x in range(W) if top[y][x])
    main = commonest(theirs)
    alike = {(x, y) for (x, y) in inside if hexes[y][x] in main
             and theirs[hexes[y][x]] > ours[hexes[y][x]] and _hex_lum(hexes[y][x]) >= 90}
    left = _flood(edge & alike, alike)
    if 2 * len(left) < len(inside):
        return left
    goes_on = set()
    for (x, y) in inside:
        c = hexes[y][x]
        if _hex_lum(c) < 90:
            continue
        for d in (2, 4, 6, 8):
            if any(0 <= xx < W and top[y][xx] and hexes[y][xx] == c for xx in (x - d, x + d)):
                goes_on.add((x, y))
                break
    left = _flood(edge & goes_on, goes_on)
    return left if 2 * len(left) < len(inside) else set()


def outline_trim(cand, hexes, under=(), taken=()):
    """A piece's pixels as its own outline - the dark line the GBA draws
    round every thing - delimits them, out of those its shape takes:

    - anything but what it stands on past its outline both along its row
      and down its column (_past), below it or beside it, never above (a
      computer's top edges are drawn in grey), in a colour that what its
      outline closes in both ways has nowhere: a box's shadow down its side,
      grey as its top edge, which is the floor's - not a book's red where
      its outline is red too;
    - what it stands on (`under`: its support's own pixels in their
      colours; `taken`: what support_left found) past what is left of the
      piece - its stuff and outline - both ways: the desk's border down a
      computer's side, the grey of its shadow under the monitor, the desk
      round a book lying askew, whose outline is mostly its red.
    What the outline closes in on either way stays the piece's: a book's
    back edge, brown as the desk's front, between its corners; its page
    edge left open on one side. Returns the pixels kept."""
    out = set(cand)
    if not cand:
        return out
    # its outline: dark, or far darker than the piece (a white book's grey)
    floor = max(90, 0.6 * sum(_hex_lum(hexes[y][x]) for (x, y) in cand) / len(cand))
    dark = {p for p in cand if _hex_lum(hexes[p[1]][p[0]]) < floor}
    theirs = {p for p in cand - dark if p in taken or p in under}
    box = (min(x for x, _ in cand), max(x for x, _ in cand),
           min(y for _, y in cand), max(y for _, y in cand))
    rest = cand - dark - theirs
    loose = _past(rest, dark, box, above=False)
    inner = rest - _past(rest, dark, box, above=True, both=False)
    body = {hexes[y][x] for (x, y) in inner}
    out -= {(x, y) for (x, y) in loose if hexes[y][x] not in body}
    # what it stands on, past what is left of it - stuff and outline - both
    # ways (a book lying askew is less than half its rectangle); never all
    # but a quarter of it
    gone = _past(theirs, out - theirs, None, above=True)
    if 4 * (len(out) - len(gone)) >= len(cand):
        out -= gone
    return out


def _past(pts, by, box, above, both=True):
    """The pixels of `pts` past those of `by` both along their row and down
    their column (outline_trim): before the row's first of them or after its
    last - in a row they never cross, beyond their extent - and the same
    down the column, above only if `above` (`both` False: either way). With
    `box`, the piece's: nothing
    when `by` spans less than three quarters of it either way - no outline
    round it to tell by."""
    if not by:
        return set()
    X0, X1 = min(x for x, _ in by), max(x for x, _ in by)
    Y0, Y1 = min(y for _, y in by), max(y for _, y in by)
    if box is not None:
        bx0, bx1, by0, by1 = box
        if 4 * (X1 - X0 + 1) < 3 * (bx1 - bx0 + 1) or 4 * (Y1 - Y0 + 1) < 3 * (by1 - by0 + 1):
            return set()
    rows, cols = collections.defaultdict(list), collections.defaultdict(list)
    for (x, y) in by:
        rows[y].append(x)
        cols[x].append(y)

    def across(x, y):
        line = rows.get(y)
        return (x < min(line) or x > max(line)) if line else not Y0 <= y <= Y1

    def down(x, y):
        line = cols.get(x)
        if not line:
            return not X0 <= x <= X1
        return y > max(line) or (above and y < min(line))

    if not both:
        return {(x, y) for (x, y) in pts if across(x, y) or down(x, y)}
    return {(x, y) for (x, y) in pts if across(x, y) and down(x, y)}


def dome_fit(obj, height, base, seg=16, rings=4):
    """A round lid drawn as one (a machine's glass dome): the ellipsoid cap
    on its support's top at `base`, crowned `height` above it, whose
    45-degree view covers every pixel of the piece's own (`obj`, local) -
    as wide as they are, its base ring's front at their foot and its crown
    over their top row; grown until the polygons that stand for it
    (vb.ProjDome) cover them too. The drawing projected on it cuts the
    rest. Returns (cx, cz, rx, rz, h, y0)."""
    W, H = obj.size
    opx = obj.load()
    pts = [(u, v) for v in range(H) for u in range(W) if opx[u, v][3] >= 128]
    x0, x1 = min(u for u, _ in pts), max(u for u, _ in pts) + 1
    top, foot = min(v for _, v in pts), max(v for _, v in pts) + 1
    # the polygons fall inside the ellipsoid by their chords' sagitta
    k = math.cos(math.pi / seg) * math.cos(math.pi / (4 * rings))

    def covered(cx, cz, rx, rz, h, u, v):
        for i in range(41):
            y = base + h * i / 40.0
            z = v + y
            if ((u - cx) / (k * rx)) ** 2 + ((z - cz) / (k * rz)) ** 2 + \
                    ((y - base) / (k * h)) ** 2 <= 1.0:
                return True
        return False

    grow = 0.0
    while True:
        cx, rx = (x0 + x1) / 2.0, (x1 - x0) / 2.0 + grow
        d, h = foot - top + 2 * grow, float(height) + grow
        rz = (d * d - h * h) / (2 * d)
        cz = foot + grow + base - rz
        if all(covered(cx, cz, rx, rz, h, u + 0.5, v + 0.5) for (u, v) in pts):
            return cx, cz, rx, rz, h, float(base)
        grow += 0.25


class ViewHull:
    """The 45-degree view of a part's polygons, emitted into it as into a
    mesh: whether a point of the drawing is under one of them."""

    def __init__(self):
        self.tris = []

    def tri(self, a, b, c, *_):
        self.tris.append([(p[0], p[2] - p[1]) for p in (a, b, c)])

    def poly(self, pts, *_):
        for k in range(1, len(pts) - 1):
            self.tri(pts[0], pts[k], pts[k + 1])

    def covers(self, u, v):
        for (ax, ay), (bx, by), (cx, cy) in self.tris:
            d1 = (bx - ax) * (v - ay) - (by - ay) * (u - ax)
            d2 = (cx - bx) * (v - by) - (cy - by) * (u - bx)
            d3 = (ax - cx) * (v - cy) - (ay - cy) * (u - cx)
            if not ((d1 < -1e-9 or d2 < -1e-9 or d3 < -1e-9) and
                    (d1 > 1e-9 or d2 > 1e-9 or d3 > 1e-9)):
                return True
        return False


def ball_fit(obj, base, seg=16, rings=8):
    """A ball drawn as a circle (a Poke Ball on a counter): the ellipsoid
    sitting on `base` whose 45-degree view is that circle - as wide as the
    piece's own pixels (`obj`, local) and as tall. The view sets z - y,
    so the ball is as deep as it is tall and both are the circle's height
    over the square root of two: from the console's camera as round as
    the room's other circles. Grown until the polygons that stand for it
    (vb.ProjLathe: `rings` bands, `seg` points round) cover those pixels'
    centres. Returns (cx, cz, rx, rz, profile)."""
    W, H = obj.size
    opx = obj.load()
    pts = [(u + 0.5, v + 0.5) for v in range(H) for u in range(W) if opx[u, v][3] >= 128]
    x0, x1 = min(u for u, _ in pts) - 0.5, max(u for u, _ in pts) + 0.5
    top, foot = min(v for _, v in pts) - 0.5, max(v for _, v in pts) + 0.5
    grow = 0.0
    while True:
        cx, rx = (x0 + x1) / 2.0, (x1 - x0) / 2.0 + grow
        e = (foot - top) / 2.0 + grow
        ry = rz = e / math.sqrt(2.0)
        yc = base + ry
        cz = (top + foot) / 2.0 + yc
        # pole to pole, from where it sits to its crown
        profile = [(yc - ry * math.cos(math.pi * i / rings),
                    0.0 if i in (0, rings) else rx * math.sin(math.pi * i / rings))
                   for i in range(rings + 1)]
        hull = ViewHull()
        vb.ProjLathe("ball", cx, cz, profile, squash=rz / rx, seg=seg).emit(hull)
        if all(hull.covers(u, v) for (u, v) in pts):
            return cx, cz, rx, rz, profile
        grow += 0.125


def post_parts(obj, pw, ph):
    """`obj` (a row of little turned things, drawn side by side `pw` wide,
    each column of them one behind another `ph` down the drawing) cut into
    one image to each: a pixel is the one whose top it is under, the
    lowest one's down to the column's foot."""
    W, H = obj.size
    opx = obj.load()
    own = [(u, v) for v in range(H) for u in range(W) if opx[u, v][3] >= 128]
    count = {}
    for (u, _) in own:
        count[u // pw] = count.get(u // pw, 0) + 1
    # (a column of them by the rectangle's own grid; a stray pixel or two of
    # a neighbour's outline is the nearest one's)
    strips = [i for i, n in count.items() if 2 * n >= pw * ph] or list(count)
    near = {i: min(strips, key=lambda j: abs(j - i)) for i in count}
    out = {}
    for strip in sorted(strips):
        mine = [(u, v) for (u, v) in own if near[u // pw] == strip]
        top, foot = min(v for _, v in mine), max(v for _, v in mine) + 1
        n = max(1, (foot - top) // ph)
        step = {}
        for (u, v) in mine:
            step.setdefault(min((v - top) // ph, n - 1), []).append((u, v))
        # (one nearly all hidden by what stands before it is the next one's)
        kept = [i for i, px in step.items() if 4 * len(px) >= pw * ph] or list(step)
        for i, px in step.items():
            j = min(kept, key=lambda j: (abs(j - i), j))
            img = out.setdefault((strip, j), Image.new("RGBA", (W, H), (0, 0, 0, 0)))
            for (u, v) in px:
                img.putpixel((u, v), opx[u, v])
    return [out[k] for k in sorted(out)]


def lathe_fit(obj, height, base, squash=0.7, bands=4, seg=16):
    """A turned thing drawn standing (a jar, its mouth open on top): a
    surface of revolution on its support's top at `base`, `height` tall,
    its plan `squash` as deep as it is wide - a ball's, the market's urn's
    - its radius at `bands` + 1 rings evenly up it fitted so that its
    45-degree view covers every pixel of the piece's own (`obj`, local)
    and as few others as it can: the drawing's belly, shoulder and neck,
    where a ball or a dome under it overhangs its outline (holes, cut by
    the drawing, from the console's camera). Each ring is an ellipse,
    seen at 45 degrees as one `squash` as tall as it is wide, `y` higher
    up the drawing: a row is covered as far out as the widest of them
    across it. Searched down the axis's foot (its Z) a quarter pixel at
    a time, from a straight drum and from the drawing's own half-widths,
    each ring's radius stepped while that misses fewer pixels or covers
    fewer that are not the piece's; then grown until the polygons that
    stand for it (vb.ProjLathe, `seg` points round) cover those pixels'
    centres. Returns (cx, cz, profile)."""
    W, H = obj.size
    opx = obj.load()
    own = {(u, v) for v in range(H) for u in range(W) if opx[u, v][3] >= 128}
    x0, x1 = min(u for u, _ in own), max(u for u, _ in own) + 1
    top, foot = min(v for _, v in own), max(v for _, v in own) + 1
    cx, h = (x0 + x1) / 2.0, float(height)
    rows = range(top - 2, foot + 2)
    # each row's pixels by their distance from the axis: its own, and the rest
    need = {v: [abs(u + 0.5 - cx) for u in range(x0, x1) if (u, v) in own] for v in rows}
    spare = {v: [abs(u + 0.5 - cx) for u in range(x0 - 2, x1 + 2) if (u, v) not in own]
             for v in rows}
    n = int(4 * h) + 1
    ys = [h * k / (n - 1) for k in range(n)]

    def radius(radii, y):
        t = y / h * bands
        i = min(int(t), bands - 1)
        return radii[i] + (radii[i + 1] - radii[i]) * (t - i)

    # (sorted, to count a row's pixels either side of a width by bisection)
    need_sorted = {v: sorted(ds) for v, ds in need.items()}
    spare_sorted = {v: sorted(ds) for v, ds in spare.items()}
    costs = {}

    def cost(cz, radii):
        # the search comes back to the same radii often: each is costed once
        key = (cz, tuple(radii))
        if key in costs:
            return costs[key]
        rs = [(y, radius(radii, y)) for y in ys]
        c = 0
        for v in rows:
            dv = v + 0.5 + base - cz
            w = -1.0
            for y, r in rs:
                if r > 0:
                    t = (dv + y) / (squash * r)
                    if -1.0 <= t <= 1.0:
                        w = max(w, r * math.sqrt(1.0 - t * t))
            nd, sp = need_sorted[v], spare_sorted[v]
            c += 1000 * (len(nd) - bisect.bisect_right(nd, w)) + bisect.bisect_right(sp, w)
        costs[key] = c
        return c

    def drawn(cz):
        return [max(need.get(int(math.floor(cz - base - h * i / bands)), []) + [0.0]) + 0.5
                for i in range(bands + 1)]

    best = None
    k = 0
    while top + h + 0.25 * k <= foot:
        cz = top + h + base + 0.25 * k
        k += 1
        for radii in ([(x1 - x0) / 2.0] * (bands + 1), drawn(cz)):
            c0 = cost(cz, radii)
            better = True
            while better:
                better = False
                for i in range(bands + 1):
                    for d in (-0.25, 0.25, -1.0, 1.0):
                        rr = list(radii)
                        rr[i] = max(0.0, rr[i] + d)
                        c1 = cost(cz, rr)
                        if c1 < c0:
                            radii, c0, better = rr, c1, True
            if best is None or c0 < best[0]:
                best = (c0, cz, radii)
    _, cz, radii = best
    pts = [(u + 0.5, v + 0.5) for (u, v) in own]
    grow = 0.0
    while True:
        profile = [(base + h * i / bands, r + grow) for i, r in enumerate(radii)]
        hull = ViewHull()
        vb.ProjLathe("lathe", cx, cz, profile, squash=squash, seg=seg).emit(hull)
        if all(hull.covers(u, v) for (u, v) in pts):
            return cx, cz, profile
        grow += 0.125


def leg_split(mine, hexes, foot, height, release, explicit=None):
    """Where a piece of furniture stands on legs: the row its top ends at -
    the first of its front's rows that no longer runs the piece's whole
    width (a tablecloth's hem above the legs, a desk's top above its leg,
    its drawers and the shadow between) - when the rows up from its foot
    are three quarters floor or less across its width: its shadow, the rug
    (`release`), or nothing drawn. Returns (row, the pixels of the piece
    from there down that are floor, not leg), or None: no legs (a row with
    no gap, a top or legs under two rows). `explicit`: the top's front
    rows, given - and then legs a row tall will do (a glass table's feet
    under its edge)."""
    xs = [x for (x, y) in mine]
    x0, x1 = min(xs), max(xs)

    def gapish(x, y):
        return (x, y) not in mine or hexes[y][x] in release

    def gapped(v):
        # legs: what stands is three quarters of the piece's width or less
        # (a chair's seat on its one front leg, a table on two); a corner
        # cut off a few pixels is not legs
        solid = [x for x in range(x0, x1 + 1) if not gapish(x, v)]
        return bool(solid) and 4 * len(solid) <= 3 * (x1 - x0 + 1)

    if explicit:
        row = foot - height + int(explicit)
    else:
        row = None
        for v in range(foot - 1, foot - height, -1):
            if not gapped(v):
                break
            row = v
        if row is None or foot - row < 2:
            return None
        # the top: the front's rows that run its whole width; what hangs
        # below them across the legs (a desk's dark under its top) is
        # under the top, not the top
        hem = foot - height
        while hem < row and not any(gapish(x, hem) for x in range(x0, x1 + 1)):
            hem += 1
        row = hem
    if row is None or foot - row < (1 if explicit else 2) or row - (foot - height) < 1:
        return None
    return row, {(x, y) for (x, y) in mine if y >= row and hexes[y][x] in release}


def is_flat(obj, height):
    """A piece drawn no deeper than its front and the one row of outline
    over it: every column's run at most `height` + 1 rows - a chair's back
    seen from behind, a tap, a bed's head board: a plane, or a pixel deep."""
    W, H = obj.size
    px = obj.load()
    longest = 0
    for u in range(W):
        run = 0
        for v in range(H):
            run = run + 1 if px[u, v][3] >= 128 else 0
            longest = max(longest, run)
    return 0 < longest <= height + 1


def front_gaps(mine, foot, rows=4):
    """Whether the lowest rows of a piece's front have floor between its
    own pixels - legs, a stool's feet: its sides and back must keep those
    gaps (projected), not close them with a plain side."""
    for v in range(foot - rows, foot):
        xs = sorted(x for (x, y) in mine if y == v)
        if xs and xs[-1] - xs[0] + 1 > len(xs):
            return True
    return False


def _set_order(seq):
    """list(set(seq)) as 64-bit CPython orders it (gen_voxel_relief.set_order).
    plain_side's ties (Counter.most_common: the colour met first) and its
    float sums follow the order its pixels come in; a set of them iterates
    in hash() order, which is 32-bit on WebAssembly (the web builder's
    Pyodide), and there a room's wall took another side than the release's."""
    import gen_voxel_relief
    return gen_voxel_relief.set_order(seq)


def plain_side(mine, fpx, height, foot, width=16, ends=3):
    """A piece's side as its front shows its casing where the two meet, row
    by row: each of the front's `height` rows at its foot in the commonest
    colour of its `ends` pixels at either end that is not its outline - a
    bookcase's frame, not the books on its shelves; the row's own commonest
    stuff where its ends are all outline. An outline row inside it (a
    drawer's edge, a door's split) takes the stuff above it; its first and
    last rows keep their outline where the front has one at its ends. Laid
    along a side it reads as the piece's own panel, not as its front again.
    `mine` in a fixed order (_set_order), not a set: the order breaks ties."""
    rgb = lambda xy: fpx[xy][:3]  # noqa: E731
    lum = lambda c: 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2]  # noqa: E731
    # its outline: dark, or far darker than its stuff - than the mean of it
    # or its commonest colour, whichever is lighter (a drawer front of two
    # lines; a terminal's frame round its screen)
    mean = max(sum(lum(rgb(xy)) for xy in mine) / len(mine),
               lum(collections.Counter(rgb(xy) for xy in mine).most_common(1)[0][0]))
    dark = lambda c: lum(c) < max(90, 0.6 * mean)  # noqa: E731
    outline = [rgb(xy) for xy in mine if dark(rgb(xy))]
    rows, rims = [], []
    for i in range(height):
        v = foot - height + i
        row = sorted(x for (x, y) in mine if y == v)
        tips = [rgb((x, v)) for x in row[:ends] + row[-ends:]]
        light = ([c for c, _ in collections.Counter(c for c in tips if not dark(c)).most_common()]
                 or [c for c, _ in collections.Counter(rgb((x, v)) for x in row).most_common()
                     if not dark(c)])
        rows.append(light[0] if light else None)
        # at its ends the front's edge: outline there, top or foot
        rims.append(bool(row) and dark(rgb((row[0], v))) and dark(rgb((row[-1], v)))
                    and sum(dark(c) for c in tips) * 2 > len(tips))
    if not any(rows):
        rows = [collections.Counter(rgb(xy) for xy in mine).most_common(1)[0][0]] * height
    for i in range(height):         # an outline row takes the stuff above it,
        if rows[i] is None:         # or below it at the top
            rows[i] = next((rows[j] for j in range(i - 1, -1, -1) if rows[j]), None) or \
                next(rows[j] for j in range(i + 1, height) if rows[j])
    rim = (collections.Counter(outline).most_common(1)[0][0] if outline
           else min(rows, key=lum))
    if height >= 2 and rims[0]:
        rows[0] = rim
    if height >= 2 and rims[-1]:
        rows[-1] = rim
    img = Image.new("RGBA", (width, height))
    ipx = img.load()
    for i, c in enumerate(rows):
        for u in range(width):
            ipx[u, i] = c + (255,)
    return img


def side_image(pc, mine, filled, fpx, shell, foot):
    """The stretch a piece's hidden sides and back are dressed with, and the
    pixels of it that are not the piece's own; (None, None) to read it off
    its own front (pick_side). `side`:

        (x0, y0, x1, y1)  that stretch of the room (the wall a side wall is
                          dressed with), the piece's own pixels kept its own;
        [rect, ...]       stretches one under another, as wide as the first
                          (a television's casing over its stand's);
        "plain"           its front's stuff, row by row (plain_side);
        "cell"            the first whole cell of its own (a rock's grain,
                          where no stretch of it is plain);
        "front"           its own front's columns (pick_side), as it was.

    Furniture with no `side` is "plain"; a shell, or furniture with gaps
    in its front's foot (legs), reads its front."""
    side = pc.get("side")
    if side is None:
        side = ("front" if shell or pc.get("card")
                or (front_gaps(mine, foot) and not pc.get("base")) else "plain")
    if side == "front":
        return None, None
    if side == "plain":
        # its pixels as the room's one pass found them, row by row: the
        # order plain_side's ties go by (_set_order)
        return plain_side(_set_order(sorted(mine, key=lambda p: (p[1], p[0]))), fpx,
                          int(pc["height"]), foot), frozenset()
    if side == "cell":
        cells = sorted({(y // 16, x // 16) for (x, y) in mine})
        whole = [(cy, cx) for (cy, cx) in cells
                 if all((cx * 16 + i, cy * 16 + j) in mine for j in range(16) for i in range(16))]
        if not whole:
            return None, None
        cy, cx = whole[0]
        side = (cx * 16, cy * 16, cx * 16 + 16, cy * 16 + 16)
    rects = [side] if isinstance(side[0], (int, float)) else list(side)
    width = rects[0][2] - rects[0][0]
    img = Image.new("RGBA", (width, sum(r[3] - r[1] for r in rects)), (0, 0, 0, 0))
    foreign, top = set(), 0
    for r in rects:
        r = (r[0], r[1], r[0] + width, r[3])
        crop = filled.crop(r)
        cpx = crop.load()
        for y in range(r[1], r[3]):
            for x in range(r[0], r[2]):
                if (x, y) in mine:
                    cpx[x - r[0], y - r[1]] = fpx[x, y]
                else:
                    foreign.add((x - r[0], y - r[1] + top))
        img.paste(crop, (0, top))
        top += r[3] - r[1]
    return img, frozenset(foreign)


POST_WIDTH = 6      # a leg this wide or less is a post: as deep as it is wide
SLAB_ROWS = 4       # a top's edge this thick or less reads its front under its edge


def slab_side(obj, rows, depth, z_back, height, ends=2):
    """A table's top seen from the side, `depth` pixels long (from its back)
    and its front's `rows` tall: under each pixel of the top's edge there,
    what the front shows under a pixel of the same colour of the top's front
    edge (the wood at a tablecloth's corners, the cloth's hem where it hangs
    over in the middle); the front's plain stuff (plain_side) where the top
    has no such colour along its front, or no drawn row at all (its depth
    behind the drawing), and all along a body thicker than SLAB_ROWS. The
    front's `ends` columns at either end stand for no colour: a corner's
    highlight is not the edge's stuff."""
    W, H = obj.size
    px = obj.load()
    own = lambda u, v: 0 <= v < H and px[u, v][3] >= 128  # noqa: E731
    lum = lambda c: 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2]  # noqa: E731
    first, last = rows[0], rows[-1] + 1
    cols = [u for u in range(W) if all(own(u, v) for v in rows)]
    plain = plain_side(_set_order([(u, v) for u in cols for v in rows]), px, len(rows), last)
    img = Image.new("RGBA", (max(1, depth), len(rows)))
    ipx = img.load()
    if not cols or len(rows) > SLAB_ROWS:
        # a body on legs (a dresser's drawers and mirror), not a top's
        # edge: its casing, as any piece's
        for d in range(img.width):
            for i in range(len(rows)):
                ipx[d, i] = plain.getpixel((0, i))
        return img
    xa, xb = cols[0], cols[-1]
    under = collections.defaultdict(list)
    for u in cols[ends:len(cols) - ends]:
        if own(u, first - 1) and lum(px[u, first - 1][:3]) >= 90:
            under[px[u, first - 1][:3]].append(u)
    for d in range(depth):
        v = z_back + d - height          # the top's row over that pixel of the side
        edge = [u for u in range(W) if own(u, v)][:3] if 0 <= v < first else []
        colour = next((px[u, v][:3] for u in edge if lum(px[u, v][:3]) >= 90), None)
        match = under.get(colour)
        if match:
            rel = (d + 0.5) / depth
            u = min(match, key=lambda u: abs((u - xa + 0.5) / (xb - xa + 1) - rel))
            for i, vv in enumerate(rows):
                ipx[d, i] = px[u, vv]
        else:
            for i in range(len(rows)):
                ipx[d, i] = plain.getpixel((0, i))
    return img


def table_plan(obj, hem, foot, height, back):
    """A piece on legs, read off its drawing below its top's front (the
    hem): column by column what stands there down to its foot - a post (a
    leg no wider than POST_WIDTH) or a pedestal (a desk's drawers) - and
    what hangs under the top without reaching the floor (the dark under a
    desk's top between its leg and its drawers: an apron). With the sides
    each is dressed with, stacked in one image: the top's (slab_side), and
    a pedestal's or an apron's own stuff (plain_side); a post's side is its
    own front, as square as it is. Returns (runs, image, {key: rect})."""
    W, Hh = obj.size
    px = obj.load()
    own = lambda u, v: px[u, v][3] >= 128  # noqa: E731
    legs = foot - hem
    first = next(v for v in range(Hh) if any(own(u, v) for u in range(W)))
    z_back = back if back is not None else first + height
    depth = max(1, foot - z_back)
    below = {u: [v for v in range(hem, foot) if own(u, v)] for u in range(W)}
    runs = []
    for u in range(W):
        if not below[u]:
            continue
        kind = "leg" if max(below[u]) >= foot - 2 else "apron"
        if runs and runs[-1]["kind"] == kind and runs[-1]["u1"] == u:
            runs[-1]["u1"] = u + 1
        else:
            runs.append({"kind": kind, "u0": u, "u1": u + 1})
    for r in runs:
        cols = range(r["u0"], r["u1"])
        r["last"] = max(max(below[u]) for u in cols) + 1
        # a post's depth: its columns that stand most of the way down
        r["core"] = sum(1 for u in cols if 2 * len(below[u]) >= legs)
        if r["kind"] == "leg":
            r["kind"] = "post" if r["core"] <= POST_WIDTH else "pedestal"
    sides = [("slab", slab_side(obj, list(range(foot - height, hem)), depth, z_back, height))]
    for n, r in enumerate(runs):
        if r["kind"] != "post":
            pts = _set_order([(u, v) for u in range(r["u0"], r["u1"]) for v in range(hem, r["last"])
                              if own(u, v)])
            sides.append((n, plain_side(pts, px, r["last"] - hem, r["last"])))
    img = Image.new("RGBA", (max(im.width for _, im in sides), sum(im.height for _, im in sides)))
    rects, top = {}, 0
    for key, im in sides:
        img.paste(im, (0, top))
        rects[key] = (0, top, im.width, top + im.height)
        top += im.height
    return runs, img, rects


def table_parts(pc, obj, plan, hem, foot, back, top_tile, voff):
    """A piece on legs (table_plan) built: its top lifted onto its legs;
    posts as deep as they are wide, each with a copy at the top's back where
    the drawing never shows one and the top hides it at 45 degrees; a
    pedestal and an apron (lifted to where it hangs) as deep as the top.
    Its sides are table_plan's image, stored at `voff` of its art."""
    runs, _, rects = plan
    W, Hh = obj.size
    height = int(pc["height"])
    legs = foot - hem
    opx = obj.load()
    tile = lambda key: vb.Tile(rects[key][0], voff + rects[key][1],  # noqa: E731
                               rects[key][2], voff + rects[key][3])
    first = next(v for v in range(Hh) if any(opx[u, v][3] >= 128 for u in range(W)))
    rear = back if back is not None else first + height
    against = pc.get("against") is not None

    def cut(u0, u1, v0, v1):
        img = obj.copy()
        ipx = img.load()
        for v in range(Hh):
            for u in range(W):
                if not (u0 <= u < u1 and v0 <= v < v1):
                    ipx[u, v] = (0, 0, 0, 0)
        return img

    parts = [vb.Lifted(vb.Relief(pc["name"], cut(0, W, 0, hem), height - legs, tile("slab"),
                                 foot=hem, solid=True,
                                 back=None if back is None else back - legs,
                                 top_tile=top_tile, proj_back=True, against=against, skirt=0),
                       legs)]
    for n, r in enumerate(runs):
        u0, u1 = r["u0"], r["u1"]
        if r["kind"] == "apron":
            lift = foot - r["last"]
            parts.append(vb.Lifted(vb.Relief(pc["name"], cut(u0, u1, hem, r["last"]),
                                             r["last"] - hem, tile(n), foot=r["last"],
                                             solid=True, back=rear - lift, proj_back=True,
                                             skirt=0), lift))
        elif r["kind"] == "pedestal":
            parts.append(vb.Relief(pc["name"], cut(u0, u1, hem, foot), legs, tile(n), foot=foot,
                                   solid=True, back=rear, proj_back=True))
        else:
            # a post: its sides are its front, its foot's shape and all
            post = vb.Relief(pc["name"], cut(u0, u1, hem, foot), legs,
                             vb.Tile(u0, hem, u1, foot), foot=foot, solid=True,
                             back=foot - r["core"], proj_back=True)
            parts.append(post)
            # its twin at the back, where the top, the front and this post
            # hide it at 45 degrees: every pixel it would cover is drawn
            dz = rear - (foot - r["core"])
            if dz < 0 and all(opx[u, v][3] >= 128 for u in range(u0, u1)
                              for v in range(max(0, rear - legs), min(Hh, rear + r["core"]))
                              if any(opx[u, vv][3] >= 128 for vv in range(hem, foot))):
                parts.append(vb.Moved(post, dz=dz, tag="~depth"))
    return parts


def decal_cells(art, cells, origin, v_offset):
    """The cells of a piece whose floor copy (`v_offset` rows down its art)
    paints something, and the box of what it paints in each, in the cell's
    pixels (vb.Decal boxes)."""
    x0, y0 = origin
    out, boxes = [], {}
    for (cx, cy) in cells:
        i, j = cx - x0, cy - y0
        box = art.crop((i * 16, j * 16 + v_offset, i * 16 + 16, j * 16 + v_offset + 16)).getbbox()
        if box:
            out.append((i, j))
            boxes[(i, j)] = box
    return out, boxes


def floor_under(layout, pc, ground, places, w, h):
    """{(x, y): [(i, j, image)]}: under a piece put at `places` (cells of its
    rectangle's corner), the floor as drawn there where that is not
    `ground`'s - a cell's lower layer, if that is floor alone (a wall's
    shadow across it), not the post it draws itself."""
    floorish = set(pc.get("leave", ())) | {
        "%02x%02x%02x" % c[:3] for c in layout.cell_image(ground).getdata()}
    plain = layout.cell_image(ground, layers=(0,)).convert("RGBA").tobytes()
    under = {}
    for (ax, ay) in places:
        for j in range(h):
            for i in range(w):
                if not (0 <= ax + i < layout.w and 0 <= ay + j < layout.h):
                    continue
                mt = layout.metatile(ax + i, ay + j)
                lower = layout.cell_image(mt, layers=(0,)).convert("RGBA")
                colours = {"%02x%02x%02x" % c[:3] for c in lower.getdata()}
                if mt != ground and colours <= floorish and lower.tobytes() != plain:
                    under.setdefault((ax, ay), []).append((i, j, lower))
    return under


def drawn_as(layout_id, draw=None):
    """A layout's drawing, the cells `draw` names ({(x, y): metatile}) laid
    with those metatiles: the room as a script leaves it (setmetatile), the
    other state of a puzzle's barriers, for the pieces that stand only then."""
    layout = vb.LayoutArt(layout_id)
    if draw:
        layout.blocks = list(layout.blocks)
        for (x, y), mt in draw.items():
            i = y * layout.w + x
            layout.blocks[i] = (layout.blocks[i] & ~0x3FF) | mt
    return layout


def when_holds(model, layout, px, py):
    """Whether a room piece that stands only while a cell of its rectangle
    holds a metatile (`when`: x, y in it, metatile - voxel_building.c
    PlacementLive) stands at (px, py) in `layout` as drawn."""
    when = model.spec.get("when")
    return not when or layout.metatile(px + when[0], py + when[1]) == when[2]


def anim_box(layout, shape, tiles, frames):
    """The pixels of `shape` (a rectangle) drawn from the upper layer's
    `tiles` (lo, hi) in any of the animation's `frames`, and their box."""
    x0, y0, x1, y1 = shape
    lo, hi = tiles
    pts = set()
    for y in range(y0, y1):
        for x in range(x0, x1):
            tile, pixel, _ = layout.upper_source(x, y)
            if lo <= tile <= hi and any(layout.tile_pixel(tile, pixel, f) for f in [None] + frames):
                pts.add((x, y))
    if not pts:
        raise SystemExit("anim_box: no pixel of tiles %d-%d in %s" % (lo, hi, shape))
    return pts, (min(x for x, _ in pts), min(y for _, y in pts),
                 max(x for x, _ in pts) + 1, max(y for _, y in pts) + 1)


# the colour a live texel never draws: clear where its tile's pixel is 0
LIVE_BG = (1, 2, 3)


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
    layout = drawn_as(room["layout"], room.get("draw"))
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
    # a piece drawn from some tiles of the upper layer alone (`tiles`): a
    # beam over the floor and the posts it runs between, its pixels only
    for k, pc in enumerate(pieces):
        if pc.get("tiles"):
            lo, hi = pc["tiles"]
            g = grids[k]
            for y in range(H):
                for x in range(W):
                    if g[y][x]:
                        tile, pixel, _ = layout.upper_source(x, y)
                        g[y][x] = lo <= tile <= hi and layout.tile_pixel(tile, pixel) != 0
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
    # on from the open floor, never a piece's. A colour named alone is the
    # floor's shadow where its metatiles also carry a piece's edge (the lip
    # of a block's top, lying on the cell north of it)
    shade = set()
    for m in room.get("shade", ()):
        if isinstance(m, str):
            shade.add(m)
        else:
            shade.update("%02x%02x%02x" % c[:3] for c in layout.cell_image(m).getdata())
    owner = [[None] * W for _ in range(H)]
    variant_px = {}
    # the room's shell, where a piece's copies never take a pixel
    walls_at = [[any(grids[j][y][x] for j, p_ in enumerate(pieces)
                     if p_.get("fill") or p_["name"].split("_")[0] in SHELL_PIECES)
                 for x in range(W)] for y in range(H)]
    # (the floor the terrain lays under a piece's cells)
    floor_hexes = {"%02x%02x%02x" % c[:3] for c in layout.cell_image(room["ground"][0]).getdata()}

    def variant_groups(k, pc, x0, y0, w, h):
        """{what it draws round its shape, a pixel out, then the marks on
        the floor in its rectangle (None: bare floor): [the places it is
        drawn so]} - a `variants` piece's ways of being drawn, at its own
        place and its `at`. variant_px[k][4] lists those pixels."""
        _, _, around, floorish = variant_px[k][:4]
        rect = [(x0 * 16 + u, y0 * 16 + v) for v in range(h * 16) for u in range(w * 16)]
        if len(variant_px[k]) == 4:
            variant_px[k] += (sorted(around), rect)
        groups = {}
        for (ax, ay) in [(x0, y0)] + list(pc.get("at", ())):
            dx, dy = (ax - x0) * 16, (ay - y0) * 16
            key = []
            for (x, y) in sorted(around):
                X, Y = x + dx, y + dy
                own_px = (0 <= X < W and 0 <= Y < H and owner[Y][X] in (k, -1)
                          and not ground[Y][X] and hexes[Y][X] not in floorish)
                key.append(fpx[X, Y] if own_px else None)
            for (x, y) in rect:
                # (its shadow, drawn with it on the floor's metatile - on
                # another of the room's floors, too, in a colour not the
                # floor's the terrain lays under it)
                X, Y = x + dx, y + dy
                # (and where it is the piece's own, a shadow its outline
                # closes in: its floor's, as at every other place it stands)
                mark = (0 <= X < W and 0 <= Y < H
                        and (owner[Y][X] is None
                             or owner[Y][X] in (k, -1) and hexes[Y][X] in floorish)
                        and (not ground[Y][X] or hexes[Y][X] not in floor_hexes))
                key.append(fpx[X, Y] if mark else None)
            groups.setdefault(tuple(key), []).append((ax, ay))
        return groups

    def variant_art(k, art, key, x0, y0, h, shape_only=False):
        """`art` textured with one of variant_groups' ways of being drawn
        (`shape_only`: what stands, not its marks on the floor)."""
        _, _, _, _, around, rect = variant_px[k]
        vart = art.copy()
        vpx = vart.load()
        for v in range(2 * h * 16):
            for u in range(art.width):
                vpx[u, v] = (0, 0, 0, 0)
        n = len(around)
        for (x, y), c in zip(around, key[:n]):
            if c is not None:
                vpx[x - x0 * 16, y - y0 * 16] = c
        for (x, y), c in zip(rect, key[n:]):
            if c is not None and not shape_only:
                # (in its drawing too, as any piece's marks are)
                vpx[x - x0 * 16, y - y0 * 16 + h * 16] = c
                if vpx[x - x0 * 16, y - y0 * 16][3] < 128:
                    vpx[x - x0 * 16, y - y0 * 16] = c
        return vart
    # the furniture an earlier room has modelled, standing here as it is:
    # its pixels are no piece's of this room's
    reused, reused_cells = place_reused(layout, room["ground"][0], room.get("pit", ()))
    for (x, y) in reused:
        owner[y][x] = -1
    # each pixel's colour as `leave` names it
    hexes = [["%02x%02x%02x" % fpx[x, y][:3] for x in range(W)] for y in range(H)]
    supports = find_supports(pieces, grids, W, H)
    # how many things' shapes hold each pixel (a room's walls hold them all)
    things = [g for g, pc in zip(grids, pieces) if not (pc.get("fill") or pc.get("facet")
                                                        or pc.get("walls"))]
    shapes_at = [[sum(g[y][x] for g in things) for x in range(W)] for y in range(H)]
    leaves = auto_leave(pieces, grids, ground, hexes, supports, W, H)
    parts_of = {}

    def leave_parts(colours):
        """The room's runs of pixels in these colours (4-connected): the
        same for every piece that leaves the same colours."""
        if colours not in parts_of:
            seen, parts = set(), []
            for y in range(H):
                for x in range(W):
                    if (x, y) in seen or hexes[y][x] not in colours:
                        continue
                    seen.add((x, y))
                    part, todo = [], [(x, y)]
                    while todo:
                        px, py = todo.pop()
                        part.append((px, py))
                        for nx, ny in ((px + 1, py), (px - 1, py), (px, py + 1), (px, py - 1)):
                            if (0 <= nx < W and 0 <= ny < H and (nx, ny) not in seen
                                    and hexes[ny][nx] in colours):
                                seen.add((nx, ny))
                                todo.append((nx, ny))
                    parts.append(part)
            parts_of[colours] = parts
        return parts_of[colours]
    for k, pc in enumerate(pieces):
        # (a piece of some tiles alone: its shape cut to them, as above)
        inside = grids[k] if pc.get("tiles") else _inside_grid(pc["shape"], W, H)
        # what the piece leaves: its `leave` colours where they run on out of
        # its shape - the wall round a machine's corners, not the machine's
        # own orange lamps inside its outline - and, but for a wall, the
        # floor's shadows; and what it stands on and the room round it
        # (auto_leave)
        leave = set(pc.get("leave", ())) | (set() if pc.get("fill") or pc.get("facet") else shade)
        # what of its support support_left finds in its shape: the outline
        # has the last word on it (outline_trim)
        taken = (support_left(k, supports[k], grids, hexes, W, H,
                              [j for j, s_ in supports.items() if s_ == supports[k] and j != k])
                 if k in supports and not pc.get("fill") else set())
        left = set()
        if leaves[k]:
            # the room round it, from its own edge in: a plant's card
            # between a bookcase and a desk has the wall's band in it, which
            # meets no band outside
            room_px = {(x, y) for y in range(H) for x in range(W)
                       if inside[y][x] and hexes[y][x] in leaves[k]}
            edge = {(x, y) for (x, y) in room_px
                    if any(not (0 <= nx < W and 0 <= ny < H and inside[ny][nx])
                           for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)))}
            left |= _flood(edge, room_px)
        if pc.get("through"):
            # the room seen through it, wherever its colours are: the
            # baseboard between a tap's spokes, the floor in a bed's frame
            left |= {(x, y) for y in range(H) for x in range(W)
                     if inside[y][x] and hexes[y][x] in pc.get("leave", ())}
        if leave:
            # each run of those colours that reaches out of the shape
            for part in leave_parts(frozenset(leave)):
                if any(not inside[y][x] for (x, y) in part):
                    left.update((x, y) for (x, y) in part if inside[y][x])
        cand = set()
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
                    cand.add((x, y))
        if not (pc.get("fill") or pc.get("facet") or pc.get("walls") or pc.get("card")
                or pc["name"].split("_")[0] in SHELL_PIECES):
            # furniture is what its outline closes: what of its support is
            # past that, and a neighbour's outline down its shape, are not
            under = set()
            if k in supports:
                # its support's own pixels in its support's colours (those
                # where nothing else stands on it)
                s_ = supports[k]
                stuff_of = {hexes[y][x] for y in range(H) for x in range(W)
                            if grids[s_][y][x] and shapes_at[y][x] == 1}
                under = {(x, y) for (x, y) in cand
                         if grids[s_][y][x] and hexes[y][x] in stuff_of}
            cand = outline_trim(cand, hexes, under, taken)
        else:
            cand -= taken
        for (x, y) in cand:
            owner[y][x] = k
        if pc.get("at") and cand:
            # the same piece again (`at`): where the room draws its pixels at
            # each place it stands, they are that copy's, no other piece's.
            # With `variants`, whatever it draws there round its own shape
            # (a pixel out, for an outline it shares with a neighbour) but
            # the floor: each copy textured as drawn where it stands
            cx0, cy0 = min(x for x, _ in cand) // 16, min(y for _, y in cand) // 16
            around = ({(x, y) for (x, y) in _around(cand) if 0 <= x < W and 0 <= y < H and inside[y][x]}
                      if pc.get("variants") else cand)
            floorish = set(pc.get("leave", ())) | shade
            if pc.get("variants"):
                # (and so its own: its turned shape is fitted to all of it)
                for (x, y) in around:
                    if owner[y][x] is None and not ground[y][x] and hexes[y][x] not in floorish:
                        owner[y][x] = k
            for (ax, ay) in pc["at"]:
                dx, dy = (ax - cx0) * 16, (ay - cy0) * 16
                for (x, y) in around:
                    X, Y = x + dx, y + dy
                    if not (0 <= X < W and 0 <= Y < H) or owner[Y][X] is not None:
                        continue
                    if walls_at[Y][X]:
                        continue    # (a wall's, the copy only standing against it)
                    if (fpx[X, Y] == fpx[x, y] if not pc.get("variants")
                            else not ground[Y][X] and hexes[Y][X] not in floorish):
                        owner[Y][X] = -1
            if pc.get("variants"):
                variant_px[k] = (cx0, cy0, around, floorish)
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
    # furniture on legs: below the hem of its top, the floor between its legs
    # is the floor's (its shadow, the rug: what it leaves), laid flat, not a
    # wall of the piece. Not the floor's own colours: a wooden table's legs
    # on a wooden floor are its legs
    legs_of = {}
    for k, pc in enumerate(pieces):
        mine = owned_by[k]
        if (pc.get("legs") is False or pc.get("base") or pc.get("card") or pc.get("fill")
                or pc.get("run") or pc.get("plan") or pc.get("side") not in (None, "plain")
                or _is_shell(pc, mine)):
            continue
        foot_k = pc["foot"] if pc.get("foot") is not None else max(y for _, y in mine) + 1
        split = leg_split(mine, hexes, foot_k, int(pc["height"]),
                          set(pc.get("leave", ())) | shade, pc.get("legs"))
        if split:
            legs_of[k], released = split
            for (x, y) in released:
                owner[y][x] = None
                mine.discard((x, y))
    walls = {k for k, pc in enumerate(pieces) if pc.get("fill")}
    standing = collections.defaultdict(set)
    for k, sup in supports.items():
        standing[sup].add(k)
    drawn, skipped = [], set()
    for k, pc in enumerate(pieces):
        mine = owned_by[k]
        fill = {}
        period = pc.get("fill")
        if standing.get(k) and not period:
            # a support under what stands on it - a desk under a book, the
            # machine under its dome: what they hide of its top is its own
            # top a few columns away (the same squares of a pattern first),
            # not a hole that its depth behind repeats
            for (x, y) in ((x, y) for j in standing[k] for (x, y) in owned_by[j]
                           if grids[k][y][x]):
                src = next(((xx, y) for d in (4, 8, 12, 16, 2, 6, 10, 14, 18, 20, 24)
                            for xx in (x - d, x + d)
                            if 0 <= xx < W and owner[y][xx] == k), None)
                if src:
                    fill[(x, y)] = fpx[src]
        if period:
            fill.update(fill_hidden(pc, k, owner, fpx, walls))
        if not mine and not pc.get("walls"):
            # all of it drawn by a piece another room modelled, standing here
            # as it is (reuse_pieces): May's book is Brendan's, once both
            # leave their desks' colours
            if any(owner[y][x] == -1 for y in range(H) for x in range(W) if grids[k][y][x]):
                print("  %s: piece %s stands here as another room's" % (spec["name"], pc["name"]))
                skipped.add(k)
            else:
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
        if k in skipped:
            rects.append(None)
            continue
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
    under_marks = {}

    def mark_under(x, y):
        """The floor mark a piece hides at (x, y): the same mark where the
        row runs out of the piece both ways - a rug under a chair, a stripe
        running on under it. Anything else (the shade down a room's edge on
        one side) is left to the floor."""
        if (x, y) not in under_marks:
            under_marks[x, y] = _mark_under(x, y)
        return under_marks[x, y]

    def _mark_under(x, y):
        found = []
        for step in (-1, 1):
            xx = x
            while 0 <= xx < W and owner[y][xx] is not None:
                xx += step
            if not (0 <= xx < W) or ground[y][xx] or opened[y][xx]:
                return None
            found.append(fpx[xx, y])
        return found[0] if found[0] == found[1] else None

    # the cells a piece stands again in (`at`): its copies', where no other
    # piece lays the drawing (the post a side wall's rectangle reaches)
    copied = set()
    for k, rect in enumerate(rects):
        if rect is not None:
            for (ax, ay) in pieces[k].get("at", ()):
                copied |= {(ax + i, ay + j) for j in range(rect[3] - rect[1])
                           for i in range(rect[2] - rect[0])}
    decal_of = {}
    for k, rect in enumerate(rects):
        if rect is None:
            continue
        x0, y0, x1, y1 = rect
        for cy in range(y0, y1):
            for cx in range(x0, x1):
                if (cx, cy) not in pieces[k].get("skip", ()) and (cx, cy) not in copied:
                    decal_of.setdefault((cx, cy), k)
    for y in range(layout.h):
        for x in range(layout.w):
            if ((x, y) not in decal_of and (x, y) not in reused_cells
                    and (layout.blocks[y * layout.w + x] >> 10) & 3):
                print("  %s: blocked cell %d,%d is in no piece" % (spec["name"], x, y))
    facts = room_facts(spec, layout, pieces, drawn, owner, shade, fpx, legs_of)
    out = []
    for k, pc in enumerate(pieces):
        if k in skipped:
            continue
        mine, fill = drawn[k]
        x0, y0, x1, y1 = rects[k]
        w, h = x1 - x0, y1 - y0
        obj = Image.new("RGBA", (w * 16, h * 16), (0, 0, 0, 0))
        opx = obj.load()
        for (x, y) in mine:
            opx[x - x0 * 16, y - y0 * 16] = fpx[x, y]
        for (x, y), c in fill.items():
            opx[x - x0 * 16, y - y0 * 16] = c
        # the art: the piece and its floor marks; the marks alone below it;
        # below those, the stretch of the room its hidden faces are dressed
        # from, which need not be its own (a side wall's inside is the back
        # wall's orange, not the line its top is drawn with)
        shell = _is_shell(pc, mine)
        foot_row = (pc["foot"] if pc.get("foot") is not None
                    else max(y for _, y in mine) + 1 if mine else None)
        sample, sample_foreign = (side_image(pc, mine, filled, fpx, shell, foot_row)
                                  if foot_row is not None and not pc.get("run")
                                  else (None, None))
        plan = None
        if k in legs_of:
            # its top's side, its pedestal's and its apron's (table_plan)
            wall_z = pc.get("back") if pc.get("against") is None else pc["against"]
            plan = table_plan(obj, legs_of[k] - y0 * 16, foot_row - y0 * 16, int(pc["height"]),
                              None if wall_z is None else wall_z - y0 * 16)
            sample, sample_foreign = plan[1], frozenset()
        if sample is None and pc.get("side") not in (None, "front", "plain"):
            # a shell drawn nowhere (a doorway's sides): its stretch of the room
            sample, sample_foreign = side_image(pc, mine, filled, fpx, True, 0)
        sh = sample.height if sample else 0
        sw = sample.width if sample else 0
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
            art.paste(sample, (0, 2 * h * 16))
        if lid:
            # and the stretch its hidden top is laid with
            art.paste(filled.crop(lid), (0, 2 * h * 16 + sh))
        apx = art.load()
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
                        # (not on its foot's last row: lifted off the floor,
                        # the mark would stand before its foot at 45 degrees)
                        under = (mark_under(x, y) if y + 1 < H and owner[y + 1][x] is not None
                                 else None)
                        if under is not None:
                            apx[lx, ly + uo] = under
        # what the proofs judge it against: its drawing, floor marks and all
        drawing = art
        if k in legs_of:
            # under a table's top the floor is the floor's alone (its decal):
            # its legs' fronts and sides read their own pixels, a post's
            # narrow foot no square of floor beside it
            drawing = art.copy()
            for v in range(legs_of[k] - y0 * 16, foot_row - y0 * 16):
                for u in range(w * 16):
                    if opx[u, v][3] < 128:
                        apx[u, v] = (0, 0, 0, 0)
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
            span = None
            if pc.get("anim"):
                # what any frame of its animation draws: the card or the
                # ribbon spans all of it, its texels following the game's
                # tiles (live, ~live faces)
                _, box = anim_box(layout, (x0 * 16, y0 * 16, x1 * 16, y1 * 16), pc["tiles"],
                                  [vb.anim_frame(f, pc["anim"][1]) for f in pc["anim"][0]])
                span = (box[0] - x0 * 16, box[2] - x0 * 16, box[1] - y0 * 16)
                voff = 2 * h * 16 + sh + th
                live_map = {(u, voff + v): (u, v, LIVE_BG)
                            for v in range(box[1] - y0 * 16, box[3] - y0 * 16)
                            for u in range(span[0], span[1])
                            if pc["tiles"][0] <= layout.upper_source(x0 * 16 + u, y0 * 16 + v)[0]
                            <= pc["tiles"][1]}
            live_tag = "~live" if pc.get("anim") else ""

            def lay_live(art):
                """Its band of texels as its tiles draw them, clear where they
                draw nothing: the post's lower layer under the beam is the
                post's."""
                bpx = art.load()
                for (u, vv), (lu, lv, _) in live_map.items():
                    tile, pixel, pal = layout.upper_source(x0 * 16 + lu, y0 * 16 + lv)
                    index = layout.tile_pixel(tile, pixel)
                    bpx[u, vv] = layout.palette(pal)[index] + (255,) if index else (0, 0, 0, 0)
            if pc.get("flat"):
                # a block whose drawing is its plan (the dark beyond a room
                # and its rims): its top `height` up over the whole of it,
                # the drawing laid on it as drawn, not projected - its rim
                # at its north edge, where its wall ends. Its sides are its
                # walls; at 45 degrees it is not its drawing (the room's
                # `redrawn`)
                band = 2 * h * 16 + sh + th
                art.paste(obj, (0, band))
                ht = pc["height"]
                xs = [x - x0 * 16 for (x, _) in mine]
                zs = [y - y0 * 16 for (_, y) in mine]
                parts = [vb.Ribbon(pc["name"] + "_lid", min(xs), max(xs) + 1, min(zs), ht,
                                   max(zs) + 1, ht, voff=band + ht)]
            elif pc.get("ribbon"):
                # running back across the plan, from one post to the next
                art.paste(obj, (0, 2 * h * 16 + sh + th))
                lay_live(art)
                (za, ya), (zb, yb) = pc["ribbon"]
                parts = [vb.Ribbon(pc["name"] + live_tag, span[0], span[1], za - y0 * 16, ya,
                                   zb - y0 * 16, yb, voff=2 * h * 16 + sh + th)]
            elif pc.get("card"):
                art.paste(obj, (0, 2 * h * 16 + sh + th))
                if pc.get("anim"):
                    lay_live(art)
                parts = [vb.Card(pc["name"] + live_tag, obj, foot, voff=2 * h * 16 + sh + th,
                                 base=pc.get("base", 0), span=span)]
            elif pc.get("run"):
                # built from the side (an escalator): each section, (x, y) in
                # the room's pixels, runs back from z0 to z1 with its drawing
                # projected on every face - exact at 45 degrees wherever it
                # covers the drawing, its slopes and balustrades its own
                ox, oz = x0 * 16, y0 * 16
                parts = [vb.ProjRun("%s_%s" % (pc["name"], part), [(x - ox, y) for (x, y) in sec],
                                    za - oz, zb - oz)
                         for (part, sec, za, zb) in pc["run"]]
            elif pc.get("plan"):
                # modelled whole (a counter): an upright prism on its plan,
                # its faces flat whichever way they turn - a corner cut on
                # the slant one face, not a stair of columns - the drawing
                # projected on those the 45-degree view sees and on its top,
                # the rest dressed with its side stretch. Or blocks, one on
                # another (a terminal's head on its pedestal), each with
                # its own stretch of its front for its sides
                ox, oz = x0 * 16, y0 * 16
                if sample:
                    strip = (0, sw, 2 * h * 16 + sh)
                else:
                    strip = (side.rect[0], side.rect[2], side.rect[3])
                b0 = pc.get("base", 0)
                blocks = pc["plan"]
                if isinstance(blocks[0][0], (int, float)):
                    blocks = [(None, blocks, b0, b0 + pc["height"], None)]
                parts = [vb.PlanPrism(pc["name"] if part is None else "%s_%s" % (pc["name"], part),
                                      [(x - ox, z - oz) for (x, z) in poly], ya, yb,
                                      strip if own is None else
                                      (own[0] - ox, own[1] - ox, own[2] - oz))
                         for (part, poly, ya, yb, own) in blocks]
            elif pc.get("round") == "ball":
                # round all over (a Poke Ball): an ellipsoid on its support's
                # top, its drawing projected on it - exact at 45 degrees
                cx, cz, rx, rz, profile = ball_fit(obj, pc.get("base", 0))
                parts = [vb.ProjLathe(pc["name"], cx, cz, profile, squash=rz / rx, seg=16)]
            elif pc.get("round") == "lathe":
                # turned (a jar): its belly, shoulder and neck as drawn, its
                # drawing projected on it - exact at 45 degrees
                if k in variant_px:
                    # (its shape round every way it is drawn: where one
                    # of them has no pixel, its texels are clear)
                    obj = obj.copy()
                    for key in variant_groups(k, pc, x0, y0, w, h):
                        obj.alpha_composite(variant_art(k, art, key, x0, y0, h, True)
                                            .crop((0, 0, w * 16, h * 16)))
                if pc.get("posts"):
                    # a row of little ones (`posts`: each one's width and
                    # how far down the drawing the next stands), each
                    # turned on its own pixels, the last down to the foot
                    # (`turn`: (sides, bands) - a plain drum takes few:
                    # the console's chunks hold so many triangles)
                    seg, bands = pc.get("turn") or (12, 4)
                    parts = []
                    for n, post in enumerate(post_parts(obj, *pc["posts"])):
                        cx, cz, profile = lathe_fit(post, pc["height"], pc.get("base", 0),
                                                    bands=bands, seg=seg)
                        parts.append(vb.ProjLathe("%s_post%d" % (pc["name"], n), cx, cz, profile,
                                                  squash=0.7, seg=seg))
                else:
                    # (`squash`: its plan that deep for its width - a
                    # column round, one in a row touching the next)
                    sq = pc.get("squash") or 0.7
                    seg, bands = pc.get("turn") or (16, 4)
                    cx, cz, profile = lathe_fit(obj, pc["height"], pc.get("base", 0), squash=sq,
                                                bands=bands, seg=seg)
                    parts = [vb.ProjLathe(pc["name"], cx, cz, profile, squash=sq, seg=seg)]
            elif pc.get("round") == "dome":
                # round, not a drawing pushed back: a smooth cap with its
                # drawing projected on it, exact at 45 degrees
                cx, cz, rx, rz, crown, rim = dome_fit(obj, pc["height"], pc.get("base", 0))
                parts = [vb.ProjDome(pc["name"], cx, cz, rx, rz, crown, y0=rim, seg=16,
                                     rings=4)]
            else:
                top_tile = (vb.Tile(0, 2 * h * 16 + sh, tw, 2 * h * 16 + sh + th)
                            if lid else None)
                # furniture is a box with sides of its own, but where its
                # front's foot has gaps (legs): those stay gaps (projected)
                solid = pc.get("solid")
                if solid is None:
                    solid = not shell and (bool(pc.get("base")) or not front_gaps(mine, foot_row))
                if (back is None and not shell and k not in legs_of
                        and is_flat(obj, int(pc["height"]))):
                    # drawn no deeper than its front (a chair's back, a tap)
                    # or a row more: two pixels of depth behind it, not a plane
                    back = foot - 2
                if k in legs_of:
                    parts = table_parts(pc, obj, plan, legs_of[k] - y0 * 16, foot, back,
                                        top_tile, 2 * h * 16)
                else:
                    relief = vb.Relief(pc["name"], obj, pc["height"], side, foot=foot,
                                       solid=solid, back=back, top_tile=top_tile,
                                       proj_back=not shell,
                                       against=pc.get("against") is not None,
                                       see_through=bool(pc.get("through")))
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
        # a top drawn deeper than the room in front of the wall it stands
        # against lies flat to the wall's face (vb.Relief `against`): the
        # rows the drawing puts behind that face are the wall's to hide, at
        # 45 degrees too. Its own pixels there are the one thing the piece
        # does not draw; the proofs skip them, and say how many
        behind_wall = hidden_fill = frozenset()
        if pc.get("against") is not None and mine and back is not None:
            cut = back - int(pc["height"])
            behind_wall = frozenset((x - x0 * 16, y - y0 * 16) for (x, y) in mine
                                    if y - y0 * 16 < cut)
            # and what it fills in there under the books on it: theirs to
            # draw in the room, the wall's in its own proof
            hidden_fill = frozenset((x - x0 * 16, y - y0 * 16) for (x, y) in fill
                                    if y - y0 * 16 < cut)
        # only the cells with a mark, each quad only over its marks: the
        # export keeps the texels a face reads, and clear ones read for
        # nothing take room on the page
        if k in variant_px:
            # (the marks of every way it is drawn, each its own on its model)
            cells = [(x0 + i, y0 + j) for j in range(h) for i in range(w)]
            marks = Image.new("RGBA", (art.width, h * 16), (0, 0, 0, 0))
            for key in variant_groups(k, pc, x0, y0, w, h):
                marks.alpha_composite(variant_art(k, art, key, x0, y0, h)
                                      .crop((0, h * 16, art.width, 2 * h * 16)))
            art.paste(marks, (0, h * 16))
        local, local_box = decal_cells(art, cells, (x0, y0), h * 16)
        if local:
            parts.append(vb.Decal(pc["name"] + "_floor", local, h * 16, boxes=local_box))
        hidden, hidden_box = decal_cells(art, cells, (x0, y0), uo)
        if hidden:
            # "~behind": at 45 degrees the piece stands over it; the room's
            # check, with every piece in place, judges it
            parts.append(vb.Decal(pc["name"] + "_under~behind", hidden, uo, lift=0.25,
                                  boxes=hidden_box))
        # `skip`: cells of its rectangle that stay the map's own, drawn as
        # the game has them (a switch a script presses beside a wall)
        owned = ({(i, j) for j in range(h) for i in range(w)
                  if (x0 + i, y0 + j) not in pc["skip"]} if pc.get("skip") else None)
        # `when`: it stands only while that cell holds that metatile; `at`:
        # it stands again with its rectangle at each of those cells. Either
        # stands only where it is put
        extra = {}
        # (and a piece of a room's other drawing stands only in that room, as
        # does one that leaves all its cells to the map: no cell to find it by)
        if pc.get("when") or pc.get("at") or room.get("draw") or owned == set():
            g0 = room["ground"][0]
            extra["placed"] = [(room["layout"], ax, ay, g0)
                               for (ax, ay) in [(x0, y0)] + list(pc.get("at", ()))]
            under = floor_under(layout, pc, g0, [(ax, ay) for (_, ax, ay, _) in extra["placed"]],
                                w, h)
            if under:
                extra["placed_patches"] = under
        if pc.get("when"):
            # its cell in its rectangle: it moves with each place it stands
            extra["when"] = (pc["when"][0] - x0, pc["when"][1] - y0, pc["when"][2])
        if pc.get("anim"):
            extra["live_map"] = live_map
        if pc.get("found"):
            extra["found"] = True
        if room.get("draw"):
            extra["layout_art"] = layout
        out.append({"name": "%s_%s" % (spec["name"], pc["name"]), "layout": room["layout"],
                    "rect": (x0, y0, w, h), "ground": room["ground"], "art": art,
                    **({"owned": owned} if owned is not None else {}), **extra,
                    "parts": (lambda ps: (lambda: ps))(parts),
                    "exact": [] if pc.get("flat") else [(0, 0, w * 16, h * 16)],
                    "interior": spec["name"],
                    "behind_wall": behind_wall,
                    "drawing": (_without(drawing, behind_wall | hidden_fill)
                                if behind_wall or drawing is not art else None),
                    "lint": piece_facts(facts[k], pc, mine, fill, (x0, y0), h,
                                        (_shifted(plan[2]["slab"], 2 * h * 16) if plan
                                         else side.rect)
                                        if (mine or pc.get("walls")) else None,
                                        sample_foreign, sh)
                    | ({"legs_side": (0, 2 * h * 16 + plan[2]["slab"][3], sw, 2 * h * 16 + sh)}
                       if plan and sh > plan[2]["slab"][3] else {})
                    | ({"slab_edge": plan[2]["slab"][3] <= SLAB_ROWS,
                        "posts": [(r["u0"], legs_of[k] - y0 * 16, r["u1"], foot_row - y0 * 16)
                                  for r in plan[0] if r["kind"] == "post"]}
                       if plan else {})})
        if k in variant_px:
            # `variants`: the same shape at every place it stands, each
            # textured as the room draws it there - a column with another
            # before it, its body a lighter green; one a neighbour's outline
            # joins - one model to each way it is drawn
            g0 = room["ground"][0]
            groups = variant_groups(k, pc, x0, y0, w, h)
            base = out.pop()
            for n, (key, places) in enumerate(sorted(groups.items(), key=lambda kv: -len(kv[1]))):
                vart = variant_art(k, art, key, x0, y0, h)
                model = dict(base, art=vart, drawing=None,
                             placed=[(room["layout"], ax, ay, g0) for (ax, ay) in places])
                if n:
                    model["name"] = "%s_v%d" % (base["name"], n)
                model.pop("placed_patches", None)
                under = floor_under(layout, pc, g0, places, w, h)
                if under:
                    model["placed_patches"] = under
                out.append(model)
        # `alone`: a room's own shell - its walls, a doorway's recess - which
        # another room does not repeat by having the same floor along an edge
        if not (pc.get("alone") or extra.get("placed")):
            register_piece(out[-1], layout, drawing,
                           {(x - x0 * 16, y - y0 * 16) for (x, y) in fill},
                           loose=not (pc.get("fill") or pc.get("facet") or pc.get("walls")),
                           own={(x - x0 * 16, y - y0 * 16) for (x, y) in mine})
    return out


def _shifted(rect, dv):
    return (rect[0], rect[1] + dv, rect[2], rect[3] + dv)


def _without(art, pixels):
    """`art` with `pixels` clear: the drawing a piece is judged against
    where the wall it stands against hides some of it."""
    out = art.copy()
    opx = out.load()
    for (u, v) in pixels:
        opx[u, v] = (0, 0, 0, 0)
    return out


def live_texels(spec, layout, tex, live):
    """A model's texels that follow a tileset animation (a fountain's jet).

    `live` maps a texel of its texture to the drawing's pixel it shows,
    (x, y, bg): that pixel's colour as the game draws it at the moment,
    clear where it is `bg` (what lies behind it is drawn elsewhere). The
    console recomposes them from the live tiles (voxel_building.c
    VoxelBuildings_ComposeAnimRun); here each is resolved to its upper-layer
    source - (u, v, tile, pixel, palette's sixteen colours, bg) - and the
    texture is proven to be what that composes to with the tilesets'
    own tiles."""
    ox, oy = spec["rect"][0] * 16, spec["rect"][1] * 16
    tpx = tex.load()
    out = []
    for (u, v), (x, y, bg) in sorted(live.items()):
        tile, pixel, pal = layout.upper_source(ox + x, oy + y)
        colours = layout.palette(pal)
        index = layout.tile_pixel(tile, pixel)
        want = (0, 0, 0, 0) if index == 0 or colours[index] == bg else colours[index] + (255,)
        got = tpx[u, v] if tpx[u, v][3] >= 128 else (0, 0, 0, 0)
        if got != want:
            raise SystemExit("%s: live texel %d,%d is %s, its tiles compose %s"
                             % (spec["name"], u, v, got, want))
        out.append((u, v, tile, pixel, tuple(colours), bg))
    return out


def live_after(model, art, tris):
    """Where compaction moved a model's live texels: by the whole texels the
    faces reading them (tagged ~live) moved. `art`, `tris`: before it."""
    W, H = art.size
    where = {}
    for (tri, _, tag), (moved, _, _) in zip(tris, model.mesh.tris):
        if "~live" not in tag:
            continue
        du = int(round(moved[0][3] - tri[0][3]))
        dv = int(round(moved[0][4] - tri[0][4]))
        for (u, v) in sampled_texels([(p[3], p[4]) for p in tri], W, H):
            where[(u, v)] = (u + du, v + dv)
    out = []
    for (u, v, *rest) in model.live:
        if (u, v) not in where:
            raise SystemExit("%s: live texel %d,%d is read by no ~live face" % (model.name, u, v))
        out.append(where[(u, v)] + tuple(rest))
    model.live = out


def glass_after(model, art, tris, mask):
    """Where compaction moved a model's window glass: by the whole texels
    the faces reading it moved, as live_after. `art`, `tris`, `mask` (an L
    image over `art`): before it. None when two faces would read one texel
    with different glass."""
    W, H = art.size
    out = Image.new("L", model.art.size)
    NW, NH = out.size
    opx, gpx = out.load(), mask.load()
    wrote = {}
    for (tri, _, _), (moved, _, _) in zip(tris, model.mesh.tris):
        du = int(round(moved[0][3] - tri[0][3]))
        dv = int(round(moved[0][4] - tri[0][4]))
        for (u, v) in sampled_texels([(p[3], p[4]) for p in tri], W, H):
            x, y = u + du, v + dv
            if 0 <= x < NW and 0 <= y < NH:
                if wrote.setdefault((x, y), gpx[u, v]) != gpx[u, v]:
                    return None
                opx[x, y] = gpx[u, v]
    return out


def rgba5551(rgb):
    return (rgb[0] >> 3) << 11 | (rgb[1] >> 3) << 6 | (rgb[2] >> 3) << 1 | 1


def anim_runs(page, layout, tw, tex, live):
    """A page's live texels as the console redraws them: runs of whole 8x8
    blocks along a row of blocks (contiguous in the PICA's order), each
    with its blocks' texels as written and the live ones' sources. `live`:
    (x, y, tile, pixel, colours, bg) in page texels."""
    blocks = {}
    for t in live:
        blocks.setdefault((t[1] // 8, t[0] // 8), []).append(t)
    runs = []
    for (by, bx) in sorted(blocks):
        if runs and runs[-1]["row"] == by and runs[-1]["x1"] == bx:
            runs[-1]["x1"] = bx + 1
            runs[-1]["live"] += blocks[(by, bx)]
        else:
            runs.append({"row": by, "x0": bx, "x1": bx + 1, "live": list(blocks[(by, bx)])})
    out = []
    for r in runs:
        first = (r["row"] * (tw // 8) + r["x0"]) * 64
        n = (r["x1"] - r["x0"]) * 64
        texels = struct.unpack_from("<%dH" % n, tex, 2 * first)
        lives = sorted((texel_offset(x, y, tw) - first, tile, pixel, colours, bg)
                       for (x, y, tile, pixel, colours, bg) in r["live"])
        out.append((page, layout, 2 * first, r["x1"] - r["x0"], texels, lives))
    return out


def anim_table(anims, offset):
    """The pages' live texels (anim_runs), appended to buildings.bin with
    `offset` its own place in the file, then that u32 and "ANIM" (a reader
    that knows nothing of it never reads it):

      u16 runs, palettes; u32 lives, statics
      runs     x 20: u16 page, layout; u32 its first byte in the page;
                     u16 8x8 blocks, lives; u32 first live, first static
      palettes x 32: sixteen RGBA5551 colours
      lives    x 8:  u16 texel (in the run's PICA order), tile (BG VRAM,
                     0-1023); u8 pixel (row * 8 + column), palette;
                     u16 bg (RGBA5551: that colour is drawn clear)
      statics  x 2:  the runs' texels as the page holds them, ungraded
    """
    palettes, pal_of = [], {}
    runs, lives, statics = bytearray(), bytearray(), bytearray()
    nlive = nstatic = 0
    for (page, layout, first, blocks, texels, live) in anims:
        runs += struct.pack("<HHIHHII", page, layout, first, blocks, len(live), nlive, nstatic)
        for (texel, tile, pixel, colours, bg) in live:
            key = tuple(rgba5551(c) for c in colours)
            if key not in pal_of:
                pal_of[key] = len(palettes)
                palettes.append(key)
            lives += struct.pack("<HHBBH", texel, tile, pixel, pal_of[key], rgba5551(bg))
        statics += struct.pack("<%dH" % len(texels), *texels)
        nlive += len(live)
        nstatic += len(texels)
    if len(palettes) > 256:
        raise SystemExit("%d palettes of live texels: a byte names 256" % len(palettes))
    table = struct.pack("<HHII", len(anims), len(palettes), nlive, nstatic) + runs
    for pal in palettes:
        table += struct.pack("<16H", *pal)
    table += lives + statics
    if anims:
        print("voxel buildings: %d animated runs, %d live texels" % (len(anims), nlive))
    return table + struct.pack("<I", offset) + b"ANIM"


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
        elif "bridge" in spec:
            expanded += voxel_bridge.block_specs(spec)
        else:
            expanded.append(spec)
    reuse_everywhere()
    for spec in expanded:
        if only and not spec["name"].startswith(only):
            continue
        lid = spec["layout"]
        if lid not in layouts:
            layouts[lid] = vb.LayoutArt(lid)
        # a room piece of a puzzle's other state is read off that drawing
        layout = spec.get("layout_art") or layouts[lid]
        x, y, w, h = spec["rect"]
        owned = spec.get("owned")
        live = None
        if "art" in spec:
            art = spec["art"]  # composed by its expander (a room's piece)
        else:
            art = layout.building_art(x, y, w, h, layout.ground_tiles(spec["ground"]), cells=owned,
                                      ground_px=layout.ground_pixels(spec["ground"])
                                      if owned or spec.get("ground_px") else None,
                                      upper=spec.get("relief", {}).get("upper", False),
                                      swap=spec.get("relief", {}).get("clean"))
            relief = spec.get("relief", {})
            if relief.get("close") or relief.get("under"):
                drawn = layout.building_art(x, y, w, h, layout.ground_tiles(spec["ground"]),
                                            cells=owned, swap=relief.get("clean"))
                if relief.get("under"):
                    fill_behind(layout, spec, art, drawn)
                if relief.get("joins"):
                    join_corners(layout, spec, art)
                if relief.get("close"):
                    close_gaps(art, drawn, relief["close"])
            if relief.get("outline"):
                trim_outline(art, relief["outline"])
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
        if "relief" in spec and spec["relief"].get("posts"):
            # a fence of posts or logs: each one stands where its top is
            # drawn (voxel_fence.py), judged against the block's own drawing
            art, parts, spec["drawing"] = voxel_fence.build(layout, spec, spec["relief"]["posts"],
                                                            kept_by_others(lid))
            spec["exact"] = [(0, 0, w * 16, h * 16)]
        elif "relief" in spec:
            height = spec["relief"]["height"]
            relief = spec["relief"]
            side = pick_side(art, height, longest=relief.get("side_longest", False))
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
                # with a texture of its own, and maybe texels that follow
                # the game's tile animation
                parts, art, *live = parts
                live = live[0] if live else None
        else:
            parts = spec["parts"]()
        model = vb.Model(spec["name"], art, parts, (w, h), spec["ground"][0])
        if spec.get("clip_art"):
            model.spec = spec
            clip_to_art(model)
        if spec.get("live_map"):
            live = spec["live_map"]
        model.live = live_texels(spec, layout, art, live) if live else []
        model.owned = owned if owned is not None else {(i, j) for i in range(w) for j in range(h)}
        model.spec = spec
        model.layout = layout
        if spec.get("relief_bare"):
            # the cells of its rectangle it stands over nothing of - a house
            # drawn against the terrace behind it, its roof's rows over the
            # face - are not its own: the relief goes on there (claimed, the
            # face was laid flat at the cell's middle, a notch in the cliff
            # either side of the house)
            tops = cell_heights(model)
            model.bare = {(i, j) for (i, j) in model.owned if tops[j * w + i] <= 0}
            # still its own, so that the console draws the relief there
            # without the house: the cell's drawing less the quarters of its
            # upper layer the house is drawn on (bare_quarters), the rock
            # face of its lower layer going on behind it
            spec["quads"] = {**spec.get("quads", {}), **bare_quarters(model)}
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

MAGIC = b"VXB9"
# the texture pages the console has room for (ctr_voxel.c, VOXEL_MAX_PAGES)
CONSOLE_PAGES = 1024
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
    if "tops" in model.spec:
        return model.spec["tops"]      # worked out in the world (voxel_bridge.py)
    tops = getattr(model, "_cell_heights", None)
    if tops is None:
        tops = model._cell_heights = _cell_heights(model)
    return tops


def bare_quarters(model):
    """{(i, j): quarters} of a `relief_bare` model's bare cells: the quarters
    of the cell's upper layer that draw something (bit 2 * row + column).
    The console lays the relief there drawn with the metatile less those
    (VoxelBuildings_BareVariant): a Sootopolis house's roof or spire is drawn
    on the upper layer over the face of the terrace behind it, and painted
    on that face it stood there a second time, flat."""
    pair = vc.pair_for(model.layout.primary, model.layout.secondary)
    x, y = model.spec["rect"][:2]
    out = {}
    for (i, j) in model.bare:
        q = 0
        for (px, py) in pair.layer_pixels(model.layout.metatile(x + i, y + j), 1):
            q |= 1 << ((py // 8) * 2 + px // 8)
        if q:
            out[(i, j)] = q
    return out


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
    if "footprints" in model.spec:
        return list(model.spec["footprints"])
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
    bare = getattr(model, "bare", set())     # the relief's, drawn less the house
    return {(i, j) for (i, j) in odd if (i, j) not in chosen and (i, j) not in bare
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
            if patch is not None and not m.spec.get("patches"):
                # the patch where no model draws over it: the map's paint
                # (not one the spec lays itself: its drawing, by design)
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
    if "placed" in model.spec:
        # a block of a bridge (voxel_bridge.py): where it was read, and every
        # block of the road built the same, each on the ground under it
        index_of = {e.get("id"): k for k, e in enumerate(layouts_json)}
        return [(index_of[lid] + 1, px, py, ground, lid, [])
                for (lid, px, py, ground) in model.spec["placed"]]
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
                    odd = sorted(set(odd) | (set(model.spec["patch_cells"])
                                             & (model.owned - getattr(model, "bare", set()))))
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
_SIGN_METATILES = {}    # layout id -> metatiles a signpost is drawn on (_sign_metatiles)
_MAP_EVENTS = None


def _sign_metatiles(layout_id):
    """The metatiles of a layout whose upper layer is a signpost's drawing
    (voxel_sign.c models it): a sign's own cell, and the cell north of it
    when the sign's head tops a wall there. Read once per layout."""
    if layout_id not in _SIGN_METATILES:
        if _MAP_EVENTS is None:
            globals()["_MAP_EVENTS"] = vc.MapEvents()
        entry = next(e for e in vc.load_layouts() if e.get("id") == layout_id)
        layout = vc.Layout(entry, _MAP_EVENTS)
        pair = vc.pair_for(layout.primary, layout.secondary)
        found = set()
        for y in range(layout.h):
            for x in range(layout.w):
                if layout.role_at(x, y) != "signpost":
                    continue
                m = layout.metatile(x, y)
                if layout.draws_post_sign(m):
                    found.add(m)
                _, head, head_ground, _ = gen_voxel_sign_masks.sign_record(layout, pair, x, y)
                if any(head) and head_ground == layout.metatile(x, y - 1):
                    found.add(head_ground)      # a head on a blocked cell: all of its upper layer
        _SIGN_METATILES[layout_id] = found
    return _SIGN_METATILES[layout_id]


def same_building_pixels(model, layout_id, metatile, i, j):
    """Does `metatile` draw every pixel the model owns in its cell (i, j)?

    A sign drawn on its upper layer is the signpost's (voxel_sign.c), which
    stands in front of the building with its own height: only the lower
    layer is asked of it. Lavaridge's gym sign tops the gym's corner."""
    if layout_id not in _LAYOUT_ART:
        _LAYOUT_ART[layout_id] = vb.LayoutArt(layout_id)
    layers = (0,) if metatile in _sign_metatiles(layout_id) else (0, 1)
    ipx = _LAYOUT_ART[layout_id].cell_image(metatile, layers).load()
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


def _guttered(img):
    """The image framed by a copy of its own edge: a face whose edge is the
    image's (a bridge's cell, met by the next cell's model) samples a hair
    past it at the seam, and reads its own texel there, not the next image
    packed beside it - a line of another colour down every seam."""
    w, h = img.size
    out = Image.new(img.mode, (w + 2, h + 2))     # clear (an L mask: unlit)
    out.paste(img, (1, 1))
    out.paste(img.crop((0, 0, w, 1)), (1, 0))
    out.paste(img.crop((0, h - 1, w, h)), (1, h + 1))
    out.paste(out.crop((1, 0, 2, h + 2)), (0, 0))
    out.paste(out.crop((w, 0, w + 1, h + 2)), (w + 1, 0))
    return out


def pack_atlas(images):
    """Pack (key, image) pairs into the smallest page: each goes where its
    top edge lands lowest on the skyline of what is placed (bottom-left). A
    shelf packer left the space beside a tall, narrow drawing - a room's
    side wall - empty, and a small room took a page of half a megabyte.

    A page is a power of two wide but only as tall as the rows it uses, in
    whole tiles of 8: the console's texture is those rows rounded up to a
    power of two (texture_rows), and the rows past them are not stored - no
    face reads them (they still take VRAM: ctr_voxel.c PageBytes). Every width is tried, tallest first and largest first,
    and the fewest texels win. Returns ((width, rows), {key: (ox, oy)})."""
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
    # and what the other pieces found here draw (a statue's ball on its
    # pedestal): theirs, not the map's paint
    others = _REUSED_PIXELS.get(layout.id, ())
    for y in range(16):
        for x in range(16):
            if (apx[i * 16 + x, j * 16 + y][3] >= 128 if own is None
                    else (i * 16 + x, j * 16 + y) in own) or \
                    ((px + i) * 16 + x, (py + j) * 16 + y) in others:
                ipx[x, y] = (0, 0, 0, 0)
    return img


def placement_patches(model, layout_json_entry_name, layouts, px, py, odd):
    """The ground patches of one placement: cells the model leaves empty
    where the map paints something the reference does not - and those its
    spec lays itself (`patches`: (i, j, image), a bridge's drawing laid on
    the ground, voxel_bridge)."""
    own = [(i, j, img) for (i, j, img) in model.spec.get("patches", ())]
    # and those it was given where it was put (`placed_patches`)
    own += model.spec.get("placed_patches", {}).get((px, py), [])
    if not odd:
        return own
    cells = own
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
    # overlapping unless some edge has the whole square outside it. The
    # square's corner furthest out along an edge's normal is the same for
    # every square (by the signs of the normal), so only that one is tested;
    # along a row that test only ever turns one way, so each edge passes a
    # run of the row's squares, found by bisection
    tests = [(ax, ay, ex, ey,
              1 if sign * ex > 0 else 0, 1 if sign * ey < 0 else 0,
              eps * math.hypot(ex, ey)) for (ax, ay), (ex, ey) in edges]
    for v in range(lo_v, hi_v + 1):
        first, last = lo_u, hi_u
        for ax, ay, ex, ey, dv, du, limit in tests:
            if first > last:
                break
            def passes(u):
                return sign * (ex * (v + dv - ay) - ey * (u + du - ax)) > limit
            if -sign * ey > 0:          # passing from some square on
                a, b = first, last + 1
                while a < b:
                    m = (a + b) // 2
                    if passes(m):
                        b = m
                    else:
                        a = m + 1
                first = a
            elif -sign * ey < 0:        # passing up to some square
                a, b = first - 1, last
                while a < b:
                    m = (a + b + 1) // 2
                    if passes(m):
                        a = m
                    else:
                        b = m - 1
                last = a
            elif not passes(first):
                first = last + 1
        for u in range(first, last + 1):
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


def page_images(models, used, crops):
    """The images of a page's models, each its art cropped to `crops`:
    {model index: key}, {key: image}, {key: glass}, {key: name} - models
    drawn alike share one image, its glass the brightest of their masks
    (model.glass, cropped and guttered as the art), its name the first's."""
    art_key, art_img, art_glass, art_name = {}, {}, {}, {}
    for k in used:
        model = models[k]
        img = model.art.crop(crops[model.name])
        gutter = int(bool(model.spec.get("gutter")))
        if gutter:
            img = _guttered(img)
        key = art_key[k] = ("m", gutter, img.size, img.tobytes())
        art_img[key] = img
        art_name.setdefault(key, model.name)
        if getattr(model, "glass", None) is not None:
            mask = model.glass.crop(crops[model.name])
            if gutter:
                mask = _guttered(mask)
            art_glass[key] = ImageChops.lighter(art_glass[key], mask) if key in art_glass else mask
    return art_key, art_img, art_glass, art_name


def pages_crops(models, used, crops, opaque, patches, name):
    """The crops of a page's models: what their faces read (read_crop) if
    the page takes them; if not, the models whose reads past their opaque
    box take the most texels keep that box instead, one by one, largest
    first, until it does. Returns the crops and the models narrowed."""
    page = {models[k].name: crops[models[k].name] for k in used}

    def fits():
        art_key, art_img, _, _ = page_images(models, used, page)
        images = list(art_img.items()) + [(("p", key), img) for key, img in patches.items()]
        try:
            (tw, rows), _ = pack_atlas(images)
        except SystemExit:
            return False
        return tw * rows <= MAX_TEXTURE[0] * MAX_TEXTURE[1]

    def area(b):
        return (b[2] - b[0]) * (b[3] - b[1])

    narrowed = []
    if not fits():
        growth = sorted(((area(page[n]) - area(opaque[n]), n) for n in page
                         if page[n] != opaque[n]), reverse=True)
        for _, n in growth:
            page[n] = opaque[n]
            narrowed.append(n)
            if fits():
                break
    return page, narrowed


def read_box(uv):
    """The texels a face with these uv corners can read, as a box (u0, v0,
    u1, v1) - sampled_texels' span, not clipped to any texture."""
    eps = 1e-4
    us, vs = [p[0] for p in uv], [p[1] for p in uv]
    lo_u = int(math.floor(min(us) + eps))
    lo_v = int(math.floor(min(vs) + eps))
    hi_u = max(lo_u, int(math.ceil(max(us) - eps)) - 1)
    hi_v = max(lo_v, int(math.ceil(max(vs) - eps)) - 1)
    return lo_u, lo_v, hi_u + 1, hi_v + 1


def read_crop(model):
    """What of a model's texture goes to its page: its opaque texels and
    every texel a face reads - clear ones too, past the drawing's edge as
    well (clear there: what the proofs read). Cropped to what is opaque
    alone, a face reading clear texels beyond it (a floor decal's cell, a
    card's margin) read the next image packed on the page instead: the
    desks' tops in a strip on the laboratory's floor."""
    boxes = [m for m in [model.art.getbbox()] if m]
    boxes += [read_box([(p[3], p[4]) for p in tri]) for (tri, _, _) in model.mesh.tris]
    if not boxes:
        return (0, 0, 1, 1)
    return (min(b[0] for b in boxes), min(b[1] for b in boxes),
            max(b[2] for b in boxes), max(b[3] for b in boxes))


def export(models, path):
    """Write buildings.bin (VXB9).

    Geometry is stored once per model, texture coordinates in pixels of the
    model's own drawing. Textures are paged by map: each layout that places
    anything gets one page holding just the drawings it uses (and its ground
    patches), and a page-model record tells where on that page a model's
    drawing went. The console loads the pages of the maps on screen only.

    A drawing's window glass (voxel_night) is compacted with it: each texel
    of the mask moves with the faces reading it (glass_after). One that
    compaction would leave reading glass where another face reads none
    keeps its drawing as drawn.

      "VXB9", u16 pages, models, pageModels, placements, heightBytes, masks,
      u32 vertices, u16 variants, u16 decks, u16 conditions, u16 0
      pages       x 8:  u16 w, h; u32 file offset of its RGBA5551 texels (h the
                        rows it holds, a multiple of 8; its texture is h
                        rounded up to a power of two - texture_rows; h's bit
                        15: the page has a glass mask)
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
      decks x 8 (u16 layout; u8 x, y, top, bottom, elevation, 0: a bridge's
      deck over that cell, where walkers stand - see voxel_bridge.decks),
      conditions x 6 (u16 placement, metatile; u8 x, y: the placement
      stands only while that cell of its rectangle holds that metatile - a
      room piece's `when`), padding to 4,
      vertices x 24 (x, y, z, u, v, shade), then the pages' texels, each
      flagged page's followed by its w*h L8 glass-mask bytes in the same
      PICA order, then the texels that follow the game's tile animations
      (anim_table).
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
    # A drawing's window glass (voxel_night) moves with the faces reading it:
    # model.glass, an L image over the drawing as exported (None: no glass).
    glass, as_drawn = 0, []
    for m in models:
        mask = voxel_night.building_mask(m.art, m.name)
        m.glass = None
        kept = (m.art, m.mesh.tris, getattr(m, "live", None))
        before = (m.art.convert("RGBA"), list(m.mesh.tris))
        compact_rows(m)
        compact_islands(m)
        same_reads(m, *before)
        if getattr(m, "live", None):
            live_after(m, *before)
        if any(mask):
            mask = Image.frombytes("L", before[0].size, bytes(mask))
            moved = glass_after(m, *before, mask)
            if moved is None:
                # two faces read one texel with and without glass: the
                # drawing stays as it was drawn
                m.art, m.mesh.tris = kept[:2]
                if kept[2] is not None:
                    m.live = kept[2]
                moved = mask
                as_drawn.append(m.name)
            m.glass = moved
            glass += 1
    if glass:
        print("voxel buildings: window glass on %d drawing(s), %d kept as drawn%s"
              % (glass, len(as_drawn), (": " + ", ".join(as_drawn)) if as_drawn else ""))
    crops = {m.name: read_crop(m) for m in models}
    opaque = {m.name: (m.art.getbbox() or (0, 0, 1, 1)) for m in models}

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
    pages, page_models, placements, texels, anims = [], [], [], [], []
    glows = []
    for lid in sorted(by_layout):
        recs = by_layout[lid]
        used = sorted({r[1] for r in recs})
        patches = {}
        for r in recs:
            for (i, j, img) in r[5]:
                patches.setdefault(img.tobytes(), img)
        # each model's texture as its faces read it; where the page has no
        # room for that, the models whose reads past their drawing take
        # most fall back to the drawing's opaque box, largest first, and
        # say so (pages_crops)
        page_crops, narrowed = pages_crops(models, used, crops, opaque, patches, recs[0][6])
        for name in narrowed:
            print("  page of %s: %s reads past its drawing (%s); no room on the page"
                  % (recs[0][6], name, crops[name]))
        # models drawn alike (a bridge's cells, built apart) share one image,
        # and its glass
        art_key, art_img, art_glass, art_name = page_images(models, used, page_crops)
        images = list(art_img.items())
        images += [(("p", key), img) for key, img in patches.items()]
        (tw, rows), spots = pack_atlas(images)
        if tw * rows > MAX_TEXTURE[0] * MAX_TEXTURE[1]:
            big = sorted(images, key=lambda kv: -kv[1].size[0] * kv[1].size[1])[:12]
            print("  page of %s: %d texels in %d images; largest: %s" % (
                recs[0][6], sum(i.size[0] * i.size[1] for _, i in images), len(images),
                ", ".join("%s %dx%d" % (art_name[k] if k[0] == "m" else "patch", i.size[0], i.size[1])
                          for k, i in big)))
            raise SystemExit("%s: building page %dx%d exceeds the console's %dx%d"
                             % (recs[0][6], tw, rows, MAX_TEXTURE[0], MAX_TEXTURE[1]))
        page = len(pages)
        tex = bytearray(tw * rows * 2)
        glow = bytearray(tw * rows)
        for key, img in images:
            ox, oy = spots[key]
            apx = img.load()
            gpx = art_glass[key].load() if key in art_glass else None
            for y in range(img.size[1]):
                for x in range(img.size[0]):
                    r_, g_, b_, a_ = apx[x, y]
                    value = (r_ >> 3) << 11 | (g_ >> 3) << 6 | (b_ >> 3) << 1 | int(a_ >= 128)
                    struct.pack_into("<H", tex, 2 * texel_offset(ox + x, oy + y, tw), value)
                    if gpx is not None:
                        glow[texel_offset(ox + x, oy + y, tw)] = gpx[x, y]
        pages.append((tw, rows))
        glows.append(glow if any(glow) else b"")
        texels.append(tex)
        pm_of = {}
        live = []
        for k in used:
            ox, oy = spots[art_key[k]]
            ox += art_key[k][1] - page_crops[models[k].name][0]
            oy += art_key[k][1] - page_crops[models[k].name][1]
            pm_of[k] = len(page_models)
            page_models.append((k, page, ox, oy))
            live += [(ox + u, oy + v) + tuple(rest) for (u, v, *rest) in getattr(models[k], "live", ())]
        anims += anim_runs(page, lid, tw, tex, live)
        for (lid_, index, px, py, ground, cells, name) in recs:
            first = len(verts) // 6
            for (i, j, img) in cells:
                ox, oy = spots[("p", img.tobytes())]
                a = [i, 0.01, j, ox, oy, 1.0]
                b = [i + 1, 0.01, j, ox + 16, oy, 1.0]
                c = [i + 1, 0.01, j + 1, ox + 16, oy + 16, 1.0]
                d = [i, 0.01, j + 1, ox, oy + 16, 1.0]
                verts += a + b + c + a + c + d
            when = models[index].spec.get("when")
            placements.append((lid, pm_of[index], px, py, ground, len(verts) // 6 - first, first,
                               (when[2], when[0], when[1]) if when else None))
    placements.sort(key=lambda p: p[:7])
    conditions = [(k,) + p[7] for k, p in enumerate(placements) if p[7] is not None]
    placements = [p[:7] for p in placements]
    nverts = len(verts) // 6
    # ctr_voxel.c keeps a flag a page (VOXEL_MAX_PAGES), and a page larger
    # than half its VRAM block cannot share it with the next map's
    if len(pages) > CONSOLE_PAGES:
        raise SystemExit("%d texture pages: the console knows %d" % (len(pages), CONSOLE_PAGES))
    variants = ground_variants(models, layouts_json)
    index_of = {e.get("id"): k + 1 for k, e in enumerate(layouts_json)}
    decks = sorted((index_of[lid], y, x, top, bottom, gate)
                   for (lid, x, y, top, bottom, gate) in voxel_bridge.deck_table() + voxel_fortree.deck_table())
    head = MAGIC + struct.pack("<HHHHHHIHHHH", len(pages), len(models), len(page_models),
                               len(placements), len(heights), len(masks), nverts,
                               len(variants), len(decks), len(conditions), 0)
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
    for (lid, y, x, top, bottom, gate) in decks:
        body += struct.pack("<HBBBBBB", lid, x, y, top, bottom, gate, 0)
    for (k, metatile, x, y) in conditions:
        body += struct.pack("<HHBB", k, metatile, x, y)
    table_size = 8 * len(pages)
    fixed = len(head) + table_size + len(body)
    pad = (-fixed) % 4
    body += bytes(pad)
    body += struct.pack("<%df" % len(verts), *verts)
    offset = len(head) + table_size + len(body)
    table = bytearray()
    for (tw, rows), tex, glow in zip(pages, texels, glows):
        table += struct.pack("<HHI", tw, rows | (0x8000 if glow else 0), offset)
        offset += len(glow)
        offset += len(tex)
    blob = head + table + body + b"".join(tex + glow for tex, glow in zip(texels, glows))
    blob += anim_table(anims, len(blob))
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
    model stands for, and every bare cell of a relief_bare model less its
    upper layer's quarters (bare_quarters). The console composes each in an atlas slot of its own
    and draws it under the model, so that the cell's drawing - its animated
    water - lies there without the rock painted flat on it. Any other copy
    of the metatile keeps its whole drawing."""
    index_of = {e.get("id"): k + 1 for k, e in enumerate(layouts_json)}
    entry_of = {e.get("id"): e for e in layouts_json}
    blocks_of = {}
    out = {}
    for m in models:
        at = list(m.spec.get("at", ()))
        if getattr(m, "bare", None):
            # a relief_bare model's bare cells, wherever it stands
            at += [(name, px, py) for (_, px, py, _, name, _) in find_placements(m, layouts_json)]
        for (lid, px, py) in at:
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
            if name != layout_id or not when_holds(m, layout, px, py):
                continue    # (or switched off as the room is drawn)
            for j in range(h):
                for i in range(w):
                    if ground != OWN_GROUND and (i, j) in m.owned:
                        override[(px + i, py + j)] = ground     # a prop keeps each cell's own
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


def room_check(models, layout_id, out_dir, draw=None, suffix=""):
    """A whole room as the console composes it, against its drawing.

    The terrain draws each cell a model covers with the placement's floor
    and every other cell with its own metatile; the pieces stand over them.
    Rendered in the GBA projection, the room must be its drawing again,
    pixel for pixel. Returns the pixels that differ.
    """
    layouts_json = json.load(open(os.path.join(vb.ROOT, "data", "layouts", "layouts.json"),
                                  encoding="utf-8"))["layouts"]
    layout = drawn_as(layout_id, draw)
    W, H = layout.w * 16, layout.h * 16
    ras = vb.Raster(W, H, bg=(0, 0, 0))
    covered, items, patches, hidden = {}, [], [], set()
    for m in models:
        w, h = m.cells
        for lid, px, py, ground, name, odd in find_placements(m, layouts_json):
            if name != layout_id or not when_holds(m, layout, px, py):
                continue    # (or switched off in this state of the room)
            for j in range(h):
                for i in range(w):
                    if (i, j) in m.owned:   # (the rest stays the map's)
                        covered[(px + i, py + j)] = ground
            items.append((m, px, py))
            hidden |= {(px * 16 + u, py * 16 + v) for (u, v) in m.spec.get("behind_wall", ())}
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
            if _inside([r for r, _ in room.get("redrawn", ())], x, y):
                continue  # stood otherwise than its drawing, on purpose
            if (x, y) in hidden:
                continue  # a top's rows behind the wall it stands against
            if fpx[x, y] != gpx[x, y]:
                bad += 1
                dpx[x, y] = (255, 0, 255)
                tag = ras.owner[y * W + x] or "terrain"
                culprits.setdefault(tag, []).append((x, y))
    for tag, pts in sorted(culprits.items(), key=lambda kv: -len(kv[1])):
        print("    %-34s %4d px, e.g. %s" % (tag, len(pts), pts[:3]))
    if hidden:
        print("    %-34s %4d px (not judged)" % ("tops behind the wall", len(hidden)))
    for r, why in room.get("redrawn", ()):
        print("    %-34s %s (not judged: %s)" % ("redrawn", r, why))
    sheet = Image.new("RGB", (W * 2 + 4, H))
    sheet.paste(full, (0, 0))
    sheet.paste(diff, (W + 4, 0))
    sheet.resize((sheet.width * 3, sheet.height * 3), Image.NEAREST).save(
        os.path.join(out_dir, layout_id.lower() + suffix + "_room_check.png"))
    return bad


def verify(models, out, complete=True):
    """The proofs: each model against its drawing, each room as composed, and
    each room's pieces by the interior lint. Writes the diagnostic images to
    `out`; True when all of them pass. `complete` when the models are the
    whole game's (not --only): only then is an allowance for a piece that was
    not built a stale one."""
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
        if reference is not None and getattr(model, "own", None) is not None:
            reference = _without(reference, {(u, v) for v in range(model.cells[1] * 16)
                                             for u in range(model.cells[0] * 16)
                                             if (u, v) not in model.own})
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
        # and as a script leaves it: a puzzle's other state (a room's `draw`)
        for n, draw in enumerate(sorted({id(s["interior"]["draw"]): s["interior"]["draw"]
                                         for s in specs.SPECS
                                         if s.get("interior", {}).get("layout") == lid
                                         and s["interior"].get("draw")}.items())):
            bad = room_check(models, lid, out, draw[1], "_state%d" % (n + 1))
            print("room %-28s in its state %d: %d pixel(s) differ from the drawing"
                  % (lid, n + 1, bad))
            failed = failed or bad != 0
    for lid in sorted(voxel_bridge.BRIDGES):
        mine = [m for m in models if m.spec.get("deck_of") == lid]
        if mine:
            bad = bridge_check(mine, lid, out)
            print("bridge %-26s composed with the terrain: %d pixel(s) differ from the drawing"
                  % (lid, bad))
            failed = failed or bad != 0
    if interior_lint(models, rooms, out, complete):
        failed = True
    return not failed


def bridge_check(models, layout_id, out_dir):
    """A bridge's models as the console lays them in their map, against its
    drawing. Each model is proved alone without the faces the drawing never
    shows (~depth); here they all stand together, those too, over the
    ground each placement lays: where one model's hidden side stood out in
    front of the next one's drawing, or a cell's own paint was lost under
    the ground laid for it, the map would show it. Judged in every cell a
    model owns but beside a ramp, where its rails are drawn coming down
    (voxel_bridge's stretched: its slot's walls stand there), and under a
    signpost (the console stands its own). Returns the pixels that differ."""
    layouts_json = json.load(open(os.path.join(vb.ROOT, "data", "layouts", "layouts.json"),
                                  encoding="utf-8"))["layouts"]
    layout = vb.LayoutArt(layout_id)
    W, H = layout.w * 16, layout.h * 16
    ras = vb.Raster(W, H, bg=(0, 0, 0))
    covered, items, patches = {}, [], []
    for m in models:
        for lid, px, py, ground, name, odd in find_placements(m, layouts_json):
            if name != layout_id:
                continue
            for (i, j) in m.owned:
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
    for (x, y, img) in patches:
        q = [(x * 16, 0, y * 16, 0, 0), (x * 16 + 16, 0, y * 16, 16, 0),
             (x * 16 + 16, 0, y * 16 + 16, 16, 16), (x * 16, 0, y * 16 + 16, 0, 16)]
        for tri in ((q[0], q[1], q[2]), (q[0], q[2], q[3])):
            ras.draw([(X, Z - Y, Y + Z + 0.01, 1.0, U, V) for (X, Y, Z, U, V) in tri], img, 1.0,
                     "patch")
    for (m, px, py) in items:
        for (tri, shade, tag) in m.mesh.tris:
            vs = [(x + px * 16, z + py * 16 - y, y + z + py * 16, 1.0, u, v)
                  for (x, y, z, u, v) in tri]
            ras.draw(vs, m.art, 1.0, tag)
    br = voxel_bridge.bridge(layout_id)
    skip = br.stretched()
    full = Image.new("RGB", (W, H))
    for y in range(layout.h):
        for x in range(layout.w):
            full.paste(layout.cell_image(layout.metatile(x, y)).convert("RGB"), (x * 16, y * 16))
    got = ras.image()
    fpx, gpx = full.load(), got.load()
    diff = got.copy()
    dpx = diff.load()
    culprits = {}
    for y in range(H):
        for x in range(W):
            c = (x // 16, y // 16)
            if c not in covered or c in br.sign_cells or (x, y) in skip:
                continue
            # the console's textures are 5 bits a channel
            if any((a >> 3) != (b >> 3) for a, b in zip(fpx[x, y], gpx[x, y])):
                dpx[x, y] = (255, 0, 255)
                tag = ras.owner[y * W + x] or "terrain"
                culprits.setdefault(tag, []).append((x, y))
    for tag, pts in sorted(culprits.items(), key=lambda kv: -len(kv[1])):
        print("    %-34s %4d px, e.g. %s" % (tag, len(pts), pts[:3]))
    sheet = Image.new("RGB", (W * 2 + 4, H))
    sheet.paste(full, (0, 0))
    sheet.paste(diff, (W + 4, 0))
    sheet.save(os.path.join(out_dir, layout_id.lower() + "_bridge_check.png"))
    return sum(len(v) for v in culprits.values())


LINT_ALLOW = os.path.join(PORT, "assets", "voxel", "interior_lint_allow.toml")


def lint_allowed(path=LINT_ALLOW):
    """The faults accepted on a room's pieces, {piece: {kind: reason}}, and
    the rooms finished (approved on the console), from the allowlist."""
    import tomllib
    if not os.path.exists(path):
        return {}, []
    with open(path, "rb") as f:
        data = tomllib.load(f)
    for piece, kinds in data.get("allow", {}).items():
        for kind, reason in kinds.items():
            if not str(reason).strip():
                raise SystemExit("%s: %s %s is allowed with no reason" % (path, piece, kind))
    return data.get("allow", {}), list(data.get("finished", []))


def lint_verdict(faults, linted, allowed, finished, rooms, complete=True):
    """The interior lint judged, in every room: it fails the build (F5).

    `faults` are the faults found, (room, piece, kind, text), on the pieces
    `linted`. Each one is a failure but one the allowlist accepts with its
    reason. So is an allowance that finds no fault any more - the piece put
    right, renamed or gone, or its kind misspelt: kept, it would hide the
    next fault of that kind on that piece unread - and a finished room the
    game has no pieces for. Without `complete` (an --only build) a piece or
    room that was not built is not judged. Returns the report's lines and the
    failures."""
    lines, failures = [], []
    found = collections.defaultdict(set)
    for lid, piece, kind, text in faults:
        found[piece].add(kind)
        reason = allowed.get(piece, {}).get(kind)
        if reason:
            lines.append("%-40s %-28s %-8s %s (allowed: %s)" % (lid, piece, kind, text, reason))
        else:
            lines.append("%-40s %-28s %-8s %s" % (lid, piece, kind, text))
            failures.append("%s %s: %s %s" % (lid, piece, kind, text))
    for piece, kinds in sorted(allowed.items()):
        if piece not in linted and not complete:
            continue
        for kind in sorted(kinds):
            if kind not in found[piece]:
                failures.append("%s %s: allowed, but %s; remove it from the allowlist"
                                % (piece, kind, "not found" if piece in linted
                                   else "no such piece is linted"))
    for lid in finished:
        if lid not in rooms and complete:
            failures.append("%s: finished, but no room has interior pieces in it" % lid)
    return lines, failures


def interior_lint(models, rooms, out, complete=True):
    """What voxel_defect_scan.py finds on each room's own pieces that the
    45-degree proofs cannot see, judged by lint_verdict: a line a room with
    faults here, every piece's in out/interior_lint.txt. Returns the number
    of failures - faults the allowlist does not accept, and stale entries in
    it. Strict in every room: the finished ones, approved on the console,
    and every other (held to no fault from a new room's first build)."""
    import voxel_defect_scan as vds     # (it imports this module)
    allowed, finished = lint_allowed()
    faults, linted = [], set()
    for lid in rooms:
        for m in models:
            lint = m.spec.get("lint")
            if (m.spec["layout"] != lid or not lint or lint["kind"] == "shell"
                    or m.name.endswith("_bare")):
                continue
            linted.add(m.name)
            for kind, text in vds.scan_piece(m, lint, collections.defaultdict(set)):
                faults.append((lid, m.name, kind, text))
    lines, failures = lint_verdict(faults, linted, allowed, finished, rooms, complete)
    for lid in rooms:
        mine = [f for f in faults if f[0] == lid]
        bad = collections.Counter(kind for _, piece, kind, _ in mine
                                  if not allowed.get(piece, {}).get(kind))
        if bad:
            print("room %-28s interior lint: FAIL %s"
                  % (lid, ", ".join("%s %d" % kv for kv in bad.most_common())))
        elif mine:
            print("room %-28s interior lint: 0 (%d allowed)" % (lid, len(mine)))
    print("interior lint: %d room(s), %d failure(s), %d fault(s) allowed; "
          "finished on the console: %d of %d"
          % (len(rooms), len(failures), len(faults) - sum(
              1 for _, piece, kind, _ in faults if not allowed.get(piece, {}).get(kind)),
             len(set(finished) & set(rooms)), len(rooms)))
    for text in failures:
        print("    " + text)
    with open(os.path.join(out, "interior_lint.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines + ["FAIL " + t for t in failures]) + "\n")
    return len(failures)


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
    if checks and not verify(models, out, complete=not args.only):
        raise SystemExit("a model does not reproduce its drawing, or a room fails its "
                         "interior lint (see above, and %s)" % os.path.join(out, "interior_lint.txt"))
    if args.output:
        export(models, args.output)
    if args.town:
        for path in town_preview(models, args.town, out):
            print("town preview:", path)


if __name__ == "__main__":
    main()
