# Provenance

Where every part of Emerald 3DS comes from, and what ships where.

| Component | Origin | Licence / status | In this repository | In a release |
|---|---|---|---|---|
| 3DS platform, compositor, audio queue, data backends, bottom-screen UI (`3ds_port/src`, `include`, `compat`) | Emerald 3DS | `LICENSE-PORT.md` | yes | compiled into the 3DSX |
| Voxel renderer (`3ds_port/src/voxel`) | Emerald 3DS, logic derived from pokeemerald-multiplatform (commit db1cab3d) | MIT (see `3ds_port/src/voxel/NOTICE.md`) | yes | compiled into the 3DSX |
| Voxel generators (`3ds_port/scripts/gen_voxel_*.py`, `voxel_*.py`) | Emerald 3DS | `LICENSE-PORT.md` | yes | run by the builder |
| Voxel tree art (`3ds_port/assets/voxel/trees/*.png`) | drawn by the Emerald 3DS author | `LICENSE-PORT.md` | yes | engine file in the 3DSX |
| Linker script (`3ds_port/emerald3ds.ld.in`) | devkitARM `3dsx.ld`, modified | MPL 2.0 | yes | used to link |
| pret/pokeemerald decompilation | pret | no licence stated; not relicensed | pinned by `upstream.lock`; changes in `patches/` | compiled game logic in the 3DSX |
| Pokémon Emerald data (graphics, maps, text, audio, tables) | the player's ROM | © Nintendo / Game Freak / Creatures | **never** | **never** — generated locally as `emerald3ds.pak` |
| Recipe (`emerald3ds.recipe`) | generated from a build | offsets, sizes, CRCs, engine pointers | no | yes |
| Precompiled voxel programs (in the recipe, op `V`) | the voxel generators' outputs, precompiled at release time | the port's geometry, heights, placements, shapes and masks; the game's texels and pixels only as references (tile, row, column, palette) into the player's ROM - see `docs/VOXEL_PRECOMPILE.md` | no | yes |
| Builder (`builder/`) | Emerald 3DS | `LICENSE-PORT.md` | yes | standalone executable |
| libctru, citro2d, citro3d | devkitPro | zlib | no | linked |
| Python, Tk, Pillow, PyInstaller runtime | their projects | PSF / BSD / HPND / GPL+exception | no | inside the builder |

## Rules the tooling enforces

- `tools/release_audit.py --strict` fails on ROMs, saves, packs, logs,
  bytecode, agent state, validation captures, generated RomFS content, local
  paths, secrets, untracked large files, media without an entry in
  `tools/release_audit_allow.toml`, and (with `--rom`) any run of the ROM's
  bytes in any file or release archive.
- `tools/voxel_precompile.py` fails when a texel of a voxel file would travel
  literally instead of as a reference into the ROM (`--max-own-texels`, 0),
  and `tools/release_audit.py` re-checks every precompiled program of a
  recipe it scans.
- `tools/gen_recipe.py` fails when more than a small, classified amount of
  the original game's own objects would have to travel as literal bytes.
  The Portuguese variant's recipe also carries the translated texts as
  literals (they are in no ROM): only the bytes inside the texts of
  `tools/locales/pt_br/`, encoded as the game holds them, are exempt
  (`--translation`, counted apart as *translation*); that text is this
  project's own and already published in the repository. Every other byte
  stays under the same limits.
- Samples: the release uses the cartridge's original samples, built by
  pret's `wav2agb` pipeline. Replacement samples are only possible through
  `SAMPLE_OVERLAY_DIR` in a local build.
