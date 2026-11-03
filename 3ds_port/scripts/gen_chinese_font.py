#!/usr/bin/env python3
"""Assemble the port's Chinese glyph sheets from rh-hideout-chinese's.

Provenance: every glyph here is copied out of a sheet committed in
rh-hideout-chinese/pokeemerald-expansion. That project ships its Chinese art as
two ready-made bitmaps -- chinese_normal.png and chinese_small.png, 256 px wide,
16x16 a glyph -- with no font file and no generator, so its glyphs cannot be
reproduced from a face even if we wanted to. Copying them keeps this port's
text in the same hand as the translation it carries. See NOTICE.md.

What this script does, and why it is not a straight copy:

* the fork holds Han characters in slots 0..7024 (slot 7025 has stray art and
  no charmap entry), and this port's nine sentence marks need slots as well:
  they go from 7026, taken from the fork's own full-width marks;
* those marks are not in the Chinese sheets at all -- the fork draws them in
  latin_normal.png and latin_small.png, over the kana/half-width bytes it
  repointed for Chinese (0x30 ・, 0x37 。, 0x39 ～, 0x3A 、, 0x3B ，, 0x3C ！,
  0x3D ？, 0x3E ：, 0x3F —). They are copied from there, and their two-bit
  values are remapped: the Latin sheets index 0/3 as background, the Chinese
  ones 0, which a straight copy would turn into noise;
* the result keeps the .latfont geometry gbagfx reads (16 glyphs a row), so the
  port's existing font loader carries it with no change.

Run with --fork pointing at a checkout of
rh-hideout-chinese/pokeemerald-expansion. Nothing is downloaded.
"""
from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]  # 3ds_port/

# 。、，！？：—～・ in CHINESE_PUNCT order (src/chinese_text.c)
MARKS = "。、，！？：—～・"
MARKS_SLOT = 7026          # the sheet slot of 。; 7025 is the fork's stray art
# The fork draws its full-width marks in the *Latin* sheets, over the kana/half-
# width bytes it repointed for Chinese: 0x30 ・, 0x37 。, 0x39 ～, 0x3A 、,
# 0x3B ，, 0x3C ！, 0x3D ？, 0x3E ：, 0x3F —. Copying those keeps the marks in
# the same hand as the Kaishu glyphs, which a locally drawn mark does not.
FORK_MARK_SLOTS = {
    "。": 0x37, "、": 0x3A, "，": 0x3B, "！": 0x3C, "？": 0x3D,
    "：": 0x3E, "—": 0x3F, "～": 0x39, "・": 0x30,
}


def copy_mark(latin: Image.Image, ch: str, out: Image.Image, slot: int) -> bool:
    """Copy one of the fork's full-width marks out of its Latin sheet.

    Only the ink is copied, and the shadow with it: the Latin sheet's cell is
    the same 16x16 the Chinese sheet uses, and the marks are left-aligned in
    both, so no repositioning is needed."""
    src = FORK_MARK_SLOTS.get(ch)
    if src is None:
        return False
    sx, sy = (src % 16) * 16, (src // 16) * 16
    dx, dy = (slot % 16) * 16, (slot // 16) * 16
    if sx + 16 > latin.width or sy + 16 > latin.height:
        return False
    # The Latin sheets index their palette as 0/3 background, 1 ink, 2 shadow;
    # the Chinese sheets use 0 background, 1 ink, 2 shadow. Map across.
    for r in range(16):
        for c in range(16):
            v = latin.getpixel((sx + c, sy + r))
            out.putpixel((dx + c, dy + r), v if v in (1, 2) else 0)
    return True


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--fork", type=Path, required=True, help="a rh-hideout-chinese checkout")
    ap.add_argument("--out", type=Path, default=ROOT / "assets" / "fonts")
    args = ap.parse_args()

    fontdir = args.fork / "graphics/fonts"
    missing = []
    for size, name, latin_name in (("normal", "chinese_normal.png", "latin_normal.png"),
                                   ("small", "chinese_small.png", "latin_small.png")):
        src = fontdir / name
        latin = Image.open(fontdir / latin_name).convert("P")
        latin.putpalette([255, 255, 255, 39, 39, 39, 128, 128, 128] + [0] * (256 * 3 - 9))
        sheet = Image.open(src).convert("P")
        sheet.putpalette([255, 255, 255, 39, 39, 39, 128, 128, 128] + [0] * (256 * 3 - 9))
        width, height = sheet.size
        # The port keeps the sheet's own geometry; the marks sit below the Han
        # characters and 7088 slots already cover 7026..7034.
        for n, ch in enumerate(MARKS):
            if not copy_mark(latin, ch, sheet, MARKS_SLOT + n):
                missing.append((size, ch))
        args.out.mkdir(parents=True, exist_ok=True)
        sheet.save(args.out / name)
        print("%s: %dx%d, %d slots -> %s" % (name, width, height,
                                             (width // 16) * (height // 16), args.out / name))
    if missing:
        print("marks the fork does not draw:", missing)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
