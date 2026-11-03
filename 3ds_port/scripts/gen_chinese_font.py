#!/usr/bin/env python3
"""Render the Chinese glyph sheets the pret patch reads.

One sheet per font is a `.latfont`-shaped PNG: 256 px wide, 2 bpp, four
colours, 16x16 a glyph, 16 glyphs a row, so the port's existing font loader
carries it with no change.

Slot n of a sheet is the character whose Han code (hi, lo) has
SheetIndex() == n, the same arithmetic src/chinese_text.c does: hi loses the
two bytes that POKéMON needs (0x06 and 0x1B) before it becomes a row. The nine
sentence punctuation marks follow the last Han glyph, in the order
sPunctCodes[] lists them: 。、，！？：—～・.

The font is not committed and neither is its output: pass --font (an
open-source CJK face) and the sheets land in 3ds_port/assets/fonts/, which the
build compiles with rsfont the way pret compiles its Latin sheets.
"""
from __future__ import annotations

import argparse
import re
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]  # 3ds_port/

# The order sPunctCodes[] in src/chinese_text.c lists them.
PUNCT = ["。", "、", "，", "！", "？", "：", "—", "～", "・"]
PUNCT_FIRST = 0x1B71  # CHINESE_PUNCT_FIRST: 7025, the slot after the last Han glyph


def charmap_pairs(path: Path) -> dict[str, tuple[int, int]]:
    """The two-byte codes of a Chinese charmap, keyed by character."""
    out: dict[str, tuple[int, int]] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.split("@")[0].rstrip()
        m = re.match(r"^'(.+)'\s*=\s*([0-9A-Fa-f]{4})$", line)
        if m:
            out[m.group(1)] = (int(m.group(2)[:2], 16), int(m.group(2)[2:], 16))
    return out


def sheet_index(code: tuple[int, int]) -> int:
    """The sheet slot of a (hi, lo) code, as src/chinese_text.c computes it."""
    hi, lo = code
    if hi > 0x1B:
        hi -= 1
    if hi > 0x06:
        hi -= 1
    return ((hi - 1) << 8) | lo


def render(slots: list[str], font_path: Path, size: int, out: Path, top: int, cols: int = 16) -> int:
    """One sheet. `top` is the first pixel row a glyph may paint, so every
    glyph sits on the same baseline band the Latin ones use."""
    rows = (len(slots) + cols - 1) // cols
    sheet = Image.new("P", (cols * 16, rows * 16), 0)
    sheet.putpalette([255, 255, 255, 39, 39, 39, 128, 128, 128] + [0] * (256 * 3 - 9))
    draw = ImageDraw.Draw(sheet)
    font = ImageFont.truetype(str(font_path), size)
    for n, ch in enumerate(slots):
        if not ch.strip():
            continue
        # One baseline for the sheet: the glyph's own em box, dropped so its
        # ascender starts at `top` in every cell.
        draw.text(((n % cols) * 16, (n // cols) * 16 + top), ch, font=font, fill=1, anchor="lt")
    out.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(out)
    return len(slots)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--charmap", type=Path,
                    help="the charmap holding the Han block (default: ../charmap.txt, the tree this port is in)")
    ap.add_argument("--font", type=Path, required=True)
    ap.add_argument("--out", type=Path, default=ROOT / "assets" / "fonts")
    args = ap.parse_args()

    charmap = args.charmap or (ROOT.parent / "charmap.txt")
    pairs = charmap_pairs(charmap)
    last = max(sheet_index(code) for code in pairs.values())
    slots = [" "] * (last + 1)
    for ch, code in pairs.items():
        slots[sheet_index(code)] = ch
    # The punctuation follows the Han glyphs, where CHINESE_PUNCT_FIRST says.
    assert last + 1 <= PUNCT_FIRST, (last, PUNCT_FIRST)
    slots.extend([" "] * (PUNCT_FIRST - len(slots)))
    slots.extend(PUNCT)

    # The geometry the game's own Chinese sheets use: 12 px glyphs from row 1 in
    # the normal font, 10 px from row 3 in the small one.
    n1 = render(slots, args.font, 10, args.out / "chinese_small.png", top=3)
    n2 = render(slots, args.font, 12, args.out / "chinese_normal.png", top=1)
    print("charmap: %d Han characters, last slot %d" % (len(pairs), last))
    print("sheet: %d slots (%d Han + %d punctuation + holes)"
          % (n1, last + 1, len(PUNCT)))
    print("wrote chinese_small.png and chinese_normal.png in %s" % args.out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
