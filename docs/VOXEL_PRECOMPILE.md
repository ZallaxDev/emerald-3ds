# Precompiled voxel data

The builder used to run the voxel generators (`3ds_port/scripts/gen_voxel_*.py`,
`gen_intro_margins.py`) on every player's machine: 10 to 15 minutes of
analysis whose result depends only on the ROM, and the release supports a
fixed set of ROMs. A release now runs them once per supported ROM and the
recipe carries, for each of their files, a program that writes it again from
the player's ROM in seconds. The data pack is byte for byte the one the
generators make.

```
release (maintainer, once per ROM)                    builder (every player)
──────────────────────────────────                    ──────────────────────
make release → romfs/voxel/*.bin, stage/leaves.bin
gen_recipe.py → recipe (CRCs of those files)
voxel_precompile.py:
  tree from the ROM (vtree.py) + the payload's scripts
  generators, as the builder runs them  ── must give the
  recipe's CRCs, else the release stops
  compile each file:                                   recipe "V" program per file:
    own bytes → LIT/ZERO/COPY                            LIT/ZERO/COPY as they are
    texels → (tile, row, column, palette) refs  ───────▶ tiles.4bpp + palettes from
    pixels → (tile, pixel) refs                          the player's ROM (recipe inputs)
  run the programs, compare byte for byte                CRC check (as before), ABI, pack
```

## What could be separated, and what could not

The proposal was to split the generators into stages - shape and silhouette
analysis, heights, meshes, cuts/masks/placements, texture distribution, and
the rebuilding of the ROM's graphics - and to precompute all but the last.
Looking at the code:

* **Every stage depends on the graphics.** The buildings' shapes are read off
  the colours (`building_art`'s ground test, `room_facts`, the outline and
  support heuristics), the compaction of a model's texture compares texels
  (`compact_rows_moved`, `same_reads`), the atlas packing depends on the crops
  the faces read; the relief's levels come from the rock colours
  (`ROCK_TOP`, `drawn_role`), its cut masks from the pixels of the ground.
  So the geometry is not "own geometry plus textures": it is a function of
  the ROM. It is therefore computed **per supported ROM**, at release time,
  which is exactly what the recipe already is (one per ROM, chosen by its
  SHA-1).
* **Splitting the generators' internals is not needed.** Their final output
  is the right boundary: every byte of it is either the port's own data
  (geometry, heights, placements, tables, masks) or a colour of the game's
  art in a known format. The compiler treats each generator as a black box
  and compiles its output; the generators keep their code, and the CRC
  equality proves the split.
* **The last stage - rebuilding the graphics - is the only one left to the
  builder**, and it is a copy: every texel of a building page is a palette
  colour of a tile pixel of the player's ROM.

Per file:

| File | Generator time (native, this machine) | Holds colours of the game? | Precompiled as |
|---|---|---|---|
| `voxel/regions.bin` | ~6 s | no: a role per map cell | own bytes |
| `voxel/signposts.bin` | ~4 s | no: 16x16 bit silhouettes, metatile ids | own bytes |
| `voxel/buildings.bin` | ~270-280 s | yes: RGBA5551 pages (4.8 M opaque texels), animation palettes and backgrounds, and the window glass (a level per texel computed from the texel's colour) | own bytes + tile-row references + palette references + copies + glass derived from the written texels |
| `voxel/relief.bin` | ~370 s | no: heights, 16x16 bit cut masks, metatile ids | own bytes |
| `stage/leaves.bin` | <1 s | yes: palette indices of pixels carried on from the intro picture | own bytes (the new art) + pixel references |

`leaves.bin` is cheap to generate, but leaving it to the builder would keep
the whole generator tree (2 204 files rebuilt from the ROM, 72 of them LZ77)
for one file, so it is precompiled too.

## Where the time went

Profiled with `cProfile` (native CPython 3.13; profiler overhead included):

* **Buildings** (`gen_voxel_buildings.py`, 694 s under the profiler):
  `build_models` 559 s, of which `interior_specs` 464 s (the rooms: the
  `<genexpr>` at line 1951 alone - 85 M calls - and the `lathe_fit` /
  `cost` fitting of round furniture, 93 s), `auto_leave`, `reuse_pieces`;
  `export` 135 s (`compact_rows` 44 s, `sampled_texels` 29 s, the skyline
  packer 17 s).
* **Relief** (`gen_voxel_relief.py`, 822 s under the profiler): `export`
  424 s - the drawn maps' levels (`world_levels` 181 s, `drawn_prepare`
  162 s, `drawn_canvas` 39 s), their pieces (`cell_shapes` 138 s,
  `plain_ground` 57 s, `cut_cells` 48 s) - and the terraces counted tile by
  tile over it, `voxel_terraces.apply_file` 389 s (`solve_auto` 291 s:
  `blocks` 191 s with 4 M calls of `cost`, `merge`/`records` 90 s); the
  64-bit set order kept for Pyodide (`set_order`) 43 s.

None of it runs on the player's machine any more.

## The program

`builder/emerald3ds_builder/precompiled.py` documents the format. A program
is a byte string in the recipe's literal pool, named by a `["V", offset,
length]` operation of a `generated` entry. Its operations:

| Op | Writes | Data in the program |
|---|---|---|
| `LIT`, `ZERO`, `COPY` | the port's own bytes, zeros, a repeat of bytes already written | own bytes |
| `PATHS`, `SRCS` | nothing: the input files (recipe `inputs` paths) and tile sources (tiles file, tile number, palette file) | names and numbers |
| `TEX` | a block of RGBA5551 texels, raster or PICA200 Morton order, as runs: `SKIP` (clear), `TILE` (1-8 texels of a tile row: source, row, column, direction, alpha), `PAL` (a palette colour, repeated), `LIT` (texels of the port's own) | references; literal texels only for colours no palette of the ROM has |
| `COLOR` | one RGBA5551 palette colour | palette file and index |
| `GLOW` | a page's window glass, one byte per texel of the block before it: clear, derived from the texel (`128 + (r+g+b)//6`, `voxel_night.window_mask`), or a fixed level of the port's (180) | the runs only |
| `PIXEL` | one palette-index byte | tiles file, tile, pixel, bank |

The builder (`build.build_generated`) rebuilds only the inputs the programs
name (tile sheets and palettes, through the recipe's existing `C`/`Z`
operations), runs each program, and checks the result against the CRC as
before. Nothing else of the build changes: same progress messages ("Generating
the 3D scenery: buildings (3/5), done in 1.7 s"), same summary, same ABI and
pack checks, same web entry point (`web.run_web_build`).

**Extending the recipe vs. a new format.** The recipe already is the
per-ROM, per-language unit the builders select by SHA-1, already carries
the voxel inputs, already self-checks every file by CRC, and its literal pool
is already what `release_audit.py --rom` scans. A new op `V` and a ~300-line
interpreter keep one file per variant and every existing check; a separate
format would have needed its own selection, hashing in the web manifest and
audit. The program itself is a small bytecode rather than more JSON
operations because a building page needs ~700 000 runs.

## Provenance of the texels

`buildings.bin`'s pages are made by the generator from PIL images composed
of tile pixels and then cropped, compacted, folded and packed; following each
pixel through those operations would mean instrumenting most of the
generator. Instead the compiler **recovers** each texel's source and proves
it: it indexes every tile row the tilesets' metatiles actually draw (16 209
(tiles, tile, palette) sources of 76 tileset pairs plus the animation frames,
86 250 distinct rows, both directions) and covers each page row by runs,
preferring

1. the source the texel to the left or above came from (the same tile row
   continued, the next row of the same tile),
2. a source of the tilesets of the page's own map,
3. any source, longest run first;

then a palette colour, then a literal texel. Every choice reproduces the
texel exactly (the program is run and compared byte for byte before the
recipe is written), so the reference is a valid provenance of the value; for
runs found by (1) and (2) it is the place the generator itself read. The
report gives the share: for this ROM 4 795 114 texels from tile rows
(`tile in map` counts those of the page's own tilesets), 3 098 palette
colours, **0 literal texels**. `--max-own-texels` (default 0) fails the
release otherwise (an animation colour no palette has is counted too), a
generated file the compiler does not know (neither in `COMPILERS` nor in
`PLAIN`, the formats checked to hold no colour) stops it, and
`release_audit.py` re-checks every program of every recipe it scans, with or
without `--rom`: a literal texel, glass written literally at another level
than the port's own, or a damaged program is a finding.

`leaves.bin` is instrumented instead (it is small): with
`gen_intro_margins.PROVENANCE`, every pixel read off the intro picture is a
`Pixel` tuple carrying its tile and pixel, and the compiler writes those as
`PIXEL` references; the new art the script draws stays literal (it is the
port's own). The output is unchanged by the instrumentation.

## What is the port's own, and what is derived from the game

The programs carry no picture of the game: no tile data, no palette, no
texel or pixel colour. But "no texture" is not "nothing derived from the
game", and the release now distributes data that was previously only ever
made on the player's machine. For the record:

* **Own, independent of the art**: vertex structure of the hand-written specs
  (`voxel_building_specs.py`), the hand corrections (`relief_fixes.json`,
  already shipped as source), the format tables, the intro margins' new art.
* **Derived from the game's data by the port's analysis** (now shipped as
  bytes in the recipe):
  * per-cell roles of every map (`regions.bin`) and per-cell heights
    (`relief.bin`): computed from the maps' layouts and art - they mirror
    the shape of the maps;
  * silhouettes: signpost masks (16x16 bits), relief cut masks (16x16 bits per
    variant), model footprints, building and furniture geometry fitted to
    the drawings' outlines, texture coordinates, and where the window glass
    is (its level is not shipped: `voxel_night` computes it from the
    window's colour, so the program derives it from the texel the player's
    ROM gave - the review of this change found it first shipped literally,
    a greyscale picture of the windows);
  * references: which tile row of which tileset goes where - comparable to a
    tilemap, and of the same nature as the recipe's existing `C`/`Z`
    operations (offsets into the ROM).
* Open questions for whoever decides on distribution: whether silhouettes
  and per-map tables derived from the art and the maps are acceptable to
  ship in a release (they were acceptable as the generators' output on the
  player's machine); whether a per-tile arrangement counts differently from
  the recipe's offsets. If some file should not ship, the release can leave
  it to the builder: `build_release.py --voxel-generate voxel/signposts.bin`
  (repeatable) keeps that file without a program and the builders run its
  generator exactly as before (the others still come from their programs,
  `voxel.run_generators(ready=...)`). Measured with `regions.bin` and
  `signposts.bin` left to the builder: 9.2 s instead of 2.4 s (the tree
  rebuilt from the ROM, 1.5 s, and the two generators side by side, 5.4 s),
  pack identical.

## Releasing

One fixed Python makes the data (`upstream.lock` `[python]`, 3.13, the web
builder's): `build_release.py` and `voxel_precompile.py` refuse another, and
`make` runs the generators with it. It matters: under 3.11 `relief.bin`
comes out different - since 3.12 `sum()` adds floats with Neumaier's
compensation, which moves the last bit of the world solve
(`gen_voxel_relief._gauss_seidel`), and that bit decides an exact tie in
`_give_up_seams` (which of Fortree's two seams, Route 119's or Route 120's,
gives way). `tools/docker_release.py` makes the whole release in the pinned
environment (docs/RELEASING.md, *Release environment*).

Nothing to do by hand: `tools/build_release.py` runs

```
python tools/voxel_precompile.py --recipe dist/emerald3ds.recipe --rom baserom.gba \
    --port 3ds_port --report dist/emerald3ds.precompile.json
```

after `gen_recipe.py`, and the same for every localized variant with its own
ROM and tree (`build/release-<lang>/3ds_port`). It always runs, also on a
reused recipe (`--skip-recipe`: its previous programs are dropped and made
again). It:

1. rebuilds the generators' tree from the ROM exactly as the builder does
   (`vtree.build_tree`) and stages the payload's scripts
   (`build_release.local_closure`);
2. runs the generators (`voxel.run_generators`, in parallel processes);
3. stops unless every output has the recipe's size and CRC - the files the
   executable was built against. A change to a model, a height, a hand
   correction or any generator that was not rebuilt into RomFS, or a RomFS
   file left from older code, fails here: no stale program can be written;
4. compiles each file, runs every program and compares it with the
   generator's output byte for byte; only then writes the recipe.

Its time is the generators' (5-7 minutes per ROM on 4 cores) plus about a
minute and a half to compile. Options: `--outputs DIR` compiles files already
generated (still checked against the CRCs), `--generate PATH` leaves a file to
the builder, `--max-own-texels N`.

To check a release by hand: build with the programs (default), and with the
generators (`build_pack(..., precompiled_ok=False)`), and compare the packs
(docs/RELEASING.md, step 4).

## Measurements

(this machine: 4 cores, CPython 3.13.17; Pyodide 0.28.3 under Node 22, Python
3.13.2 wasm32. English ROM, SHA-1 f3ae0881…)

Generation of the 3D data only (the rest of a build - extraction, pack - is
under a second either way). Native times were taken with other jobs on the
machine; Pyodide times include its ~3.5 s start.

| Step | Generators, native | Generators, Pyodide | Programs, native | Programs, Pyodide |
|---|---|---|---|---|
| tree rebuilt from the ROM | 1.2 s | - | not needed | not needed |
| `regions.bin` | 6.2 s (9.3 s in the builder) | 12.6 s | <0.01 s | <0.1 s |
| `signposts.bin` | 3.7 s (6.9 s) | 9.7 s | <0.01 s | <0.1 s |
| `buildings.bin` | 281.5 s (271.0 s) | 539.6 s (differed before the fix below) | 1.7 s | 4.4 s |
| `relief.bin` | 370.9 s (362.0 s, after the buildings) | 653.1 s | <0.01 s | <0.1 s |
| `leaves.bin` | 0.4 s (0.7 s) | 3.5 s | 0.04 s | 0.14 s |
| **whole build** (`build_pack`, followers off) | **635.2 s** (desktop: parallel processes) | ~1 220 s (web: one after the other, summed) | **2.2-2.4 s** | **4.6-5.7 s** |

Peak memory: the generators reach 1.7 GB (buildings) and 1.45 GB (relief)
per process natively, 1.2 GB / 1.1 GB of Node RSS under Pyodide; the whole
build with programs peaks at 267 MB natively (the builder process, pack
included) and 459 MB of Node RSS under Pyodide (runtime included).

Size: the recipe grows by 1.05 MiB (LZMA; the programs are 18.2 MB raw, of
which 15.7 MB is `buildings.bin`'s: 12.6 MB of own bytes - mostly vertices -
and ~700 000 texel runs). The Windows ZIP and the web payload grow by about
as much; `payload/voxelgen/` (2.5 MB) stays, for files without a program.

Release side: `voxel_precompile.py` took 700.7 s here (generators 610 s,
tile index 43 s, compiling 39 s, proving 1.6 s), 1.7 GB peak. Run twice, it
writes the same recipe byte for byte.

Equivalence, English ROM (`f3ae0881…`): every file of the pack built with the
programs is byte-identical to the generators' (native `build_pack` with
`precompiled_ok=False`: `cmp` of the two packs, and file by file against
standalone generator runs); the pack built under Pyodide through
`web.run_web_build` is byte-identical to the native one; two native
generator runs are identical (buildings, relief). The CRC and ABI checks of
the builder pass unchanged.

## Limits

* Only the English ROM was available here: the Spanish, French, Italian and
  German variants are precompiled by the same tool from their own ROM and
  tree when `build_release.py` makes them, but were not run on this
  machine. A variant without programs (or an older recipe) still builds,
  with the generators.
* Generator output under Pyodide: `buildings.bin` differed from the native
  one (2 068 bytes: `plain_side` broke a colour tie by the iteration order
  of a set, which follows the 32-bit hash of WebAssembly; fixed in this
  change with `gen_voxel_relief.set_order`, and both now give the release's
  bytes). The other four files were already identical. The builders no
  longer depend on it unless a file is left to them; other order-dependent
  spots found (`room_facts` lint, a tie in `voxel_bridge`'s road cell) do not
  reach the output today.
* The texel provenance of `buildings.bin` is recovered by matching, then
  proved; it is the generator's own read for the runs found by continuation,
  and a value-exact source otherwise. Instrumenting the generator's image
  pipeline would give the read itself for every texel.
* `leaves.bin`'s provenance relies on the tagging of `gen_intro_margins.pic`:
  a future edit that rebuilt a picture pixel as a plain tuple would ship it
  as the port's own art without notice. The intro's new art and its carried
  pixels are both palette indices, so the compiler cannot tell them apart.
* The interpreter refuses damaged programs before allocating anything past
  the file's size (`ProgramError`, reported as `generator_output_mismatch`).
* The release takes longer (the generators run once per ROM); the players'
  builds no longer do.
