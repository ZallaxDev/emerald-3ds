#!/usr/bin/env python3
"""
The bottom screen's own pixel art: the RUN button's shoe (off and on), the
Y badge of the registered-item button, the dock's Pokédex icon (Emerald has
no Pokédex item graphic), the header's grass (the wild list's toggle), sun and
moon (the clock), the Nuzlocke tracker's tombstone, the trainer card's star,
a fallen Pokémon's cross, and the title screen's "Developed by" logo
(assets/artwork/logo.png). Everything else the bottom screen draws comes from
the game.

The indexed PNGs in assets/artwork/bottom/ are the source; this script turns
them into src/3ds_bottom_art.h (4bpp tiles in the GBA's one-dimensional
sprite layout and two 16-colour BGR555 palettes, colour 0 transparent; the
logo as one coverage byte a pixel, its alpha, to lay its white over the
screen).

    python scripts/gen_bottom_art.py          # PNG -> header
    python scripts/gen_bottom_art.py --init   # (re)draw the PNGs from the
                                              # ASCII below first
"""
import math
import os
import sys

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.dirname(HERE)
ART = os.path.join(PORT, "assets", "artwork", "bottom")
LOGO = os.path.join(PORT, "assets", "artwork", "logo.png")
OUT = os.path.join(PORT, "src", "3ds_bottom_art.h")

# One palette for all of it: the shoe's colours in the item icons' manner
# (outline 48,48,48 and four reds), then the red and black of the bag's SEL
# pill (graphics/bag/select_button.png). Colours are 8-bit, written as the
# GBA's 15-bit values (r >> 3).
PALETTE = [
    ("_", (0, 0, 0)),         # transparent
    ("k", (48, 48, 48)),      # outline
    ("r", (232, 56, 40)),     # red
    ("d", (168, 24, 32)),     # red, shade
    ("l", (255, 128, 104)),   # red, light
    ("w", (248, 248, 248)),   # sole, lining, laces
    ("g", (184, 184, 192)),   # sole stripes
    ("n", (80, 80, 96)),      # laces' shadow
    ("y", (248, 208, 64)),    # heel tag
    ("s", (144, 216, 168)),   # speed lines
    ("S", (255, 255, 255)),   # speed lines' tips
    ("K", (0, 0, 0)),         # badge outline and letter
    ("R", (255, 0, 0)),       # badge red
    ("e", (48, 216, 72)),     # Pokédex LED
    ("E", (24, 140, 48)),     # Pokédex LED shade
]

SHOE = [
    "........................",
    "........................",
    "........................",
    "........................",
    "........................",
    "......kkkkk.............",
    ".....krrlllk............",
    ".....krwwwwrk...........",
    "....kdrwnnwkrk..........",
    "....kdrrwwknwrk.........",
    "....kdrrrkwnwnrkk.......",
    "....kddrrrkwnwnrrkk.....",
    "....kddrrrrkwnwrrrrkk...",
    "....kdddrrrrkkkrrrrlrk..",
    "....kydddrrrrrrrrrrrllk.",
    "....kyydddrrrrrrrrrwwwk.",
    "....kwwwwwwwwwwwwwwwwwk.",
    "....kgwgwgwgwgwgwgwgggk.",
    ".....kkkkkkkkkkkkkkkkk..",
    "........................",
    "........................",
    "........................",
    "........................",
    "........................",
]
# Running on: three speed lines behind the heel, (x, y, length).
SPEED = [(0, 9, 3), (0, 12, 3), (0, 15, 3)]

BADGE_Y = [
    "..KKKKKKKKK.....",
    ".KKRRRRRRRKK....",
    "KKRRKRRRKRRKK...",
    "KRRRKRRRKRRRK...",
    "KRRRRKRKRRRRK...",
    "KRRRRRKRRRRRK...",
    "KRRRRRKRRRRRK...",
    "KRRRRRKRRRRRK...",
    "KKRRRRKRRRRKK...",
    ".KKRRRRRRRKK....",
    "..KKKKKKKKK.....",
    "................",
    "................",
    "................",
    "................",
    "................",
]

# The second palette: the header's and the Nuzlocke screen's new pictures.
PALETTE2 = [
    ("_", (0, 0, 0)),         # transparent
    ("k", (24, 72, 40)),      # grass outline
    ("g", (96, 184, 72)),     # grass
    ("l", (168, 224, 120)),   # grass, light
    ("d", (48, 128, 48)),     # grass, shade
    ("o", (248, 168, 32)),    # sun rays
    ("y", (248, 208, 64)),    # sun, star
    ("O", (200, 112, 24)),    # sun's and star's outline
    ("h", (255, 248, 200)),   # sun's highlight, moon
    ("K", (48, 48, 48)),      # tombstone outline
    ("w", (248, 248, 248)),   # tombstone, light
    ("G", (184, 184, 200)),   # tombstone
    ("s", (128, 128, 152)),   # tombstone, shade
    ("n", (144, 144, 168)),   # tombstone's engraving
    ("r", (232, 56, 40)),     # the ball on the tombstone
    ("L", (255, 128, 104)),   # its light
]

GRASS = [
    "................",
    "....k.....k.....",
    "...kgk...kgk....",
    "...kgk.k.kgk..k.",
    "..kglgkgkkgk.kgk",
    "..kgggkglkgkkglk",
    ".kglggkgggkkgggk",
    ".kggggkggggkgggk",
    "kgdggggdgggggdgk",
    "kddgdddgddgddddk",
    "kkkkkkkkkkkkkkkk",
] + ["." * 16] * 5

NUZ = [
    "........................", "........................",
    "........KKKKKKKK........",
    "......KKwwwwwwwwKK......",
    ".....KwwwGGGGGGGGsK.....",
    "....KwwGGGGGGGGGGssK....",
    "....KwGGGGGKKKKGGGGsK...",
    "....KwGGGGKrrrrKGGGsK...",
    "....KwGGGGKrLrrKGGGsK...",
    "....KwGGGGKKKKKKGGGsK...",
    "....KwGGGGKwwwwKGGGsK...",
    "....KwGGGGGKKKKGGGGsK...",
    "....KwGGGGGGGGGGGGGsK...",
    "....KwGGnnnnnnnnGGGsK...",
    "....KwGGGGGGGGGGGGGsK...",
    "....KwGGGnnnnnnGGGGsK...",
    "....KwGGGGGGGGGGGGGsK...",
    "....KwGGGGGGGGGGGGssK...",
    "...KsssssssssssssssssK..",
    "..KgggLggggLggggLggggK..",
    "..KgdgggdgggdggggdgggK..",
    "...KKKKKKKKKKKKKKKKKK...",
    "........................", "........................",
]
# The tombstone's own grass: the palette's grass, its light and its shade.
NUZ = [r.replace("L", "l") if i >= 19 else r for i, r in enumerate(NUZ)]

# A fallen Pokémon's cross, in the first palette's reds.
TINY_X = [
    "rd....rd",
    ".rd..rd.",
    "..rdrd..",
    "..rdrd..",
    ".rd..rd.",
    "rd....rd",
    "........",
    "........",
]

# The map's zoom button (from public PR #108's MAP zoom): a magnifying glass
# with a plus to zoom in, a minus to zoom back out, in the first palette.
ZOOM_IN = [
    "................",
    "...kkkkk........",
    "..kwwwwwk.......",
    ".kwwwkwwwk......",
    ".kwwwkwwwk......",
    ".kwkkkkkwk......",
    ".kwwwkwwwk......",
    ".kwwwkwwwk......",
    "..kwwwwwkk......",
    "...kkkkknkk.....",
    "........knkk....",
    ".........knkk...",
    "..........knkk..",
    "...........kkk..",
    "................",
    "................",
]
ZOOM_OUT = [r.replace("kwwwkwwwk", "kwwwwwwwk") for r in ZOOM_IN]

SPRITES = [
    # (file, C name, tiles wide, tiles high, palette)
    ("shoe_off.png", "sArtShoeOff", 3, 3, 1),
    ("shoe_on.png", "sArtShoeOn", 3, 3, 1),
    ("badge_y.png", "sArtBadgeY", 2, 2, 1),
    # Mastered from Pokémon Platinum's menu Pokédex icon (TSR sheet 25668).
    ("pokedex.png", "sArtPokedex", 3, 3, 1),
    ("tiny_x.png", "sArtTinyX", 1, 1, 1),
    ("grass.png", "sArtGrass", 2, 2, 2),
    ("sun.png", "sArtSun", 2, 2, 2),
    ("moon.png", "sArtMoon", 2, 2, 2),
    ("star.png", "sArtStar", 2, 2, 2),
    ("nuzlocke.png", "sArtNuzlocke", 3, 3, 2),
    ("zoom_in.png", "sArtZoomIn", 2, 2, 1),
    ("zoom_out.png", "sArtZoomOut", 2, 2, 1),
]


def ascii_image(rows, palette=PALETTE):
    index = {ch: i for i, (ch, _) in enumerate(palette)}
    im = Image.new("P", (len(rows[0]), len(rows)), 0)
    flat = []
    for _, c in palette:
        flat += list(c)
    im.putpalette(flat + [0] * (768 - len(flat)))
    px = im.load()
    for y, row in enumerate(rows):
        for x, ch in enumerate(row):
            px[x, y] = index.get(ch, 0)
    return im


def init_pngs(force):
    def save(im, name):
        # Only a missing PNG is drawn again, unless asked: they may have been touched up by hand.
        path = os.path.join(ART, name)
        if force or not os.path.exists(path):
            im.save(path)

    os.makedirs(ART, exist_ok=True)
    on = [list(r) for r in SHOE]
    for x0, y, n in SPEED:
        for i in range(n):
            on[y][x0 + i] = "S" if i == n - 1 else "s"
    save(ascii_image(SHOE), "shoe_off.png")
    save(ascii_image(["".join(r) for r in on]), "shoe_on.png")
    save(ascii_image(BADGE_Y), "badge_y.png")
    save(ascii_image(TINY_X), "tiny_x.png")
    save(ascii_image(ZOOM_IN), "zoom_in.png")
    save(ascii_image(ZOOM_OUT), "zoom_out.png")
    save(ascii_image(GRASS, PALETTE2), "grass.png")
    save(ascii_image(NUZ, PALETTE2), "nuzlocke.png")
    save(drawn_image(draw_sun), "sun.png")
    save(drawn_image(draw_moon), "moon.png")
    save(drawn_image(draw_star), "star.png")


def draw_sun(d):
    for a in range(0, 360, 45):
        x = 7 + round(6.5 * math.cos(math.radians(a)))
        y = 7 + round(6.5 * math.sin(math.radians(a)))
        d.point((x, y), fill=PALETTE2_INDEX["o"])
    d.ellipse((3, 3, 11, 11), fill=PALETTE2_INDEX["y"], outline=PALETTE2_INDEX["O"])
    d.point((5, 5), fill=PALETTE2_INDEX["h"])
    d.point((6, 5), fill=PALETTE2_INDEX["h"])


def draw_moon(d):
    # A crescent: the disc less the same disc moved up and right.
    d.ellipse((2, 2, 12, 12), fill=PALETTE2_INDEX["O"])
    d.ellipse((3, 3, 11, 11), fill=PALETTE2_INDEX["h"])
    d.ellipse((6, 0, 14, 8), fill=0)


def draw_star(d):
    pts = [(5 + 5 * math.cos(math.radians(-90 + 36 * k)) * (1 if k % 2 == 0 else 0.45),
            5 + 5 * math.sin(math.radians(-90 + 36 * k)) * (1 if k % 2 == 0 else 0.45)) for k in range(10)]
    d.polygon(pts, fill=PALETTE2_INDEX["y"], outline=PALETTE2_INDEX["O"])


PALETTE2_INDEX = {ch: i for i, (ch, _) in enumerate(PALETTE2)}


def drawn_image(draw):
    """A 16x16 picture drawn with PIL's primitives in the second palette's indices."""
    im = Image.new("P", (16, 16), 0)
    flat = []
    for _, c in PALETTE2:
        flat += list(c)
    im.putpalette(flat + [0] * (768 - len(flat)))
    draw(ImageDraw.Draw(im))
    return im


def tiles_4bpp(im, wt, ht):
    px = im.load()
    out = bytearray()
    for ty in range(ht):
        for tx in range(wt):
            for y in range(8):
                for x in range(0, 8, 2):
                    lo = px[tx * 8 + x, ty * 8 + y] & 15
                    hi = px[tx * 8 + x + 1, ty * 8 + y] & 15
                    out.append(lo | (hi << 4))
    return out


def main():
    if "--init" in sys.argv or not all(os.path.exists(os.path.join(ART, f)) for f, *_ in SPRITES):
        init_pngs("--init" in sys.argv)
    lines = [
        "/* Generated by scripts/gen_bottom_art.py from the PNGs in assets/artwork/bottom. Do not edit. */",
        "#ifndef CTR_BOTTOM_ART_H",
        "#define CTR_BOTTOM_ART_H",
        "",
        "/* BGR555; colour 0 is transparent. */",
        "static const u16 sArtPalette[16] = {",
    ]
    pal = [((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)) for _, (r, g, b) in PALETTE]
    pal += [0] * (16 - len(pal))
    lines.append("    " + ", ".join("0x%04X" % v for v in pal) + ",")
    lines.append("};")
    lines.append("static const u16 sArtPalette2[16] = {")
    pal = [((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)) for _, (r, g, b) in PALETTE2]
    pal += [0] * (16 - len(pal))
    lines.append("    " + ", ".join("0x%04X" % v for v in pal) + ",")
    lines.append("};")
    for name, cname, wt, ht, palette in SPRITES:
        im = Image.open(os.path.join(ART, name))
        if im.mode != "P" or im.size != (wt * 8, ht * 8):
            sys.exit("%s: expected an indexed %dx%d PNG" % (name, wt * 8, ht * 8))
        data = tiles_4bpp(im, wt, ht)
        lines.append("")
        lines.append("/* %s: %dx%d tiles, sArtPalette%s. */" % (name, wt, ht, "" if palette == 1 else "2"))
        lines.append("static const u8 %s[%d] = {" % (cname, len(data)))
        for i in range(0, len(data), 16):
            lines.append("    " + ", ".join("0x%02X" % b for b in data[i:i + 16]) + ",")
        lines.append("};")
    logo = Image.open(LOGO).convert("RGBA")
    w, h = logo.size
    alpha = logo.getchannel("A").tobytes()
    lines.append("")
    lines.append("/* logo.png: %dx%d, white at this coverage, row by row. */" % (w, h))
    lines.append("#define ART_LOGO_W %d" % w)
    lines.append("#define ART_LOGO_H %d" % h)
    lines.append("static const u8 sArtLogo[%d] = {" % len(alpha))
    for i in range(0, len(alpha), 16):
        lines.append("    " + ", ".join("0x%02X" % b for b in alpha[i:i + 16]) + ",")
    lines.append("};")
    lines += ["", "#endif", ""]
    with open(OUT, "w", newline="\n") as f:
        f.write("\n".join(lines))
    print("wrote", OUT)


if __name__ == "__main__":
    main()
