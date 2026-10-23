#!/usr/bin/env python3
"""
The bottom screen's menu backdrop: the port's own art, drawn here once and
read by 3ds_bottom_ui.c from romfs/bottom/menu.bin.

A light emerald with the column's 8px diamond lattice over it, a soft
vignette and a step-cut emerald engraved in the middle; a dark strip along
the top, edged mint at row 10; and the dock, the dark band the section
buttons sit on, from row 204 down, edged mint too. Everything a screen puts
on it (header, dock buttons, panels) is drawn by the game side: the plates
there are gen_battle_art.py's, rendered at run time (3ds_bottom_plate.inc).

The file is the battle art's format (gen_battle_art.py): magic, a count,
then elements; the one element here is the backdrop, RGB565 in the canvas'
layout (column by column, each from the bottom row up).

    python scripts/gen_menu_art.py --output romfs/bottom/menu.bin
"""
import argparse
import math
import struct
import sys
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_battle_art as G  # noqa: E402

MAGIC = b"EM3DMNU1"
W, H = 320, 240
ID_BACKDROP = 0

# The backdrop's colours: the inside light, the strips dark.
GRADIENT = [(0, (168, 226, 190)), (0.55, (146, 212, 172)), (1, (118, 192, 148))]
VIGNETTE, VIGNETTE_ALPHA = (70, 140, 104), 0.35
LATTICE, LATTICE_ALPHA = (236, 250, 240), 0.22
GEM_FACE, GEM_LIP_LOW, GEM_LIP_HIGH = (120, 196, 152), (236, 252, 242), (72, 144, 106)
GEM_ALPHA = 0.7
GEM_CENTRE, GEM_SIZE = (160, 112), (196, 150)
MINT = (160, 224, 184)
STRIP_TOP = [(0, (14, 40, 30)), (1, (24, 72, 56))]
DOCK_Y = 204
DOCK = [(0, (24, 72, 56)), (1, (10, 32, 24))]


def backdrop():
    SS = G.SS
    p = G.Pic(W, H, (0, 0, 0))
    sz = p.im.size
    p.paint_img(G.vgrad(p, 0, H, GRADIENT), Image.new("L", sz, 255))
    rg = Image.radial_gradient("L").resize((sz[0] * 13 // 10, sz[1] * 15 // 10))
    vig = Image.new("L", sz, 255)
    vig.paste(rg, ((sz[0] - rg.width) // 2, (sz[1] - rg.height) // 2))
    vig = vig.point(lambda v: max(0, min(255, (v - 120) * 2)))
    p.paint(VIGNETTE, vig, VIGNETTE_ALPHA)
    # the column's rail facet, an 8px diamond lattice
    lat = p.blank()
    d = ImageDraw.Draw(lat)
    for y in range(0, H, 8):
        for x in range(0, W, 8):
            d.polygon([((x + 4) * SS, y * SS), ((x + 8) * SS, (y + 4) * SS), ((x + 4) * SS, (y + 8) * SS),
                       (x * SS, (y + 4) * SS)], outline=255)
    p.paint(LATTICE, lat, LATTICE_ALPHA)
    # an emerald, step cut: outer octagon, table, the facet lines between them
    cx, cy = GEM_CENTRE
    gw, gh = GEM_SIZE
    shapes = p.blank()
    sd = ImageDraw.Draw(shapes)

    def octo(w, h, c):
        return [(x * SS, y * SS) for x, y in G.chamfer_pts(cx - w / 2, cy - h / 2, w, h, c)]
    sd.polygon(octo(gw, gh, 44), fill=255)
    sd.polygon(octo(gw - 16, gh - 16, 38), fill=0)
    sd.polygon(octo(gw - 52, gh - 52, 24), fill=255)
    sd.polygon(octo(gw - 62, gh - 62, 20), fill=0)
    outer = G.chamfer_pts(cx - gw / 2 + 8, cy - gh / 2 + 8, gw - 16, gh - 16, 38)
    inner = G.chamfer_pts(cx - (gw - 52) / 2, cy - (gh - 52) / 2, gw - 52, gh - 52, 24)
    for a, b in zip(outer, inner):
        sd.line([(a[0] * SS, a[1] * SS), (b[0] * SS, b[1] * SS)], fill=255, width=3 * SS)
    table = p.blank()
    ImageDraw.Draw(table).polygon(octo(gw - 62, gh - 62, 20), fill=255)
    p.paint((150, 230, 180), table, 0.07)
    lip_low = ImageChops.subtract(ImageChops.offset(shapes, 0, SS), shapes)
    lip_high = ImageChops.subtract(ImageChops.offset(shapes, 0, -SS), shapes)
    p.paint(GEM_FACE, shapes, GEM_ALPHA)
    p.paint(GEM_LIP_LOW, lip_low, 0.8 * GEM_ALPHA)
    p.paint(GEM_LIP_HIGH, lip_high, 0.8 * GEM_ALPHA)
    gloss = p.blank()
    ImageDraw.Draw(gloss).polygon([((cx - 40) * SS, (cy - 30) * SS), ((cx - 4) * SS, (cy - 30) * SS),
                                   ((cx - 30) * SS, (cy + 6) * SS), ((cx - 48) * SS, (cy + 6) * SS)], fill=255)
    p.paint((255, 255, 255), gloss.filter(ImageFilter.GaussianBlur(3 * SS)), 0.10)
    p.paint((0, 0, 0), G.scanlines(p), 0.04)
    # a straight top strip, edged with the rail's light green
    top = p.blank()
    ImageDraw.Draw(top).rectangle((0, 0, sz[0], 10 * SS), fill=255)
    p.paint_img(G.vgrad(p, 0, 11, STRIP_TOP), top)
    line = p.blank()
    ImageDraw.Draw(line).line([(0, 10 * SS), (sz[0], 10 * SS)], fill=255, width=SS)
    p.paint(MINT, line, 0.9)
    # the dock: a dark band to the bottom edge, its top edged mint, glowing a little
    band = G.poly(p, [(0, DOCK_Y), (W, DOCK_Y), (W, H), (0, H)])
    p.paint_img(G.vgrad(p, DOCK_Y, H - DOCK_Y, DOCK), band)
    edge = p.blank()
    ImageDraw.Draw(edge).line([(0, DOCK_Y * SS), (W * SS, DOCK_Y * SS)], fill=255, width=round(1.5 * SS))
    p.paint(MINT, edge.filter(ImageFilter.GaussianBlur(1.5 * SS)), 0.35)
    p.paint(MINT, edge)
    p.paint((0, 0, 0), G.mul(G.scanlines(p), band), 0.06)
    return p.im.resize((W, H), Image.LANCZOS)


def build():
    bd = backdrop().load()
    raw = bytearray(struct.pack("<BBhhHH", ID_BACKDROP, 8, 0, 0, W, H))
    for x in range(W):
        for y in range(H - 1, -1, -1):
            raw += struct.pack("<H", G.rgb565(bd[x, y]))
    return MAGIC + struct.pack("<H", 1) + bytes(raw)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--output", required=True)
    ap.add_argument("--png", help="also write the backdrop as a PNG, to look at")
    args = ap.parse_args()
    Path(args.output).parent.mkdir(parents=True, exist_ok=True)
    Path(args.output).write_bytes(build())
    if args.png:
        backdrop().save(args.png)
    print("menu art: %s" % args.output)


if __name__ == "__main__":
    main()
