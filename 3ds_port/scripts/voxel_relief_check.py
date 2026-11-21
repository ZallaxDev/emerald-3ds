#!/usr/bin/env python3
"""The mountains' geometry checked against what a 45-degree drawing allows.

gen_voxel_relief.py lifts every point of the art to (u, h, v + h): seen at
45 degrees the result is the drawing whatever h is, so the drawing alone
never shows a wrong height - the game's camera does. Each tile of rock has
one shape the drawing means, and between two lattice points (4 pixels) a
surface can only be one of:

    across a row (u)    |dh| <= 4   flat to a band's 45-degree slope; more is
                                    the texture stretched sideways ("stretch")
    down a column (v)   -4 <= dh <= 0   a vertical south face to flat; a rise
                                    southward is a slope turned north, which
                                    the drawing never shows ("north"); a fall
                                    of more than a pixel a pixel leans out
                                    over its foot ("overhang")
    a quad              its four corners on one plane, within 4 px; more is
                                    a crease across the tile ("twist")

A wall a level tall within one lattice step, either way (a cap's end over
the sea, a rim's hidden riser, the outline of a rock against the water) is
the silhouette the drawing has, as Route 116's rims are, and is not counted:
what is counted is a slope the drawing does not draw, the texture spread
over it.

Route 116 is the reference: its mountains are what the tiles mean, and the
check is that every other map is as clean, cell by cell.

    python voxel_relief_check.py                     every drawn group
    python voxel_relief_check.py --groups route105,route106 --images DIR
    python voxel_relief_check.py --save now.pkl      keep this run
    python voxel_relief_check.py --against old.pkl   what got better or worse
    python voxel_relief_check.py --view route105:31,14:45:6 --out v.png
                                 cell 31,14 through the game's camera, pitch
                                 45, 6 cells away, lit as the game lights it
    python voxel_relief_check.py --holes --groups route105
                                 every cut tile seen from 25, 40 and 60
                                 degrees: where the terrain is seen through
    make check-voxel-relief      all of it, against the last run kept

The workflow for a map: list its flagged cells (--list), look at them
through the camera (--view), change the tile pattern in gen_voxel_relief.py
(never a single map), and run it again --against the run before: Route 116
must not get worse, and no cell anywhere should.

`--images` draws each group's art with every flagged cell boxed (red severe,
orange mild) and its score, to read the faults off the tiles.
"""

import argparse
import collections
import math
import os
import pickle
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from voxel_image import image_pixels  # noqa: E402

STEP_PX = 4         # lattice spacing (gen_voxel_relief.STEP)
SPREAD = 1.25       # how much steeper than true a fall spread evenly may be (gen_voxel_relief.SPREAD)
WALL = 12           # a rise southward this tall in one step is a hidden wall
SEVERE = 16         # a cell scoring this much is boxed red
REFERENCE = "route116"


def snapshot(groups=None):
    """{group: what the check needs of its solve}."""
    import gen_voxel_relief as g
    g.world_levels()
    out = {}
    for name in g.DRAWN:
        if (groups and name not in groups) or not g.drawn_ok(name):
            continue
        res = g.solve_drawn(name)
        prep = g._PREP[name]
        top, kinds, soil, mt = g._ROCK[name]
        CW, CH = prep["CW"], prep["CH"]
        P = g.PER_CELL
        H = [[None] * (CW * P + 1) for _ in range(CH * P + 1)]
        for lid, (ox, oy) in g.DRAWN[name].items():
            b = g._BASE.get(lid, 0)
            for j, row in enumerate(res[lid]):
                for i, v in enumerate(row):
                    H[oy * P + j][ox * P + i] = v + b
        cuts, grids, tops, walls = {}, {}, {}, {}
        for lid, (ox, oy) in g.DRAWN[name].items():
            if g.drawn_group(lid) != name:
                continue
            roles_layout, h, _ = g.layout_heights(lid)
            b = g._BASE.get(lid, 0)
            cut = g.cut_cells(lid, roles_layout, h)
            for (x, y, m, mask, foot, ground) in cut:
                cuts[(ox + x, oy + y)] = (mask, foot + b, ground)
            rim, fills, wall = g.cell_shapes(lid, roles_layout, h, cut)
            for (x, y), m in wall.items():
                walls[(ox + x, oy + y)] = m[0]
            for (qx, qy), lv in getattr(g, "TOPS", {}).get(lid, {}).items():
                tops[(ox * 16 + qx, oy * 16 + qy)] = lv + b
            for (x, y), grid in rim.items():
                grids[(ox + x, oy + y)] = [[v + b for v in row] for row in grid]
            for (x, y, foot, ground, *_) in fills:
                cuts[(ox + x, oy + y)] = (None, foot + b, ground)
            # as exported: the lattice levelled under the cut tiles
            for j in range(roles_layout.h * P + 1):
                for i in range(roles_layout.w * P + 1):
                    if H[oy * P + j][ox * P + i] is not None:
                        H[oy * P + j][ox * P + i] = h[j][i] + b
        out[name] = dict(members=dict(g.DRAWN[name]), CW=CW, CH=CH, H=H, top=dict(top), cuts=cuts,
                         grids=grids, tops=tops, walls=walls,
                         owned=[(ox, oy, g.open_roles(lid).w, g.open_roles(lid).h)
                                for lid, (ox, oy) in g.DRAWN[name].items() if g.drawn_group(lid) == name],
                         kinds=dict(kinds), mt=[[mt(x, y) for x in range(CW)] for y in range(CH)],
                         cell=[row[:] for row in g._CELLS[name]], side=prep["side"],
                         flat=prep["flat"], soil=soil)
    return out


def faults(G, cx, cy):
    """{kind: excess in pixels} of one cell's lattice, as exported (its own
    grid where it has one), over the quads that draw something: a quad of
    clear background (a cut tile's) is never seen and may do anything.

    What a drawn quad may be is what the tiles draw: level (a top), a band's
    45 degrees across, a south face's drop down, or the two at once (a
    corner). Anything else spreads the drawing over a surface it does not
    draw: "stretch" (steeper than a band across, a wall within a step
    included - a silhouette is drawn by the cut, not by stretching the
    outline's pixels down it), "north" (rising southward: the back, which
    no camera looks at, stretched over), "overhang" (falling more than a
    pixel a pixel southward), "twist" (a crease across the quad)."""
    f = {"stretch": 0.0, "north": 0.0, "overhang": 0.0, "twist": 0.0}
    P = 4
    grid = G.get("grids", {}).get((cx, cy))
    if grid is None:
        H = G["H"]
        grid = [[H[cy * P + j][cx * P + i] for i in range(P + 1)] for j in range(P + 1)]
    cut = G.get("cuts", {}).get((cx, cy))
    mask = cut[0] if cut and cut[0] is not None and not isinstance(cut[0], str) else None
    for j in range(P):
        for i in range(P):
            q = (grid[j][i], grid[j][i + 1], grid[j + 1][i], grid[j + 1][i + 1])
            if None in q:
                continue
            if mask is not None and all((mask[y] >> x) & 1 for y in range(j * 4, j * 4 + 4)
                                        for x in range(i * 4, i * 4 + 4)):
                continue        # clear: nothing drawn on it
            for (a, b) in ((q[0], q[1]), (q[2], q[3])):
                f["stretch"] += max(0.0, abs(b - a) - STEP_PX * SPREAD) / 2
            for (a, b) in ((q[0], q[2]), (q[1], q[3])):
                d = b - a
                if d > 0.01:
                    f["north"] += d / 2
                else:
                    f["overhang"] += max(0.0, -STEP_PX * SPREAD - d) / 2
            f["twist"] += max(0.0, abs(q[0] - q[1] - q[2] + q[3]) - STEP_PX)
    return f


def score(G):
    """{cell: (total, faults)} of every cell of rock or terrace that has any."""
    out = {}
    cells = set(G["kinds"]) | set(G.get("grids", {}))
    for (cx, cy) in cells:
        if not (0 <= cx < G["CW"] and 0 <= cy < G["CH"]):
            continue
        f = faults(G, cx, cy)
        t = sum(f.values())
        if t > 0.01:
            out[(cx, cy)] = (t, f)
    return out


def image(name, G, scores, path):
    import voxel_building as vb
    from PIL import Image, ImageDraw
    Z = 3
    im = Image.new("RGB", (G["CW"] * 16 * Z, G["CH"] * 16 * Z))
    for lid, (ox, oy) in G["members"].items():
        A = vb.LayoutArt(lid)
        for cy in range(A.h):
            for cx in range(A.w):
                im.paste(A.cell_image(A.metatile(cx, cy)).convert("RGB").resize(
                    (16 * Z, 16 * Z), Image.NEAREST), ((ox + cx) * 16 * Z, (oy + cy) * 16 * Z))
    d = ImageDraw.Draw(im)
    S = 16 * Z
    for (cx, cy), (t, f) in scores.items():
        colour = (255, 0, 0) if t >= SEVERE else (255, 150, 0)
        d.rectangle((cx * S, cy * S, cx * S + S - 1, cy * S + S - 1), outline=colour, width=3)
        d.text((cx * S + 3, cy * S + 3), "%d" % round(t), fill=colour)
        d.text((cx * S + 3, cy * S + 14), "".join(k[0] for k, v in f.items() if v > 0.01),
               fill=colour)
    im.save(path)
    return path


# where a cut tile's ground runs on under the rock beside it
# (voxel_mesh_builder.c: kCutFill)
FILL = ((0, 1), (-1, 1), (1, 1), (-1, 0), (1, 0))
SUN = (0.85, 0.55)     # voxel_lighting.h: VOXEL_SUN_DX, VOXEL_SUN_DZ
AMBIENT = 0.70


HOLE = (255, 0, 255)


def view(G, at, path, pitch=40.0, distance=9.0, radius=9, scale=2, yaw=0.0, ortho=False, plan=False,
         distort=False):
    """The group's relief round cell `at` as the game's camera sees it
    (voxel_camera.c: 40 degrees, fov 35), lit by its faces as the game's
    relief is (voxel_lighting.c), every point of the art at (u, h, v + h).
    Under it all, a plane of HOLE: whatever of it shows is a hole in the
    terrain, seen through to nothing (the game's clear colour, black).
    Returns (path, how many pixels of hole)."""
    import voxel_building as vb
    from PIL import Image
    W, Hh = G["CW"] * 16, G["CH"] * 16
    tex = Image.new("RGBA", (W, Hh), (0, 0, 0, 255))
    for lid, (ox, oy) in G["members"].items():
        A = vb.LayoutArt(lid)
        for cy in range(A.h):
            for cx in range(A.w):
                # opaque, as the atlas composes a metatile (voxel_atlas.c):
                # only a cut tile's background is clear
                tex.paste(A.cell_image(A.metatile(cx, cy)).convert("RGB"), ((ox + cx) * 16, (oy + cy) * 16))
    # the cut tiles (gen_voxel_relief.py): their rock alone, below the rest
    cuts = G.get("cuts", {})
    full = tex
    tex = Image.new("RGBA", (W, Hh * 2), (0, 0, 0, 255))
    tex.paste(full, (0, 0))
    tex.paste(full, (0, Hh))
    px = tex.load()
    where = {}      # metatile -> a cell drawing it, for a ground run on under a cell
    for y in range(G["CH"]):
        for x in range(G["CW"]):
            where.setdefault(G["mt"][y][x], (x, y))
    for (cx, cy), (mask, foot, ground) in cuts.items():
        if mask is None:
            continue
        for j in range(16):
            for i in range(16):
                if (mask[j] >> i) & 1:
                    px[cx * 16 + i, Hh + cy * 16 + j] = (0, 0, 0, 0)
    H = G["H"]
    tx, ty = at
    tris = []

    grids = G.get("grids", {})

    def point(i, j, dv=0, cell=None):
        v = H[j][i]
        if cell in grids:
            v = grids[cell][j - cell[1] * 4][i - cell[0] * 4]
        v = 0.0 if v is None else v
        return (i / 4.0, v / 16.0, j / 4.0 + v / 16.0, i * 4, j * 4 + dv)

    def on_foot(cx, cy, a, b, foot):
        # the cell's own grid, as the engine has it (EmitRelief's onFloor)
        own = grids.get((cx, cy))
        at = (lambda i, j: own[j][i]) if own else (lambda i, j: H[cy * 4 + j][cx * 4 + i] or 0.0)
        return all(at(a + ii, b + jj) <= foot + 0.01 for ii in (0, 1) for jj in (0, 1))

    tags, tag = [], None
    for cy in range(max(0, ty - radius), min(G["CH"], ty + radius)):
        for cx in range(max(0, tx - radius - 3), min(G["CW"], tx + radius + 3)):
            tags += [tag] * (len(tris) - len(tags))
            tag = ("cell", cx, cy)
            if G["mt"][cy][cx] is None:
                # off every map: no relief, only something to stop the eye
                tag = ("void", cx, cy)
                q = [(cx + a, 0.0, cy + b, cx * 16 + a * 15, cy * 16 + b * 15)
                     for (a, b) in ((0, 0), (1, 0), (1, 1), (0, 1))]
                tris += [((q[0], q[1], q[2]), 1.0), ((q[0], q[2], q[3]), 1.0)]
                continue
            dv = 0
            if (cx, cy) in cuts:
                # (voxel_mesh_builder.c) a cut tile lies whole and flat at its
                # foot under its relief, its rock alone, and the ground behind
                # it runs on a pixel lower under the raised cells beside it
                mask, foot, ground = cuts[(cx, cy)]
                f = foot / 16.0
                if mask is None:
                    # the ground behind a rim, two pixels under its level
                    src = where.get(ground)
                    tags += [tag] * (len(tris) - len(tags))
                    tag = ("fill", cx, cy)
                    if src is not None:
                        g_, e = f - 2 / 16.0, 2 / 16.0     # two pixels past the cell
                        q = [(cx + a + (2 * a - 1) * e, g_, cy + b + (2 * b - 1) * e + g_,
                              (src[0] + a) * 16, (src[1] + b) * 16)
                             for (a, b) in ((0, 0), (1, 0), (1, 1), (0, 1))]
                        tris += [((q[0], q[1], q[2]), 1.0), ((q[0], q[2], q[3]), 1.0)]
                    mask = "rim"
                    tags += [tag] * (len(tris) - len(tags))
                    tag = ("cell", cx, cy)
                dv = Hh if mask != "rim" else 0
                for b4 in range(4 if mask != "rim" else 0):
                    for a4 in range(4):
                        q = [(cx + (a4 + a) / 4.0, f, cy + (b4 + b) / 4.0 + f,
                              cx * 16 + (a4 + a) * 4, cy * 16 + (b4 + b) * 4)
                             for (a, b) in ((0, 0), (1, 0), (1, 1), (0, 1))]
                        tris += [((q[0], q[1], q[2]), 1.0), ((q[0], q[2], q[3]), 1.0)]
                src = where.get(ground) if ground is not None and mask != "rim" else None
                tags += [tag] * (len(tris) - len(tags))
                tag = ("fill", cx, cy)
                for (ex, ey) in FILL:
                    nx, ny = cx + ex, cy + ey
                    if src is None or not (0 <= nx < G["CW"] and 0 <= ny < G["CH"]):
                        continue
                    nb = [H[ny * 4 + j][nx * 4 + i] for j in range(5) for i in range(5)]
                    if None in nb or max(nb) <= foot:
                        continue
                    g_, e = f - 1 / 16.0, 2 / 16.0
                    q = [(nx + a + (2 * a - 1) * e, g_, ny + b + (2 * b - 1) * e + g_,
                          (src[0] + a) * 16, (src[1] + b) * 16)
                         for (a, b) in ((0, 0), (1, 0), (1, 1), (0, 1))]
                    tris += [((q[0], q[1], q[2]), 1.0), ((q[0], q[2], q[3]), 1.0)]
                tags += [tag] * (len(tris) - len(tags))
                tag = ("cell", cx, cy)
            for j in range(cy * 4, cy * 4 + 4):
                for i in range(cx * 4, cx * 4 + 4):
                    cell = (cx, cy)
                    a, b, c, d = (point(i, j, dv, cell), point(i + 1, j, dv, cell),
                                  point(i + 1, j + 1, dv, cell), point(i, j + 1, dv, cell))
                    if dv and cuts[(cx, cy)][0] is not None and on_foot(cx, cy, i - cx * 4, j - cy * 4, cuts[(cx, cy)][1]):
                        continue    # on the flat ground already (voxel_mesh_builder.c)
                    for tri in ((a, b, c), (a, c, d)):
                        p0, p1, p2 = tri
                        e1 = [p1[k] - p0[k] for k in range(3)]
                        e2 = [p2[k] - p0[k] for k in range(3)]
                        n = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                             e1[0] * e2[1] - e1[1] * e2[0]]
                        if n[1] < 0:
                            n = [-k for k in n]
                        ln = math.sqrt(sum(k * k for k in n)) or 1.0
                        f = (n[1] - SUN[0] * n[0] - SUN[1] * n[2]) / ln
                        shade = AMBIENT if f <= 0 else min(1.0, AMBIENT + (1 - AMBIENT) * f)
                        tris.append((tri, shade))
    # the cliffs (voxel_mesh_builder.c: EmitCliffWalls), drawn with the face
    def cellgrid(c):
        if c in grids:
            return grids[c]
        if not (0 <= c[0] < G["CW"] and 0 <= c[1] < G["CH"]):
            return None
        return [[H[c[1] * 4 + j][c[0] * 4 + i] or 0.0 for i in range(5)] for j in range(5)]
    for (cx, cy), face in G.get("walls", {}).items():
        tags += [tag] * (len(tris) - len(tags))
        tag = ("wall", cx, cy)
        if not (ty - radius <= cy < ty + radius and tx - radius - 3 <= cx < tx + radius + 3):
            continue
        src = where.get(face) or (0, 0)
        g0 = cellgrid((cx, cy))
        if g0 is None:
            continue
        for (dx, dy) in ((-1, 0), (1, 0), (0, 1)):      # EmitCliffWalls
            o = cellgrid((cx + dx, cy + dy))
            if o is None:
                continue
            for k in range(4):
                if dx == -1:
                    ours, theirs = ((0, k), (0, k + 1)), ((4, k), (4, k + 1))
                elif dx == 1:
                    ours, theirs = ((4, k), (4, k + 1)), ((0, k), (0, k + 1))
                else:
                    ours, theirs = ((k, 4), (k + 1, 4)), ((k, 0), (k + 1, 0))
                (ia, ja), (ib, jb) = ours
                (ic, jc), (id_, jd) = theirs
                ta, tb = g0[ja][ia], g0[jb][ib]
                ba, bb = min(ta, o[jc][ic]), min(tb, o[jd][id_])
                if ta - ba <= 0 and tb - bb <= 0:
                    continue

                def P_(ii, jj, hgt, c):
                    return (c[0] + ii / 4.0, hgt / 16.0, c[1] + jj / 4.0 + hgt / 16.0)
                A = P_(ia, ja, ta, (cx, cy)); B = P_(ib, jb, tb, (cx, cy))
                C = P_(ic, jc, ba, (cx + dx, cy + dy)); D = P_(id_, jd, bb, (cx + dx, cy + dy))
                if dy and face == 0xFFFE:
                    continue            # no face in the map: no south cliff
                if dy:
                    # south: the face, a level of it a level of the wall
                    u0 = src[0] * 16 + k * 4
                    va, vb_ = src[1] * 16 + min(16, ta - ba), src[1] * 16 + min(16, tb - bb)
                    q = [A + (u0, src[1] * 16), B + (u0 + 4, src[1] * 16), D + (u0 + 4, vb_), C + (u0, va)]
                else:
                    # west, east: the cell's own edge column drawn down
                    half = Hh if (cx, cy) in cuts and cuts[(cx, cy)][0] is not None else 0
                    ue = cx * 16 + (0.5 if dx < 0 else 15.5)
                    v0_, v1_ = half + cy * 16 + k * 4, half + cy * 16 + k * 4 + 4
                    q = [A + (ue, v0_), B + (ue, v1_), D + (ue, v1_), C + (ue, v0_)]
                shade = 1.0 if dx else 0.60     # west, east: the drawing's own light
                tris += [((q[0], q[1], q[2]), shade), ((q[0], q[2], q[3]), shade)]
    tags += [tag] * (len(tris) - len(tags))
    h0 = H[ty * 4 + 2][tx * 4 + 2] or 0.0
    if ortho:
        return _ortho(G, at, path, tris, tex, radius)
    if plan:
        return _plan(G, at, path, tris, tex, radius)
    cam = vb.Camera((tx + 0.5, h0 / 16.0, ty + 0.5 + h0 / 16.0), pitch=pitch, yaw=yaw, distance=distance)
    low = min(v for row in H for v in row if v is not None) / 16.0 - 2.0
    x0, x1 = max(0, tx - radius - 3), min(G["CW"], tx + radius + 3)
    y0, y1 = max(0, ty - radius), min(G["CH"], ty + radius)
    under = Image.new("RGBA", (1, 1), HOLE + (255,))
    # no further south than the skirt below (a point stands as far south as
    # it is high: the sea under the base, north of the cells' edge)
    south = min(point(i, y1 * 4)[2] for i in range(x0 * 4, x1 * 4 + 1))
    q = [(x, low, z, 0, 0) for (x, z) in ((x0, y0 - 2), (x1, y0 - 2), (x1, south), (x0, south))]
    floor = [((q[0], q[1], q[2]), 1.0), ((q[0], q[2], q[3]), 1.0)]
    # where the relief drawn ends, a skirt down to the plane: what is past
    # the edge is no hole
    skirt = []
    edges = ([(i, y1 * 4) for i in range(x0 * 4, x1 * 4)], [(x0 * 4, j) for j in range(y0 * 4, y1 * 4)],
             [(x1 * 4, j) for j in range(y0 * 4, y1 * 4)])
    for k, edge in enumerate(edges):
        for (i, j) in edge:
            a = point(i, j)
            b = point(i + 1, j) if k == 0 else point(i, j + 1)
            qa = [a, b, (b[0], low, b[2], 0, 0), (a[0], low, a[2], 0, 0)]
            skirt += [((qa[0], qa[1], qa[2]), 0.5), ((qa[0], qa[2], qa[3]), 0.5)]
    grey = Image.new("RGBA", (1, 1), (128, 128, 128, 255))
    for t in range(len(skirt)):
        tri, shade = skirt[t]
        skirt[t] = (tuple((x, y, z, 0, 0) for (x, y, z, _, _) in tri), shade)
    if distort:
        return _distort(cam, path, [(floor, under), (skirt, grey)], tris, tags, tex, scale, pitch, Hh)
    img = vb.render_scene(cam, [(floor, under), (skirt, grey), (tris, tex)], scale=scale)
    if path:
        img.save(path)
    # the middle of the view only: at its edges the plane shows past the
    # relief drawn
    w, h = img.size
    mid = img.crop((w // 4, h // 4, w - w // 4, h - h // 4))
    return path, sum(1 for c in image_pixels(mid) if c == HOLE)


def _legit(pitch):
    """How far the camera at `pitch` may distort a tile's drawing that has
    its true shape: the worst of a top, a band (45 degrees either way), a
    south face (a pixel down a pixel) and the corners between them. Seen at
    exactly 45 degrees every one of them is the drawing itself."""
    worst = 1.0
    for du in (-1.0, 0.0, 1.0):
        for dv in (-1.0, 0.0):
            worst = max(worst, _anisotropy(_oblique(pitch, du, dv)))
    return worst


def _oblique(pitch, du, dv):
    """The drawing's (u, v) to the screen of an orthographic camera at
    `pitch`, for a surface rising du, dv pixels a pixel: x = u, y = the
    point (u, h, v + h) seen from the south, up."""
    p = math.radians(pitch)
    sp, cp = math.sin(p), math.cos(p)
    # screen y of a point: (v + h) sin p - h cos p, h = du u + dv v; against
    # the drawing laid flat (sin p a pixel down)
    return ((1.0, 0.0), (du * (sp - cp) / sp, (sp + dv * (sp - cp)) / sp))


def _anisotropy(m):
    """The ratio of a 2x2 map's singular values (1: no distortion)."""
    (a, b), (c, d) = m
    t = a * a + b * b + c * c + d * d
    det = abs(a * d - b * c)
    disc = max(0.0, t * t - 4 * det * det)
    s1 = math.sqrt((t + math.sqrt(disc)) / 2)
    s2 = math.sqrt(max(1e-12, (t - math.sqrt(disc)) / 2))
    return s1 / s2


def _distort(cam, path, under, tris, tags, tex, scale, pitch, Hh):
    """Every triangle the camera sees, its drawing's distortion on screen
    against the same drawing laid flat where it is - the camera's own
    perspective divided out, so a top is 1 - and against what the tiles'
    true shapes allow at this pitch (_legit). A triangle seen from behind,
    or more distorted than any true shape, is a fault: a stretched outline,
    a wall the tile does not draw, a sliver standing up off the rock.
    Returns (path, [(tag, pixels, distortion)]); the image marks them."""
    import voxel_building as vb
    from PIL import Image
    W, Hgt = cam.width * scale, cam.height * scale
    cam2 = vb.Camera(cam.target, cam.pitch, cam.yaw, cam.distance, cam.fov, W, Hgt)
    ras = vb.Raster(W, Hgt)
    for items, t in under:
        for (tri, shade) in items:
            vs = []
            for (x, y, z, u, v) in tri:
                pr = cam2.project((x, y, z))
                if pr is None:
                    break
                vs.append((pr[0], pr[1], pr[2], pr[3], u * pr[3], v * pr[3]))
            else:
                ras.draw(vs, t, shade, owner=-1)
    proj = []
    for k, (tri, shade) in enumerate(tris):
        vs = []
        for (x, y, z, u, v) in tri:
            pr = cam2.project((x, y, z))
            if pr is None:
                break
            vs.append((pr[0], pr[1], pr[2], pr[3], u * pr[3], v * pr[3]))
        if len(vs) != 3:
            proj.append(None)
            continue
        proj.append(vs)
        ras.draw(vs, tex, shade, owner=k)
    seen = collections.Counter(o for o in ras.owner if o is not None and o >= 0)
    faults = collections.defaultdict(lambda: [0, 0.0])
    bad = set()
    for k, n in seen.items():
        tri = tris[k][0]
        vs = proj[k]
        (x0, y0, _, _, _, _), (x1, y1, _, _, _, _), (x2, y2, _, _, _, _) = vs
        (_, _, _, u0, v0), (_, _, _, u1, v1), (_, _, _, u2, v2) = tri
        du1, dv1, du2, dv2 = u1 - u0, v1 - v0, u2 - u0, v2 - v0
        dt = du1 * dv2 - du2 * dv1
        if abs(dt) < 1e-9:
            continue
        # screen per texel, as drawn
        a = ((x1 - x0) * dv2 - (x2 - x0) * dv1) / dt
        b = ((x2 - x0) * du1 - (x1 - x0) * du2) / dt
        c = ((y1 - y0) * dv2 - (y2 - y0) * dv1) / dt
        d = ((y2 - y0) * du1 - (y1 - y0) * du2) / dt
        # and as the drawing laid flat at the triangle's middle
        cx_ = sum(p[0] for p in tri) / 3
        cy_ = sum(p[1] for p in tri) / 3
        cz_ = sum(p[2] for p in tri) / 3
        e = 1 / 16.0
        pc = cam2.project((cx_, cy_, cz_))
        pu = cam2.project((cx_ + e, cy_, cz_))
        pv = cam2.project((cx_, cy_, cz_ + e))
        if None in (pc, pu, pv):
            continue
        ra, rc = pu[0] - pc[0], pu[1] - pc[1]
        rb, rd = pv[0] - pc[0], pv[1] - pc[1]
        rdet = ra * rd - rb * rc
        if abs(rdet) < 1e-12:
            continue
        # actual = ref . E  =>  E = ref^-1 . actual
        ia, ib, ic, id_ = rd / rdet, -rb / rdet, -rc / rdet, ra / rdet
        E = ((ia * a + ib * c, ia * b + ib * d), (ic * a + id_ * c, ic * b + id_ * d))
        back = (E[0][0] * E[1][1] - E[0][1] * E[1][0]) < 0
        an = _anisotropy(E)
        if tags[k] and tags[k][0] == "fill":
            # the ground run on under the rock, seen: right past a back where
            # the camera looks down more steeply than the drawing (45
            # degrees: the perspective's lower screen); anywhere shallower it
            # shows through a gap in the rock, a notch of the wrong drawing
            fx = sum(p[0] for p in tri) / 3
            fy = sum(p[1] for p in tri) / 3
            fz = sum(p[2] for p in tri) / 3
            ex, ey, ez = cam2.eye
            if math.degrees(math.atan2(ey - fy, math.hypot(ex - fx, ez - fz))) < 44.0:
                f = faults[tags[k]]
                f[0] += n
                bad.add(k)
            continue
        if tags[k] and tags[k][0] in ("cell", "void"):
            if tags[k][0] == "void":
                continue
            # the drawing's own surface: a fault only if it is no true tile
            # shape - steeper than a band across, rising southward, falling
            # faster than a face - however the camera's angle distorts it
            h0_, h1_, h2_ = tri[0][1] * 16, tri[1][1] * 16, tri[2][1] * 16
            sa_ = ((h1_ - h0_) * dv2 - (h2_ - h0_) * dv1) / dt      # dh/du
            sb_ = ((h2_ - h0_) * du1 - (h1_ - h0_) * du2) / dt      # dh/dv
            ex_ = max(0.0, abs(sa_) - SPREAD, sb_, -SPREAD - sb_)
            if ex_ > 0.05:
                f = faults[tags[k]]
                f[0] += n
                f[1] = max(f[1], round(1 + ex_, 2))
                bad.add(k)
            continue
        # the true shapes' distortion seen from here, the perspective's own
        # included (up the screen shallower, down it steeper, to the sides
        # turned): a top, a band either way, a south face, the corners
        limit = 1.0
        for su in (-1.0, 0.0, 1.0):
            for sv in (-1.0, 0.0):
                qu = cam2.project((cx_ + e, cy_ + su * e, cz_ + su * e))
                qv = cam2.project((cx_, cy_ + sv * e, cz_ + e + sv * e))
                if None in (qu, qv):
                    continue
                sa, sc = qu[0] - pc[0], qu[1] - pc[1]
                sb, sd = qv[0] - pc[0], qv[1] - pc[1]
                L = ((ia * sa + ib * sc, ia * sb + ib * sd), (ic * sa + id_ * sc, ic * sb + id_ * sd))
                limit = max(limit, _anisotropy(L))
        limit *= 1.15
        if back or an > limit:
            f = faults[tags[k]]
            f[0] += n
            f[1] = max(f[1], 99.0 if back else an)
            bad.add(k)
    if path:
        img = ras.image()
        px = img.load()
        own = ras.owner
        for i, o in enumerate(own):
            if o in bad:
                px[i % W, i // W] = (255, 0, 255)
        img.save(path)
    # and a hole: the plane under it all, seen through the terrain (the
    # middle of the view only - past its edges no relief is drawn)
    img = ras.image() if not path else img
    holes = 0
    for yy in range(Hgt // 4, Hgt - Hgt // 4):
        for xx in range(W // 4, W - W // 4):
            if ras.owner[yy * W + xx] == -1 and ras.color[yy * W + xx] == HOLE:
                holes += 1
    if holes:
        faults[("hole",) + tuple(int(v) for v in cam.target[::2])] = [holes, 0.0]
    out = sorted(((t, n, round(dist, 2)) for t, (n, dist) in faults.items() if n >= 1),
                 key=lambda r: -r[1])
    return path, out


def _ortho(G, at, path, tris, tex, radius):
    """The relief seen straight along the drawing's own 45 degrees, without
    perspective: every point of the art at (u, h, v + h) lands on (u, v), so
    this must be the drawing itself, pixel for pixel. Returns (path, [(cell,
    pixels that differ)]) against the art; what differs is a fault of the
    model - a hole, the wrong ground, a surface in front of what is drawn."""
    import voxel_building as vb
    from PIL import Image
    tx, ty = at
    r = radius - 3
    x0, y0, n = tx - r, ty - r, 2 * r
    ras = vb.Raster(n * 16, n * 16, bg=HOLE)
    for (tri, shade) in tris:
        vs = []
        for (x, y, z, u, v) in tri:
            vs.append(((x - x0) * 16, (z - y - y0) * 16, y + z, 1.0, u, v))
        ras.draw(vs, tex, 1.0)
    img = ras.image()
    art = Image.new("RGB", (n * 16, n * 16))
    for lid, (ox, oy) in G["members"].items():
        import voxel_building as vb2
        A = vb2.LayoutArt(lid)
        for cy in range(n):
            for cx in range(n):
                X, Y = x0 + cx - ox, y0 + cy - oy
                if 0 <= X < A.w and 0 <= Y < A.h:
                    art.paste(A.cell_image(A.metatile(X, Y)).convert("RGB"), (cx * 16, cy * 16))
    a, b = img.load(), art.load()
    bad = {}
    diff = Image.new("RGB", (n * 16, n * 16))
    d = diff.load()
    for y in range(n * 16):
        for x in range(n * 16):
            if a[x, y] != b[x, y]:
                c = (x0 + x // 16, y0 + y // 16)
                bad[c] = bad.get(c, 0) + 1
                d[x, y] = (255, 0, 255)
            else:
                d[x, y] = tuple(v // 3 for v in b[x, y])
    if path:
        out = Image.new("RGB", (n * 16 * 3, n * 16))
        out.paste(art, (0, 0)); out.paste(img, (n * 16, 0)); out.paste(diff, (n * 32, 0))
        out = out.resize((out.width * 2, out.height * 2), Image.NEAREST)
        out.save(path)
    return path, sorted(bad.items(), key=lambda kv: -kv[1])


def _plan(G, at, path, tris, tex, radius):
    """The relief from straight above. A terrace's top is level, so seen
    from above it is its own drawing moved south as far as it is high (a
    point stands at v + h): every pixel the art draws as a top must be found
    there, its colour and its lines. Returns (path, [(cell, pixels of top
    not where they belong)]); the image is art | plan | the top pixels that
    are missing or moved (magenta)."""
    import voxel_building as vb
    import gen_voxel_relief as g
    from PIL import Image
    tx, ty = at
    r = radius - 3
    x0, y0, n = tx - r, ty - r, 2 * r
    ras = vb.Raster(n * 16, (n + 4) * 16, bg=HOLE)
    for (tri, shade) in tris:
        vs = [((x - x0) * 16, (z - y0) * 16, y, 1.0, u, v) for (x, y, z, u, v) in tri]
        ras.draw(vs, tex, 1.0)
    img = ras.image()
    art = Image.new("RGB", (n * 16, n * 16))
    for lid, (ox, oy) in G["members"].items():
        A = vb.LayoutArt(lid)
        for cy in range(n):
            for cx in range(n):
                X, Y = x0 + cx - ox, y0 + cy - oy
                if 0 <= X < A.w and 0 <= Y < A.h:
                    art.paste(A.cell_image(A.metatile(X, Y)).convert("RGB"), (cx * 16, cy * 16))
    a, b = img.load(), art.load()
    H, grids = G["H"], G.get("grids", {})
    bad = {}
    marks = img.copy()
    m = marks.load()
    for py in range(n * 16):
        for px in range(n * 16):
            c = (x0 + px // 16, y0 + py // 16)
            if not (0 <= c[0] < G["CW"] and 0 <= c[1] < G["CH"]):
                continue
            if c not in G["kinds"] and c not in grids and G["cell"][c[1]][c[0]] is None:
                continue
            col = b[px, py]
            lv = G.get("tops", {}).get(((x0 * 16) + px, (y0 * 16) + py))
            if lv is None:
                if c in G["kinds"] or c in grids:
                    continue        # rock's face: not a top
                gr = [[H[c[1] * 4 + j][c[0] * 4 + i] for i in range(5)] for j in range(5)]
                if None in sum(gr, []):
                    continue
                lv = max(max(row) for row in gr)
            qy = py + int(round(lv))
            if 0 <= qy < (n + 4) * 16 and a[px, qy] != col:
                bad[c] = bad.get(c, 0) + 1
                m[px, qy] = (255, 0, 255)
    if path:
        out = Image.new("RGB", (n * 16 * 3, (n + 4) * 16), (255, 255, 255))
        out.paste(art, (0, 0)); out.paste(img, (n * 16, 0)); out.paste(marks, (n * 32, 0))
        out = out.resize((out.width * 2, out.height * 2), Image.NEAREST)
        out.save(path)
    return path, sorted(bad.items(), key=lambda kv: -kv[1])


def holes(G, pitches=(40.0, 60.0, 25.0), distance=6.0, step=4):
    """Every view round the group's cut tiles that sees a hole:
    [(cell, pitch, pixels)]."""
    cuts = G.get("cuts", {})
    seen, out = set(), []
    for (cx, cy) in sorted(cuts, key=lambda c: (c[1], c[0])):
        at = (cx // step * step + step // 2, cy // step * step + step // 2)
        if at in seen or not (0 <= at[0] < G["CW"] and 0 <= at[1] < G["CH"]):
            continue
        seen.add(at)
        for pitch in pitches:
            _, n = view(G, at, None, pitch=pitch, distance=distance, radius=8, scale=1)
            if n:
                out.append((at, pitch, n))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--groups", default=None)
    ap.add_argument("--save", default=None)
    ap.add_argument("--load", default=None, help="check a saved run instead of solving")
    ap.add_argument("--against", default=None, help="a saved run to compare with")
    ap.add_argument("--images", default=None)
    ap.add_argument("--list", action="store_true", help="every flagged cell")
    ap.add_argument("--view", default=None, help="GROUP:X,Y[:PITCH[:DISTANCE]] - the game's camera on a cell")
    ap.add_argument("--out", default="view.png")
    ap.add_argument("--ortho", default=None, help="GROUP:X,Y - the drawing's own 45 degrees: art | model | diff")
    ap.add_argument("--yaw", type=float, default=0.0, help="--view turned this many degrees")
    ap.add_argument("--distort", default=None,
                    help="GROUP[:X,Y] - the game's cameras (pitch 34..46, 11 cells away) over every cell "
                         "of rock: every triangle more distorted than a true tile shape, or seen from behind")
    ap.add_argument("--plan", default=None,
                    help="GROUP:X,Y - from straight above: every top must be its drawing, moved south by its height")
    ap.add_argument("--sheet", default=None,
                    help="GROUP:X,Y - the art and the cell from six cameras: pitch 40/65, yaw -35/0/35")
    ap.add_argument("--pitches", default=None, help="--distort's pitches (34,40,46)")
    ap.add_argument("--distance", type=float, default=11.0, help="--distort's camera distance, cells")
    ap.add_argument("--holes", action="store_true",
                    help="views round every cut tile: where the terrain is seen through")
    args = ap.parse_args()
    groups = args.groups.split(",") if args.groups else None
    if args.view or args.ortho or args.sheet or args.plan or args.distort:
        parts = (args.view or args.ortho or args.sheet or args.plan or args.distort).split(":")
        groups = [parts[0]]
    snap = pickle.load(open(args.load, "rb")) if args.load else snapshot(groups)
    if groups:
        snap = {k: v for k, v in snap.items() if k in groups}
    old = pickle.load(open(args.against, "rb")) if args.against and os.path.exists(args.against) else {}
    if args.save:
        os.makedirs(os.path.dirname(os.path.abspath(args.save)), exist_ok=True)
        pickle.dump(snap, open(args.save, "wb"))
    if args.distort:
        G = snap[parts[0]]
        def owned(c):
            # a map shared with another group is that group's to export
            return not G.get("owned") or any(ox <= c[0] < ox + w and oy <= c[1] < oy + h
                                             for (ox, oy, w, h) in G["owned"])
        if len(parts) > 1:
            spots = [tuple(int(v) for v in parts[1].split(","))]
        else:
            cells = {c for c in set(G["kinds"]) | set(G.get("grids", {})) if owned(c)}
            spots = sorted({(min(G["CW"] - 1, x // 8 * 8 + 4), min(G["CH"] - 1, y // 6 * 6 + 3))
                            for (x, y) in cells if 0 <= x < G["CW"] and 0 <= y < G["CH"]})
        pitches = [float(v) for v in (args.pitches or "34,40,46").split(",")]
        total = collections.Counter()
        worst = {}
        for at in spots:
            for pitch in pitches:
                tmp = None if not args.images else os.path.join(
                    args.images, "distort_%s_%d_%d_%d.png" % (parts[0], at[0], at[1], pitch))
                if tmp:
                    os.makedirs(args.images, exist_ok=True)
                _, found = view(G, at, tmp, pitch=pitch, distance=args.distance, radius=10,
                                scale=1, distort=True)
                for (t, n, d) in found:
                    if t[0] in ("cell", "wall") and not owned(t[1:]):
                        continue
                    total[t] += n
                    worst[t] = max(worst.get(t, 0), d)
        print("%s: %d spots x %d pitches, %d faulty cells, %d px" % (
            parts[0], len(spots), len(pitches), len(total), sum(total.values())))
        for t, n in total.most_common(40):
            print("   %-22s %5d px  distortion %s" % (t, n, worst[t]))
        return
    if args.sheet:
        from PIL import Image
        import voxel_building as vb
        x, y = (int(v) for v in parts[1].split(","))
        G = snap[parts[0]]
        shots = []
        for pitch in (40.0, 65.0):
            for yaw in (-35.0, 0.0, 35.0):
                tmp = args.out + ".tmp.png"
                view(G, (x, y), tmp, pitch=pitch, distance=7.0, yaw=yaw)
                shots.append(Image.open(tmp).convert("RGB").resize((400, 240)))
        art = Image.new("RGB", (14 * 16, 10 * 16))
        for lid, (ox, oy) in G["members"].items():
            A = vb.LayoutArt(lid)
            for cy in range(10):
                for cx in range(14):
                    X, Y = x - 7 + cx - ox, y - 5 + cy - oy
                    if 0 <= X < A.w and 0 <= Y < A.h:
                        art.paste(A.cell_image(A.metatile(X, Y)).convert("RGB"), (cx * 16, cy * 16))
        out = Image.new("RGB", (1200 + 448, 480), (255, 255, 255))
        out.paste(art.resize((448, 320), Image.NEAREST), (0, 80))
        for k, im in enumerate(shots):
            out.paste(im, (448 + 400 * (k % 3), 240 * (k // 3)))
        out.save(args.out)
        os.remove(tmp)
        print("sheet:", args.out)
        return
    if args.plan:
        x, y = (int(v) for v in parts[1].split(","))
        path, bad = view(snap[parts[0]], (x, y), args.out, plan=True)
        print("plan: %s, %d px of top not where drawn; worst cells %s" % (
            path, sum(k for _, k in bad), bad[:12]))
        return
    if args.ortho:
        x, y = (int(v) for v in parts[1].split(","))
        path, bad = view(snap[parts[0]], (x, y), args.out, ortho=True)
        print("ortho: %s, %d px differ from the art; worst cells %s" % (
            path, sum(n for _, n in bad), bad[:10]))
        return
    if args.view:
        x, y = (int(v) for v in parts[1].split(","))
        pitch = float(parts[2]) if len(parts) > 2 else 40.0
        distance = float(parts[3]) if len(parts) > 3 else 9.0
        print("view: %s, %d px of hole" % view(snap[parts[0]], (x, y), args.out, pitch=pitch,
                                                 distance=distance, yaw=args.yaw))
        return
    if args.holes:
        total = 0
        for name in sorted(snap):
            found = holes(snap[name])
            total += len(found)
            print("%-30s %d views with holes %s" % (name, len(found), found[:12]))
        print("views with holes: %d" % total)
        return
    worse_anywhere = 0
    print("%-30s %5s %5s %8s  %s" % ("group", "rock", "bad", "score", "stretch/north/overhang/twist"))
    for name in sorted(snap):
        G = snap[name]
        s = score(G)
        tot = {k: sum(f[k] for (_, f) in s.values()) for k in ("stretch", "north", "overhang", "twist")}
        line = "%-30s %5d %5d %8.0f  %s" % (name, len(G["kinds"]), len(s), sum(t for t, _ in s.values()),
                                           "/".join("%.0f" % v for v in tot.values()))
        if name in old:
            so = score(old[name])
            worse = sorted(c for c in set(s) | set(so) if s.get(c, (0,))[0] > so.get(c, (0,))[0] + 0.5)
            better = sorted(c for c in set(s) | set(so) if s.get(c, (0,))[0] + 0.5 < so.get(c, (0,))[0])
            line += "   was %.0f: %d cells better, %d worse" % (sum(t for t, _ in so.values()),
                                                             len(better), len(worse))
            if worse:
                worse_anywhere += len(worse)
                line += " " + str(worse[:8])
        print(line)
        if args.list:
            for (cx, cy), (t, f) in sorted(s.items(), key=lambda kv: (kv[0][1], kv[0][0])):
                print("    %3d,%-3d %03x %-6s top %-4s %5.1f  %s" % (
                    cx, cy, G["mt"][cy][cx] or 0, G["kinds"][(cx, cy)], G["top"].get((cx, cy)), t,
                    " ".join("%s %.0f" % (k, v) for k, v in f.items() if v > 0.01)))
        if args.images:
            os.makedirs(args.images, exist_ok=True)
            image(name, G, s, os.path.join(args.images, "check_%s.png" % name))
    if REFERENCE in snap:
        print("reference %s: %d flagged cells" % (REFERENCE, len(score(snap[REFERENCE]))))
    if old:
        print("cells worse than before: %d" % worse_anywhere)


if __name__ == "__main__":
    main()
