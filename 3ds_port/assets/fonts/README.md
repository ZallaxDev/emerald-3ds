# Chinese glyph sheets

`chinese_small.png` and `chinese_normal.png` are the Simplified Chinese glyph
sheets `src/chinese_text.c` draws Han characters from. They are the port's own
art: nothing in them comes from a cartridge.

Regenerate them with an open-source CJK face:

```sh
python 3ds_port/scripts/gen_chinese_font.py \
    --charmap build/upstream/charmap.txt \
    --font /path/to/NotoSansCJKsc-Regular.otf
```

The face itself is **not committed** (14 MiB, and its licence is the font's,
not this project's). `Noto Sans CJK SC` (SIL OFL 1.1) is what these sheets were
rendered from: download it from
<https://github.com/notofonts/noto-cjk/tree/main/Sans/OTF/SimplifiedChinese>.

## Layout

One sheet is a `.latfont`: 256 px wide, 2 bpp, four colours (`#ffffff` the
background, `#272727` the ink, `#808080` the shadow), 16x16 a glyph, 16 glyphs
a row — the format `tools/gbagfx` reads for the game's own Latin fonts, so the
port's font loader (`3ds_compat.c`) carries these unchanged.

Slot `n` holds the character whose Han code `(hi, lo)` has

```
n = ((hi - 1 - (hi > 0x06) - (hi > 0x1B)) << 8) | lo
```

which is what `SheetIndex()` in `src/chinese_text.c` computes: the code space
skips `0x06` and `0x1B`, the bytes `POKéMON` needs. The nine sentence
punctuation marks follow the last Han glyph, in the order `sPunctCodes[]` lists
them (slot 7025 onwards).

The two sheets carry the same slots; the small one draws them 10 px from row 3
and the normal one 12 px from row 1, which is the geometry the game's own
Chinese sheets use.
