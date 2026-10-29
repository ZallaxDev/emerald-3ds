#!/usr/bin/env python3
"""A map as the console builds it, from the console's own code and data.

The previews of the modelling tools (townview, dev.py) draw the models from
the Python that made them; the console draws what the exporter wrote, as its
C code builds it, inside its budgets. When the two part (a buffer that
refuses triangles, a ground patch the exporter dropped, memory that runs out)
the preview says nothing. This one is built from tests/voxel_scene_dump.c:
the engine's builder, trees, buildings, relief and lighting, compiled for the
host over romfs/voxel/*.bin, chunk by chunk as ctr_voxel.c JobStep builds
them, at the console's capacity. What it shows is what the console has.

    python voxel_engine_view.py view LAYOUT X,Y OUT.png [--pitch 40]
                                [--yaw 0] [--dist 9] [--scale 2]
        the game's camera on cell X,Y
    python voxel_engine_view.py gba LAYOUT OUT.png [--top 40]
        the console's scene drawn as the GBA sees it, unlit, against the
        map's drawing: OUT_console/_drawing/_diff.png and the cells that
        differ (cells with trees are left out, green; the relief differs
        on purpose)
    python voxel_engine_view.py under LAYOUT [--step 6] [--near 2] [--top 40]
                                 [--out PREFIX]
        the ground laid under the models where the game's camera sees it
        (a patch's edge, under a lifted roof) against the drawing round
        it: the cells where it shows a colour the drawing has not there
        (PREFIX_under.png: those cells in red on the map's drawing)
    python voxel_engine_view.py check [LAYOUT ...]
        every map (or those): chunks that refuse triangles (the console
        drops them: holes), static models over their memory budget; with
        no layouts also connected maps whose building pages cannot share
        the page arena. Exit 1 when any.

The dump is rebuilt when its sources are newer (msys2 mingw64 gcc).
"""

import argparse
import glob
import json
import math
import os
import shlex
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import voxel_building as vb  # noqa: E402
import voxel_cells as vc  # noqa: E402
import voxel_relief_lines as vl  # noqa: E402
from PIL import Image  # noqa: E402

DUMP = os.path.join(PORT, "build", "voxel_scene_dump" + (".exe" if os.name == "nt" else ""))
SOURCES = ["tests/voxel_scene_dump.c"] + ["src/voxel/%s.c" % n for n in (
    "voxel_mesh_builder", "voxel_tree", "voxel_sign", "voxel_building", "voxel_relief",
    "voxel_grade", "voxel_lighting", "voxel_regions")] + ["src/3ds_video_decode.c"]
# msys2 mingw64 on Windows; elsewhere the host's own (or VOXEL_HOST_CC, or
# the build's HOSTCC)
GCC = (os.environ.get("VOXEL_HOST_CC") or os.environ.get("HOSTCC")
       or (r"C:\msys64\mingw64\bin\gcc.exe" if os.name == "nt" else "gcc"))
SCRATCH = 8192 + 64 * 18          # ctr_voxel.c VOXEL_CHUNK_SCRATCH
# A map's static models (ctr_voxel.c StaticBuildSlice), in the console's linear
# memory: what one map may hold at once, with room left for the rest of the
# game. Measured 2026-10-10: 517 KiB of linear memory free in Slateport, but
# not in one block (the lantern alone at 275 KiB did not fit); each model is
# its own block, the largest ~80 KiB. To confirm on the console: no "no
# linear memory" line in port.log.
STATIC_BUDGET = 160 * 1024
PAGE_ARENA = 1088 * 1024          # ctr_voxel.c VOXEL_PAGE_VRAM_BUDGET
SHAPE_FLAT, SHAPE_WATER = 0, 7    # voxel_world.h VoxelVisualShape
SUN_DX, SUN_DZ, AMBIENT = 0.85, 0.55, 0.70
TREE_DIM = 128
# the secondary tilesets voxel_tree.c tells apart, as voxel_scene_dump.c reads them
TREE_TILESETS = {"gTileset_Dewford": 1, "gTileset_Fortree": 2, "gTileset_Mauville": 3,
                 "gTileset_Lavaridge": 4, "gTileset_Rustboro": 5, "gTileset_Lilycove": 6,
                 "gTileset_EverGrande": 7, "gTileset_BattleFrontierOutsideWest": 8,
                 "gTileset_Mossdeep": 9, "gTileset_Fallarbor": 10}


def build_dump():
    srcs = [os.path.join(PORT, s) for s in SOURCES]
    deps = srcs + [os.path.abspath(__file__)] + glob.glob(os.path.join(PORT, "src", "voxel", "*.h")) \
        + glob.glob(os.path.join(PORT, "include", "*.h"))
    if os.path.exists(DUMP) and os.path.getmtime(DUMP) >= max(os.path.getmtime(d) for d in deps):
        return
    hostinc = os.path.join(PORT, "build", "hostinc")
    os.makedirs(hostinc, exist_ok=True)
    with open(os.path.join(hostinc, "port_log.h"), "w") as f:
        f.write("/* host build of the scene dump */\n#ifndef PORT_LOG\n#define PORT_LOG(...) ((void)0)\n#endif\n")
    compiler = shlex.split(GCC, posix=os.name != "nt")
    compiler = [part.strip('"') for part in compiler]
    if not compiler or shutil.which(compiler[0]) is None:
        raise SystemExit("host compiler not found; set HOSTCC to a native C compiler")
    cmd = compiler + ["-std=c99", "-O2", "-I" + hostinc, "-Iinclude", "-Isrc/voxel",
           "-DPORT_LOG(...)=((void)0)", "-DCTR_VOXEL_LIGHTING=1", "-DVOXEL_HOST_FILES",
           '-DVOXEL_BUILDINGS_PATH="romfs/voxel/buildings.bin"',
           # a trial relief (VOXEL_RELIEF, as voxel_relief_lines.use_relief)
           '-DVOXEL_RELIEF_PATH=(getenv("VOXEL_RELIEF") ? getenv("VOXEL_RELIEF") : "romfs/voxel/relief.bin")',
           '-DVOXEL_REGIONS_PATH="romfs/voxel/regions.bin"',
           '-DSIGN_MASK_PATH="romfs/voxel/signposts.bin"'] + SOURCES + ["-lm", "-o", DUMP]
    if os.name != "nt":
        cmd.insert(len(compiler), "-D_POSIX_C_SOURCE=200809L")
    env = dict(os.environ)
    env["PATH"] = os.path.dirname(shutil.which(compiler[0])) + os.pathsep + env.get("PATH", "")
    r = subprocess.run(cmd, cwd=PORT, capture_output=True, text=True, env=env)
    if r.returncode:
        raise SystemExit("building the scene dump failed:\n" + r.stderr)


# ── the shipped buildings.bin ──────────────────────────────────────────────

class Buildings:
    """romfs/voxel/buildings.bin as the console reads it."""

    def __init__(self, path=None):
        with open(path or os.path.join(PORT, "romfs", "voxel", "buildings.bin"), "rb") as source:
            b = self.blob = source.read()
        if b[:4] not in (b"VXB8", b"VXB9"):
            raise ValueError("unsupported buildings format: %r" % b[:4])
        (npages, nmodels, npm, nplaces, hbytes, nmasks) = struct.unpack_from("<6H", b, 4)
        nverts, nvar, ndecks = struct.unpack_from("<IHH", b, 16)
        # VXB9: the placements' conditions (voxel_building.c PlacementLive)
        nconds = struct.unpack_from("<H", b, 24)[0] if b[:4] == b"VXB9" else 0
        head = o = 28 if b[:4] == b"VXB9" else 24
        raw_pages = [struct.unpack_from("<HHI", b, o + 8 * i) for i in range(npages)]
        self.page_glow = [b[:4] in (b"VXB8", b"VXB9") and bool(height & 0x8000)
                          for width, height, offset in raw_pages]
        self.pages = [(width, height & 0x7FFF if b[:4] in (b"VXB8", b"VXB9") else height, offset)
                      for width, height, offset in raw_pages]
        o += npages * 8
        self.models = []
        for i in range(nmodels):
            w, h, ground, first, count, heights = struct.unpack_from("<BBHIII", b, o + 16 * i)
            self.models.append((w, h, first, count & 0x7FFFFFFF, bool(count & 0x80000000)))
        o += nmodels * 16
        self.page_models = [struct.unpack_from("<HHhh", b, o + 8 * i) for i in range(npm)]
        o += npm * 8
        # (layout, pageModel, x, y, ground, extraCount, extraFirst)
        self.placements = [struct.unpack_from("<HHHHHHI", b, o + 16 * i) for i in range(nplaces)]
        o += nplaces * 16
        # a model's cells: its offset into them, 255 a cell it does not own
        self.model_heights = [struct.unpack_from("<I", b, head + npages * 8 + 16 * i + 12)[0]
                              for i in range(nmodels)]
        self.heights = b[o:o + hbytes]
        o += hbytes
        o += o & 1
        o += hbytes * 2 + nmasks * 32 + hbytes
        o += o & 1
        o += nvar * 6 + ndecks * 8
        # placement index -> (metatile, x, y): it stands while that cell holds it
        self.conditions = {}
        for i in range(nconds):
            k, metatile, x, y = struct.unpack_from("<HHBB", b, o + 6 * i)
            self.conditions[k] = (metatile, x, y)
        o += nconds * 6
        o += (-o) & 3
        self.vert_at = o
        self._pages = {}

    def page(self, k):
        """Page k's texture, decoded from the console's tiled RGBA5551: its
        rows, and below them, clear, the rest of the power of two the GPU
        samples it as (voxel_building.c TakePage)."""
        if k not in self._pages:
            w, h, off = self.pages[k]
            img = Image.new("RGBA", (w, texture_rows(h)), (0, 0, 0, 0))
            px = img.load()
            for y in range(h):
                for x in range(w):
                    t = struct.unpack_from("<H", self.blob, off + 2 * texel_offset(x, y, w))[0]
                    px[x, y] = ((t >> 11) << 3, ((t >> 6) & 31) << 3, ((t >> 1) & 31) << 3,
                                255 if t & 1 else 0)
            self._pages[k] = img
        return self._pages[k]

    def model_tris(self, page_model):
        """A static model's triangles as StaticBuildSlice packs them: page uv,
        lit by the sun's facing."""
        model, page, ox, oy = self.page_models[page_model]
        w, h, first, count, _ = self.models[model]
        pw, ph = self.pages[page][0], texture_rows(self.pages[page][1])
        out = []
        for k in range(0, count - count % 3, 3):
            vs = [struct.unpack_from("<6f", self.blob, self.vert_at + 24 * (first + k + j))
                  for j in range(3)]
            shade = model_shade(vs)
            out.append(tuple((x, y, z, (ox + u) / pw, 1.0 - (oy + v) / ph, shade)
                             for (x, y, z, u, v, s) in vs))
        return page, out


def texture_rows(rows):
    """The height of the texture that samples a page of `rows` rows."""
    th = 8
    while th < rows:
        th *= 2
    return th


def texel_offset(x, y, width):
    morton = 0
    for bit in range(3):
        morton |= ((x >> bit) & 1) << (2 * bit) | ((y >> bit) & 1) << (2 * bit + 1)
    return ((y // 8) * (width // 8) + x // 8) * 64 + morton


def model_shade(vs):
    """voxel_lighting.c VoxelLighting_ModelShade."""
    a, b, c = vs
    ux, uy, uz = b[0] - a[0], b[1] - a[1], b[2] - a[2]
    wx, wy, wz = c[0] - a[0], c[1] - a[1], c[2] - a[2]
    nx, ny, nz = uy * wz - uz * wy, uz * wx - ux * wz, ux * wy - uy * wx
    s = a[5]
    if abs(s - 0.90) < 0.02:
        flip = False
    elif abs(s - 0.80) < 0.02:
        flip = nx > 0
    elif abs(s - 0.72) < 0.02:
        flip = nx < 0
    elif abs(s - 0.66) < 0.02:
        flip = ny + nz > 0
    else:
        flip = ny + nz < 0
    if flip:
        nx, ny, nz = -nx, -ny, -nz
    L = math.sqrt(nx * nx + ny * ny + nz * nz)
    if L <= 0:
        return 1.0
    f = (ny - SUN_DX * nx - SUN_DZ * nz) / L
    return AMBIENT if f <= 0 else 1.0 if f >= 1 else AMBIENT + (1 - AMBIENT) * f


def tree_atlas():
    blob = open(os.path.join(PORT, "romfs", "voxel", "trees.rgba5551"), "rb").read()
    img = Image.new("RGBA", (TREE_DIM, TREE_DIM))
    px = img.load()
    for y in range(TREE_DIM):
        for x in range(TREE_DIM):
            t = struct.unpack_from("<H", blob, 2 * texel_offset(x, y, TREE_DIM))[0]
            px[x, y] = ((t >> 11) << 3, ((t >> 6) & 31) << 3, ((t >> 1) & 31) << 3,
                        255 if t & 1 else 0)
    return img


# ── the dump ───────────────────────────────────────────────────────────────

def layout_entry(layout_id):
    L = vl.layouts()
    entry = next(e for e in L if e["id"] == layout_id)
    return entry, L.index(entry) + 1


def dump(layout_id):
    """[chunk dicts] of the layout as the console builds it."""
    build_dump()
    entry, index = layout_entry(layout_id)
    w, h = entry["width"], entry["height"]
    blocks = vb.read_u16(os.path.join(vb.ROOT, entry["blockdata_filepath"]))
    cells = vc.Layout(entry, vc.MapEvents())
    shapes = bytes(SHAPE_WATER if cells.behaviour(x, y) in vc.WATER else SHAPE_FLAT
                   for y in range(h) for x in range(w))
    primary, secondary = entry["primary_tileset"], entry["secondary_tileset"]
    indoor = primary != "gTileset_General"
    flags = (1 if indoor else 0) | (2 if primary == "gTileset_General" else 0) \
        | TREE_TILESETS.get(secondary, 0) << 2
    tmp = os.path.join(PORT, "build", "scene_%s_%d" % (layout_id, os.getpid()))
    with open(tmp + ".in", "wb") as f:
        f.write(struct.pack("<HHHH", index, w, h, flags))
        f.write(struct.pack("<%dH" % (w * h), *[b_ & 0x3FF for b_ in blocks[:w * h]]))
        f.write(shapes)
    r = subprocess.run([DUMP, tmp + ".in", tmp + ".out"], cwd=PORT, capture_output=True, text=True)
    if r.returncode:
        raise SystemExit("voxel_scene_dump failed (%d): %s" % (r.returncode, r.stderr))
    data = open(tmp + ".out", "rb").read()
    os.remove(tmp + ".in")
    os.remove(tmp + ".out")
    n = struct.unpack_from("<I", data)[0]
    o = 4
    chunks = []
    for _ in range(n):
        cx, cy, count, dropped, terrain, first, nstat, page = struct.unpack_from("<hhIIIIIi", data, o)
        o += 28
        statics = []
        for _ in range(nstat):
            pm, lit, x, y, z = struct.unpack_from("<II3f", data, o)
            statics.append((pm, lit, (x, y, z)))
            o += 20
        verts = struct.unpack_from("<%df" % (count * 6), data, o)
        o += count * 24
        vs = [verts[i * 6:i * 6 + 6] for i in range(count)]
        chunks.append({"cx": cx, "cy": cy, "count": count, "dropped": dropped,
                       "terrain": terrain, "first": first, "page": page, "statics": statics,
                       "verts": vs})
    return chunks, index


# ── the picture ────────────────────────────────────────────────────────────

def scene(layout_id):
    """The layout's triangles as the console builds them, by texture: the
    terrain atlas, the trees, each building page; and its chunks."""
    chunks, index = dump(layout_id)
    groups = {"atlas": [], "trees": [], }
    pages = {}
    bld = Buildings()
    for c in chunks:
        vs = c["verts"]
        for i in range(0, c["count"] - c["count"] % 3, 3):
            tri = tuple(vs[i:i + 3])
            if i < c["terrain"]:
                groups["atlas"].append(tri)
            elif i < c["first"]:
                groups["trees"].append(tri)
            else:
                pages.setdefault(c["page"], []).append(tri)
        for (pm, lit, (x, y, z)) in c["statics"]:
            page, tris = bld.model_tris(pm)
            pages.setdefault(page, []).extend(
                tuple((px + x, py + y, pz + z, u, v, s) for (px, py, pz, u, v, s) in t) for t in tris)
    return groups, pages, bld, chunks


def textures(layout_id, groups, bld, pages):
    """(triangles, texture, uv map to texels) for every group of the scene."""
    atlas = vl.Atlas(layout_id)
    for t in groups["atlas"]:
        atlas.need(vl.atlas_id(t))
    S = vl.GRID * 16
    out = [(groups["atlas"], atlas.img, lambda u, v: (u * S, v * S)),
           (groups["trees"], tree_atlas(), lambda u, v: (u * TREE_DIM, (1.0 - v) * TREE_DIM))]
    for page, tris in pages.items():
        if page >= 0:
            img = bld.page(page)
            out.append((tris, img, lambda u, v, img=img: (u * img.size[0],
                                                         (1.0 - v) * img.size[1])))
    return out


def view(args):
    groups, pages, bld, chunks = scene(args.layout)
    at = tuple(int(v) for v in args.at.split(","))
    allt = groups["atlas"] + groups["trees"] + [t for ts in pages.values() for t in ts]
    cam = vl.camera(allt, at, args.pitch, args.dist, args.yaw)
    W, H = cam.width * args.scale, cam.height * args.scale
    cam2 = vb.Camera(cam.target, cam.pitch, cam.yaw, cam.distance, cam.fov, W, H)
    ras = vl.Raster(W, H, bg=(255, 0, 255))
    reach = cam.distance * 2.2

    def draw(tris, tex, uvmap):
        tpx = tex.load()
        for k, tri in enumerate(tris):
            mx = sum(p[0] for p in tri) / 3 - cam.target[0]
            mz = sum(p[2] for p in tri) / 3 - cam.target[2]
            if abs(mx) > reach or abs(mz) > reach:
                continue
            out = []
            for (x, y, z, u, v, s) in tri:
                pr = cam2.project((x, y, z))
                if pr is None:
                    break
                sx, sy, d, iw = pr
                tu, tv = uvmap(u, v)
                out.append((sx, sy, d, iw, tu * iw, tv * iw))
            else:
                ras.draw(out, tex, tpx, tuple(p[5] for p in tri), k)

    for tris, tex, uvmap in textures(args.layout, groups, bld, pages):
        draw(tris, tex, uvmap)
    ras.image().save(args.out)
    bad = [(c["cx"], c["cy"], c["dropped"]) for c in chunks if c["dropped"]]
    print("%s: %d chunks, %d vertices%s" % (args.layout, len(chunks), sum(c["count"] for c in chunks),
                                           ", DROPPED in %s" % bad if bad else ""))


# ── the drawing ────────────────────────────────────────────────────────────

def gba(args):
    """The scene the console builds, drawn as the GBA sees it - (x, z - y),
    a texel a pixel, unlit - beside the map's own drawing. Every model is
    proved alone against its art; this proves the map: the ground the
    console lays round and under them (patches, variants), what it drops,
    what stands where. Trees and the relief are drawn otherwise on purpose
    (a tree is a model of its own; a cliff's real depth): their cells show
    as differences, the rest should not."""
    groups, pages, bld, chunks = scene(args.layout)
    entry, _ = layout_entry(args.layout)
    w, h = entry["width"], entry["height"]
    W, H = w * 16, h * 16
    ras = vl.Raster(W, H, bg=(255, 0, 255))
    for g, (tris, tex, uvmap) in enumerate(textures(args.layout, groups, bld, pages)):
        tpx = tex.load()
        for k, tri in enumerate(tris):
            out = []
            for (x, y, z, u, v, s) in tri:
                tu, tv = uvmap(u, v)
                out.append((x * 16, (z - y) * 16, y + z, 1.0, tu, tv))
            ras.draw(out, tex, tpx, 1.0, (g, k))
    got = ras.image()
    # the cells a tree shows in (group 1): drawn as a model of its own
    treed = {(i % W // 16, i // W // 16) for i, o in enumerate(ras.tri) if o != -1 and o[0] == 1}
    layout = vb.LayoutArt(args.layout)
    art = Image.new("RGB", (W, H))
    for y in range(h):
        for x in range(w):
            art.paste(layout.cell_image(layout.metatile(x, y)).convert("RGB"), (x * 16, y * 16))
    gpx, apx = got.load(), art.load()
    diff = art.convert("L").convert("RGB")
    dpx = diff.load()
    # what shows where the two part: nothing (a hole), the terrain's atlas
    # (the ground laid wrong: a patch, a variant), a model (its real depth
    # past its drawing, a lifted roof - or a model out of place)
    kinds = {"hole": (255, 0, 255), "ground": (255, 0, 0), "model": (255, 170, 0)}
    bad = {k: {} for k in kinds}
    for y in range(H):
        for x in range(W):
            # the console's textures are 5 bits a channel (RGBA5551)
            if any((a >> 3) != (b >> 3) for a, b in zip(gpx[x, y], apx[x, y])):
                cell = (x // 16, y // 16)
                if cell in treed:
                    dpx[x, y] = (0, 160, 0)
                    continue
                o = ras.tri[y * W + x]
                kind = "hole" if o == -1 else "ground" if o[0] == 0 else "model"
                dpx[x, y] = kinds[kind]
                bad[kind][cell] = bad[kind].get(cell, 0) + 1
    base = os.path.splitext(args.out)[0]
    got.save(base + "_console.png")
    art.save(base + "_drawing.png")
    diff.save(base + "_diff.png")
    print("%s: %d cells with trees left out -> %s_diff.png (hole magenta, ground red, "
          "model orange)" % (args.layout, len(treed), base))
    for kind in kinds:
        cells = sorted(bad[kind].items(), key=lambda kv: -kv[1])
        print("%-6s %6d px in %d cells" % (kind, sum(bad[kind].values()), len(cells)))
        for (cx, cy), n in cells[:args.top]:
            print("    cell %d,%d  %3d px  metatile 0x%03x" % (cx, cy, n, layout.metatile(cx, cy)))
    return 1 if bad["hole"] or bad["ground"] else 0


def owned_cells(bld, layout_index):
    """{(x, y): (placement, ground)}: every cell a model of the layout owns,
    by the first placement that owns it, whose ground the console lays
    there (voxel_building.c ModelCell)."""
    out = {}
    for k, (lay, pm, x, y, ground, _, _) in enumerate(bld.placements):
        if lay != layout_index:
            continue
        model = bld.page_models[pm][0]
        w, h = bld.models[model][:2]
        for j in range(h):
            for i in range(w):
                if bld.heights[bld.model_heights[model] + j * w + i] != 0xFF:
                    out.setdefault((x + i, y + j), (k, ground))
    return out


def under(args):
    """The ground the console lays under its models, where the game's camera
    sees it. A patch lies a hair over the ground and a model is not its
    drawing in depth, so along their edges the ground under them shows - a
    line a pixel wide, the floor under a lifted roof - where the GBA's view
    (gba) sees none of it. Every pixel of a model's cell's terrain that a
    camera shows, from one every --step cells at the game's pitch and
    distance, is matched against the drawing within --near pixels of where
    it lies (5 bits a channel); a colour the drawing has nowhere near (the
    sea under a pier's curb, grass under an awning over a path) counts
    against its cell. The floor the drawing hides (a stall's under its
    roof) counts too: a report to judge by eye. --out PREFIX writes
    PREFIX_under.png, the map's drawing with each such cell in red, the
    stronger the more of it shows."""
    groups, pages, bld, chunks = scene(args.layout)
    entry, index = layout_entry(args.layout)
    w, h = entry["width"], entry["height"]
    owned = owned_cells(bld, index)
    layout = vb.LayoutArt(args.layout)
    art = Image.new("RGB", (w * 16, h * 16))
    for y in range(h):
        for x in range(w):
            art.paste(layout.cell_image(layout.metatile(x, y)).convert("RGB"), (x * 16, y * 16))
    apx = art.load()
    near = {}

    def drawn_near(X, Y):
        if (X, Y) not in near:
            r = args.near
            near[(X, Y)] = {tuple(c >> 3 for c in apx[X + dx, Y + dy])
                            for dy in range(-r, r + 1) for dx in range(-r, r + 1)
                            if 0 <= X + dx < w * 16 and 0 <= Y + dy < h * 16}
        return near[(X, Y)]

    groups_tex = textures(args.layout, groups, bld, pages)
    allt = groups["atlas"] + groups["trees"] + [t for ts in pages.values() for t in ts]
    bad, views = {}, 0
    for cz in range(args.step // 2, h, args.step):
        for cx in range(args.step // 2, w, args.step):
            views += 1
            cam = vl.camera(allt, (cx, cz), args.pitch, args.dist, 0.0)
            W, H = cam.width, cam.height
            cam2 = vb.Camera(cam.target, cam.pitch, cam.yaw, cam.distance, cam.fov, W, H)
            reach = cam.distance * 2.2
            depth, shown = [-1e30] * (W * H), [None] * (W * H)
            for g, (tris, tex, uvmap) in enumerate(groups_tex):
                tpx = tex.load()
                tw, th = tex.size
                for tri in tris:
                    mx = sum(p[0] for p in tri) / 3
                    mz = sum(p[2] for p in tri) / 3
                    if abs(mx - cam.target[0]) > reach or abs(mz - cam.target[2]) > reach:
                        continue
                    cell = (int(math.floor(mx)), int(math.floor(mz)))
                    mine = g == 0 and cell in owned
                    vs = []
                    for (x, y, z, u, v, s) in tri:
                        pr = cam2.project((x, y, z))
                        if pr is None:
                            break
                        sx, sy, d, iw = pr
                        tu, tv = uvmap(u, v)
                        vs.append((sx, sy, d, iw, tu * iw, tv * iw, x * iw, z * iw))
                    else:
                        (x0, y0, d0, w0, u0, v0, X0, Z0), (x1, y1, d1, w1, u1, v1, X1, Z1), \
                            (x2, y2, d2, w2, u2, v2, X2, Z2) = vs
                        area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
                        if abs(area) < 1e-9:
                            continue
                        inv = 1.0 / area
                        for py in range(max(0, int(min(y0, y1, y2))),
                                        min(H - 1, int(math.ceil(max(y0, y1, y2)))) + 1):
                            ry = py + 0.5
                            for px in range(max(0, int(min(x0, x1, x2))),
                                            min(W - 1, int(math.ceil(max(x0, x1, x2)))) + 1):
                                rx = px + 0.5
                                a = ((x1 - rx) * (y2 - ry) - (x2 - rx) * (y1 - ry)) * inv
                                b = ((x2 - rx) * (y0 - ry) - (x0 - rx) * (y2 - ry)) * inv
                                c = 1.0 - a - b
                                if a < -1e-7 or b < -1e-7 or c < -1e-7:
                                    continue
                                d = a * d0 + b * d1 + c * d2
                                i = py * W + px
                                if d <= depth[i]:
                                    continue
                                iw = a * w0 + b * w1 + c * w2
                                tu = int(math.floor((a * u0 + b * u1 + c * u2) / iw))
                                tv = int(math.floor((a * v0 + b * v1 + c * v2) / iw))
                                if not (0 <= tu < tw and 0 <= tv < th) or tpx[tu, tv][3] < 128:
                                    continue
                                depth[i] = d
                                shown[i] = (cell, tpx[tu, tv], (a * X0 + b * X1 + c * X2) / iw,
                                            (a * Z0 + b * Z1 + c * Z2) / iw) if mine else None
            for seen in shown:
                if seen is None:
                    continue
                cell, col, X, Z = seen
                q = tuple(c >> 3 for c in col[:3])
                at = (min(w * 16 - 1, int(X * 16)), min(h * 16 - 1, int(Z * 16)))
                if not any(all(abs(a - b) <= 2 for a, b in zip(q, t)) for t in drawn_near(*at)):
                    bad[cell] = bad.get(cell, 0) + 1
    print("%s: %d px of ground under its models the drawing has nowhere near, in %d cells "
          "(%d views)" % (args.layout, sum(bad.values()), len(bad), views))
    for (cx, cy), n in sorted(bad.items(), key=lambda kv: -kv[1])[:args.top]:
        k, ground = owned[(cx, cy)]
        print("    cell %d,%d  %4d px  metatile 0x%03x  ground %s  (placement at %d,%d)" % (
            cx, cy, n, layout.metatile(cx, cy), "own" if ground == 0xFFFF else "0x%03x" % ground,
            bld.placements[k][2], bld.placements[k][3]))
    if args.out:
        most = max(bad.values()) if bad else 1
        tint = Image.new("L", art.size, 0)
        for (cx, cy), n in bad.items():
            tint.paste(int(60 + 160 * n / most), (cx * 16, cy * 16, cx * 16 + 16, cy * 16 + 16))
        art.paste(Image.new("RGB", art.size, (255, 0, 0)), (0, 0), tint)
        art.save(args.out + "_under.png")
        print("  -> %s_under.png" % args.out)
    return 0


# ── the checks ─────────────────────────────────────────────────────────────

def static_bytes(bld, page_model):
    """The console's linear memory for one static model: shared vertices
    (16 bytes) and u16 indices (StaticBuildSlice)."""
    page, tris = bld.model_tris(page_model)
    seen = set()
    for t in tris:
        for p in t:
            seen.add(tuple(round(c, 4) for c in p))
    return len(seen) * 16 + len(tris) * 3 * 2


def layout_pages(bld):
    """Layout id (1-based) -> its page's (width, rows), from the placements
    (one page a layout)."""
    out = {}
    for lid, pm, *_ in bld.placements:
        out[lid] = bld.pages[bld.page_models[pm][1]][:2]
    return out


def page_pairs(bld):
    """Maps joined by a connection stand in view together, and with them
    their pages in the console's page arena (ctr_voxel.c
    VOXEL_PAGE_VRAM_BUDGET): two that cannot share it leave one map without
    its buildings, retried for seconds, at the border. [(map, other, KiB)]
    over the arena."""
    layouts = json.load(open(os.path.join(vb.ROOT, "data", "layouts", "layouts.json")))["layouts"]
    lid_of = {e["id"]: k + 1 for k, e in enumerate(layouts) if e and "id" in e}
    sizes = layout_pages(bld)
    maps = {}
    for path in glob.glob(os.path.join(vb.ROOT, "data", "maps", "*", "map.json")):
        m = json.load(open(path))
        maps[m["id"]] = m
    bad = set()
    for m in maps.values():
        a = sizes.get(lid_of.get(m.get("layout")))
        for c in m.get("connections") or []:
            other = maps.get(c.get("map"))
            b = sizes.get(lid_of.get(other.get("layout"))) if other else None
            # a page takes its texture's power-of-two rows in the arena
            # (ctr_voxel.c PageBytes), not only the rows it holds
            need = (a[0] * texture_rows(a[1]) + b[0] * texture_rows(b[1])) * 2 if a and b else 0
            if need > PAGE_ARENA:
                pair = tuple(sorted((m["id"], other["id"])))
                bad.add(pair + (need >> 10,))
    return sorted(bad)


def check(args):
    names = args.layouts or [e["id"] for e in vl.layouts() if "id" in e
                             and e.get("primary_tileset") == "gTileset_General"]
    bld = Buildings()
    failed = False
    for name in names:
        try:
            chunks, _ = dump(name)
        except SystemExit as e:
            print("%-40s dump failed: %s" % (name, e))
            failed = True
            continue
        worst = max(chunks, key=lambda c: c["count"]) if chunks else None
        drops = [c for c in chunks if c["dropped"]]
        models = sorted({pm for c in chunks for (pm, lit, p) in c["statics"]})
        mem = sum(static_bytes(bld, pm) for pm in models)
        line = "%-40s fullest chunk %5d/%d" % (name, worst["count"] if worst else 0, SCRATCH)
        if drops:
            failed = True
            line += "  DROPS " + ", ".join("%d,%d:%d tris" % (c["cx"] * 8, c["cy"] * 8, c["dropped"])
                                           for c in drops)
        if models:
            line += "  static %d model(s) %d KiB" % (len(models), mem >> 10)
            if mem > STATIC_BUDGET:
                failed = True
                line += " OVER %d KiB" % (STATIC_BUDGET >> 10)
        if drops or models or args.verbose:
            print(line)
    if not args.layouts:
        for (a, b, kib) in page_pairs(bld):
            failed = True
            print("%s + %s: building pages %d KiB, OVER the %d KiB page arena"
                  % (a, b, kib, PAGE_ARENA >> 10))
    print("FAILED" if failed else "ok")
    return 1 if failed else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    v = sub.add_parser("view")
    v.add_argument("layout")
    v.add_argument("at")
    v.add_argument("out")
    v.add_argument("--pitch", type=float, default=40.0)
    v.add_argument("--yaw", type=float, default=0.0)
    v.add_argument("--dist", type=float, default=9.0)
    v.add_argument("--scale", type=int, default=2)
    g = sub.add_parser("gba")
    g.add_argument("layout")
    g.add_argument("out")
    g.add_argument("--top", type=int, default=40)
    u = sub.add_parser("under")
    u.add_argument("layout")
    u.add_argument("--step", type=int, default=6)
    u.add_argument("--near", type=int, default=2)
    u.add_argument("--pitch", type=float, default=40.0)
    u.add_argument("--dist", type=float, default=9.0)
    u.add_argument("--top", type=int, default=40)
    u.add_argument("--out", default=None)
    c = sub.add_parser("check")
    c.add_argument("layouts", nargs="*")
    c.add_argument("--verbose", action="store_true")
    args = ap.parse_args()
    if args.cmd == "view":
        view(args)
        return 0
    if args.cmd == "gba":
        return gba(args)
    if args.cmd == "under":
        return under(args)
    return check(args)


if __name__ == "__main__":
    sys.exit(main())
