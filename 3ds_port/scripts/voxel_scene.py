#!/usr/bin/env python3
"""A spot of a map as the game shows it, and what builds it: the tools for
working on the mountains (a trial relief, the game's view of a cell, what
draws a pixel, the cells' readings).

    python voxel_scene.py trial OUT.bin [LAYOUT ...]
        build/relief_old.bin with voxel_terraces over the given groups (all
        of voxel_terraces.GROUPS by default): a trial relief in seconds
    python voxel_scene.py view LAYOUT X,Y OUT.png [--relief R] [--dist 11]
                          [--pitch 43] [--yaw 0] [--scale 3] [--only tex|shape]
        the game's camera on cell X,Y: the textured view over the shapes
        (each surface by the way it faces, each level of top its colour,
        W/E walls dark brown, slants red) - it matches Azahar. Drawn as the
        engine draws it, leant (voxel_relief.h; VOXEL_LEAN=0: the faces
        vertical); the shapes are judged with the lean undone
    python voxel_scene.py probe LAYOUT X,Y R.bin PX,PY [PX,PY ...]
        which triangle that view (--scale 3) shows at screen pixels: atlas
        id, cell, vertices (in the drawing's space, the lean undone) - to
        name the cell behind a fault in a shot
    python voxel_scene.py cells LAYOUT x0,y0,x1,y1 [--relief R]
        per cell: metatile, kind (G/T/R/?), band side, the piece the solve
        chose (F/S/W/E/W2/E2/cSE/cSW/vSE/vSW + level) and the lattice
        corners in levels (NW NE SW SE)
"""

import argparse
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

OLD = os.path.join(PORT, "build", "relief_old.bin")


def trial(out, layouts):
    import voxel_terraces as vt
    shutil.copy(OLD, out)
    vt.apply_file(out, layouts or list(vt.GROUPS))


def view(args):
    import voxel_relief_lines as vl
    import voxel_grid_check as gc
    from PIL import Image
    if args.relief:
        vl.use_relief(args.relief)
    # what the console draws (leant) is what is seen; what it is judged by
    # is the same triangles in the drawing's space
    drawn, _ = vl.dump(args.layout, leant=True)
    tris = [vl.unlean_tri(t) for t in drawn]
    at = tuple(int(v) for v in args.at.split(","))
    cam = vl.camera(drawn, at, args.pitch, args.dist, args.yaw, leant=vl.lean())
    ras = vl.render(drawn, vl.Atlas(args.layout), cam, args.scale)
    tex = ras.image()
    shape = tex.copy()
    px = shape.load()
    for i, o in enumerate(ras.tri):
        if o >= 0:
            px[i % ras.w, i // ras.w] = gc.plain(tris[o])
    for i, o in enumerate(ras.tri):
        x, y = i % ras.w, i // ras.w
        if o >= 0 and x + 1 < ras.w and y + 1 < ras.h:
            p, q = ras.tri[i + 1], ras.tri[i + ras.w]
            if any(n >= 0 and gc.plain(tris[n]) != gc.plain(tris[o]) for n in (p, q)):
                px[x, y] = (30, 25, 20)
    if args.only == "tex":
        out = tex
    elif args.only == "shape":
        out = shape
    else:
        out = Image.new("RGB", (tex.width, tex.height * 2))
        out.paste(tex, (0, 0))
        out.paste(shape, (0, tex.height))
    out.save(args.out)


def probe(args):
    import voxel_relief_lines as vl
    import voxel_grid_check as gc
    vl.use_relief(args.relief)
    drawn, _ = vl.dump(args.layout, leant=True)
    tris = [vl.unlean_tri(t) for t in drawn]
    at = tuple(int(v) for v in args.at.split(","))
    cam = vl.camera(drawn, at, args.pitch, args.dist, args.yaw, leant=vl.lean())
    ras = vl.render(drawn, vl.Atlas(args.layout), cam, args.scale)
    for spec in args.pixels:
        x, y = map(int, spec.split(","))
        o = ras.tri[y * ras.w + x]
        if o < 0:
            print(spec, "nothing (the void)")
            continue
        t = tris[o]
        print(spec, "atlas", vl.atlas_id(t), "cell", gc.cell_of(t),
              " ".join("(%.2f %.2f %.2f)" % p[:3] for p in t))


def cells(args):
    import voxel_relief_fixes as vrf
    import voxel_terraces as vt
    ref = vrf.parse(open(args.relief or OLD, "rb").read())
    sol = vt.solve(args.layout, ref)
    c = sol["cells"]
    inv = {v: k for k, v in c.src.items()}
    x0, y0, x1, y1 = map(int, args.box.split(","))
    print("     " + "".join("%-15d" % x for x in range(x0, x1)))
    for y in range(y0, y1):
        top, low = [], []
        for x in range(x0, x1):
            k = inv.get((args.layout, x, y))
            if k is None:
                top.append("-")
                low.append("")
                continue
            p = sol["pieces"].get(k, ("?", 0))
            g = sol["grids"].get(k)
            top.append("%03x%s%s" % (c.metatile(k), c.kind[k], c.band.get(k, "")))
            corners = "" if g is None else " ".join("%g" % (v / vt.LEVEL) for v in (g[0][0], g[0][vt.N], g[vt.N][0], g[vt.N][vt.N]))
            low.append("%s%d %s" % (p[0], p[1] // vt.LEVEL, corners))
        print("%3d  " % y + "".join("%-15s" % s for s in top))
        print("     " + "".join("%-15s" % s for s in low))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    t = sub.add_parser("trial")
    t.add_argument("out")
    t.add_argument("layouts", nargs="*")
    for name in ("view", "probe"):
        v = sub.add_parser(name)
        v.add_argument("layout")
        v.add_argument("at")
        if name == "view":
            v.add_argument("out")
            v.add_argument("--relief", default=None)
            v.add_argument("--only", default="both", choices=("both", "tex", "shape"))
        else:
            v.add_argument("relief")
            v.add_argument("pixels", nargs="+")
        v.add_argument("--pitch", type=float, default=43.0)
        v.add_argument("--dist", type=float, default=11.0)
        v.add_argument("--yaw", type=float, default=0.0)
        v.add_argument("--scale", type=int, default=3)
    c = sub.add_parser("cells")
    c.add_argument("layout")
    c.add_argument("box")
    c.add_argument("--relief", default=None)
    args = ap.parse_args()
    if args.cmd == "trial":
        trial(args.out, args.layouts)
    elif args.cmd == "view":
        view(args)
    elif args.cmd == "probe":
        probe(args)
    else:
        cells(args)


if __name__ == "__main__":
    main()
