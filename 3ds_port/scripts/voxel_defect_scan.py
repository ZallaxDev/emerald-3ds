#!/usr/bin/env python3
"""Find the faults of the voxel models without looking for them by eye.

The proofs of gen_voxel_buildings.py only see the GBA's 45-degree view: a
model can match its drawing pixel for pixel there and still look wrong from
the game's camera, because every face the drawing does not show (sides,
backs, undersides, the real depth) is invented. This scan measures every
triangle of every model placed in a layout and flags what goes wrong on
those invented faces:

    stretch   one texel smeared over several pixels in one direction (a
              column or a row pulled along a face): texels per pixel along
              the face's weaker axis below --stretch (default 0.4)
    ground    a face standing up (not a top) textured with the ground round
              the model (grass, paving, water): the drawing's ground leaking
              into a wall
    holes     a face with many transparent texels: seen through from the
              game's camera, though at 45 degrees something hid it
    steps     tiny facets: a curved object built in stair steps (boats,
              bushes); scored per model as small triangles per square pixel
              of surface
    thin      a part narrower than --thin pixels on the ground plan and
              taller than it is wide modelled as a box: from the game's
              camera its sides repeat its front (posts, legs)

A room's pieces (gen_voxel_buildings.interior_specs) are judged further, by
what their spec's "lint" knows of them - which pixels are theirs, what they
stand on, what their sides are dressed with:

    stripes   a side dressed with a stretch of the front that changes colour
              column after column: laid along the side, a barcode (the
              television's side, a chair's back)
    copied    a side dressed with a stretch of the front that carries its
              drawing - outline lines across or down it: the drawers seen
              again on the desk's end
    foreign   a hidden face (side, back, the depth behind the drawing)
              reading what is not the piece's own: the floor, the room
              round it, empty texels (fins at a chair's legs)
    dark      a side dressed almost all in one dark colour: a black slab
    under     a front whose lowest rows are the floor, the rug or the shadow
              under a table: the room's floor stood up as a wall
    bleed     a piece standing on another that carries its support's colours
              (the desk's top on the front of a book)
    cut       a support with holes where what stands on it took its pixels
    flat      a piece with no depth at all: a paper plane (a chair's back)
    backdrop  a piece carrying the room round it - the wall seen behind a
              computer's corners: at 45 degrees it lies on the wall, from
              the game's camera it stands on the piece (its top laid with it)
    card_bg   a card (a plant) carrying the room's wall or floor
    floating  a piece lifted onto nothing

Cards are leaves on a transparent card: their holes are not faults.

    python voxel_defect_scan.py LAYOUT [--cache FILE] [--out DIR] [--only P,P]
                                [--view name:cx,cz,pitch,yaw,dist ...]
                                [--console [--pitches 34,40,46]]

Writes DIR/report.txt (every model, its flagged faces by kind and tag) and,
for every --view, DIR/<name>.png (the scene from the game's camera) and
DIR/<name>_flags.png (the same with every flagged face tinted: red stretch,
blue ground, magenta holes, yellow steps, cyan thin; orange stripes, sand
copied, green foreign, grey dark, brown under, lime bleed, violet cut, pink flat, sky
backdrop, teal card_bg, white floating). With no --view, one view per model placed, framed
on it. --console: the follow camera of src/voxel/voxel_camera.c instead -
yaw 0, the player's pitches, the distance it gives the layout's size - from
six places in the layout, as a room is reviewed. --only builds only the
models whose names start with the prefixes (a room that stands another
room's pieces, May's house, needs both).

--cache keeps the built models (pickled) so that a second scan does not
rebuild them; delete it after changing a spec.
"""

import argparse
import json
import math
import os
import sys
import types
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_voxel_buildings as g  # noqa: E402
import voxel_cache
import voxel_relief_fixes
import voxel_building as vb  # noqa: E402
from PIL import Image  # noqa: E402

TINT = {"stretch": (255, 40, 40), "ground": (40, 120, 255), "holes": (255, 0, 255),
        "steps": (255, 220, 0), "thin": (0, 230, 230),
        "stripes": (255, 140, 0), "foreign": (0, 200, 90), "dark": (110, 110, 110),
        "under": (150, 80, 20), "bleed": (170, 255, 0), "cut": (140, 60, 255),
        "flat": (255, 120, 200), "copied": (255, 210, 120), "backdrop": (120, 200, 255), "card_bg": (0, 150, 150),
        "floating": (255, 255, 255)}

# the faces of a room's piece no 45-degree proof sees
HIDDEN = (".flank", ".back", "~depth", ".riser")
STRIPED_ROWS = 0.25      # of a side tile's rows changing colour every other pixel
FOREIGN = 0.05           # of the texels of a piece's hidden faces
UNDER = 0.25             # of its front's four lowest rows
BLEED = (0.10, 24)       # of its pixels, or pixels
CUT = 16                 # pixels taken out of a support
CARD_BG = 0.05           # of a card's pixels
BACKDROP = (0.03, 12)    # of a piece's pixels, or pixels
DARK = 0.6               # of the piece's mean luminance


def _sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def _dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _len(a):
    return math.sqrt(_dot(a, a))


def tri_frame(tri):
    """The triangle's area (px^2), unit normal and the singular values of its
    texture map (texels per pixel along the face's two principal axes)."""
    p = [t[:3] for t in tri]
    uv = [t[3:5] for t in tri]
    e1, e2 = _sub(p[1], p[0]), _sub(p[2], p[0])
    n = _cross(e1, e2)
    area2 = _len(n)
    if area2 < 1e-9:
        return 0.0, (0.0, 1.0, 0.0), (0.0, 0.0)
    n = tuple(c / area2 for c in n)
    # an orthonormal frame on the face
    l1 = _len(e1)
    ax = tuple(c / l1 for c in e1)
    ay = _cross(n, ax)
    a = (l1, 0.0)
    b = (_dot(e2, ax), _dot(e2, ay))
    du1, dv1 = uv[1][0] - uv[0][0], uv[1][1] - uv[0][1]
    du2, dv2 = uv[2][0] - uv[0][0], uv[2][1] - uv[0][1]
    det = a[0] * b[1] - a[1] * b[0]
    if abs(det) < 1e-9:
        return area2 / 2, n, (0.0, 0.0)
    # J = dUV / dXY on the face
    j11 = (du1 * b[1] - du2 * a[1]) / det
    j12 = (du2 * a[0] - du1 * b[0]) / det
    j21 = (dv1 * b[1] - dv2 * a[1]) / det
    j22 = (dv2 * a[0] - dv1 * b[0]) / det
    # singular values of a 2x2 matrix
    s = j11 * j11 + j12 * j12 + j21 * j21 + j22 * j22
    d = abs(j11 * j22 - j12 * j21)
    r = math.sqrt(max(s * s / 4 - d * d, 0.0))
    s1 = math.sqrt(max(s / 2 + r, 0.0))
    s2 = math.sqrt(max(s / 2 - r, 0.0))
    return area2 / 2, n, (s1, s2)


def tri_texels(tri, art_px, size, samples=6):
    """The art's texels the triangle covers, sampled on a barycentric grid."""
    W, H = size
    out = []
    for i in range(samples + 1):
        for j in range(samples + 1 - i):
            a, b = (i + 0.5) / (samples + 1.5), (j + 0.5) / (samples + 1.5)
            c = 1 - a - b
            u = a * tri[0][3] + b * tri[1][3] + c * tri[2][3]
            v = a * tri[0][4] + b * tri[1][4] + c * tri[2][4]
            tu, tv = int(math.floor(u)), int(math.floor(v))
            if 0 <= tu < W and 0 <= tv < H:
                out.append(art_px[tu, tv])
            else:
                out.append((0, 0, 0, 0))
    return out


def ground_colours(layout, taken):
    """The colours of the layout's own ground: every cell no model takes
    and nothing stands on (no upper layer: a tree's crown is not ground),
    its commonest colours (grass, paving, sand, water)."""
    count = Counter()
    for y in range(layout.h):
        for x in range(layout.w):
            m = layout.metatile(x, y)
            if (x, y) in taken or any(i for block in layout.subtiles(m, 1) for _, i in block):
                continue
            for block in layout.subtiles(m, 0):
                for rgb, _ in block:
                    count[rgb] += 1
    total = sum(count.values())
    keep, acc = set(), 0
    for c, k in count.most_common():
        if acc > 0.85 * total:
            break
        keep.add(c)
        acc += k
    return keep


def scan_model(m, ground, opt):
    """Flags for every triangle of a model: {index: set(kinds)}, its step
    score, and what a room's piece was found with (kind, text)."""
    flags = defaultdict(set)
    art_px = m.art.load()
    size = m.art.size
    lint = getattr(m, "lint", None)
    # a room's walls are often the floor's own colours: no leak there
    shell = lint is not None and lint["kind"] == "shell"
    small, surface = 0, 0.0
    by_tag = defaultdict(list)
    for k, (tri, shade, tag) in enumerate(m.mesh.tris):
        if "~depth" in tag or "~behind" in tag:
            continue
        area, n, (s1, s2) = tri_frame(tri)
        if area <= 0:
            continue
        surface += area
        if area < opt.tiny:
            small += 1
        by_tag[tag.split("~")[0]].append(k)
        upright = abs(n[1]) < 0.7
        # flat fills (one colour on purpose: a Tent's hips) have no texture
        # map at all, so they are not stretched
        if s1 > 1e-3 and s2 < opt.stretch:
            flags[k].add("stretch")
        tex = tri_texels(tri, art_px, size)
        opaque = [t for t in tex if t[3] >= 128]
        if (tex and len(opaque) < 0.7 * len(tex) and len(opaque) > 0 and upright
                and not tag.endswith(".card")):
            flags[k].add("holes")
        if upright and opaque and not shell and \
                sum(1 for t in opaque if t[:3] in ground) > 0.5 * len(opaque):
            flags[k].add("ground")
    steps = small / max(surface / 256.0, 1e-6)    # small facets per cell of surface
    if steps > opt.steps:
        for k, (tri, _, _) in enumerate(m.mesh.tris):
            if tri_frame(tri)[0] < opt.tiny:
                flags[k].add("steps")
    # thin parts: per tag, the ground-plan box of each connected group
    for tag, ks in by_tag.items():
        for group in _groups(m.mesh.tris, ks):
            xs = [p[0] for k in group for p in m.mesh.tris[k][0]]
            ys = [p[1] for k in group for p in m.mesh.tris[k][0]]
            zs = [p[2] for k in group for p in m.mesh.tris[k][0]]
            wx, wz, hy = max(xs) - min(xs), max(zs) - min(zs), max(ys) - min(ys)
            normals = {_axis(tri_frame(m.mesh.tris[k][0])[1]) for k in group}
            if (min(wx, wz) < opt.thin and max(wx, wz) < 2 * opt.thin and hy > max(wx, wz)
                    and len(normals & {"x", "z"}) == 2):
                for k in group:
                    flags[k].add("thin")
    notes = []
    if lint and not shell:
        notes = scan_piece(m, lint, flags)
    return flags, steps, notes


def _lum(c):
    return 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2]


def striped_rows(art_px, rect):
    """The share of a side tile's rows that change colour (by 40 of
    luminance, or opaque to clear) at every other pixel or more, taken round
    (the tile is laid one after another)."""
    u0, v0, u1, v1 = rect
    w = u1 - u0
    rows = striped = 0
    if w < 2:
        return 0.0
    for v in range(v0, v1):
        cs = [art_px[u, v] for u in range(u0, u1)]
        if all(c[3] < 128 for c in cs):
            continue
        rows += 1
        n = sum(1 for i in range(w)
                if (cs[i][3] >= 128) != (cs[(i + 1) % w][3] >= 128)
                or (cs[i][3] >= 128 and abs(_lum(cs[i]) - _lum(cs[(i + 1) % w])) >= 40))
        striped += n * 2 >= w
    return striped / float(rows) if rows else 0.0


def copied_lines(art_px, rect, mean):
    """The lines of a side tile drawn in a colour far darker than the piece
    (`mean`, its mean luminance: its outline): rows across it one or two
    pixels thick, clear of its first and last rows (its rims; a band of
    another stuff at its foot, the stand under a television, is not a line),
    and columns down it. A stack of books' covers are not outline."""
    u0, v0, u1, v1 = rect
    if v1 - v0 < 3:
        return 0, 0
    floor = DARK * mean

    def dark(u, v):
        c = art_px[u, v]
        return c[3] >= 128 and _lum(c) < floor

    across = [sum(dark(u, v) for u in range(u0, u1)) * 5 >= 3 * (u1 - u0)
              for v in range(v0, v1)]
    rows, v = 0, 0
    while v < len(across):
        if not across[v]:
            v += 1
            continue
        start = v
        while v < len(across) and across[v]:
            v += 1
        rows += start > 0 and v < len(across) and v - start <= 2
    inner = range(v0 + 1, v1 - 1)
    cols = sum(1 for u in range(u0, u1)
               if sum(dark(u, v) for v in inner) * 5 >= 3 * len(inner)) if u1 - u0 >= 3 else 0
    return rows, cols


def scan_piece(m, lint, flags):
    """A room's piece, by what its spec knows of it (gen_voxel_buildings
    room_facts): flags its faces, returns (kind, text) for the report."""
    art_px = m.art.load()
    W, H = m.art.size
    h16 = m.cells[1] * 16
    own, fill = lint["own"], lint["fill"]
    side_foreign = lint.get("side_foreign", frozenset())
    top_foreign = lint.get("top_foreign", frozenset())
    bleed = lint.get("bleed", frozenset())
    side, top = lint.get("side"), lint.get("top")
    notes = []
    tris = m.mesh.tris

    def mark(kind, ks):
        for k in ks:
            flags[k].add(kind)

    def within(rect, u, v):
        return rect is not None and rect[0] <= u < rect[2] and rect[1] <= v < rect[3]

    posts = lint.get("posts", ())

    def foreign(u, v):
        """Not the piece's own: empty, or the room round it - but for a
        table's post, whose sides are its own front, narrow foot and all,
        and a frame's holes, seen through from behind as from before."""
        if any(within(r, u, v) for r in posts):
            return False
        if art_px[u, v][3] < 128 and lint.get("through"):
            return False
        if art_px[u, v][3] < 128 or (u, v) in bleed:
            return True
        if within(side, u, v):
            return (u, v) in side_foreign
        if within(top, u, v):
            return (u, v) in top_foreign
        return v < h16 and (u, v) not in own and (u, v) not in fill

    hidden = [k for k, (_, _, tag) in enumerate(tris)
              if any(h in tag for h in HIDDEN) and "~proj" not in tag]
    flank = [k for k in hidden if ".flank" in tris[k][2]]
    # stripes: the side tile the flanks are dressed with
    if flank and side is not None and striped_rows(art_px, side) >= STRIPED_ROWS:
        mark("stripes", flank)
        notes.append(("stripes", "side tile %s, %d px wide: %d%% of its rows alternate"
                      % (side, side[2] - side[0], 100 * striped_rows(art_px, side))))
    # copied: the side tile carries the front's own drawing - a line of
    # outline across it (a drawer's edge, a door's split, a book's spine):
    # laid along the side, the drawers stand on the desk's end
    body = [_lum(art_px[u, v]) for (u, v) in own if art_px[u, v][3] >= 128]
    if flank and side is not None and body:
        rows, cols = copied_lines(art_px, side, sum(body) / len(body))
        if lint.get("slab_edge"):
            cols = 0    # a table top's edge: its front's hem, outline where cloth meets wood
        if rows or cols:
            mark("copied", flank)
            notes.append(("copied", "side tile %s carries the front's drawing: %d line(s) "
                          "across, %d down" % (side, rows, cols)))
    # dark: a side mostly of one colour much darker than the piece (its
    # outline), dressed or projected - not a dark thing's own dark side
    flanks = [k for k, (_, _, tag) in enumerate(tris) if ".flank" in tag]
    legs = lint.get("legs_side")
    flanks = [k for k in flanks if not (legs is not None and all(
        within(legs, u, v)
        for (u, v) in g.sampled_texels([(p[3], p[4]) for p in tris[k][0]], W, H)))]
    seen = Counter()
    if flank and side is not None:
        # dressed: the tile itself, each of its pixels once (a thin top's
        # rim, laid again under it, is still one row)
        seen.update(art_px[u, v][:3] for v in range(side[1], side[3])
                    for u in range(side[0], side[2]) if art_px[u, v][3] >= 128)
    else:
        # projected: what the sides read
        for k in flanks:
            for (u, v) in g.sampled_texels([(p[3], p[4]) for p in tris[k][0]], W, H):
                if art_px[u, v][3] >= 128:
                    seen[art_px[u, v][:3]] += 1
    if seen and body:
        c, n = seen.most_common(1)[0]
        if n * 2 > sum(seen.values()) and _lum(c) < DARK * sum(body) / len(body):
            mark("dark", flanks)
            notes.append(("dark", "sides %d%% #%02x%02x%02x"
                          % (100 * n / sum(seen.values()), c[0], c[1], c[2])))
    # foreign: what the hidden faces read that is not the piece's
    bad = total = 0
    for k in hidden:
        ts = g.sampled_texels([(p[3], p[4]) for p in tris[k][0]], W, H)
        n = sum(1 for (u, v) in ts if foreign(u, v))
        bad += n
        total += len(ts)
        if ts and n * 4 >= len(ts):
            flags[k].add("foreign")
    if total and bad > FOREIGN * total:
        notes.append(("foreign", "%d%% of its hidden faces' texels are not its own"
                      % (100 * bad / total)))
    else:
        for k in hidden:
            flags[k].discard("foreign")
    fronts = [k for k, (_, _, tag) in enumerate(tris) if tag.endswith(".front")]
    if "under" in lint:
        n, of = lint["under"]
        if of and n > UNDER * of:
            # the whole of it stands to the floor: its front, and the sides
            # and back dressed with it (a tablecloth hanging to the floor)
            mark("under", [k for k, (_, _, tag) in enumerate(tris)
                           if any(f in tag for f in (".front", ".flank", ".back"))])
            notes.append(("under", "%d%% of its front's foot rows are floor, rug or shadow"
                          % (100 * n / of)))
    if bleed and (len(bleed) > BLEED[0] * len(own) or len(bleed) >= BLEED[1]):
        mark("bleed", fronts)
        notes.append(("bleed", "%d px of %s's colours (%d%% of it)"
                      % (len(bleed), lint["support"], 100 * len(bleed) / max(1, len(own)))))
    taken = sum(lint.get("cut", {}).values())
    if taken >= CUT:
        mark("cut", [k for k, (_, _, tag) in enumerate(tris) if ".top" in tag])
        notes.append(("cut", "holes in its top where %s stand%s (%d px)"
                      % (", ".join(sorted(lint["cut"])), "" if len(lint["cut"]) > 1 else "s",
                         taken)))
    pts = [p for (tri, _, tag) in tris if "_floor" not in tag and "~behind" not in tag
           for p in tri]
    cards = [k for k, (_, _, tag) in enumerate(tris) if tag.endswith(".card")]
    if pts and not cards:
        zs, ys = [p[2] for p in pts], [p[1] for p in pts]
        if max(zs) - min(zs) < 1.5 and max(ys) - min(ys) >= 4:
            mark("flat", range(len(tris)))
            notes.append(("flat", "no depth: a plane %.0f px tall" % (max(ys) - min(ys))))
    backdrop = lint.get("backdrop", frozenset())
    if backdrop and (len(backdrop) > BACKDROP[0] * len(own) or len(backdrop) >= BACKDROP[1]):
        # the faces that read it: its drawn top and front, and the depth
        # laid behind with the top's rows
        mark("backdrop", [k for k, (tri, _, tag) in enumerate(tris)
                          if (".top" in tag or tag.endswith(".front"))
                          and g.sampled_texels([(p[3], p[4]) for p in tri], W, H) & backdrop])
        notes.append(("backdrop", "%d px of the room round it (%d%% of it)"
                      % (len(backdrop), 100 * len(backdrop) / max(1, len(own)))))
    if "card_bg" in lint:
        n, of = lint["card_bg"]
        if of and n > CARD_BG * of:
            mark("card_bg", cards)
            notes.append(("card_bg", "%d%% of the card is the room's wall or floor"
                          % (100 * n / of)))
    if lint.get("floating"):
        mark("floating", range(len(tris)))
        notes.append(("floating", "lifted %d px onto nothing" % lint["base"]))
    return notes


def _axis(n):
    a = max(range(3), key=lambda i: abs(n[i]))
    return "xyz"[a]


def _groups(tris, ks):
    """Triangles of `ks` joined by shared vertices."""
    key = lambda p: (round(p[0], 2), round(p[1], 2), round(p[2], 2))  # noqa: E731
    parent = {k: k for k in ks}

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a
    seen = {}
    for k in ks:
        for p in tris[k][0]:
            q = key(p)
            if q in seen:
                parent[find(k)] = find(seen[q])
            else:
                seen[q] = k
    out = defaultdict(list)
    for k in ks:
        out[find(k)].append(k)
    return list(out.values())


def render(cam, ground_item, placed, scale=2):
    """The scene, and the same with the flagged faces tinted."""
    ras = vb.Raster(cam.width * scale, cam.height * scale)
    cam2 = vb.Camera(cam.target, cam.pitch, cam.yaw, cam.distance, cam.fov,
                     cam.width * scale, cam.height * scale)

    def draw(tris, tex, owner_of):
        for k, (tri, shade) in enumerate(tris):
            vs = []
            for (x, y, z, u, v) in tri:
                pr = cam2.project((x, y, z))
                if pr is None:
                    break
                sx, sy, d, iw = pr
                vs.append((sx, sy, d, iw, u * iw, v * iw))
            else:
                ras.draw(vs, tex, shade, owner_of(k))
    draw(ground_item[0], ground_item[1], lambda k: None)
    for i, (tris, tex, flags) in enumerate(placed):
        draw(tris, tex, lambda k, i=i: (i, k))
    plain = ras.image()
    tinted = []
    for idx, col in enumerate(ras.color):
        o = ras.owner[idx]
        kinds = o and placed[o[0]][2].get(o[1])
        if kinds:
            t = TINT[sorted(kinds, key=list(TINT).index)[0]]
            col = tuple((c + 2 * tc) // 3 for c, tc in zip(col, t))
        tinted.append(col)
    img = Image.new("RGB", (ras.w, ras.h))
    img.putdata(tinted)
    return plain, img


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("layout")
    ap.add_argument("--cache", default=None)
    ap.add_argument("--out", default=os.path.join(g.PORT, "build", "defects"))
    ap.add_argument("--view", action="append", default=[])
    ap.add_argument("--stretch", type=float, default=0.4)
    ap.add_argument("--tiny", type=float, default=3.0)
    ap.add_argument("--steps", type=float, default=6.0)
    ap.add_argument("--thin", type=float, default=8.0)
    ap.add_argument("--only", default=None, help="comma-separated model name prefixes")
    ap.add_argument("--console", action="store_true",
                    help="views from the game's follow camera over the whole layout")
    ap.add_argument("--pitches", default="40", help="the --console views' pitches, e.g. 34,40,46")
    opt = ap.parse_args()
    os.makedirs(opt.out, exist_ok=True)
    g.DIAGNOSTICS = opt.out
    layout = vb.LayoutArt(opt.layout)
    key = (opt.layout, opt.only)
    cached = None
    if opt.cache:
        key += (voxel_cache.fingerprint(vb.ROOT, g.PORT, voxel_relief_fixes.FIXES),)
        cached = voxel_cache.read(opt.cache, key)
    if cached is None:
        lj = json.load(open(os.path.join(vb.ROOT, "data", "layouts", "layouts.json"),
                            encoding="utf-8"))["layouts"]
        found = []
        for m in g.build_models(tuple(opt.only.split(",")) if opt.only else None):
            at = [(px, py, gm) for _, px, py, gm, name, odd in g.find_placements(m, lj)
                  if name == opt.layout]
            if at:
                # what the scan needs of a model, without its spec (lambdas)
                slim = types.SimpleNamespace(name=m.name, cells=m.cells, art=m.art,
                                             mesh=types.SimpleNamespace(tris=m.mesh.tris),
                                             lint=m.spec.get("lint"))
                found.append((slim, at))
        cached = found
        if opt.cache:
            voxel_cache.write(opt.cache, key, cached)
    override, placements, taken = {}, [], set()
    for m, at in cached:
        w, h = m.cells
        for px, py, gm in at:
            for j in range(h):
                for i in range(w):
                    taken.add((px + i, py + j))
                    if gm != g.OWN_GROUND:
                        override[(px + i, py + j)] = gm
            placements.append((m, px, py))
    ground = ground_colours(layout, taken)
    report, placed, scanned = [], [], {}
    allowed, _ = g.lint_allowed()
    for m, px, py in placements:
        if m.name not in scanned:
            scanned[m.name] = scan_model(m, ground, opt)
        flags, steps, notes = scanned[m.name]
        placed.append((vb.model_world_tris(m, px, py), m.art, flags))
    for name, (flags, steps, notes) in sorted(scanned.items()):
        kinds = Counter(k for ks in flags.values() for k in ks)
        if not kinds and not notes:
            continue
        m = next(m for m, _, _ in placements if m.name == name)
        tags = defaultdict(Counter)
        for k, ks in flags.items():
            for kind in ks:
                tags[kind][m.mesh.tris[k][2]] += 1
        at = ["%d,%d" % (px, py) for mm, px, py in placements if mm.name == name]
        report.append("%s  at %s  steps %.1f" % (name, " ".join(at[:6]), steps))
        for kind, n in kinds.most_common():
            report.append("    %-8s %4d tris: %s" % (kind, n, ", ".join(
                "%s(%d)" % t for t in tags[kind].most_common(5))))
        for kind, text in notes:
            reason = allowed.get(name, {}).get(kind)
            report.append("      %-8s %s%s" % (kind, text, " (allowed: %s)" % reason if reason else ""))
    # what the allowlist accepts is listed, not counted
    faults = {name: [kind for kind, _ in notes if not allowed.get(name, {}).get(kind)]
              for name, (_, _, notes) in scanned.items()}
    if any(faults.values()):
        pieces = sorted(name for name, kinds in faults.items() if kinds)
        report.append("room: %d piece(s) with interior faults: %s"
                      % (len(pieces), Counter(k for kinds in faults.values()
                                              for k in kinds).most_common()))
    with open(os.path.join(opt.out, "report.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(report) + "\n")
    print("\n".join(report))
    gt, gtex = vb.ground_tris(layout, 0, 0, layout.w, layout.h, override, None)
    views = opt.view
    if opt.console:
        # the follow camera (voxel_camera.c AdaptDistance, zoom 100%) on six
        # places of the layout, as a room is walked
        dist = 8.0 + min(5.0, max(layout.w, layout.h) * 0.2)
        for fz in (0.35, 0.75):
            for fx in (0.2, 0.5, 0.8):
                for pitch in opt.pitches.split(","):
                    views.append("console_%d_%d_p%s:%.1f,%.1f,%s,0,%.1f"
                                 % (int(fx * 100), int(fz * 100), pitch,
                                    layout.w * fx, layout.h * fz, pitch, dist))
    if not views:
        done = set()
        for m, px, py in placements:
            if m.name in done or not scanned[m.name][0]:
                continue
            done.add(m.name)
            w, h = m.cells
            views.append("%s:%.1f,%.1f,40,0,%.1f" % (m.name, px + w / 2, py + h / 2,
                                                       max(4.0, 1.4 * max(w, h))))
    for spec in views:
        name, rest = spec.split(":")
        cx, cz, pitch, yaw, dist = map(float, rest.split(","))
        cam = vb.Camera((cx, 0.0, cz), pitch=pitch, yaw=yaw, distance=dist)
        plain, tinted = render(cam, (gt, gtex), placed)
        plain.save(os.path.join(opt.out, name + ".png"))
        tinted.save(os.path.join(opt.out, name + "_flags.png"))
        print(os.path.join(opt.out, name + "_flags.png"))


if __name__ == "__main__":
    main()
