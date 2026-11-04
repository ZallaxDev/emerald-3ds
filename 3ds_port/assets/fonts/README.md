# Chinese glyph sheets

`chinese_small.png` and `chinese_normal.png` are the Simplified Chinese glyph
sheets `src/chinese_text.c` draws Han characters and punctuation from.

## Where they come from

Every glyph is copied from a sheet committed in
[rh-hideout-chinese/pokeemerald-expansion](https://github.com/rh-hideout-chinese/pokeemerald-expansion):

| Source | What is taken |
| :--- | :--- |
| `graphics/fonts/chinese_normal.png` | the Han characters, slots 0–7024 |
| `graphics/fonts/chinese_small.png` | the same, small size |
| `graphics/fonts/latin_normal.png` | the nine full-width marks, slots 0x30–0x3F |
| `graphics/fonts/latin_small.png` | the same, small size |

That project ships its Chinese art as ready-made bitmaps -- no font file, no
generator -- so the glyphs cannot be reproduced from a typeface. Copying them
keeps this port's text in the same hand as the translation it carries.

The sheets carry the same provenance and the same condition as that
translation: a copy with attribution, removed if its authors object. See
`NOTICE.md` at the repository root. Nothing here comes from a cartridge.

## Regenerate

```sh
git clone https://github.com/rh-hideout-chinese/pokeemerald-expansion /tmp/cn
python 3ds_port/scripts/gen_chinese_font.py --fork /tmp/cn
```

The script does more than copy, and the reasons are in its docstring:

- the source's slot 7025 is stray art with no charmap entry, so this port's
  nine marks start at **7026**;
- the marks are not in the Chinese sheets — the fork draws them in the *Latin*
  sheets, over the kana/half-width bytes it repointed for Chinese (0x30 ・,
  0x37 。, 0x39 ～, 0x3A 、, 0x3B ，, 0x3C ！, 0x3D ？, 0x3E ：, 0x3F —);
- those Latin sheets index `0/3` as background where the Chinese ones use `0`,
  so the two-bit values are remapped (a straight copy is noise).

## Layout

One sheet is a `.latfont`: 256 px wide, 2 bpp, four colours (`#ffffff`
background, `#272727` ink, `#808080` shadow), 16x16 a glyph, 16 glyphs a row —
the format `tools/gbagfx` reads for the game's own Latin fonts, so the port's
font loader (`3ds_compat.c`) carries these unchanged.

Slot `n` of a Han character is

```
n = ((hi - 1 - (hi > 0x06) - (hi > 0x1B)) << 8) | lo
```

which is what `SheetIndex()` in `src/chinese_text.c` computes: the code space
skips `0x06` and `0x1B`, the bytes `POKéMON` needs. The nine marks follow the
Han characters from slot 7026, in the order `sPunctCodes[]` lists them.

Mark widths match the source's `GetChineseFontWidthFunc`: in the small font `。`
is 6 px, `—`/`～` 7, `、，！？：・` 5; in the normal font every mark is 12 except
`・` at 7. They are drawn in the left of the cell and the width cuts the empty
right off, which is why they look left-aligned.
