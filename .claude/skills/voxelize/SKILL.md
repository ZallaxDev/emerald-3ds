---
name: voxelize
description: Method for voxel work in Emerald 3DS - modelling buildings, interiors, props or statues from the map art, fixing mountains/cliffs/terrain relief, reviewing voxel faults from screenshots, and checking voxel changes before a pull request. Use whenever the task touches 3ds_port/scripts/*voxel*, gen_voxel_*.py, voxel_terraces.py, relief_fixes.json or the voxel world as seen in game.
---

# Voxel work in Emerald 3DS

You are helping a contributor voxelize part of Hoenn. The method below is the
project maintainer's; follow it as written. Do not invent shortcuts the
guides reject.

## Read first

Before proposing anything, read the guide of the area and the docstring of
every generator you will change:

- `docs/VOXEL_GUIDE.md` - setup, rules for every model, the working loop,
  pull request checklist.
- `docs/VOXEL_STATUS.md` - the status of every map and room on this branch,
  and its known faults. Read the row of the place first.
- Buildings, interiors, props: `3ds_port/assets/voxel/buildings/README.md`.
- Mountains, cliffs, terrain: `docs/VOXEL_MOUNTAINS.md` and
  `3ds_port/scripts/terrace_debug/README.md`. Its "Tried and rejected"
  section is binding: never reintroduce those approaches (shared height
  lattice, slanted/hip/planar pieces, per-column refinement, fractional
  levels, per-map level forcing), even when a fault looks local.

## Setup facts

- The generators run inside the bootstrapped tree `build/upstream`
  (`python tools/bootstrap.py --make -j8` once). Edit files in the
  repository, then re-run `python tools/bootstrap.py` (no `--clean`) to copy
  them in; edits made only under `build/upstream` are overwritten.
- Run the checks from `build/upstream/3ds_port/scripts`. The full relief
  takes about 6 minutes: run it in the background and keep
  `VOXEL_RELIEF_CACHE=build/relief_cache` set.

## Non-negotiable rules

1. The drawing is the truth: pixel-exact at 45 degrees, one texel per pixel,
   and the true 3D form the drawing depicts, judged from the game's camera
   (yaw 0, pitch 34-46) and from the sides - not only the outline.
2. Simplest geometry that follows the drawing. Terrain stays on the 16 px
   grid: whole tops, vertical faces, 45-degree bands, corner pieces. Drop any
   attempt that adds small pieces to move a metric.
3. Reuse what is modelled; model only what is new (read the generator's
   `reuse:` lines before writing a spec).
4. General rules only: a rule names tiles and shapes, never a map or cell.
   An unsolvable spot gets a hand correction as data in
   `3ds_port/assets/voxel/relief_fixes.json`, never a special case in code.
5. Never change berry soil, seams between maps or a ledge's shape
   (`ledgecheck.py` must report 0 differences outside ledges between two
   levels). A ledge whose two sides the terraces put at different levels is
   a step laid by the general rule (`voxel_terraces.py`, "ledges between two
   levels"); never reshape one by hand.
6. Never commit renders, screenshots, extracted tiles or generated `.bin`
   files; nothing from the cartridge. `python tools/release_audit.py --repo .
   --strict` must pass.

## The loop (do every step, in order)

1. Locate the place: map and cells (`3ds_port/scripts/emu/setpos.py` for the
   emulator's save, `warp_events` in `map.json` for caves and doors). Render
   the GBA drawing of the area.
2. Mark every fault on the screenshot, numbered by type (hole, slit, side
   wall, deformed shape, stretched texture, ghost, tiny pieces) and show it
   to the human. Do not dismiss any as residual.
3. Reproduce with the engine's own code: `voxel_scene.py view/probe/cells`,
   `voxel_view_scan.py --at X,Y --view`, `--box ... --overlay`,
   `voxel_engine_view.py view`; for models also `gen_voxel_buildings.py
   --preview` side cameras and `voxel_defect_scan.py`.
4. Make the detector report the fault (`voxel_view_scan.py` for terrain,
   `voxel_defect_scan.py` for models). If it does not, extend the detector
   before touching geometry.
5. Change one general rule or one spec. Try terrain rules in minutes with
   `terrace_debug/trial.py`.
6. Regressions everywhere the change applies: `terrace_debug/regress.sh` +
   `summary.py` over every terraced map, `terrace_debug/rulediff.py` for
   every cell whose piece changed (render each one both ways),
   `voxel_terraces.py LAYOUT --check` (`open edges: 0`), `ledgecheck.py`;
   for models `gen_voxel_buildings.py --verify`. A change to an already
   finished map is a regression unless it is clearly better: ask the human.
7. Build, look at the exact place in the emulator (`scripts/emu`), and make a
   before/after sheet: marked screenshot | new screenshot | GBA drawing.
   Hardware has the last word; say what was and was not tested on it.
8. Report with numbers (per map, per fault class, before → after) what is
   fixed and what remains.

## How to behave

- Show each result to the human as you go (previews, sheets), not only at
  the end.
- Critique every fix as you make it: which assumptions it makes, which cells
  or models outside the one you looked at it can touch, what only hardware
  can show.
- Metrics improving is not success; the view at the reported place is.
- Never claim a check passed without running it, and quote its numbers.
- Before a pull request: an issue claims the area, the checks of the area
  and `make verify` pass, the rows of `docs/VOXEL_STATUS.md` the work
  changes are updated in the same pull request (then
  `python 3ds_port/scripts/voxel_status.py --write` refreshes its Progress block), and the sheets and numbers go in the pull request,
  not in the repository (see `CONTRIBUTING.md`).
