#!/usr/bin/env python3
"""Preview of the Portuguese type icons (the menu_info sheet), drawn over the
player's own sheet: the English letters are cleared and the Portuguese name
is written in a small capital font drawn for this project.

    python tools/type_icons_ptbr.py --tree build/upstream --out preview.png

This is the reference for the port's own drawing at load time; it reads the
sheet from the tree (built from pret, or staged from the player's ROM) and
writes nothing into the repository.
"""
from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image

# A 3-pixel-wide, 7-pixel-high capital font (# ink), drawn for this project;
# accents and the tilde sit on the row above (row -1).
GLYPHS = {
    'A': ['.#.', '#.#', '#.#', '###', '#.#', '#.#', '#.#'],
    'B': ['##.', '#.#', '#.#', '##.', '#.#', '#.#', '##.'],
    'C': ['.##', '#..', '#..', '#..', '#..', '#..', '.##'],
    'D': ['##.', '#.#', '#.#', '#.#', '#.#', '#.#', '##.'],
    'E': ['###', '#..', '#..', '##.', '#..', '#..', '###'],
    'F': ['###', '#..', '#..', '##.', '#..', '#..', '#..'],
    'G': ['.##', '#..', '#..', '#.#', '#.#', '#.#', '.##'],
    'H': ['#.#', '#.#', '#.#', '###', '#.#', '#.#', '#.#'],
    'I': ['###', '.#.', '.#.', '.#.', '.#.', '.#.', '###'],
    'L': ['#..', '#..', '#..', '#..', '#..', '#..', '###'],
    'M': ['#...#', '##.##', '#.#.#', '#.#.#', '#...#', '#...#', '#...#'],
    'N': ['#..#', '##.#', '##.#', '#.##', '#.##', '#..#', '#..#'],
    'O': ['.#.', '#.#', '#.#', '#.#', '#.#', '#.#', '.#.'],
    'P': ['##.', '#.#', '#.#', '##.', '#..', '#..', '#..'],
    'Q': ['.#.', '#.#', '#.#', '#.#', '#.#', '##.', '.##'],
    'R': ['##.', '#.#', '#.#', '##.', '#.#', '#.#', '#.#'],
    'S': ['.##', '#..', '#..', '.#.', '..#', '..#', '##.'],
    'T': ['###', '.#.', '.#.', '.#.', '.#.', '.#.', '.#.'],
    'U': ['#.#', '#.#', '#.#', '#.#', '#.#', '#.#', '###'],
    'V': ['#.#', '#.#', '#.#', '#.#', '#.#', '.#.', '.#.'],
    '.': ['...', '...', '...', '...', '...', '...', '.#.'],
}
ACCENTS = {'Á': ('A', '.#.'), 'É': ('E', '.#.'), 'Í': ('I', '.#.'), 'Ã': ('A', '###'), 'Õ': ('O', '###')}
CEDILLA = {'Ç': 'C'}

# The Portuguese names, short enough for the 32-pixel icons (6 letters, as
# NORMAL and PSYCHC are in English).
TYPES = {  # menu_info tile offset: name
    0x20: 'NORMAL', 0x64: 'LUTADOR', 0x60: 'VOADOR', 0x80: 'VENENO', 0x48: 'TERRA', 0x44: 'PEDRA',
    0x6C: 'INSETO', 0x68: 'FANTAS', 0x88: 'AÇO', 0x24: 'FOGO', 0x28: 'ÁGUA', 0x2C: 'PLANTA',
    0x40: 'ELÉTR.', 0x84: 'PSÍQ.', 0x4C: 'GELO', 0xA0: 'DRAGÃO', 0x8C: 'SOMBR.',
}
INK, SHADOW = 0xF, 0xE
# The move window's labels, 42x12: white letters outlined in the colour of
# the bar they sit on (rows 4-8).
LABELS = {0xA8: 'TIPO', 0xC0: 'PODER', 0xC8: 'PRECISÃO', 0xE0: 'PP', 0xE8: 'EFEITO'}


def glyph(ch):
    """(rows from -1 to 7, width): '#' ink, '.' none."""
    if ch in ACCENTS:
        base, accent = ACCENTS[ch]
        rows = [accent] + GLYPHS[base]
    else:
        rows = ['.'] + GLYPHS[CEDILLA.get(ch, ch)]
    if ch in CEDILLA:
        rows = rows + ['.#.']
    if ch == '.':
        return [r[1:2] for r in rows], 1
    width = max(len(r) for r in rows)
    return [r.ljust(width, '.') for r in rows], width


def draw_type(px, off, name):
    x0, y0 = (off % 16) * 8, (off // 16) * 8
    # Some icons fade to a second colour at the bottom: each row is cleared
    # with its own background, read at the icon's right edge.
    row_bg = {}
    for y in range(y0, y0 + 12):
        row_bg[y] = next(px[x, y] for x in range(x0 + 31, x0, -1) if px[x, y] not in (INK, SHADOW, 0))
        for x in range(x0, x0 + 32):
            if px[x, y] in (INK, SHADOW):
                px[x, y] = row_bg[y]
    glyphs = [glyph(ch) for ch in name]
    total = sum(w for _, w in glyphs) + (len(glyphs) - 1)
    x = x0 + (31 - total) // 2
    for rows, w in glyphs:
        for r, row in enumerate(rows):
            y = y0 + 1 + r  # row -1 lands on the icon's row 1, the letters on rows 2-8
            for c, ch in enumerate(row):
                if ch == '#':
                    px[x + c, y] = INK
                    for dx, dy in ((1, 0), (0, 1), (1, 1)):
                        if px[x + c + dx, y + dy] == row_bg[y + dy]:
                            px[x + c + dx, y + dy] = SHADOW
        x += w + 1


def draw_label(px, off, name):
    x0, y0 = (off % 16) * 8, (off // 16) * 8
    # The bar back without its letters, and nothing above or below it.
    for y in range(y0, y0 + 12):
        for x in range(x0, x0 + 42):
            if 4 <= y - y0 <= 8:
                if px[x, y] != 0:
                    px[x, y] = SHADOW
            else:
                px[x, y] = 0
    glyphs = [glyph(ch) for ch in name]
    total = sum(w for _, w in glyphs) + (len(glyphs) - 1)
    x = x0 + (41 - total) // 2
    inked = set()
    for rows, w in glyphs:
        for r, row in enumerate(rows):
            for c, ch in enumerate(row):
                if ch == '#':
                    inked.add((x + c, y0 + 2 + r))  # row -1 on the label's row 2, the letter on rows 3-9
        x += w + 1
    for (ix, iy) in inked:
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                if (ix + dx, iy + dy) not in inked:
                    px[ix + dx, iy + dy] = SHADOW
    for (ix, iy) in inked:
        px[ix, iy] = INK


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--tree', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    im = Image.open(args.tree / 'graphics/interface/menu_info.png')
    px = im.load()
    for off, name in TYPES.items():
        draw_type(px, off, name)
    for off, name in LABELS.items():
        draw_label(px, off, name)
    big = im.convert('RGB').resize((im.width * 4, im.height * 4), Image.NEAREST)
    big.save(args.out)
    print('preview:', args.out)


if __name__ == '__main__':
    main()
