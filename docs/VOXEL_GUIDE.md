# Contributing to the voxel world

The voxel overworld turns Hoenn's 2D maps into 3D on the console. Every model
is read off the game's own drawing on the host, by Python generators, and
written to `voxel/*.bin`; the console only draws what they wrote. This page
is the entry point for anyone who wants to voxelize something: how to set up,
the rules every model follows, the working loop, and what a pull request
needs.

| Area | Where | Guide |
|---|---|---|
| Buildings, interiors, props, statues | `3ds_port/scripts/voxel_building*.py`, `gen_voxel_buildings.py`, `voxel_props.py` | [buildings README](../3ds_port/assets/voxel/buildings/README.md) |
| Mountains, cliffs, rocks (terrain relief) | `voxel_terraces.py`, `gen_voxel_relief.py`, `assets/voxel/relief_fixes.json` | [VOXEL_MOUNTAINS.md](VOXEL_MOUNTAINS.md), [terrace_debug](../3ds_port/scripts/terrace_debug/README.md) |
| Trees | `gen_voxel_trees.py`, `assets/voxel/trees/` | the trees README |
| Signs, regions | `gen_voxel_sign_masks.py`, `gen_voxel_regions.py` | their docstrings |
| Runtime (chunks, camera, lighting) | `3ds_port/src/voxel/` | [ARCHITECTURE.md](ARCHITECTURE.md) |

The generators' docstrings are the detailed reference; read the one of the
file you change.

What is done, what has faults and who is working on what, map by map and
room by room: [VOXEL_STATUS.md](VOXEL_STATUS.md).

## Setup

1. Get a tree and build it once ([DEVELOPMENT.md](DEVELOPMENT.md)):
   `python tools/bootstrap.py --make -j8`. The generators read the maps,
   tilesets and metatiles of the decompilation in `build/upstream`; nothing
   comes from your ROM except at the builder's runtime.
2. **Edit in your clone, run in `build/upstream`.** `bootstrap.py` copies
   `3ds_port/`, `builder/` and `tools/` into `build/upstream` every time it
   runs, over whatever is there. So change files in the repository, re-run
   `python tools/bootstrap.py` (without `--clean`: seconds) to copy them, and
   run the generators and checks from `build/upstream/3ds_port`. Changes
   made only inside `build/upstream` are lost at the next bootstrap.
3. `make -j8` in `build/upstream/3ds_port` regenerates `romfs/voxel/*.bin`
   when a generator or its data changes (the build always verifies the
   buildings), and `make verify` runs the host tests.
4. Look at the result on a console (development build or loose data, see
   DEVELOPMENT.md) or in an emulator. On Windows, `3ds_port/scripts/emu/`
   drives Azahar: `drive.ps1` (buttons by sequence), `grab.ps1` (screenshot),
   `crop.ps1`, and `setpos.py` (put the save's player at any map and cell;
   keep a copy of the save). The emulator shows what is drawn faithfully, not
   the Old 3DS's memory or speed: hardware has the last word.

## Rules for every model

1. **The drawing is the truth.** Emerald's art is an oblique projection: a
   point (X, Y, Z) lands at `u = X, v = Z − Y`. Every model must reproduce
   its drawing pixel for pixel at 45 degrees (the generators prove it and the
   build fails otherwise), with every texel one pixel - nothing stretched.
2. **The true 3D form, not only the outline.** A silhouette that matches at
   45 degrees is not enough: the solid must be what the drawing depicts (a
   convex dome, a crown on top, cloth in relief), seen from the game's
   camera in perspective (yaw 0, pitch 34-46) and from the sides.
3. **The simplest geometry that follows the drawing.** Square pieces on the
   grid for terrain; flat faces for flat things; no little pieces added to
   improve a number.
4. **Model once, reuse everywhere.** Whatever is already modelled is reused
   as is wherever the same tiles or the same pixels repeat (the generators
   find the copies: read their `reuse:` lines); a new spec models only what
   is new. Never place or lift the same thing by hand map by map.
5. **General rules, not exceptions.** A rule names tiles and shapes, never a
   map or a cell. A place no general rule fixes gets a hand correction **as
   data** (`relief_fixes.json` for terrain), not a special case in code.
6. **Some things are never touched**: berry soil, a ledge's shape (gameplay
   reads them) and the seams between connected maps. A ledge only follows
   the levels either side of it: a berm on level land, a step between two.
7. **Nothing from the cartridge is committed**: no renders, screenshots,
   extracted tiles or generated `.bin` files. Specs hold coordinates, tile
   ids, heights and names only. `python tools/release_audit.py --repo .
   --strict` must pass.
8. **Mind the Old 3DS.** Triangles and texture space cost memory and chunk
   build time; the buildings README has the budgets. Do not try to win speed
   by culling faces the camera "never sees": it was measured at -0.3%
   triangles and no frame-rate change.

## The working loop

The same loop for a building, a room or a mountain:

1. **Locate** the place (map, cells) and render its GBA drawing.
2. **Mark every fault** on a screenshot, numbered by type (hole, slit, side
   wall, deformed shape, stretched texture, ghost, tiny pieces). None is
   "residual".
3. **Reproduce** it with the engine's own code (`voxel_engine_view.py`,
   `voxel_scene.py`, `voxel_view_scan.py --at`), not only with the
   generator's preview.
4. **Detect it automatically**: the checks of the area must report it. If
   they do not, extend the check first.
5. **Fix it with a general rule** (or a spec, for a model), simplest
   geometry first.
6. **Check for regressions** everywhere the rule applies, not only where you
   looked (each guide lists its commands). A change to an already finished
   map counts as a regression unless it is clearly better.
7. **Look at the exact place** in the emulator and on hardware, and make a
   before/after sheet: marked screenshot | new screenshot | GBA drawing.
8. **Report with numbers** what is fixed and what remains.

## Before opening a pull request

- Coordinate first: open (or comment on) an issue naming the map or area you
  take, so two people do not voxelize the same place. One area per branch.
- Update the rows of [VOXEL_STATUS.md](VOXEL_STATUS.md) your work changes, in
  the same pull request.
- The checks of your area pass (see the buildings README and
  VOXEL_MOUNTAINS.md), and `make verify` passes.
- In the pull request (not in the repository): the before/after sheets, the
  numbers before and after of every check you ran, and where you tested
  (emulator, Old 3DS, New 3DS).
- Follow [CONTRIBUTING.md](../CONTRIBUTING.md) for branches and review.

## Working with an AI agent

The repository carries a project skill for Claude Code,
`.claude/skills/voxelize/SKILL.md`, with this method in agent form: it loads
when you ask for voxel work and points the agent at these guides and checks.
Whatever tool you use, you are responsible for the result
([AI_DISCLOSURE.md](../AI_DISCLOSURE.md)).
