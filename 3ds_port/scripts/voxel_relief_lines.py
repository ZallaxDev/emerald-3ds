#!/usr/bin/env python3
"""The mountains as the game builds them, checked against the drawing's lines.

The terrain is the C emitter's own (tests/voxel_mesh_dump.c runs
voxel_mesh_builder.c over a whole layout with the shipped relief.bin), so
what is judged here is the geometry the console draws, not a model of it.

The drawing is the 45-degree view. Its outlines - where rock meets the sea
or the sand, where a top meets its flank or its face - are straight runs of
pixels. Every pixel of the drawing stands in the game at (u, h, v + h), so
the camera must show each of those lines where its pixels are lifted to:
an unbroken line, with the drawing on either side of it as the art has.
A run of an outline the game breaks (a notch of sea in a flank), hides
(rock lifted in front of it) or moves off its neighbours (a tooth) is a
fault the eye catches at once, and is found here pixel by pixel.

    python voxel_relief_lines.py LAYOUT_ROUTE105 --at 32,27 --out v.png
        the game's camera on cell 32,27: render | the lines over it
    python voxel_relief_lines.py LAYOUT_ROUTE105 --scan
        every outline run of the map checked from the cameras the player has
    --relief build/x.bin     a trial export (gen_voxel_relief.py --layouts
                             A,B --output build/x.bin) instead of the shipped one
    --dump OTHER.exe         another build of the dump (an older engine)

The dump is built on the host, from 3ds_port (msys2 mingw64 gcc):

    gcc -std=c99 -O2 -Iinclude -Isrc/voxel -D'PORT_LOG(...)=((void)0)'         -DVOXEL_HOST_FILES -DVOXEL_BUILDINGS_PATH='"romfs/voxel/buildings.bin"'         -DVOXEL_RELIEF_PATH='(getenv("VOXEL_RELIEF") ? getenv("VOXEL_RELIEF") : "romfs/voxel/relief.bin")'         -DSIGN_MASK_PATH='"romfs/voxel/signposts.bin"' tests/voxel_mesh_dump.c         src/voxel/voxel_mesh_builder.c src/voxel/voxel_tree.c src/voxel/voxel_sign.c         src/voxel/voxel_building.c src/voxel/voxel_relief.c src/voxel/voxel_grade.c         src/3ds_video_decode.c -lm -o build/voxel_mesh_dump.exe

Modelled props (buildings.bin: sea rocks, boulders) are another pass the
dump does not run; their cells show magenta and are not judged.
"""

import argparse
import collections
import json
import math
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import voxel_building as vb  # noqa: E402
import voxel_relief_fixes as vrf  # noqa: E402
from PIL import Image  # noqa: E402

GRID = 64                      # tests/voxel_mesh_dump.c: the virtual atlas
METATILE_REAL = 1024           # voxel_atlas.h
VARIANTS = 160
CUT_FIRST = METATILE_REAL + VARIANTS
DUMP = os.path.join(PORT, "build", "voxel_mesh_dump.exe")
RELIEF = os.path.join(PORT, "romfs", "voxel", "relief.bin")


def use_relief(path):
    """Another relief.bin than the shipped one (a trial export of a few
    layouts); the dump reads it from VOXEL_RELIEF."""
    global RELIEF
    RELIEF = os.path.abspath(path)
    os.environ["VOXEL_RELIEF"] = RELIEF
FOV = 35.0


def layouts():
    return json.load(open(os.path.join(vb.ROOT, "data", "layouts", "layouts.json"),
                          encoding="utf-8"))["layouts"]


def dump(layout_id):
    """[(x, y, z, u, v, shade) * 3] of the layout's terrain, as the game emits it."""
    L = layouts()
    entry = next(e for e in L if e["id"] == layout_id)
    index = L.index(entry) + 1
    blocks = vb.read_u16(os.path.join(vb.ROOT, entry["blockdata_filepath"]))
    w, h = entry["width"], entry["height"]
    tmp = os.path.join(PORT, "build", "dump_%s_%d" % (layout_id, os.getpid()))
    with open(tmp + ".in", "wb") as f:
        f.write(struct.pack("<HHH", index, w, h))
        f.write(struct.pack("<%dH" % (w * h), *[b & 0x3FF for b in blocks[:w * h]]))
    r = subprocess.run([os.environ.get("VOXEL_DUMP", DUMP), tmp + ".in", tmp + ".out"], cwd=PORT, capture_output=True, text=True)
    if r.returncode:
        raise SystemExit("voxel_mesh_dump failed (%d): %s" % (r.returncode, r.stderr))
    data = open(tmp + ".out", "rb").read()
    n = struct.unpack_from("<I", data)[0]
    vals = struct.unpack_from("<%df" % (n * 6), data, 4)
    verts = [vals[i * 6:i * 6 + 6] for i in range(n)]
    return [tuple(verts[i:i + 3]) for i in range(0, n - n % 3, 3)], index


def relief_cuts():
    """[(layout, metatile, rows)] of relief.bin's cut variants."""
    blob = open(RELIEF, "rb").read()
    at = struct.unpack_from("<I", blob, len(blob) - 8)[0]
    nv, _ = struct.unpack_from("<HH", blob, at)
    out = []
    for k in range(nv):
        o = at + 4 + 36 * k
        lay, mt = struct.unpack_from("<HH", blob, o)
        rows = struct.unpack_from("<16H", blob, o + 4)
        out.append((lay, mt, rows))
    return out


def building_variants():
    """[(layout, metatile, quarters)] of buildings.bin's ground variants."""
    b = open(os.path.join(PORT, "romfs", "voxel", "buildings.bin"), "rb").read()
    assert b[:4] in (b"VXB8", b"VXB9")
    pages, models, pmodels, places, hbytes, masks = struct.unpack_from("<6H", b, 4)
    nverts, nvar = struct.unpack_from("<IH", b, 16)
    o = (28 if b[:4] == b"VXB9" else 24) + pages * 8 + models * 16 + pmodels * 8 + places * 16
    o += hbytes
    o += o & 1
    o += hbytes * 2 + masks * 32 + hbytes
    o += o & 1
    out = []
    for k in range(nvar):
        lay, mt, q = struct.unpack_from("<HHB", b, o + 6 * k)
        out.append((lay, mt, q))
    return out


class Atlas:
    """The virtual atlas of tests/voxel_mesh_dump.c, composed as voxel_atlas.c
    composes the console's: a metatile id of the layout's tilesets, a ground
    variant (less the quarters a model stands for), a cut variant (its
    background clear)."""

    def __init__(self, layout_id):
        self.art = vb.LayoutArt(layout_id)
        self.L = layouts()
        self.cuts = relief_cuts()
        self.variants = building_variants()
        self.img = Image.new("RGBA", (GRID * 16, GRID * 16), (0, 0, 0, 0))
        self.done = set()
        self._arts = {}

    def _art(self, index):
        if index not in self._arts:
            self._arts[index] = vb.LayoutArt(self.L[index - 1]["id"])
        return self._arts[index]

    def tile(self, aid):
        if aid < METATILE_REAL:
            return self.art.cell_image(aid)
        if aid >= CUT_FIRST:
            k = aid - CUT_FIRST
            if k >= len(self.cuts):
                return None
            lay, mt, rows = self.cuts[k]
            im = self._art(lay).cell_image(mt).copy()
            px = im.load()
            clear = vrf.cut_clear(rows)     # as voxel_atlas.c composes it
            for (i, j), (si, sj) in vrf.cut_fringe(rows, clear).items():
                px[i, j] = px[si, sj]
            for j in range(16):
                for i in range(16):
                    if (clear[j] >> i) & 1:
                        px[i, j] = (0, 0, 0, 0)
            return im
        k = aid - METATILE_REAL
        if k >= len(self.variants):
            return None
        lay, mt, q = self.variants[k]
        A = self._art(lay)
        im = A.cell_image(mt).copy()
        low = A.cell_image(mt, layers=(0,))
        for quarter in range(4):
            if (q >> quarter) & 1:
                box = ((quarter & 1) * 8, (quarter >> 1) * 8, (quarter & 1) * 8 + 8, (quarter >> 1) * 8 + 8)
                im.paste(low.crop(box), box[:2])
        return im

    def need(self, aid):
        if aid in self.done:
            return
        self.done.add(aid)
        t = self.tile(aid)
        if t is not None:
            self.img.paste(t.convert("RGBA"), ((aid % GRID) * 16, (aid // GRID) * 16))


def atlas_id(tri):
    """The atlas id a triangle samples: its UVs' middle, in slots."""
    u = sum(p[3] for p in tri) / 3.0
    v = sum(p[4] for p in tri) / 3.0
    return int(v * GRID) * GRID + int(u * GRID)


class Raster:
    """A z-buffered rasteriser that keeps, per pixel, what it shows: the
    triangle and the texel (in atlas pixels)."""

    def __init__(self, w, h, bg=(0, 0, 0)):
        self.w, self.h = w, h
        self.color = [bg] * (w * h)
        self.depth = [-1e30] * (w * h)
        self.tri = [-1] * (w * h)
        self.tex = [None] * (w * h)

    def draw(self, verts, tex, tpx, shade, owner):
        """`shade` one for the triangle, or one a vertex: interpolated across
        it as the console's GPU interpolates a vertex colour (a cast shadow
        is a vertex's, not a triangle's)."""
        (x0, y0, d0, w0, u0, v0), (x1, y1, d1, w1, u1, v1), (x2, y2, d2, w2, u2, v2) = verts
        if isinstance(shade, (tuple, list)):
            s0, s1, s2 = (s * w for s, w in zip(shade, (w0, w1, w2)))
            flat = None
        else:
            flat = shade
        area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
        if abs(area) < 1e-9:
            return
        minx = max(0, int(math.floor(min(x0, x1, x2))))
        maxx = min(self.w - 1, int(math.ceil(max(x0, x1, x2))))
        miny = max(0, int(math.floor(min(y0, y1, y2))))
        maxy = min(self.h - 1, int(math.ceil(max(y0, y1, y2))))
        tw, th = tex.size
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
                tu, tv = int(math.floor(u)), int(math.floor(v))
                if not (0 <= tu < tw and 0 <= tv < th):
                    continue
                r, g, bb, al = tpx[tu, tv]
                if al < 128:
                    continue
                self.depth[idx] = d
                self.tri[idx] = owner
                self.tex[idx] = (tu, tv)
                sh = flat if flat is not None else (a * s0 + b * s1 + c * s2) / iw
                self.color[idx] = (int(r * sh), int(g * sh), int(bb * sh))

    def image(self):
        img = Image.new("RGB", (self.w, self.h))
        img.putdata(self.color)
        return img


def camera(tris, at, pitch=46.0, distance=13.0, yaw=0.0, w=400, h=240):
    """The game's camera (voxel_camera.c) on cell `at`, at the height of the
    terrain there."""
    cx, cy = at
    best = None
    for tri in tris:
        for (x, y, z, u, v, s) in tri:
            if cx <= x <= cx + 1 and cy <= z - y <= cy + 1:
                best = y if best is None else max(best, y)
    g = best or 0.0
    return vb.Camera((cx + 0.5, g, cy + 0.5 + g), pitch=pitch, yaw=yaw, distance=distance,
                     fov=FOV, width=w, height=h)


def render(tris, atlas, cam, scale=2, box=None):
    W, H = cam.width * scale, cam.height * scale
    cam2 = vb.Camera(cam.target, cam.pitch, cam.yaw, cam.distance, cam.fov, W, H)
    ox, oy = 0, 0
    if box:
        # only this part of the screen (fractions x0, y0, x1, y1)
        ox, oy = int(box[0] * W), int(box[1] * H)
        W, H = int(box[2] * W) - ox, int(box[3] * H) - oy
    ras = Raster(W, H, bg=(255, 0, 255))
    for t in tris:
        atlas.need(atlas_id(t))
    tex = atlas.img
    tpx = tex.load()
    S = GRID * 16
    reach = cam.distance * 2.2
    for k, tri in enumerate(tris):
        if METATILE_REAL <= atlas_id(tri) < CUT_FIRST:
            continue        # under a modelled rock, which the dump does not draw
        mx = sum(p[0] for p in tri) / 3 - cam.target[0]
        mz = sum(p[2] for p in tri) / 3 - cam.target[2]
        if abs(mx) > reach or abs(mz) > reach:
            continue
        vs = []
        for (x, y, z, u, v, s) in tri:
            pr = cam2.project((x, y, z))
            if pr is None:
                break
            sx, sy, d, iw = pr
            vs.append((sx - ox, sy - oy, d, iw, u * S * iw, v * S * iw))
        else:
            ras.draw(vs, tex, tpx, tuple(p[5] for p in tri), k)
    return ras


# The drawing's classes: what an outline separates.
G, T, F, R = "G", "T", "F", "R"     # ground, rock top, rock face/flank, either (the speckle)


def art_classes(art):
    """{(px, py): class} of every pixel of the layout's drawing."""
    import gen_voxel_relief as g
    top = g.ROCK_TOP | g.ROCK_RIM
    face = g.ROCK_FACE
    either = g.ROCK_FLECK
    cache = {}
    out = {}
    for y in range(art.h):
        for x in range(art.w):
            m = art.metatile(x, y)
            if m not in cache:
                im = art.cell_image(m).convert("RGB").load()
                cls = []
                for j in range(16):
                    for i in range(16):
                        c = im[i, j]
                        cls.append(T if c in top else F if c in face else R if c in either else G)
                cache[m] = cls
            cls = cache[m]
            for j in range(16):
                for i in range(16):
                    out[(x * 16 + i, y * 16 + j)] = cls[j * 16 + i]
    return out


def compatible(a, b):
    return a == b or (R in (a, b) and G not in (a, b))


def sources(tris, art, atlas):
    """Per triangle, the cell whose drawing it shows, or None (a ground run
    on under the rock, a fill: drawn with another tile's picture)."""
    cuts = atlas.cuts
    out = []
    for tri in tris:
        aid = atlas_id(tri)
        mx = sum(p[0] for p in tri) / 3
        my = sum(p[1] for p in tri) / 3
        mz = sum(p[2] for p in tri) / 3
        bx, by = int(math.floor(mx)), int(math.floor(mz - my))
        if METATILE_REAL <= aid < CUT_FIRST:
            # a ground variant: under a modelled rock the dump does not
            # draw (the buildings' pass); ground, not judged
            out.append(None)
            continue
        if aid >= CUT_FIRST and aid - CUT_FIRST < len(cuts):
            want = cuts[aid - CUT_FIRST][1]
        elif aid < METATILE_REAL:
            want = aid
        else:
            want = None
        best = None
        if want is not None:
            for dy in (0, -1, 1, -2, 2):
                for dx in (0, -1, 1):
                    x, y = bx + dx, by + dy
                    if 0 <= x < art.w and 0 <= y < art.h and art.metatile(x, y) == want:
                        d = abs(x + 0.5 - mx) + abs(y + 0.5 - (mz - my))
                        if best is None or d < best[0]:
                            best = (d, (x, y))
        out.append(best[1] if best else None)
    return out


def check(ras, tris, src, classes, cam, radius=2):
    """Screen pixels where two classes meet that the drawing never puts
    side by side there (a line the drawing does not have), and the art
    position each pixel shows. Returns (faults {pixel: (a, b)}, art)."""
    W, H = ras.w, ras.h
    pos, cls = [None] * (W * H), [None] * (W * H)
    for idx in range(W * H):
        k = ras.tri[idx]
        if k < 0:
            continue
        s = src[k]
        tu, tv = ras.tex[idx]
        if s is not None:
            a = (s[0] * 16 + tu % 16, s[1] * 16 + tv % 16)
            pos[idx] = a
            cls[idx] = classes.get(a, G)
        else:
            cls[idx] = G

    def support(a, want):
        if a is None:
            return want == G
        for dy in range(-radius, radius + 1):
            for dx in range(-radius, radius + 1):
                c = classes.get((a[0] + dx, a[1] + dy))
                if c is not None and compatible(c, want):
                    return True
        return False

    faults = {}
    for y in range(H):
        for x in range(W):
            i = y * W + x
            if cls[i] is None:
                continue
            for j in ((i + 1) if x + 1 < W else None, (i + W) if y + 1 < H else None):
                if j is None or cls[j] is None or compatible(cls[i], cls[j]):
                    continue
                if support(pos[i], cls[j]) or support(pos[j], cls[i]):
                    continue
                faults[i] = faults[j] = (pos[i] or pos[j])
    return faults, pos, cls


def art_runs(classes, w, h, least=6):
    """The drawing's straight outlines: runs of rock pixels along a column
    with the ground beside them on the same side (a flank's or a face's
    side against the sea), and along a row with the ground above or below
    (a top's back, a face's foot). [(kind, [(px, py), ...])]."""
    rock = lambda c: c is not None and c != G
    runs = []
    # what a line separates: rock from the ground (the outline), and a top
    # from its flank or face (the crest) - the lines a mountain's shape is
    # drawn with, each straight in the drawing and straight in the game
    edges = (("", lambda c: rock(c), lambda c: c == G),
             ("crest ", lambda c: c == T, lambda c: c == F))
    steps = {"v": (0, 1), "h": (1, 0), "d": (1, 1), "a": (-1, 1)}
    for name, inner, outer in edges:
        for side, (dx, dy), alongs in (("W", (-1, 0), "vda"), ("E", (1, 0), "vda"),
                                       ("N", (0, -1), "h"), ("S", (0, 1), "h")):
            edge = {a for a, c in classes.items()
                    if inner(c) and outer(classes.get((a[0] + dx, a[1] + dy)))}
            for along in alongs:
                sx, sy = steps[along]
                seen = set()
                for a in sorted(edge, key=lambda a: (a[1], a[0])):
                    if a in seen or (a[0] - sx, a[1] - sy) in edge:
                        continue
                    run = [a]
                    nxt = (a[0] + sx, a[1] + sy)
                    while nxt in edge:
                        run.append(nxt)
                        nxt = (nxt[0] + sx, nxt[1] + sy)
                    seen.update(run)
                    if len(run) >= least:
                        runs.append((name + side, run))
    return runs


def flat_flanks(ras, tris, src, classes):
    """A band or a face is a slope: its drawing (the face colours) lying on
    a level surface over its cell's foot is the band laid flat on a top - a
    notch in the mountain's line. {art cell: pixels}."""
    level, foot = {}, {}
    for k, tri in enumerate(tris):
        c = src[k]
        if c is None:
            continue
        ys = [p[1] for p in tri]
        level[k] = max(ys) - min(ys) < 1e-4
        foot[c] = min(foot.get(c, 1e9), min(ys))
    # the foot round a cell: the lowest of it and the cells beside it
    low = {c: min(foot.get((c[0] + dx, c[1] + dy), 1e9) for dx in (-1, 0, 1) for dy in (-1, 0, 1))
           for c in foot}
    out = collections.Counter()
    for idx, k in enumerate(ras.tri):
        if k < 0 or not level.get(k) or src[k] is None:
            continue
        c = src[k]
        if tris[k][0][1] < low[c] + 4 / 16.0:
            continue
        tu, tv = ras.tex[idx]
        a = (c[0] * 16 + tu % 16, c[1] * 16 + tv % 16)
        if classes.get(a) == F:
            out[c] += 1
    return {c: n for c, n in out.items() if n >= 40}


def line_faults(runs, pos, W, scale, tol=1.0):
    """Each straight outline of the drawing, where the game shows its pixels:
    a run whose pixels leave a straight line by more than `tol` screen
    pixels (at 400x240) is broken - teeth, a step, a pixel lifted off it.
    Returns [(side, run, worst deviation, [(art px, screen xy, deviation)])],
    worst first; pixels the camera does not see are left out."""
    where = collections.defaultdict(list)
    for i, a in enumerate(pos):
        if a is not None:
            where[a].append((i % W, i // W))
    out = []
    for side, run in runs:
        pts = []
        for t, a in enumerate(run):
            if a in where:
                ps = where[a]
                pts.append((t, a, (sum(p[0] for p in ps) / len(ps), sum(p[1] for p in ps) / len(ps))))
        if len(pts) < max(4, len(run) // 2):
            continue
        # the straight line through them, x and y each linear in t
        n = len(pts)
        mt = sum(p[0] for p in pts) / n
        st = sum((p[0] - mt) ** 2 for p in pts) or 1.0
        fit = []
        for k in (0, 1):
            m = sum(p[2][k] for p in pts) / n
            b = sum((p[0] - mt) * (p[2][k] - m) for p in pts) / st
            fit.append((m, b))
        dev = []
        for (t, a, xy) in pts:
            ex = fit[0][0] + fit[0][1] * (t - mt)
            ey = fit[1][0] + fit[1][1] * (t - mt)
            dev.append((a, xy, math.hypot(xy[0] - ex, xy[1] - ey) / scale))
        worst = max(d for _, _, d in dev)
        if worst > tol:
            out.append((side, run, worst, dev))
    out.sort(key=lambda r: -r[2])
    return out


def overlay(ras, pos, classes, faults, path, broken=()):
    """render | the drawing's outlines where the game puts them (yellow) and
    the lines it adds (red)."""
    img = ras.image()
    lines = img.copy()
    px = lines.load()
    W = ras.w
    for i, a in enumerate(pos):
        if a is None:
            continue
        c = classes.get(a)
        # the outline: rock against the ground
        edge = any((classes.get((a[0] + dx, a[1] + dy), c) == G) != (c == G)
                   for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)))
        if edge:
            px[i % W, i // W] = (255, 230, 0)
    for i in faults:
        px[i % W, i // W] = (255, 0, 0)
    for (side, run, worst, dev) in broken:
        for (a, xy, d) in dev:
            if d > 1.0:
                x, y = int(xy[0]), int(xy[1])
                for yy in range(y - 1, y + 2):
                    for xx in range(x - 1, x + 2):
                        if 0 <= xx < W and 0 <= yy < ras.h:
                            px[xx, yy] = (255, 0, 0)
    out = Image.new("RGB", (W * 2, ras.h))
    out.paste(img, (0, 0))
    out.paste(lines, (W, 0))
    out.save(path)


_SCAN = {}


def _scan_one(job):
    """One camera of a scan (a worker process): its broken outlines and the
    lines it adds, by cell."""
    layout, at, pitch, distance, scale, trial = job
    if trial:
        use_relief(trial)
    if "tris" not in _SCAN:
        _SCAN["tris"], _ = dump(layout)
        _SCAN["atlas"] = Atlas(layout)
        _SCAN["classes"] = art_classes(_SCAN["atlas"].art)
        _SCAN["src"] = sources(_SCAN["tris"], _SCAN["atlas"].art, _SCAN["atlas"])
        A = _SCAN["atlas"].art
        _SCAN["runs"] = art_runs(_SCAN["classes"], A.w * 16, A.h * 16)
    tris, atlas, classes = _SCAN["tris"], _SCAN["atlas"], _SCAN["classes"]
    cam = camera(tris, at, pitch, distance)
    ras = render(tris, atlas, cam, scale)
    faults, pos, cls = check(ras, tris, _SCAN["src"], classes, cam)
    broken = line_faults(_SCAN["runs"], pos, ras.w, scale)
    out = collections.Counter()
    for (side, run, worst, dev) in broken:
        for (a, xy, d) in dev:
            if d > 1.0:
                out[(a[0] // 16, a[1] // 16, "line")] += 1
    for a in faults.values():
        if a:
            out[(a[0] // 16, a[1] // 16, "extra")] += 1
    for c, n in flat_flanks(ras, tris, _SCAN["src"], classes).items():
        out[(c[0], c[1], "flat")] += n // (scale * scale)
    return at, out


def scan(layout, step=5, pitch=46.0, distance=15.0, scale=2, trial=False):
    """Cameras over the whole map, a player every `step` cells: every cell
    whose outline any of them shows broken, or that shows a line the drawing
    does not have. {(x, y): {kind: pixels}}, and the camera that saw most."""
    import multiprocessing
    A = vb.LayoutArt(layout)
    spots = [(x, y) for y in range(2, A.h - 1, step) for x in range(2, A.w - 1, step)]
    jobs = [(layout, at, pitch, distance, scale, trial) for at in spots]
    total = collections.defaultdict(collections.Counter)
    seen_by = {}
    with multiprocessing.Pool(max(1, os.cpu_count() - 2)) as pool:
        for at, out in pool.imap_unordered(_scan_one, jobs):
            for (x, y, kind), n in out.items():
                total[(x, y)][kind] += n
                if n > seen_by.get((x, y), (0, None))[0]:
                    seen_by[(x, y)] = (n, at)
    return total, seen_by


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("layout")
    ap.add_argument("--at", default=None)
    ap.add_argument("--pitch", type=float, default=46.0)
    ap.add_argument("--distance", type=float, default=15.0)
    ap.add_argument("--yaw", type=float, default=0.0)
    ap.add_argument("--out", default="lines.png")
    ap.add_argument("--scale", type=int, default=2)
    ap.add_argument("--box", default=None, help="x0,y0,x1,y1: the part of the screen, in fractions")
    ap.add_argument("--relief", default=None, help="a trial relief.bin, not the shipped one")
    ap.add_argument("--dump", default=None, help="another build of tests/voxel_mesh_dump.c (an older engine)")
    ap.add_argument("--scan", action="store_true", help="cameras over the whole map")
    ap.add_argument("--step", type=int, default=5)
    ap.add_argument("--focus", default=None, help="X,Y: the box round this cell (with --at, the camera)")
    args = ap.parse_args()
    if args.relief:
        use_relief(args.relief)
    if args.dump:
        os.environ["VOXEL_DUMP"] = os.path.abspath(args.dump)
    if args.scan:
        total, seen_by = scan(args.layout, args.step, args.pitch, args.distance, args.scale, args.relief)
        rows = sorted(total.items(), key=lambda kv: -sum(kv[1].values()))
        print("%s: %d cells with faults, %d px" % (args.layout, len(rows),
                                                  sum(sum(v.values()) for _, v in rows)))
        for (c, kinds) in rows[:60]:
            print("  %3d,%-3d line %4d  extra %4d  flat %4d   worst from %s" % (
                c[0], c[1], kinds["line"], kinds["extra"], kinds["flat"], seen_by[c][1]))
        return
    tris, index = dump(args.layout)
    print("%s (layout %d): %d triangles" % (args.layout, index, len(tris)))
    atlas = Atlas(args.layout)
    if args.at:
        at = tuple(int(v) for v in args.at.split(","))
        cam = camera(tris, at, args.pitch, args.distance, args.yaw)
        box = tuple(float(v) for v in args.box.split(",")) if args.box else None
        if args.focus:
            fx, fy = (int(v) for v in args.focus.split(","))
            hy = 0.0
            for tri in tris:
                for (x, y, z, u, v, sh) in tri:
                    if fx <= x <= fx + 1 and fy <= z - y <= fy + 1:
                        hy = max(hy, y)
            pr = cam.project((fx + 0.5, hy, fy + 0.5 + hy))
            if pr:
                cx_, cy_ = pr[0] / cam.width, pr[1] / cam.height
                box = (max(0.0, cx_ - 0.17), max(0.0, cy_ - 0.22), min(1.0, cx_ + 0.17), min(1.0, cy_ + 0.22))
        ras = render(tris, atlas, cam, args.scale, box)
        classes = art_classes(atlas.art)
        src = sources(tris, atlas.art, atlas)
        faults, pos, cls = check(ras, tris, src, classes, cam)
        broken = line_faults(art_runs(classes, atlas.art.w * 16, atlas.art.h * 16), pos, ras.w, args.scale)
        flats = flat_flanks(ras, tris, src, classes)
        if flats:
            print("  flank laid flat:", sorted(flats.items(), key=lambda kv: -kv[1])[:12])
        overlay(ras, pos, classes, faults, args.out, broken)
        for (side, run, worst, dev) in broken[:20]:
            print("  %s outline %d,%d..%d,%d (cell %d,%d): %d px, off its line by %.1f" % (
                side, run[0][0], run[0][1], run[-1][0], run[-1][1], run[0][0] // 16, run[0][1] // 16,
                len(run), worst))
        cells = collections.Counter((a[0] // 16, a[1] // 16) for a in faults.values() if a)
        print("view: %s, %d fault pixels; cells %s" % (args.out, len(faults), cells.most_common(12)))


if __name__ == "__main__":
    main()
