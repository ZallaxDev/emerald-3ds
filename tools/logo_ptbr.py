#!/usr/bin/env python3
"""Preview of the Portuguese title logo, VERSÃO over ESMERALDA:

    python tools/logo_ptbr.py --tree build/upstream --out logo.png

The logo is the table sLogo in 3ds_port/src/3ds_icons_ptbr.c (one hex digit a
pixel, an index into the English logo's palette), which DrawLogo writes over
the title screen's logo when it is loaded. This reads the table and the
palette from the tree and writes nothing into the repository.
"""
import argparse
import re
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
W, H = 128, 32


def table(source=ROOT / '3ds_port/src/3ds_icons_ptbr.c'):
    """sLogo's rows, as lists of palette indices."""
    text = Path(source).read_text(encoding='utf-8')
    body = text[text.index('sLogo[LOGO_H][LOGO_W + 1] = {'):]
    rows = re.findall(r'"([0-9a-f]{%d})"' % W, body[:body.index('};')])
    if len(rows) != H:
        raise SystemExit('logo_ptbr: sLogo has %d rows of %d, not %d' % (len(rows), W, H))
    return [[int(c, 16) for c in row] for row in rows]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--tree', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    pal = Image.open(args.tree / 'graphics/title_screen/emerald_version.png').getpalette()
    img = table()
    im = Image.new('P', (W, H))
    im.putpalette(pal)
    for y in range(H):
        for x in range(W):
            im.putpixel((x, y), img[y][x])
    im.convert('RGB').resize((W * 8, H * 8), Image.NEAREST).save(args.out)
    print('preview:', args.out)


if __name__ == '__main__':
    main()
