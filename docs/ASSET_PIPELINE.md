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

The relocation tables (`*.rel`), the asset index and the ABI are **engine
files**: they describe the executable, not the game, and ship inside the
3DSX's RomFS together with the voxel shader and the port's own tree textures.
`tools/port_common/staging.py list` shows the classification of every file.

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
runs (`voxel.py`) - or, for every file the release precompiled, runs its
program instead: `tools/voxel_precompile.py` runs those generators once per
supported ROM when the release is made and stores in the recipe (op `V`) the
port's own bytes of each file and, for every texel and pixel that comes from
the game's art, the tile, row, column and palette of the player's ROM it is
read from ([VOXEL_PRECOMPILE.md](VOXEL_PRECOMPILE.md)).

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
