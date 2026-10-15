# Asset pipeline

The executable never contains game content. Everything derived from the game
reaches the console as **game data**, served by one of three backends with the
same relative paths.

## What leaves the executable, and how

| Kind | In the executable | Game data file | Mechanism |
|---|---|---|---|
| INCBIN graphics, tilesets, fonts | a 4-byte stub per INCBIN | `graphics/**`, `data/tilesets/**`, `generated/assets/**` | `asset_index.bin` maps each stub address to a file; `3ds_assets.c` resolves pointers (exact, `base + offset`, interior) and caches payloads |
| Map and border blockdata | a 12-byte descriptor | `maps/layouts.bin` | `tools/port_common/gen_map_data.py`; `3ds_map_loader.c` keeps the file resident |
| Event, battle, field-effect and AI scripts | a reserved region with every script symbol at its offset | `scripts/scripts.bin` | bundle + relocations (`scripts/ctr_bundle.py`, `3ds_script_loader.c`) — their pointers are unaligned, which 3dsxtool refuses |
| MP2K songs | reserved region | `sound/songs.bin` | same bundle mechanism |
| All other read-only game data (tables, text, samples, map events) | `.gamedata`, reserved NOLOAD | `gamedata/gamedata.bin` | linker script `emerald3ds.ld.in`; two links with the same layout, contents from the image link (`scripts/gen_gamedata_bundle.py`) |
| Voxel data | nothing | `voxel/{regions,signposts,buildings,relief}.bin` | generated on the host from the maps' own art |
| Stereoscopic battle's depth planes | nothing | `stereo/mons.bin` (engine file) | `scripts/gen_stereo_mons.py` from the Sprite Stereo Lab's depth maps (`STEREO_WORK` in `local.mk`), a finished collection collapsed to four planes (`STEREO_PACK`), or the release's four-plane packs copied as they are (`STEREO_MONS_PACK`, `STEREO_BACKS_PACK`); a depth plane number for every pixel of every 64x64 frame and nothing else: no colours and no outline (no transparent pixel); the picture and which of its pixels are drawn are the game's own |

The relocation tables (`*.rel`), the asset index and the ABI are **engine
files**: they describe the executable, not the game, and ship inside the
3DSX's RomFS together with the voxel shader and the port's own tree textures.
`tools/port_common/staging.py list` shows the classification of every file.

### Four-plane sprite exports

The build exports at most four planes using `--smart --cleanup`. For finished front
packs, `STEREO_ANNOTATIONS` names a JSON object mapping sprite IDs to anatomy
JSON paths (relative paths resolve beside that mapping file). Back/trainer
exports read each asset's `anatomy.json` directly. The raw front exporter
validates the original masks before smart grouping. Annotated builds always
regenerate so editing or removing a mask cannot leave stale packed data.

`stereo_depth_groups.py` treats each annotated body part as indivisible across
all poses. Eyes, outlines and markings covered by that part cannot become
separate planes. It clusters normalized authored depths with an optimal
ordered least-squares partition into at most four groups. Each anatomical
part has one vote, keeping small limbs relevant beside a large torso; equal
depths stay together. An optional nonempty `collapse_group` string on parts
explicitly unites pieces of the same surface before clustering. Names are
not guessed: near and far wings must not merge merely because both are wings.
The JSON report records original depths, assigned groups, output planes,
cluster centres and any ordering overrides introduced by explicit surface
merges. Close surfaces may become coplanar; strict separation of every old
front/back relationship is not guaranteed with four groups.

Without masks, the tool clusters the occupied depth levels, weighted by pixel
count. With `--cleanup`, it then merges depth components of at most eight
pixels into larger edge-adjacent surfaces, moving at most one group in depth.
Components use eight-connected pixels to preserve diagonal features, and all
merge decisions use the original component map. Detached details and large
regions are left alone. The report records every changed coordinate and labels
these entries `automatic-spatial-draft`, with review required. Without cleanup,
the fallback remains `depth-only-needs-part-review`. Neither mode infers anatomy;
large shading bands, wrong part boundaries and pose correspondence can still
need editor corrections. Cleanup never changes annotated parts. A mask
must cover the complete opaque silhouette of every frame without overlaps or
background pixels, and every declared part must appear. Invalid masks fail
export. Original art, masks and ten-plane packs remain untouched. The simple
uniform conversion remains available by omitting `--smart`.

Local review assets are in `output/stereo-smart-four/`: `mons.bin`, `backs.bin`,
`front-report.json`, `back-report.json`, `validation.json`, and
`starter-comparison.png`. The front mapping uses 337 pre-existing anatomical
masks plus new Bulbasaur, Ivysaur and Squirtle masks for both animation poses.
Their source hashes and boundary-fragment checks pass; these are visual review
drafts, not hardware approval. Another 45 front sprites still need masks.
All 487 back/trainer entries have existing masks. Clustering reuses those
annotations; it does not independently establish their anatomical accuracy.

The complete automatic pass is in `output/stereo-all-four/`: `mons.bin` and
`backs.bin` cover all 872 currently supported entries (385 fronts and 487
back/trainer entries). Of these, 827 use anatomical masks and 45 are automatic
front drafts. `review-sheet.png` shows both poses of the 45 automatic drafts;
`review-order.txt` and `review-queue.json` sort them by changed pixel count.
This is a review priority heuristic, not an anatomy quality score. The pass
merged 1,118 small components (2,816 pixels) and preserved all frame layouts,
transparency and annotated outputs. Existing Castform front-form and alternate
back-form runtime exclusions still apply. Original source packs are not overwritten. The build regenerates the cleaned packs;
a new game build is still required to embed them in the executable.

The battle renderer keeps the original 14.4-unit overall depth span
independently of plane count, and cap battle groups at each sprite's actual
plane count. Fewer occupied groups spread over the available range; a single
surface uses the plane nearest the 0.62 pivot. Cluster centres are audit data;
the current file format still renders uniformly spaced planes. Independent
per-plane spacing is not implemented. Existing depth controls scale spacing
uniformly.

### No outline in the packs

The last step of every export (`collapse_stereo_planes.py`, `gen_stereo_mons.py`,
`gen_stereo_backs.py`) fills each transparent pixel (plane 15) with the plane
of its nearest opaque pixel (chessboard distance, a flood from all opaque
pixels at once), so a finished pack is a full 64x64 depth field per frame and
carries no sprite outline. The grouping before it still reads the sources'
transparent pixels; the fill comes after it. A finished four-plane pack given
to `collapse_stereo_planes.py` keeps its planes and is only filled. At run
time the composition masks every plane with the game's own picture (a pixel
is drawn where the ROM picture is not 0), and the battle tells whether a
position's picture buffer holds the Pokémon or the trainer by comparing it
with the game's own pictures of both, not with the planes.
`make verify` checks that the built `mons.bin` and `backs.bin` hold no 15.

Run `python3 3ds_port/tests/stereo_collapse_test.py` and
`python3 3ds_port/tests/stereo_depth_groups_test.py` for conversion checks.
The generated files are asset packs, not a rebuilt installable game.

### Anatomical sprite depth corrections

`gen_stereo_mons.py` accepts an optional `STEREO_WORK/<national>/anatomy.json`
beside the existing depth map. It contains reviewed part masks for both 64×64
front poses. Shared part IDs carry depth between poses independently of colour
and position: eyes and markings belong to the head/body, claws to their feet,
and a moving blade retains its own depth. This is annotated correspondence,
not automatic segmentation. It never changes source sprite pixels.

The version-1 schema uses `size: [64,64]`, `parts` with unique positive `id`
and `name`, and two `frames`, each with 64 `rows` of
`[start_x, end_x_exclusive, part_id]` runs. Each opaque pixel must be covered
exactly once; background coverage, gaps and overlaps are errors. A part's
optional `depth` is normalized from 0 (far) to 1 (near). Without it, the part
inherits the median key-pose depth. One coherent plane per part prevents
colour patches from becoming separate depth surfaces. An optional
`source_sha256` binds annotations to the original `anim_front.png` bytes.

Invalid annotations abort export. Pokémon without annotations keep legacy
colour-based transfer. Review both exported poses and depth ordering before
accepting a correction; valid masks alone do not prove correct anatomy.
Annotated exports also reject unreviewed transparent-boundary fragments:
detached components of at most eight pixels, smaller than the same part's
largest component, touching transparency or the canvas edge and adjacent to a
different exported depth. Eight-connected labeling preserves diagonal links;
whole small parts remain valid. The check never dilates, smooths, or reassigns
pixels automatically. Exact `boundary_exceptions` entries with `frame`,
`part_id`, `pixels: [[x,y], ...]`, and a review `reason` preserve legitimate
fragments such as flame tips. Duplicate or stale exception geometry fails;
exceptions remain valid when export quantization merges adjacent planes.
This catches small detached edge artifacts, not every connected or large
misassignment; visual review remains necessary. Run
`python3 output/stereo-rom-pilot/check_boundaries.py` to audit the four local
pilots and replay the reported Machamp shoulder regression.
Run `python3 3ds_port/tests/stereo_anatomy_test.py` for transfer regressions.
The isolated Ponyta/Scyther pilot can be regenerated without APIs using
`python3 output/stereo-pilot/process_pilot.py --control`; it needs the local
Sprite Stereo Lab assets and `build/upstream`. The pilot's baseline directory
retains the earlier export for comparison. Force regeneration after deleting
an annotation or changing source artwork; the build tracks existing sidecar
edits but does not detect removed prerequisites.

For an exact ROM-grid source, `python3 3ds_port/scripts/extract_stereo_fronts.py
--rom PATH --ids 12 68 --out OUTPUT` reads a verified English Emerald ROM
locally. It locates unique compiled upstream compressed sprite/palette blobs
inside the ROM, independently decodes GBA LZ77 and 4bpp tiles, and verifies
every palette index, 15-bit palette entry and tile byte against the upstream
reference. It requires those compiled assets in `build/upstream`; it is not
a standalone ROM-table parser. Unsupported pose dimensions fail explicitly.
The manifest records offsets, hashes and mismatch counts. Extracted indexed
PNGs retain the original 64×64 coordinates and transparency; the generated
two-pose GIFs are checked pixel-for-pixel as well. PNG RGB is an exact
representation of the ROM's 5-bit channels shifted left three bits, which can
differ from an upstream PNG's 8-bit display values without changing GBA colour.

The resulting `root` and `sprites` directories can be passed directly to the
stereo exporter, avoiding downloaded sprite canvas alignment. This preserves
geometry but does not infer anatomy; reviewed per-pose masks are still needed.
Run `python3 3ds_port/tests/stereo_rom_extract_test.py` for decoder tests.

Exact duplicate compressed palette streams are accepted because their decoded
bytes are identical; all matching ROM offsets are recorded. Sprite locations
remain unique in the standard extractor. Unown uses the canonical A folder and
shared root palette. Empty second slots (Unown and Spinda) remain empty; review
animations omit these unused slots instead of inventing another pose.

The local `output/stereo-remaining/` review batch covers 323 standard sheets,
plus Castform's four separately extracted single-frame weather forms. Castform
is not packed as two animation poses; runtime form support remains pending.
`process_remaining.py` validates source-bound masks at 2/8/10/15 planes, exports
`mons.bin`, and creates original/depth and stereo previews. `make_report.py`
builds the searchable gallery. `work/audit_remaining.py` independently checks
ROM pixels, palettes, both GIF poses, and packed data against current sidecars.
These are conservative anatomy drafts, often only two surfaces. Eight coherent
coplanar drafts are explicitly labeled. Integrity checks do not establish
anatomical precision or hardware stereo comfort. Spinda previews show base ROM
art; personality-dependent spots remain the game's responsibility.


## Backends

`CtrData_Init` picks one at start-up (`DATA_BACKEND=0`, the default):

1. `sdmc:/3ds/emerald3ds/devdata/.emerald3ds-dev` exists → **loose** files in
   `devdata/`;
2. the 3DSX embeds its data (`romfs:/data.embedded`, development builds with
   `EMBED_DATA=1`) → **romfs**;
3. otherwise → **pak**: `sdmc:/3ds/emerald3ds/emerald3ds.pak`.

A file inside the pack opens as an ordinary `FILE*` (`fopencookie`), so every
reader only changed the call that opens it. Pack reads share one SD handle
under a lock and are safe from the voxel worker thread.

## Engine ABI

`romfs:/engine/abi.bin` holds a CRC-32 over the path, size and CRC of every
game-data file the executable was built with (`staging.py abi`). The pack's
header carries the same value; any difference — another release, another
build, another ROM — is refused with a readable screen, never a crash.

## emerald3ds.pak (schema 1)

Little endian. Header (64 bytes): magic `EM3DPAK\0`, schema, engine ABI,
ROM SHA-1, entry count, index offset, data offset, index CRC-32, header CRC-32.
Index: 40-byte entries sorted by id (FNV-1a 64 of the path) — id, type,
flags, offset, stored size, raw size, CRC-32. Payloads are 32-byte aligned and
uncompressed. Writer: `builder/emerald3ds_builder/pak.py`; reader:
`3ds_port/src/3ds_pak.c` (host test: `make verify-pak`).

## Recipes: rebuilding the game data from a ROM

A release ships `emerald3ds.recipe` instead of any data. For every game-data
file it lists size, CRC-32 and operations applied to the player's ROM:

- `C` copy a ROM range, `F` repeat a byte, `Z` LZ77-decompress a ROM stream;
- `R` copy records whose layout changed between the original compiler and
  GCC (field order, padding, bitfield packing), as a per-bit map learnt by
  matching bit columns across every record of the table;
- `L` literal bytes, only for what the ROM does not contain: pointers into
  the executable, compiler switch tables, alignment padding and the port's
  own additions. `tools/gen_recipe.py` classifies and caps them, and
  `tools/release_audit.py --rom` checks that the literal pool holds no run of
  the ROM's bytes.

The recipe verifies itself when it is written, and the builder verifies every
file it rebuilds against its CRC, so the pack is byte-identical to the data
the release was built with.

The voxel files are not copied: the builder rebuilds the decomp-shaped inputs
the voxel generators read (tileset art, palettes, metatiles, blockdata, and
`layouts.json`/`map.json` read back from the ROM's own structures,
`builder/emerald3ds_builder/vtree.py`) and runs the same generators the build
runs (`voxel.py`).

Where the relief generator reads a height wrong, the correction is written as
numbers in `3ds_port/assets/voxel/relief_fixes.json` rather than as a special
case in the generator: per map, cell lattices, levels, cliff walls and the
map's base, and a catalogue of pieces (one shape per tileset metatile, enabled
map by map), plus height overrides for modelled interior furniture. It holds
no cartridge data, ships with the generators, and `scripts/voxel_relief_fixes.py`
applies it after `gen_voxel_relief.py` and inside `gen_voxel_buildings.py`, so
the builder produces the same files the build does.

## Development loop

| Change | Command | Copy to the console |
|---|---|---|
| C, rendering, input | `make` | `emerald3ds.3dsx` (embeds its data) |
| One generator or asset | `make devdata`, then `tools/dev_assets.py` | changed files into `devdata/` |
| Release path | `make release pak` | `dist/Emerald3DS.3dsx` + `build/emerald3ds.pak` |
