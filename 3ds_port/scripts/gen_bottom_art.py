#!/usr/bin/env python3
"""
The bottom screen's own pixel art: the RUN button's shoe (off and on), the
Y badge of the registered-item button, the column's Pokédex icon (Emerald has
no Pokédex item graphic), and the title screen's "Developed by" logo
(assets/artwork/logo.png). Everything else the bottom screen draws comes from
the game.

The indexed PNGs in assets/artwork/bottom/ are the source; this script turns
them into src/3ds_bottom_art.h (4bpp tiles in the GBA's one-dimensional
sprite layout and one 16-colour BGR555 palette, colour 0 transparent; the
logo as one coverage byte a pixel, its alpha, to lay its white over the
screen).

    python scripts/gen_bottom_art.py          # PNG -> header
    python scripts/gen_bottom_art.py --init   # (re)draw the PNGs from the
                                              # ASCII below first
"""
import os
import sys

from PIL import Image

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

SPRITES = [
    # (file, C name, tiles wide, tiles high)
    ("shoe_off.png", "sArtShoeOff", 3, 3),
    ("shoe_on.png", "sArtShoeOn", 3, 3),
    ("badge_y.png", "sArtBadgeY", 2, 2),
    # Mastered from Pokémon Platinum's menu Pokédex icon (TSR sheet 25668).
    ("pokedex.png", "sArtPokedex", 3, 3),
]


def ascii_image(rows):
    index = {ch: i for i, (ch, _) in enumerate(PALETTE)}
    im = Image.new("P", (len(rows[0]), len(rows)), 0)
    flat = []
    for _, c in PALETTE:
        flat += list(c)
    im.putpalette(flat + [0] * (768 - len(flat)))
    px = im.load()
    for y, row in enumerate(rows):
        for x, ch in enumerate(row):
            px[x, y] = index.get(ch, 0)
    return im


def init_pngs():
    os.makedirs(ART, exist_ok=True)
    on = [list(r) for r in SHOE]
    for x0, y, n in SPEED:
        for i in range(n):
            on[y][x0 + i] = "S" if i == n - 1 else "s"
    ascii_image(SHOE).save(os.path.join(ART, "shoe_off.png"))
    ascii_image(["".join(r) for r in on]).save(os.path.join(ART, "shoe_on.png"))
    ascii_image(BADGE_Y).save(os.path.join(ART, "badge_y.png"))


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
        init_pngs()
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
    for name, cname, wt, ht in SPRITES:
        im = Image.open(os.path.join(ART, name))
        if im.mode != "P" or im.size != (wt * 8, ht * 8):
            sys.exit("%s: expected an indexed %dx%d PNG" % (name, wt * 8, ht * 8))
        data = tiles_4bpp(im, wt, ht)
        lines.append("")
        lines.append("/* %s: %dx%d tiles. */" % (name, wt, ht))
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
