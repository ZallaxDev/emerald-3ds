#!/usr/bin/env python3
"""What the player sees wrong in a map's mountains, from the game's own
cameras over the console's whole scene.

voxel_shape_check.py judges the relief alone (tests/voxel_mesh_dump.c): no
trees, no buildings, so it leaves every cell beside a tree, a prop or a
house out - and the faults the user found on Routes 111 and 115 were all
there: the void seen through a gap above a door under a building, a black
wedge between a band's head and the meadow beside a tree, the dark slits of
block sides in a staircase of bands between two woods. This scan draws what
the console draws (tests/voxel_scene_dump.c: relief, trees, buildings,
props, cut tiles alpha-tested as the GPU does) from the follow camera of
src/voxel/voxel_camera.c - yaw 0, every pitch the options offer (34 to 46),
the distance of a connected map at zoom 100 and 120 - over a target every
STEP cells, and judges every pixel by the triangle it shows:

  hole      the void: a floor laid under the whole map (a level under the
            lowest terrain round each cell) shows through - on the console
            that is the black of the clear colour
  side      a vertical wall of the relief facing west or east (or turning a
            corner): the drawing never has one; from the game's camera a
            dark slit or a box's side
  deformed  a relief surface that is no top, no face and no band
  flat      a top where the drawing has rock that falls (a band, a face, a
            corner of the two): the drawing's rock laid flat on the ground

Nothing is left out but ledges and berry soil (never altered) and a map's
outermost ring (the next map's own). Trees and models hide what they hide,
as on the console.

    python voxel_view_scan.py LAYOUT[,LAYOUT...] [--relief R.bin] [--step 2]
                              [--list 30] [--png OUT.png]
        every camera; the cells by what they show wrong (pixels summed over
        the views, and in how many views); --png: the map's drawing with
        each faulty cell tinted (magenta hole, orange side, red deformed)
    python voxel_view_scan.py LAYOUT ... --pitches 75
        the same from steeper cameras (from above: the ground behind the
        mountains' backs, where the game's camera never looks)
    python voxel_view_scan.py LAYOUT ... --sheet DIR [--min-px 2000]
        also a picture of every cluster of faulty cells (DIR/index.txt)
    python voxel_view_scan.py LAYOUT --at X,Y [--pitch 40] [--dist 13]
                              --view OUT.png [--relief R.bin]
        one camera: the scene as the console draws it (unlit; the void
        magenta) over its shapes (each surface by the way it faces, trees
        and models grey) with every faulty pixel painted

Exit status 1 when any cell shows a fault in MIN_VIEWS views or more.
"""

import argparse
import collections
import math
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import voxel_building as vb  # noqa: E402
import voxel_engine_view as ev  # noqa: E402
import voxel_grid_check as gc  # noqa: E402
import voxel_relief_lines as vl  # noqa: E402
import voxel_shape_check as sc  # noqa: E402

RASTER = os.path.join(PORT, "build", "voxel_view_raster" + (".exe" if os.name == "nt" else ""))
RASTER_SRC = os.path.join(PORT, "tests", "voxel_view_raster.c")
PITCHES = (34.0, 40.0, 46.0)        # src/3ds_settings.c sPitches (both ends and the default)
DISTANCES = (13.0, 13.0 * 100 / 120)  # voxel_camera.c AdaptDistance, connected map, zoom 100 and 120
STEP = 2
W, H = 800, 480                     # the top screen at twice its size: a slit one pixel wide shows
FLOOR = 1.0                         # tiles under the lowest terrain round a cell
MIN_PX = 4                          # pixels a fault must cover in a view to count
MIN_VIEWS = 1
HOLE, SIDE, DEFORMED, FLAT, STANDING, DRAWN_FLAT, DRAWN_RAISED = 1, 2, 3, 4, 5, 6, 7
NAMES = {HOLE: "hole", SIDE: "side", DEFORMED: "deformed", FLAT: "flat", STANDING: "standing",
         DRAWN_FLAT: "drawn-flat", DRAWN_RAISED: "drawn-raised"}
PAINT = {HOLE: (255, 0, 255), SIDE: (255, 140, 0), DEFORMED: (255, 0, 0), FLAT: (0, 200, 255),
         STANDING: (150, 0, 255), DRAWN_FLAT: (0, 120, 255), DRAWN_RAISED: (255, 220, 0)}
# the drawing's own projection (the overlay): a cell is wrong where this many
# of its 256 pixels show the drawing's rock on a top (drawn-flat), or ground
# and tops on a face or a band (drawn-raised)
DRAWN_MIN = 80
EXPOSED = 4.0                       # px a face's top edge may stand over the ground behind it


def build_raster():
    if os.path.exists(RASTER) and os.path.getmtime(RASTER) >= os.path.getmtime(RASTER_SRC):
        return
    os.makedirs(os.path.dirname(RASTER), exist_ok=True)
    cc = os.environ.get("VOXEL_HOST_CC") or os.environ.get("HOSTCC") or ev.GCC
    r = subprocess.run([cc, "-std=c99", "-O2", RASTER_SRC, "-lm", "-o", RASTER],
                       capture_output=True, text=True)
    if r.returncode:
        raise SystemExit("building the rasteriser failed:\n" + r.stderr)


class Scene:
    """The console's scene of a layout, packed for the rasteriser: every
    triangle with its texture's texel coordinates, its class and its cell."""

    def __init__(self, layout, relief=None):
        if relief:
            vl.use_relief(relief)
        self.layout = layout
        groups, pages, bld, chunks = ev.scene(layout)
        entry, _ = ev.layout_entry(layout)
        self.w, self.h = entry["width"], entry["height"]
        keep = gc.kept_cells(layout)
        # a signpost is a model of its own (voxel_sign.c, in the terrain's
        # atlas): its sides are the box's, not the relief's
        import gen_voxel_relief as gr
        roles = gr.open_roles(layout)
        # (its east side stands on the next cell's west edge, its foot a row
        # south with the depth it has)
        keep = set(keep) | {(x + i, y + j) for y in range(self.h) for x in range(self.w)
                            if roles.role_at(x, y) == "signpost" for i in (-1, 0, 1) for j in (-1, 0, 1)}
        # a cell from a point: the map's base lifts it straight up, its
        # relief with the depth of a drawn map (a point at height h drawn h
        # rows north), a map leveled whole (bit 15) straight up too
        import voxel_relief_fixes as vrf
        whole = vrf.parse(open(vl.RELIEF, "rb").read())
        t = whole["layouts"].get(vrf.layout_index(layout))
        self.base = t["base"] / 16.0 if t else 0.0
        self.depth = bool(t and t["flags"] & vrf.DRAWN and not t["level"])
        # (a cut tile's rock is where its drawing has it: an edge its
        # background takes stands over nothing)
        cut = {}
        for (li, x, y), rec in whole["cuts"].items():
            if li == vrf.layout_index(layout) and rec[0] < len(whole["variants"]):
                cut[(x, y)] = whole["variants"][rec[0]][2]
        self.standing = self._standing(t["cells"] if t else {}, keep, cut)
        # the cells drawn as rock that falls - a band, a face, a corner of
        # the two - read as the terraces read them (voxel_terraces.Cells):
        # their relief lying flat is the drawing's rock laid on the ground
        import voxel_terraces as vt
        cells = vt.Cells(vt.group_of(layout))
        falls = set()
        # (stairs up a face are that face's level, drawn as steps)
        self.stairs = {cells.src[cell][1:] for cell in cells.kind
                       if cells.src[cell][0] == layout and cells.is_stair(cell)}
        for cell, k in cells.kind.items():
            lid, x, y = cells.src[cell]
            if lid != layout or k != vt.ROCK or cell in cells.keep or cells.is_stair(cell):
                continue
            if cell in cells.band or vt.is_face(cells, cell):
                falls.add((x, y))
        art = gr.terrace_art(layout)
        face = set(gr.ROCK_FACE) | {own for own, general in getattr(art, "colour", {}).items()
                                    if general in gr.ROCK_FACE}
        self.face = face
        self.tris, self.tex, self.texs, self.cls, self.cell = [], [], [], [], []
        self.judged = []                # a terrain triangle of a cell this judges
        self.kind = []                  # "terrain", "model" or "floor"
        lowest = {}
        tex_index = {}
        for tris, img, uvmap in ev.textures(layout, groups, bld, pages):
            rgba = img.convert("RGBA")
            alpha = rgba.split()[3].tobytes()
            k = tex_index.setdefault(id(img), len(self.texs))
            if k == len(self.texs):
                # the face's colours, the General's or a map's own recoloured
                # (an alias reading), marked: rock drawn falling
                mark = bytes(len(alpha))
                if tris is groups["atlas"]:
                    rgb = rgba.convert("RGB").tobytes()
                    mark = bytes(1 if tuple(rgb[i:i + 3]) in face else 0 for i in range(0, len(rgb), 3))
                self.texs.append((img.size[0], img.size[1], alpha, img, mark))
            terrain = tris is groups["atlas"]
            for t in tris:
                verts = []
                for (x, y, z, u, v, s) in t:
                    tu, tv = uvmap(u, v)
                    verts.append((x, y, z, tu, tv))
                cls, cell, judged = 0, -1, False
                if terrain:
                    cx, cy = self.cell_of(t)
                    inside = 0 <= cx < self.w and 0 <= cy < self.h
                    if inside:
                        cell = cy * self.w + cx
                        h = min(p[1] for p in t)
                        if h < lowest.get((cx, cy), 1e9):
                            lowest[(cx, cy)] = h
                    prop = vl.METATILE_REAL <= vl.atlas_id(t) < vl.CUT_FIRST
                    # the outermost ring is where the next map meets this one (its
                    # wall down to a lowered edge, voxel_mesh_builder.c's belt)
                    ring = cx in (0, self.w - 1) or cy in (0, self.h - 1)
                    if inside and not ring and not prop and (cx, cy) not in keep:
                        judged = True
                        kind = sc.surface(t)
                        # a north face behind a back (voxel_mesh_builder.c
                        # EmitNorthFace) lies on one row of the drawing, as a
                        # south step does: from the game it is the slope the
                        # camera sees from above the back, not a fault
                        if kind == "deformed" and self.south_step(t):
                            kind = "top"
                        cls = SIDE if kind == "side" else DEFORMED if kind == "deformed" else 0
                        if kind == "top" and (cx, cy) in falls:
                            cls = FLAT
                        if kind in ("face", "band") and (cx, cy) in self.standing:
                            cls = STANDING
                self.tris.append(verts)
                self.cls.append(cls)
                self.cell.append(cell)
                self.judged.append(judged)
                self.kind.append("terrain" if terrain else "model")
                self.tex.append(k)
        # the floor: a level under the lowest terrain round each cell, laid
        # with the depth the relief has there (a point at height h is drawn
        # h rows north), so a hole shows the floor of its own cell; and under
        # it all one plane, so no ray finds a way between two cells' floors
        # (each at its own height) to nothing at all
        bottom = self._bottom = min(lowest.values()) - 2 * FLOOR
        for y in range(self.h):
            for x in range(self.w):
                near = [lowest[(x + i, y + j)] for i in (-1, 0, 1) for j in (-1, 0, 1)
                        if (x + i, y + j) in lowest]
                edge = x in (0, self.w - 1) or y in (0, self.h - 1)
                for low in ((min(near) if near else 0.0) - FLOOR, bottom):
                    z = y + ((low - self.base) if self.depth else 0.0)
                    a = (x, low, z, 0, 0)
                    b = (x + 1, low, z, 0, 0)
                    c = (x + 1, low, z + 1, 0, 0)
                    d = (x, low, z + 1, 0, 0)
                    for tri in ((a, b, c), (a, c, d)):
                        self.tris.append(list(tri))
                        self.cls.append(0 if edge else HOLE)
                        self.cell.append(y * self.w + x)
                        self.kind.append("floor")
                        self.tex.append(-1)
        # the map's edges: the next map stands there on the console (a wall
        # down to the floor, so the camera does not look under the edge)
        self._skirts()
        # where the camera stands on each cell: the top of its terrain
        self.ground = {}
        for verts, kind in zip(self.tris, self.kind):
            if kind != "terrain":
                continue
            for (x, y, z, u, v) in verts:
                cell = self.cell_of([(x, y, z)])
                if y > self.ground.get(cell, -1e9):
                    self.ground[cell] = y

    @staticmethod
    def share(tri):
        """A band or a face falling a share of a level (voxel_terraces: a
        fall shared out between more bands or rows of face than it has
        levels): a plane along the grid, west/east or north/south - not
        twisted, only less steep."""
        nm = gc.normal(tri)
        return nm is not None and (abs(nm[2]) < gc.TOL_N or abs(nm[0]) < gc.TOL_N)

    @staticmethod
    def south_step(tri):
        """A step south with no face drawn for it (a top over the flat cell
        of a tree in front of it, behind_trees): the wall between them at
        the cell's edge, slanted so that it stands on one row of the
        drawing - at 45 degrees it is nothing, from the game a face seen
        steeply (voxel_grid_check "step"). Not a fault."""
        nm = gc.normal(tri)
        if nm is None or abs(nm[0]) > gc.TOL_N:
            return False
        vs = [p[2] - p[1] for p in tri]
        return max(vs) - min(vs) < 0.01 and abs(vs[0] - round(vs[0])) < gc.TOL_GRID

    def cell_of(self, tri):
        x = sum(p[0] for p in tri) / len(tri)
        y = sum(p[1] for p in tri) / len(tri)
        z = sum(p[2] for p in tri) / len(tri)
        return (int(math.floor(x)), int(math.floor(z - (y - self.base) if self.depth else z)))

    def _skirts(self):
        top = {}
        # the lowest point of each column's north edge, as drawn: a point
        # at height h stands h - base rows south, so the lowest is the
        # northernmost
        north = {}
        for verts, kind in zip(self.tris, self.kind):
            if kind != "terrain":
                continue
            for (x, y, z, u, v) in verts:
                top[self.cell_of([(x, y, z)])] = max(y, top.get(self.cell_of([(x, y, z)]), -1e9))
                row = z - (y - self.base) if self.depth else z
                if abs(row) < 1e-3:
                    col = min(self.w - 1, int(math.floor(x)))
                    north[col] = min(y, north.get(col, 1e9))
        bottom = self._bottom - 1.0
        w, h = self.w, self.h
        for x in range(w):
            for y, side in ((0, 0.0), (h - 1, 1.0)):
                g = top.get((x, y), self.base)
                z = y + side + ((g - self.base) if self.depth else 0.0)
                if side == 0.0 and self.depth and x in north:
                    # the north wall behind the edge's lowest point, a hair
                    # north of it: hung where the cell's top is it stood in
                    # the plane of a face along the edge (the face's pixels
                    # fought it: speckles, Lavaridge's north rock) and in
                    # front of an edge falling west or east (a dark wedge)
                    z = north[x] - self.base - 0.01
                self._quad((x, g, z), (x + 1, g, z), (x + 1, bottom, z), (x, bottom, z))
        for y in range(h):
            for x, side in ((0, 0.0), (w - 1, 1.0)):
                g = top.get((x, y), self.base)
                z0 = y + ((g - self.base) if self.depth else 0.0)
                self._quad((x + side, g, z0), (x + side, g, z0 + 1), (x + side, bottom, z0 + 1),
                           (x + side, bottom, z0))

    def _quad(self, a, b, c, d):
        for tri in ((a, b, c), (a, c, d)):
            self.tris.append([p + (0, 0) for p in tri])
            self.cls.append(0)
            self.cell.append(-1)
            self.kind.append("floor")
            self.tex.append(-1)

    def write(self, path):
        with open(path, "wb") as f:
            f.write(struct.pack("<I", len(self.texs)))
            for w, h, alpha, _, mark in self.texs:
                f.write(struct.pack("<HH", w, h))
                f.write(alpha)
                f.write(mark)
            f.write(struct.pack("<I", len(self.tris)))
            for verts, k, cls, cell in zip(self.tris, self.tex, self.cls, self.cell):
                f.write(struct.pack("<15f", *[c for p in verts for c in p]))
                f.write(struct.pack("<iB3xi", k, cls, cell))

    def _standing(self, lattice, keep, cut):
        """The cells whose rock falls (a face south, a band west or east)
        from a top edge standing over the ground behind it: a face is the
        front of the terrace on it, never a wall with lower ground behind -
        from the game's camera a box, a fin over the meadow (the drawing
        shows nothing wrong: its rock is where it is drawn)."""
        flat = [0.0] * 25

        def at(x, y):
            return lattice.get((x, y)) or flat
        out = set()
        for (x, y), g in lattice.items():
            if not (0 < x < self.w - 1 and 0 < y < self.h - 1) or (x, y) in keep:
                continue
            exposed = 0
            rows = cut.get((x, y))
            for back, fall in (((x, y - 1), [(i, 20 + i, 20 + i) for i in range(5)]),       # south
                               ((x + 1, y), [(5 * j + 4, 5 * j, 5 * j) for j in range(5)]),  # west
                               ((x - 1, y), [(5 * j, 5 * j + 4, 5 * j + 4) for j in range(5)])):  # east
                if back in keep:
                    continue
                b = at(*back)
                # (top, foot, the edge behind the top: the next cell's own)
                exposed += sum(1 for hi, lo, _ in fall
                               if g[hi] - g[lo] > EXPOSED and g[hi] - b[lo] > EXPOSED
                               and not (rows and rows[min(15, 4 * (hi // 5))] >> min(15, 4 * (hi % 5)) & 1))
            if exposed >= 2:
                out.add((x, y))
        return out

    def camera(self, at, pitch, dist, yaw=0.0):
        """voxel_relief_lines.camera: the target on the top of the cell."""
        g = self.ground.get(at, 0.0)
        return (at[0] + 0.5, g, at[1] + 0.5 + g, pitch, yaw, dist, vl.FOV)

    def drawn(self, box, scale):
        """The drawing's own projection over the cells of `box` (x0, y0,
        x1, y1), 16 * `scale` pixels a cell: every surface where the GBA
        draws it (a point at height h, h rows up), so the relief's shapes lie
        on the map's own picture."""
        x0, y0, x1, y1 = box
        k = 16 * scale
        return ((x0 + x1) / 2.0, self.base, (y0 + y1) / 2.0, 45.0, 0.0, 100.0, -float(k),
                (x1 - x0) * k, (y1 - y0) * k)


def run(scene, cams, owners=False):
    """[{(cell, class): pixels}] per camera (and, with `owners`, per camera
    the triangle each pixel shows and its texel)."""
    build_raster()
    tmp = os.path.join(PORT, "build", "viewscan_%d" % os.getpid())
    scene_path = tmp + ".scene"
    if getattr(scene, "_written", None) != scene_path:
        scene.write(scene_path)
        scene._written = scene_path
    with open(tmp + ".cams", "wb") as f:
        f.write(struct.pack("<I", len(cams)))
        for cam in cams:
            f.write(struct.pack("<7f2I", *(tuple(cam[:7]) + (tuple(cam[7:]) or (W, H)))))
    args = [RASTER, scene_path, tmp + ".cams", tmp + ".out"] + ([tmp + ".own"] if owners else [])
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode:
        raise SystemExit("voxel_view_raster failed (%d): %s" % (r.returncode, r.stderr))
    data = open(tmp + ".out", "rb").read()
    out, o = [], 0
    for _ in cams:
        n = struct.unpack_from("<I", data, o)[0]
        o += 4
        got = {}
        for i in range(n):
            cell, cls, px = struct.unpack_from("<iII", data, o + 12 * i)
            got[(cell, cls)] = px
        o += 12 * n
        out.append(got)
    own = None
    if owners:
        raw = open(tmp + ".own", "rb").read()
        own, at = [], 0
        for cam in cams:
            n = cam[7] * cam[8] if len(cam) > 7 else W * H
            ids = struct.unpack_from("<%di" % n, raw, at)
            at += 4 * n
            uv = struct.unpack_from("<%df" % (2 * n), raw, at)
            at += 8 * n
            own.append((ids, uv))
        os.remove(tmp + ".own")
    for p in (tmp + ".cams", tmp + ".out"):
        os.remove(p)
    return out, own


def cleanup(scene):
    path = getattr(scene, "_written", None)
    if path and os.path.exists(path):
        os.remove(path)


def scan(layout, relief=None, step=STEP, scene=None, box=None):
    """{(x, y): {class: [pixels, views]}} over every camera (`box`: only the
    cameras on the cells x0 <= x < x1, y0 <= y < y1)."""
    scene = scene or Scene(layout, relief)
    spots = [(x, y) for y in range(1, scene.h - 1, step) for x in range(1, scene.w - 1, step)
             if box is None or (box[0] <= x < box[2] and box[1] <= y < box[3])]
    cams = [scene.camera(at, p, d) for at in spots for p in PITCHES for d in DISTANCES]
    views, _ = run(scene, cams)
    total = collections.defaultdict(lambda: collections.defaultdict(lambda: [0, 0]))
    for cell, by in drawn_faults(scene, layout).items():
        if box is None or (box[0] <= cell[0] < box[2] and box[1] <= cell[1] < box[3]):
            for cls, (px, v) in by.items():
                total[cell][cls] = [px, v]
    for got in views:
        for (cell, cls), px in got.items():
            if px < MIN_PX:
                continue
            t = total[(cell % scene.w, cell // scene.w)][cls]
            t[0] += px
            t[1] += 1
    return total, scene


def overview(layout, total, path, scale=3):
    from PIL import Image, ImageDraw
    art = vb.LayoutArt(layout)
    img = Image.new("RGB", (art.w * 16, art.h * 16))
    for y in range(art.h):
        for x in range(art.w):
            img.paste(art.cell_image(art.metatile(x, y)).convert("RGB"), (x * 16, y * 16))
    img = img.convert("L").convert("RGB").resize((art.w * 16 * scale, art.h * 16 * scale), Image.NEAREST)
    d = ImageDraw.Draw(img, "RGBA")
    S = 16 * scale
    for (x, y), by in total.items():
        cls = max(by, key=lambda k: by[k][0])
        col = PAINT[cls]
        d.rectangle([x * S, y * S, x * S + S - 1, y * S + S - 1], fill=col + (110,), outline=col + (255,))
        d.text((x * S + 2, y * S + 2), "%d,%d" % (x, y), fill=(0, 0, 0))
    for y in range(0, art.h, 5):
        for x in range(0, art.w, 5):
            d.text((x * S + 2, y * S + S - 12), "%d,%d" % (x, y), fill=(0, 0, 255))
    img.save(path)


def pictures(scene, at, pitch, dist, cam=None):
    """(textured, shapes, {class: pixels}) of one camera (`cam`, or the
    game's at `at`): the scene as the console draws it (unlit) and its
    shapes - each surface by the way it faces, trees and models grey, the
    floor dark - every fault painted."""
    from PIL import Image
    cam = cam or scene.camera(at, pitch, dist)
    W, H = tuple(cam[7:9]) if len(cam) > 7 else (globals()["W"], globals()["H"])
    views, own = run(scene, [cam], owners=True)
    ids, uv = own[0]
    tex = Image.new("RGB", (W, H))
    shp = Image.new("RGB", (W, H))
    tpx, spx = tex.load(), shp.load()
    loads = [img.convert("RGBA").load() for _, _, _, img, _ in scene.texs]
    colour = {}
    for i, o in enumerate(ids):
        x, y = i % W, i // W
        if o < 0:
            tpx[x, y] = spx[x, y] = (0, 0, 0)
            continue
        k = scene.tex[o]
        if k >= 0:
            tw, th = scene.texs[k][0], scene.texs[k][1]
            u = min(tw - 1, max(0, int(math.floor(uv[2 * i]))))
            v = min(th - 1, max(0, int(math.floor(uv[2 * i + 1]))))
            tpx[x, y] = loads[k][u, v][:3]
        else:
            tpx[x, y] = (255, 0, 255) if scene.cls[o] == HOLE else (60, 60, 60)
        cls = scene.cls[o]
        if cls == FLAT and k >= 0:
            tw = scene.texs[k][0]
            u, v = int(math.floor(uv[2 * i])), int(math.floor(uv[2 * i + 1]))
            if not (0 <= u < tw and 0 <= v < scene.texs[k][1]) or not scene.texs[k][4][v * tw + u]:
                cls = 0
        if cls:
            spx[x, y] = PAINT[cls]
            continue
        if o not in colour:
            kind = scene.kind[o]
            if kind == "terrain":
                colour[o] = gc.plain([v + (0,) for v in scene.tris[o]])
                if colour[o] == (230, 30, 30):
                    colour[o] = (205, 175, 175)     # slanted, not judged (a ledge's lip)
            elif kind == "floor":
                colour[o] = (70, 70, 70)
            else:
                colour[o] = (150, 160, 150)
        spx[x, y] = colour[o]
    # the outlines between surfaces of other colours
    base = shp.copy().load()
    for y in range(H - 1):
        for x in range(W - 1):
            if base[x, y] != base[x + 1, y] or base[x, y] != base[x, y + 1]:
                spx[x, y] = (30, 25, 20)
    seen = collections.Counter()
    for (cell, cls), n in views[0].items():
        seen[NAMES[cls]] += n
    return tex, shp, dict(seen)


def view(layout, at, pitch, dist, path, relief=None):
    """One camera: the textured view over the shapes, faults painted."""
    from PIL import Image
    scene = Scene(layout, relief)
    try:
        tex, shp, seen = pictures(scene, at, pitch, dist)
    finally:
        cleanup(scene)
    out = Image.new("RGB", (W, 2 * H))
    out.paste(tex, (0, 0))
    out.paste(shp, (0, H))
    out.save(path)
    print("view %s pitch %g dist %g: %s" % (at, pitch, dist, seen))


def drawing(layout, box):
    """The map's own picture over the cells of `box`, 16 pixels a cell."""
    from PIL import Image
    x0, y0, x1, y1 = box
    art = vb.LayoutArt(layout)
    img = Image.new("RGB", ((x1 - x0) * 16, (y1 - y0) * 16))
    for y in range(y0, y1):
        for x in range(x0, x1):
            if 0 <= x < art.w and 0 <= y < art.h:
                img.paste(art.cell_image(art.metatile(x, y)).convert("RGB"), ((x - x0) * 16, (y - y0) * 16))
    return img


def drawn_faults(scene, layout, box=None):
    """The relief laid on the drawing (the drawing's own projection, every
    surface where the GBA draws it), judged against it pixel by pixel: the
    drawing's rock (a face's colours) shown on a top is rock laid flat
    (drawn-flat); ground or a top shown on a face or a band is ground stood
    up as rock (drawn-raised). Unlike `flat` this does not ask the
    terraces' reading of a cell: a tile they misread (a band they took for
    a top, a lip they took for the sea) is judged by what it shows.
    Each pixel is read as the terraces read the drawing
    (voxel_terraces.pixel_classes). {(x, y): {class: [pixels, 1]}}."""
    box = box or (0, 0, scene.w, scene.h)
    x0, y0, x1, y1 = box
    import gen_voxel_relief as gr
    import voxel_terraces as vt
    _, own = run(scene, [scene.drawn(box, 1)], owners=True)
    ids = own[0][0]
    # each pixel read as the terraces read the drawing: 'F' a face's
    # colours, 'T' a top's, 'G' anything else (grass, sand, water, a path)
    art = gr.terrace_art(layout)
    reading = {}
    w = (x1 - x0) * 16
    surf = {}
    count = collections.defaultdict(collections.Counter)
    for i, o in enumerate(ids):
        if o < 0 or o >= len(scene.judged) or not scene.judged[o]:
            continue
        if o not in surf:
            surf[o] = sc.surface([v[:3] + (0, 0, 0) for v in scene.tris[o]])
        k = surf[o]
        px, py = i % w, i // w
        cell = (x0 + px // 16, y0 + py // 16)
        if cell not in reading:
            inside = 0 <= cell[0] < art.w and 0 <= cell[1] < art.h
            reading[cell] = vt.pixel_classes(art, art.metatile(*cell)) if inside else None
        if reading[cell] is None or cell in scene.stairs:
            continue
        p = reading[cell][py % 16][px % 16]
        if p == "F" and k == "top":
            count[cell][DRAWN_FLAT] += 1
        elif p == "G" and k in ("face", "band"):
            count[cell][DRAWN_RAISED] += 1
    out = {}
    for cell, by in count.items():
        got = {cls: [n, 1] for cls, n in by.items() if n >= DRAWN_MIN}
        if got:
            out[cell] = got
    return out


def overlay(layout, box, path, relief=None, scale=3, alpha=0.45):
    """The map's own picture with the relief's shapes laid over it, in the
    drawing's projection (every surface where the GBA draws it): a face
    the relief lays flat shows its top's green over the drawn rock, a top
    standing as a face its blue over the grass, a fault its paint. Three
    panels: the drawing, the shapes over it, the shapes alone."""
    from PIL import Image, ImageDraw
    scene = Scene(layout, relief)
    try:
        cam = scene.drawn(box, scale)
        tex, shp, seen = pictures(scene, None, 0, 0, cam)
        bad = drawn_faults(scene, layout, box)
    finally:
        cleanup(scene)
    x0, y0, x1, y1 = box
    k = 16 * scale
    gba = drawing(layout, box).resize(shp.size, Image.NEAREST)
    mixed = Image.blend(gba, shp, alpha)
    panels = [gba, mixed, shp]
    out = Image.new("RGB", (shp.width, shp.height * 3))
    for i, im in enumerate(panels):
        d = ImageDraw.Draw(im)
        for x in range(x0, x1 + 1):
            d.line([((x - x0) * k, 0), ((x - x0) * k, im.height)], fill=(255, 255, 0))
        for y in range(y0, y1 + 1):
            d.line([(0, (y - y0) * k), (im.width, (y - y0) * k)], fill=(255, 255, 0))
        for y in range(y0, y1):
            for x in range(x0, x1):
                d.text(((x - x0) * k + 2, (y - y0) * k + 2), "%d,%d" % (x, y), fill=(0, 0, 0))
        if i:
            # the cells the drawing contradicts, framed in their class's paint
            for (x, y), by in bad.items():
                cls = max(by, key=lambda c: by[c][0])
                d.rectangle([(x - x0) * k + 2, (y - y0) * k + 2, (x - x0 + 1) * k - 3, (y - y0 + 1) * k - 3],
                            outline=PAINT[cls], width=3)
        out.paste(im, (0, i * shp.height))
    out.save(path)
    for cls in (DRAWN_FLAT, DRAWN_RAISED):
        seen[NAMES[cls]] = sum(by[cls][0] for by in bad.values() if cls in by)
    print("overlay %s: %s" % (",".join(map(str, box)), seen))


def clusters(total, min_px):
    """Groups of faulty cells (each with at least `min_px` pixels of one
    class), cells two apart joined: [(cells, worst cell)]."""
    bad = {cell for cell, by in total.items() if any(px >= min_px for px, _ in by.values())}
    out, seen = [], set()
    for start in sorted(bad):
        if start in seen:
            continue
        group, todo = [], [start]
        seen.add(start)
        while todo:
            x, y = todo.pop()
            group.append((x, y))
            for dx in (-2, -1, 0, 1, 2):
                for dy in (-2, -1, 0, 1, 2):
                    n = (x + dx, y + dy)
                    if n in bad and n not in seen:
                        seen.add(n)
                        todo.append(n)
        worst = max(group, key=lambda c: sum(px for px, _ in total[c].values()))
        out.append((sorted(group), worst))
    return out


def sheet(scene, total, directory, min_px, pitch=40.0, dist=13.0):
    """One picture a cluster of faults (DIR/NN_X_Y.png: textured over
    shapes, the camera a cell south of its worst cell) and DIR/index.txt."""
    from PIL import Image
    os.makedirs(directory, exist_ok=True)
    lines = []
    for n, (group, worst) in enumerate(sorted(clusters(total, min_px), key=lambda g: g[1][1])):
        at = (worst[0], min(scene.h - 2, worst[1] + 1))
        tex, shp, seen = pictures(scene, at, pitch, dist)
        out = Image.new("RGB", (W, 2 * H))
        out.paste(tex, (0, 0))
        out.paste(shp, (0, H))
        name = "%02d_%d_%d.png" % (n, worst[0], worst[1])
        out.save(os.path.join(directory, name))
        sums = collections.Counter()
        for cell in group:
            for cls, (px, _) in total[cell].items():
                sums[NAMES[cls]] += px
        lines.append("%s  cells %s  %s" % (name, " ".join("%d,%d" % c for c in group), dict(sums)))
    with open(os.path.join(directory, "index.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    return lines


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("layouts")
    ap.add_argument("--relief", default=None)
    ap.add_argument("--step", type=int, default=STEP)
    ap.add_argument("--list", type=int, default=30)
    ap.add_argument("--png", default=None)
    ap.add_argument("--at", default=None)
    ap.add_argument("--pitch", type=float, default=40.0)
    ap.add_argument("--dist", type=float, default=13.0)
    ap.add_argument("--pitches", default=None,
                    help="the whole map's cameras at these pitches instead (\"75\": from above, the ground "
                         "behind every mountain's back, as the editor's free camera shows it)")
    ap.add_argument("--view", default=None)
    ap.add_argument("--box", default=None, help="x0,y0,x1,y1: only the cameras on these cells")
    ap.add_argument("--overlay", default=None,
                    help="with --box: the map's picture with the shapes over it, in the drawing's projection")
    ap.add_argument("--sheet", default=None, help="a picture of every cluster of faults into this directory")
    ap.add_argument("--min-px", type=int, default=2000,
                    help="pixels (summed over the views) a cell needs to be pictured by --sheet")
    args = ap.parse_args()
    global PITCHES
    if args.pitches:
        PITCHES = tuple(float(v) for v in args.pitches.split(","))
    relief = os.path.abspath(args.relief) if args.relief else None
    if args.overlay:
        overlay(args.layouts, tuple(int(v) for v in args.box.split(",")), args.overlay, relief)
        return
    if args.view:
        view(args.layouts, tuple(int(v) for v in args.at.split(",")), args.pitch, args.dist,
             args.view, relief)
        return
    bad = 0
    for layout in args.layouts.split(","):
        scene = Scene(layout, relief)
        try:
            box = tuple(int(v) for v in args.box.split(",")) if args.box else None
            total, _ = scan(layout, step=args.step, scene=scene, box=box)
            if args.sheet:
                d = args.sheet if len(args.layouts.split(",")) == 1 else \
                    os.path.join(args.sheet, layout[7:].lower())
                for line in sheet(scene, total, d, args.min_px):
                    print("  sheet", line)
        finally:
            cleanup(scene)
        sums, cells = collections.Counter(), collections.Counter()
        for cell, by in total.items():
            for cls, (px, views) in by.items():
                if views >= MIN_VIEWS:
                    sums[cls] += px
                    cells[cls] += 1
        bad += sum(cells.values())
        print("%s: %s" % (layout, ", ".join("%s %d cells (%d px)" % (NAMES[k], cells[k], sums[k])
                                             for k in (HOLE, SIDE, DEFORMED, FLAT, STANDING,
                                                       DRAWN_FLAT, DRAWN_RAISED))))
        worst = sorted(total.items(), key=lambda kv: -sum(px for px, _ in kv[1].values()))
        for cell, by in worst[:args.list]:
            print("  %3d,%-3d %s" % (cell[0], cell[1], ", ".join(
                "%s %d px/%d views" % (NAMES[k], px, v) for k, (px, v) in sorted(by.items()))))
        if args.png:
            out = args.png if len(args.layouts.split(",")) == 1 else \
                args.png.replace(".png", "_%s.png" % layout[7:].lower())
            overview(layout, total, out)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
