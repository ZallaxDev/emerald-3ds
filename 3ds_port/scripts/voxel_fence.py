#!/usr/bin/env python3
"""Fences built post by post from their drawing (gen_voxel_buildings).

A fence is not a wall: the General tileset's grey fence is a row of posts
with two-pixel rails between them, and Mauville's is a row of short logs
standing apart. Read column by column (voxel_building.Relief) a run down a
column of the map became a solid band with the post tops painted on it, and
a log fence a lump of bark. Here every post is found by its own drawn top -
the 4x2 light cap of a fence post, the cut face of a log - wherever it is
drawn, and stands where the drawing puts it:

  * its foot is the row where its front ends on the ground (its shadow,
    or nothing): a post's height is read, not assumed. Where the next post
    south hides it (a fence down a column) the family's own height is used;
  * a fence post is a box `post` wide and `depth` deep, a rail a bar of the
    same depth between two posts of one row, wherever its top and front are
    drawn between them, and a bar from one post to the next down a column;
  * a log is an upright cylinder, its cut face the drawn ellipse.

The faces the 45-degree view sees take the drawing projected onto them
(u = X, v = Z - Y), exactly as the GBA shows them; the sides take the
family's reference metatile, appended below the art. What the drawing paints
round the posts on the ground - the shadow under the rails, the dark grass at
a log's foot - is laid flat on the ground (voxel_building.Decal).

A fence cut into blocks (component_specs) builds each post where its top is
drawn: a post whose top is in the block's rows is the block's, even when its
front reaches into the cell south of it, and the pixels a post of the block
north draws into this one are that block's to reproduce.
"""

import os
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import voxel_building as vb  # noqa: E402

CELL = 16


def _compose(layout, cells, x0, y0, w, h, keep):
    """RGBA image of the map's cells (absolute (x, y)) inside the rect: their
    upper layer, and on their lower layer the pixels of the family's own
    colours `keep` (a post's foot drawn on the bottom layer, under the ground
    it stands in: Route 110's fence along the water)."""
    img = Image.new("RGBA", (w * CELL, h * CELL), (0, 0, 0, 0))
    px = img.load()
    for (X, Y) in cells:
        i, j = X - x0, Y - y0
        if not (0 <= i < w and 0 <= j < h) or not (0 <= X < layout.w and 0 <= Y < layout.h):
            continue
        m = layout.metatile(X, Y)
        low, high = layout.subtiles(m, 0), layout.subtiles(m, 1)
        for q in range(4):
            for k in range(64):
                u = i * CELL + (q & 1) * 8 + k % 8
                v = j * CELL + (q >> 1) * 8 + k // 8
                rgb1, i1 = high[q][k]
                rgb0, i0 = low[q][k]
                if i1:
                    px[u, v] = rgb1 + (255,)
                elif i0 and rgb0 in keep:
                    px[u, v] = rgb0 + (255,)
    return img


def _ref_image(layout, metatile):
    img = Image.new("RGBA", (CELL, CELL), (0, 0, 0, 0))
    px = img.load()
    for q, sub in enumerate(layout.subtiles(metatile, 1)):
        for k, (rgb, index) in enumerate(sub):
            if index:
                px[(q & 1) * 8 + k % 8, (q >> 1) * 8 + k // 8] = rgb + (255,)
    return img


def _rect_colours(img, rect):
    px = img.load()
    u0, v0, u1, v1 = rect
    return {px[u, v][:3] for v in range(v0, v1) for u in range(u0, u1) if px[u, v][3] >= 128}


def _find(img, pattern):
    """Every (u, v) where the pattern (rows of colours) is drawn whole."""
    W, H = img.size
    px = img.load()
    ph, pw = len(pattern), len(pattern[0])
    out = []
    for v in range(H - ph + 1):
        for u in range(W - pw + 1):
            if all(px[u + a, v + b][3] >= 128 and px[u + a, v + b][:3] == pattern[b][a]
                   for b in range(ph) for a in range(pw)):
                out.append((u, v))
    return out


class Post:
    """One post or log found in the drawing: its plan and height, the art
    rectangle it covers in the GBA's view, and whether this block builds it."""

    def __init__(self, x0, x1, z0, z1, h, own, log=None):
        self.x0, self.x1, self.z0, self.z1, self.h, self.own = x0, x1, z0, z1, h, own
        self.log = log      # (cx, cz, rx, rz) for a log

    def covers(self):
        """The art pixels its faces draw over at 45 degrees (a box: its top
        and front)."""
        return (int(self.x0), int(self.z0 - self.h), int(-(-self.x1 // 1)), int(-(-self.z1 // 1)))


def _strip(ref, rect):
    """A cell-tall strip of the reference's front (rect): its rows repeated
    down, for the faces the drawing never shows - a post's sides, a log's
    back - however tall the post."""
    u0, v0, u1, v1 = rect
    out = Image.new("RGBA", (u1 - u0, CELL), (0, 0, 0, 0))
    for v in range(CELL):
        row = ref.crop((u0, v0 + v % (v1 - v0), u1, v0 + v % (v1 - v0) + 1))
        out.paste(row, (0, v))
    return out


def _foot(img, u, v, trunk, body, drawn):
    """Down column u from row v: the row where the post's front ends on the
    ground, or None where another of the fence's drawing goes on over it
    (the next post south hides its foot), or it runs into a cell that is
    not composed (the next map's, over a seam) or off the art."""
    W, H = img.size
    px = img.load()
    while v < H and px[u, v][3] >= 128 and px[u, v][:3] in trunk:
        v += 1
    if v >= H or (u // CELL, v // CELL) not in drawn:
        return None
    if px[u, v][3] >= 128 and px[u, v][:3] in body:
        return None
    return v


def _drawn_by(parts, tex, W, H):
    """The art pixels the parts draw in the GBA's 45-degree view."""
    model = vb.Model("fence", tex, parts, (1, 1), 0)
    ras = vb.Raster(W, H, bg=(0, 0, 0))
    for (tri, shade, tag) in model.mesh.tris:
        if "~behind" not in tag:
            ras.draw([(x, z - y, y + z, 1.0, u, v) for (x, y, z, u, v) in tri], tex, shade, tag)
    return {(k % W, k // W) for k, o in enumerate(ras.owner) if o is not None}


def build(layout, spec, family, foreign=()):
    """(art, parts, reference) of one block of a fence run: the posts whose
    tops it draws, the rails from them, the drawing's leftovers on the
    ground. `foreign`: the map's pixels another model keeps."""
    x, y, w, h = spec["rect"]
    owned = spec["owned"]
    ref = _ref_image(layout, family["ref"])
    fu, fv, fw, fh = family["find"]
    rpx = ref.load()
    pattern = [[rpx[fu + a, fv + b][:3] for a in range(fw)] for b in range(fh)]
    body = _rect_colours(ref, family["body"])
    trunk = _rect_colours(ref, family["trunk"])
    # the block's cells, the run's cells north and south of it (their posts
    # draw into this block's rows, or this block's into theirs)
    cells = {(x + i, y + j) for (i, j) in owned}
    cells |= {(x + i, y + j - 1) for (i, j) in spec.get("north", ())}
    cells |= {(x + i, y + j + 1) for (i, j) in spec.get("south", ())}
    ext = _compose(layout, cells, x, y - 1, w, h + 2, body)
    # what another model keeps of these cells (a building's wall) is not
    # the fence's
    epx = ext.load()
    for (X, Y) in foreign:
        u, v = X - x * CELL, Y - (y - 1) * CELL
        if 0 <= u < w * CELL and 0 <= v < (h + 2) * CELL:
            epx[u, v] = (0, 0, 0, 0)
    drawn = {(X - x, Y - y + 1) for (X, Y) in cells}
    W, H0 = w * CELL, h * CELL
    # ext rows: 16 north, then the block's, then 16 south; model z = row - 16
    posts = []
    for (u, v) in _find(ext, pattern):
        r = v - CELL                     # the pattern's row in model z
        own = 0 <= r < H0 and (u // CELL, r // CELL) in owned
        if family["kind"] == "box":
            px0 = u - fu + family["post"][0]
            px1 = u - fu + family["post"][1]
            d = family["depth"]
            top = r - fv + family["top"]          # the cap's first row
            foot = _foot(ext, u - fu + family["scan"], v - fv + family["top"] + d,
                         trunk, body, drawn)
            hgt = foot - CELL - top - d if foot is not None else family["hidden"]
            z0 = top + hgt
            posts.append(Post(px0, px1, z0, z0 + d, hgt, own))
        else:
            dx, rx, rz = family["log"]
            cx = u - fu + dx
            mid = r - fv + family["top"]          # the cut face's middle row
            foot = _foot(ext, u - fu + family["scan"], v - fv + family["trunk_row"],
                         trunk, body, drawn)
            cz = (foot - CELL - rz) if foot is not None else mid + family["hidden"]
            hgt = cz - mid
            posts.append(Post(cx - rx, cx + rx, cz - rz, cz + rz, hgt, own, (cx, cz, rx, rz)))
    art = ext.crop((0, CELL, W, CELL + H0 + CELL))
    apx = art.load()
    # the texture: the block and the cells south (rows 0 .. H0 + 16), then
    # what lies on the ground (Decal), then the strip the hidden faces take
    o = H0 + CELL
    R = o + H0
    strip = _strip(ref, family["strip"])
    tex = Image.new("RGBA", (max(W, strip.width), R + CELL), (0, 0, 0, 0))
    tex.paste(art, (0, 0))
    tex.paste(strip, (0, R))
    side = (0, strip.width, R + CELL)
    parts = []
    rails = []
    if family["kind"] == "box":
        ry0, ry1 = family["rail"]
        for p in posts:
            if p.own:
                parts.append(vb.PlanPrism("post", [(p.x0, p.z0), (p.x1, p.z0), (p.x1, p.z1),
                                                   (p.x0, p.z1)], -1.0, p.h, side, art=tex))
        # rails along a row: between the posts of one row, wherever the
        # drawing has their top and front's first row
        levels = sorted({(p.z0, p.z1) for p in posts if p.own})
        for (z0, z1) in levels:
            rows = range(int(z0 - ry1), int(z1 - ry1) + 1)
            if rows.start < 0:
                continue
            inside = [(p.x0, p.x1) for p in posts if (p.z0, p.z1) == (z0, z1)]
            run = []
            for u in range(W + 1):
                ok = (u < W and all(apx[u, v][3] >= 128 for v in rows)
                      and not any(a <= u < b for (a, b) in inside))
                if ok:
                    run.append(u)
                    continue
                if run:
                    rails.append((run[0], run[-1] + 1, z0, z1))
                run = []
        # rails down a column: from a post to the next one south in its line,
        # where the drawing goes on unbroken between their tops
        by_x = {}
        for p in posts:
            by_x.setdefault((p.x0, p.x1), []).append(p)
        for (a, b), line in by_x.items():
            line.sort(key=lambda p: p.z0)
            for p, q in zip(line, line[1:]):
                if not p.own or q.z0 <= p.z1:
                    continue
                u = int(a) + 1
                v0, v1 = int(p.z0 - p.h), int(q.z0 - q.h)
                if v1 - v0 > 2 * CELL or not all(
                        0 <= vv < art.size[1] and apx[u, vv][3] >= 128 for vv in range(max(0, v0), v1)):
                    continue
                inset = family.get("rail_inset", 1)
                rails.append((a + inset, b - inset, p.z1, q.z0))
        for (a, b, z0, z1) in rails:
            parts.append(vb.PlanPrism("rail", [(a, z0), (b, z0), (b, z1), (a, z1)],
                                      ry0, ry1, side, art=tex))
    else:
        bark = vb.Tile(0, R, strip.width, R + CELL)
        for p in posts:
            if p.own:
                cx, cz, rx, rz = p.log
                parts.append(vb.Cylinder("log", cx, cz, rx, rz, -1.0, p.h, bark,
                                         sides=family.get("sides", 16)))
    # what the drawing paints on the ground round them: its pixels no post
    # or rail draws at 45 degrees, of no colour of the fence's own
    covered = _drawn_by(parts, tex, W, H0)
    north = set()
    for p in posts:
        if not p.own:
            a, b, c, d = p.covers()
            north |= {(u, v) for v in range(max(0, b), min(H0, d)) for u in range(max(0, a), min(W, c))}
    # a cell with no fence north of it: on the map's top edge the fence goes
    # on in the map north, and the posts standing there draw their fronts
    # into these rows; inside the map the drawing may start a run with a
    # post's front from nowhere (Route 110's 140 under the grass at 11,76),
    # which no post here stands for
    for (i, j) in owned:
        if (i, j - 1) in owned or (i, j) in spec.get("north", ()):
            continue
        for u in range(i * CELL, i * CELL + CELL):
            for v in range(j * CELL, j * CELL + CELL):
                if (u, v) in covered:
                    break
                if apx[u, v][3] >= 128 and apx[u, v][:3] in body:
                    north.add((u, v))
    north -= covered
    decal = {}
    tpx = tex.load()
    for v in range(H0):
        for u in range(W):
            c = apx[u, v]
            if c[3] < 128 or (u, v) in covered or (u, v) in north or c[:3] in body:
                continue
            tpx[u, o + v] = c
            cell = (u // CELL, v // CELL)
            bx = decal.get(cell)
            lu, lv = u % CELL, v % CELL
            decal[cell] = ((lu, lv, lu + 1, lv + 1) if bx is None else
                           (min(bx[0], lu), min(bx[1], lv), max(bx[2], lu + 1), max(bx[3], lv + 1)))
    if decal:
        parts.append(vb.Decal("shadow", sorted(decal), o, boxes=decal))
    # judged against the block's own drawing less what the posts of the
    # block north draw into it
    reference = tex.copy()
    rpx2 = reference.load()
    for (u, v) in north:
        rpx2[u, v] = (0, 0, 0, 0)
    return tex, parts, reference
