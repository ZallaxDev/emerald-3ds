"""The buildings' animated texels, end to end: Rustboro's fountain plays its jet.

buildings.bin is composed by the console's own code (voxel_building.c, through
tests/voxel_building_anim_test.c) from BG VRAM as the game holds it at each
frame of the fountain's animation (tileset_anims.c: secondary tiles 448-451,
two frames). At each frame the fountain, drawn from the file's own mesh on its
page so patched, in the GBA's projection, must be the map's drawing at that
frame pixel for pixel. Composed from the tilesets' own tiles, every run must be
its page as written. A file without the table loads with none; a broken one
does not load.

    python tests/voxel_building_anim_test.py [--cc gcc]
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import voxel_building as vb  # noqa: E402
import voxel_building_specs as specs  # noqa: E402

LAYOUT = "LAYOUT_RUSTBORO_CITY"
SOURCES = ["src/voxel/voxel_building.c", "src/voxel/voxel_grade.c", "src/3ds_video_decode.c"]


def build(cc, tmp, path):
    exe = tmp / "anim_test.exe"
    subprocess.run([cc, "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror", "-Iinclude", "-Isrc/voxel",
                    "-iquote", "../include", "-D_GNU_SOURCE", "-DPORT_LOG(...)=((void)0)",
                    "-DVOXEL_HOST_FILES", '-DVOXEL_BUILDINGS_PATH="%s"' % path.as_posix(),
                    "tests/voxel_building_anim_test.c"] + SOURCES + ["-lm", "-o", str(exe)],
                   cwd=ROOT, check=True)
    return exe


def compose(exe, tmp, vram):
    """{run: (page, offset, texel bytes)} as the console composes them."""
    src, out = tmp / "vram.bin", tmp / "runs.bin"
    src.write_bytes(vram)
    r = subprocess.run([str(exe), str(src), str(out)], cwd=ROOT, capture_output=True, text=True)
    if r.returncode:
        raise SystemExit("FAIL harness: " + r.stderr)
    data, runs, o = out.read_bytes(), {}, 0
    while o < len(data):
        run, page, offset, n = struct.unpack_from("<4I", data, o)
        runs[run] = (page, offset, data[o + 16:o + 16 + n])
        o += 16 + n
    return runs


def vram_of(art, frame):
    """BG VRAM's 1024 tiles for the layout's tilesets, with `frame`'s."""
    def half(tiles):
        return bytes(tiles[:512 * 32]).ljust(512 * 32, b"\0")
    vram = bytearray(half(art.ts.pt) + half(art.ts.st))
    for tile, data in frame.items():
        vram[tile * 32:tile * 32 + 32] = data
    return bytes(vram)


def drawing(frame):
    """The fountain's drawing as the game shows it at `frame`."""
    art = vb.LayoutArt(LAYOUT)
    st = bytearray(art.ts.st)
    for tile, data in frame.items():
        st[(tile - 512) * 32:(tile - 512) * 32 + 32] = data
    art.ts.st = bytes(st)
    vb._SUBTILES.clear()
    vb._CELL_IMAGES.clear()
    x, y = specs.FOUNTAIN_RECT
    ground = [0x2BB, 0x2C3, specs.GRASS]
    out = art.building_art(x, y, 3, 3, art.ground_tiles(ground))
    vb._SUBTILES.clear()
    vb._CELL_IMAGES.clear()
    return out


def header_size(blob):
    """buildings.bin's header: VXB9 carries its conditions' count after VXB8's."""
    return 28 if blob[:4] == b"VXB9" else 24


def fountain_tris(blob, layout_index):
    """The fountain's triangles: its page and (x, y, z, u, v) in page texels."""
    npages, nmodels, npm, nplaces = struct.unpack_from("<4H", blob, 4)
    hbytes, nmasks = struct.unpack_from("<2H", blob, 12)
    nverts, nvar, ndecks = struct.unpack_from("<IHH", blob, 16)
    # VXB9: the placements' conditions, after VXB8's header and the decks
    nconds = struct.unpack_from("<H", blob, 24)[0] if blob[:4] == b"VXB9" else 0
    o = header_size(blob)
    # the height's top bit flags a page with window glass
    pages = [(w, h & 0x7FFF, off) for (w, h, off) in
             (struct.unpack_from("<HHI", blob, o + 8 * i) for i in range(npages))]
    o += npages * 8
    models = [struct.unpack_from("<BBHIII", blob, o + 16 * i) for i in range(nmodels)]
    o += nmodels * 16
    pms = [struct.unpack_from("<HHhh", blob, o + 8 * i) for i in range(npm)]
    o += npm * 8
    places = [struct.unpack_from("<HHHH", blob, o + 16 * i) for i in range(nplaces)]
    o += nplaces * 16 + hbytes
    o += o & 1
    o += hbytes * 2 + nmasks * 32 + hbytes
    o += o & 1
    o += nvar * 6 + ndecks * 8 + nconds * 6
    o += (-o) & 3
    x, y = specs.FOUNTAIN_RECT
    pm = next(p[1] for p in places if p[0] == layout_index and (p[2], p[3]) == (x, y))
    model, page, ox, oy = pms[pm]
    first, count = models[model][3], models[model][4] & 0x7FFFFFFF
    vs = [struct.unpack_from("<6f", blob, o + 24 * (first + k)) for k in range(count)]
    tris = [[(vx * 16, vy * 16, vz * 16, ox + u, oy + v) for (vx, vy, vz, u, v, _) in vs[k:k + 3]]
            for k in range(0, count - count % 3, 3)]
    return page, pages[page], tris


def page_image(blob, page, patch):
    """A page's texels, `patch` ({byte offset: bytes}) laid over them."""
    w, h, off = page
    tex = bytearray(blob[off:off + w * h * 2])
    for at, data in patch.items():
        tex[at:at + len(data)] = data
    img = vb.Image.new("RGBA", (w, h))
    px = img.load()
    for yy in range(h):
        for xx in range(w):
            t = struct.unpack_from("<H", tex, 2 * texel_offset(xx, yy, w))[0]
            px[xx, yy] = ((t >> 11) << 3, ((t >> 6) & 31) << 3, ((t >> 1) & 31) << 3, 255 if t & 1 else 0)
    return img


def texel_offset(x, y, width):
    morton = 0
    for bit in range(3):
        morton |= ((x >> bit) & 1) << (2 * bit) | ((y >> bit) & 1) << (2 * bit + 1)
    return ((y // 8) * (width // 8) + x // 8) * 64 + morton


def projected(tri):
    """A face laid with the drawing as the GBA projects it (the jet's card
    too, a band lower): its texels follow (x, z - y). The rest - the sides
    the drawing never shows, dressed with a strip, a shadow of one texel -
    are what the generator's proof leaves out too (~behind)."""
    du = {round(u - x, 3) for (x, y, z, u, v) in tri}
    dv = {round(v - (z - y), 3) for (x, y, z, u, v) in tri}
    return len(du) == 1 and len(dv) == 1


def differing(tris, tex, art):
    """Pixels of the fountain, drawn in the GBA's projection with depth,
    that are not its drawing."""
    M = 16
    W, H = art.size
    ras = vb.Raster(W + 2 * M, H + M, bg=(0, 0, 0))
    for tri in tris:
        if projected(tri):
            ras.draw([(x + M, z - y + M, y + z, 1.0, u, v) for (x, y, z, u, v) in tri], tex, 1.0, 1)
    apx = art.load()
    bad = 0
    for yy in range(H):
        for xx in range(W):
            idx = (yy + M) * ras.w + xx + M
            want = apx[xx, yy]
            if want[3] >= 128:
                # the page holds five bits a channel, as the GBA's colours
                bad += ras.owner[idx] is None or (tuple(c >> 3 for c in ras.color[idx])
                                                  != tuple(c >> 3 for c in want[:3]))
            else:
                bad += ras.owner[idx] is not None
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", default="gcc")
    a = ap.parse_args()
    path = ROOT / "romfs" / "voxel" / "buildings.bin"
    blob = path.read_bytes()
    layouts = vb._layouts_json()
    index = next(i for i, e in enumerate(layouts) if e["id"] == LAYOUT) + 1
    art = vb.LayoutArt(LAYOUT)
    frames = [vb.anim_frame(f, specs.FOUNTAIN_ANIM_TILE) for f in specs.FOUNTAIN_ANIM]
    page_k, page, tris = fountain_tris(blob, index)
    os.makedirs(ROOT / "build", exist_ok=True)
    with tempfile.TemporaryDirectory(dir=ROOT / "build", prefix="anim-test-") as tmp:
        tmp = Path(tmp)
        exe = build(a.cc, tmp, path)
        # each map's own tiles, still: every run of that map is its page as
        # written (the fountain's, Mauville's gym's beams)
        cut = struct.unpack_from("<I", blob, len(blob) - 8)[0]
        nruns = struct.unpack_from("<H", blob, cut)[0]
        run_layout = {k: struct.unpack_from("<HH", blob, cut + 12 + 20 * k)[1] for k in range(nruns)}
        still = {}
        for lid in sorted(set(run_layout.values())):
            mine = compose(exe, tmp, vram_of(vb.LayoutArt(layouts[lid - 1]["id"]), {}))
            still.update({k: r for k, r in mine.items() if run_layout[k] == lid})
        if not still:
            raise SystemExit("FAIL no animated runs in buildings.bin")
        for run, (pk, offset, data) in still.items():
            w, h, off = struct.unpack_from("<HHI", blob, header_size(blob) + 8 * pk)
            if data != blob[off + offset:off + offset + len(data)]:
                raise SystemExit("FAIL run %d: composed from the still tiles, not its page" % run)
        # each frame: the fountain as drawn, jet and all
        seen = []
        for k, frame in enumerate(frames):
            runs = compose(exe, tmp, vram_of(art, frame))
            patch = {offset: data for (pk, offset, data) in runs.values() if pk == page_k}
            if not patch:
                raise SystemExit("FAIL the fountain's page has no animated run")
            bad = differing(tris, page_image(blob, page, patch), drawing(frame))
            if bad:
                raise SystemExit("FAIL frame %d: %d pixels of the fountain are not its drawing" % (k, bad))
            seen.append(b"".join(d for (_, _, d) in sorted(runs.values())))
        if len(set(seen)) != len(frames):
            raise SystemExit("FAIL the frames compose the same texels: nothing would move")
        # without the table: loads, nothing animates; a broken table: no load
        plain = tmp / "plain.bin"
        plain.write_bytes(blob[:cut])
        r = subprocess.run([str(build(a.cc, tmp, plain)), str(tmp / "vram.bin"), str(tmp / "o.bin")],
                           cwd=ROOT, capture_output=True, text=True)
        if r.returncode or not r.stdout.startswith("0 animated runs"):
            raise SystemExit("FAIL a file without the table: %s %s" % (r.stdout, r.stderr))
        broken = tmp / "broken.bin"
        broken.write_bytes(blob[:-12] + struct.pack("<I", len(blob)) + b"ANIM")
        r = subprocess.run([str(build(a.cc, tmp, broken)), str(tmp / "vram.bin"), str(tmp / "o.bin")],
                           cwd=ROOT, capture_output=True, text=True)
        if r.returncode == 0:
            raise SystemExit("FAIL a table out of the file loaded")
    print("PASS animated texels: %d run(s); the fountain is its drawing at each of %d frames"
          % (len(still), len(frames)))


if __name__ == "__main__":
    main()
