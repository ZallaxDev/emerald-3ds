# Notices

Emerald 3DS combines original port code with the pret decompilation and a few
third-party components. Each keeps its own terms; `LICENSE-PORT.md`
(CC BY-NC-SA 4.0) covers only the original Emerald 3DS work.

## Upstream decompilation

- **pret/pokeemerald** — https://github.com/pret/pokeemerald, pinned in
  `upstream.lock`. Fetched by `tools/bootstrap.py`, never vendored here. The
  repository does not state a licence; Emerald 3DS does not relicense it. The
  files in `patches/pokeemerald/` carry the port's changes and, as patch
  context, lines of the upstream files they modify.

## Code derived from third parties

- **pokeemerald-multiplatform** (gradenGnostic/pokeemerald-multiplatform,
  commit db1cab3d2dc9f0e0a9f4a3d67983e5acf30a9a46), MIT. The voxel overworld
  in `3ds_port/src/voxel/` reuses the logic of its SDL2/OpenGL voxel renderer
  (map instances, metatile classification, atlas composition, follow camera).
  Full text and scope: `3ds_port/src/voxel/NOTICE.md`.
- **devkitARM `3dsx.ld`**, Mozilla Public License 2.0.
  `3ds_port/emerald3ds.ld.in` is a modified copy and stays under the MPL 2.0
  (https://mozilla.org/MPL/2.0/).

## Translations

The port's own interface strings (`3ds_port/include/3ds_locale.h`) are original
to Emerald 3DS and under `LICENSE-PORT.md`. The two fan translations are not:
they are the work of their translators.

- **Brazilian Portuguese** (`tools/locales/pt_br/`) — by Carlos Mozart
  (AllGenWiki), written for this port rather than copied from a cartridge; the
  repository holds it as this project's own text. See `docs/PORTUGUESE.md` and
  `docs/ROADMAP-PTBR.md`.
- **Simplified Chinese** (`tools/locales/zh_cn/`) — from
  [rh-hideout-chinese/pokeemerald-expansion](https://github.com/rh-hideout-chinese/pokeemerald-expansion),
  a fan translation of the Expansion project's text. That repository states no
  licence, and this port does not relicense it: the catalog here is a copy of
  its texts, adapted to this tree's labels and glyph encoding, reproduced with
  attribution to its source. **If its authors object to the copy, it will be
  removed from the repository** — the port would then either ask the player for
  the catalog or drop the Chinese build, whichever its maintainer prefers.
  Until that happens, treat `tools/locales/zh_cn/` as theirs, not as Emerald
  3DS's.

Neither translation contains cartridge text beyond what the underlying game's
own wording required: both are derived from the English the labels already hold
in the pret tree.

### Glyph sheets

`3ds_port/assets/fonts/chinese_small.png` and `chinese_normal.png` are assembled
by `3ds_port/scripts/gen_chinese_font.py` from the sheets of the same Chinese
project: every Han character is copied from its `chinese_normal.png` and
`chinese_small.png`, and the nine full-width marks from `latin_normal.png` and
`latin_small.png`, where that project draws them over bytes it repointed for
Chinese. That project ships no font file and no generator, so its glyphs cannot
be reproduced from a face.

These sheets therefore carry the **same provenance as the translation above**
and the same condition: a copy with attribution, removed if its authors object.
Neither holds cartridge glyphs -- the underlying Chinese shapes are the
translator's rendering, not a Nintendo font.

## Libraries used by the build

- **libctru, citro2d, citro3d** (devkitPro), zlib licence — linked into the 3DSX.
- **devkitARM / GCC / newlib** — toolchain and C library.

## Bundled with the Windows builder

- **Python** (PSF License Agreement) and its standard library, **Tcl/Tk**
  (BSD-style) and **Pillow** (MIT-CMU / HPND), packaged by **PyInstaller**
  (GPL 2.0 with the bootloader exception).

## Game content

Pokémon Emerald's code, data, graphics, audio and text are © Nintendo, Game
Freak and Creatures. None of it is included in this repository or in the
releases; the builder produces the data pack on the player's computer from
their own ROM.

**HeartGold and SoulSilver's follower sprites and messages**, fetched by the
builder on the player's computer, never included in this repository or the
releases: the overworld sprites from pret/pokeheartgold
(https://github.com/pret/pokeheartgold, commit
`3291d88a7e3e264b503024ead80f4d8b55b835cd`), the messages in the ROM's
language from Poké Corpus by abcboy101 (https://github.com/abcboy101/poke-corpus,
commit `5d1da078d70dea5c9feaaba7575626eab4f10a08`). The builder downloads both
from forks that keep those commits (https://github.com/ZallaxDev/pokeheartgold,
https://github.com/ZallaxDev/poke-corpus), each file checked against
the SHA-256 listed in `builder/emerald3ds_builder/followers_sources.py` and
converted for the console. They are Nintendo, Game Freak and Creatures' work;
not relicensed.
