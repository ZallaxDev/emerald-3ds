# Voxel mountains: whole levels counted tile by tile

How the mountains, cliffs and rocks of the voxel overworld are built, and how
to work on them. Read [VOXEL_GUIDE.md](VOXEL_GUIDE.md) first; the code is
`3ds_port/scripts/voxel_terraces.py` (its docstring is the full reference) on
top of `3ds_port/scripts/gen_voxel_relief.py`.

This page describes the **current** method. The section
[Tried and rejected](#tried-and-rejected) lists the earlier approaches: do not
bring them back.

## The idea

The General tileset draws a mountain with square pieces, and **every piece
spans exactly one level** (16 px):

| Piece | Tiles (General) | Geometry |
|---|---|---|
| top | 071, rims 06c/0be/0b4/081... | flat, at a whole level |
| face | 07c, 0a9, 079, 091, caves 064/09f/0a7 | the south wall, vertical: one tile down = one level |
| band | west 070/073/088, east 072/075/0a2/08a/0a4, and their sea versions 172-17c | a 45° slope one tile wide: one tile across = one level |
| end | 074, 07d, 089, 07b, 092, 090 | a face's end: one level; one end over another in the same column is a diagonal edge, still one level |

So heights are **counted, not solved**:

1. Every cell is classified by its pixels: ground, top, band, other rock, or
   unknown (trees, loose rocks, props). Walkable cells, soil, berry soil,
   signs and houses are **never** rock. Secondary-tileset tiles that copy a
   General one take its role.
2. Every column of rock between two surfaces votes "the upper one is k levels
   higher"; every row of bands too. Votes with more columns are taken first;
   a vote that contradicts what is already decided is dropped and leaves a
   **wall**, never a bent surface.
3. Ground connected without a ledge or stairs (the collision map) is one
   level, and water is one body. Land joins land first, shores last.
4. A map's ground at its edges keeps the height the world solver gave it
   where the neighbouring map meets it: **no seam between maps moves**.
5. **Each cell is one fixed piece of the grid, inside its square**: top,
   face, band, or a corner piece (`cSE`/`cSW` convex, `vSE`/`vSW` concave:
   half face, half band, as 07d draws it). Nothing bends or stretches out of
   its cell. A diagonal cliff is a stair of steps whose side is the band's
   slope, not a west/east wall.
6. The rock pieces **settle together**: each picks its level (and face or
   top) to fit its neighbours at the least cost. Shared edges at the same
   height are free, a side wall costs little, a top over lower ground with no
   drawn face costs more, a **pit** (rock rising behind the foot of a face or
   band, open to the sky) costs a great deal, a **fin** (a band rising above
   the terrace it flanks) costs 3.
7. Where two pieces differ on their shared edge the engine raises a wall; the
   ground behind a raised block goes on under every row it hides.

Which maps use it: `voxel_terraces.GROUPS`. `VOXEL_TERRACES=all|none|LAYOUT_A,LAYOUT_B`
tries others. Maps outside the groups keep the generic relief of
`gen_voxel_relief.py`.

## The rules

Every rule is **general**: it names tiles and shapes of the drawing, never a
map or a cell. The current ones (`voxel_terraces.py`):

**Reading the drawing**

- Mountain tiles drawn over grass (068, a band's corner) are rock, not trees.
- 092 and 090 are lower ends, like 07d/07b: 074 over 092 is one level.
- 0a4 is an east band (0x080 mirrored, the flank over sand).
- Soil and berry soil are ground even when drawn brown.
- A face gap drawn with the town's tiles at the foot of a face (a door under
  a cliff) is the face.
- Mountain rock (General 0x064-0x0bf, blocked, open on three sides) is never
  a signpost (`voxel_cells.free_post`).
- An unread patch over a face is that face's top, even if water surrounds it.

**Pieces**

- **Low side of a band**: top pixels of a band beyond its foot belong to the
  terrace behind; they are cut and lie flat at the band's foot.
- **Gaps that only 3D opens**: where the fill behind would show just under
  rock, it is filled with that rock's own block, not the ground behind.
- Steps carved in a face are not cut away (their shaded treads are not
  ground).
- **Cuts follow the drawing**: a rock tile's background is cut pixel for
  pixel, never straightened into a polygon (that lifted background with the
  rock, painted its colour). Water drawing a boulder the hand lifted is cut
  the same way, its water left at the river's level. A house or a prop is
  never the ground behind a cut.
- **Ridge boulders**: the lower half (09b/09c) under a band is the ridge's
  rounded end (rock); the upper half (093/094) next to a higher top is that
  top's rounded flank (a band); over a band it takes the band's piece.
- **Staircases of bands** (`staircases`): each column head 074/089 + bands +
  foot 07a/07d/092 (07b/078/090) is a band of one level, the high side's;
  head = concave corner, bands = ramps, foot = convex corner. Not when there
  is water on the low side, and not when the head or foot does not meet the
  levels around it (the drawing changes level along the column).
- **More tiles than levels** (`level_bands`): a row of bands (column of
  faces) between two grounds fewer levels apart than tiles drawn gets whole
  pieces from the top, one level each, as many as there are levels; the rest
  lie flat at the foot. Not when the column keeps falling above or below
  (chains), and not with water at the foot. A column falling exactly as many
  levels as rows is laid from the top.
- **No standing rock** (`unstand`): a rock piece whose upper edge stands above
  the ground behind it drops to that ground (as itself or flat, whichever fits
  its four neighbours best). Staircases and bands are recomputed together
  until nothing changes.
- **Fins**: rock with the same ground west, east and behind that sticks out
  stays at that level.

**Behind the backs (seen from above)**

- A rim cell of rock or top with no mask of its own, half a level or more
  over the ground north of it, is cut along the lip it draws (`lip_mask`):
  every north corner follows its outline, with the ground behind under it.
- A fill that would be a boulder's top over plain ground takes the map's
  plainest top tile instead (`plain_top`).
- A band on rough rock is cut by the colours of the ground it draws round
  its corner; a boulder's upper half laid as a band (a crest's rounded head)
  is cut as the head it is.
- The engine closes a back k levels over the ground behind (`EmitNorthFace`):
  the ground north of it goes on flat up to its north edge, and behind a
  whole top a wall stands straight up from there, a tile a level of its
  wall (never behind a cut tile: it showed through the cleared pixels).
  Where the top slopes along that edge the wall steps, square to the north;
  the cell's west and east walls stop at it. The border belt does the same
  between its rows (`EmitBorderNorth`).
- Which ground shows behind a back can be chosen by hand per cell
  (`"behind"` in a map's fixes), and what fills past a map's edge per edge
  and stretch, with its 2x2 block (as the game's border) and its height
  (`"edges"`, read from relief.bin's `EDGE` table); a height only lifts or
  lowers the fill, it stays against the map's edge.

**Trees, rocks and small patches**

- Trees are 3D models, so the mountain goes on under them as if they were
  not there (`behind_trees`): the face above continues down to the wood's
  ground. A neighbouring tree costs nothing when settling.
- The crown cell of a tree (the cell north of its trunk) hides what is under
  it; if its lower layer is the water next to it, it is water.
- Small trees are a card on their trunk's cell: a crown in a face's row is the
  face, and takes its trunk's level.
- Woods of big trees surrounded by one ground level are all at that level.
- Modelled rocks are their own patch, resting on what is behind and beside
  them, not on the terrace their foot hides.
- Ground patches of 4 cells or fewer with no vote take the level of the
  ground round them.

**When the drawing contradicts itself**

The vote that loses is the hidden one (a face whose foot is a house or a
wood), then a seam between maps. Measure the votes with
`terrace_debug/votes.py` before deciding anything. If the result is still
wrong and no general rule fixes it without breaking other maps, the fix is a
**hand correction as data** in `3ds_port/assets/voxel/relief_fixes.json`
(format in the docstring of `scripts/voxel_relief_fixes.py`), never a rule
for one map.

## Never

- **Berry soil is never touched, nor a ledge's shape.** A ledge on level
  land stays the generator's berm. A ledge between two levels (the ground it
  is jumped from higher than the ground it is jumped to) is a step, its lip
  falling from one to the other; ledges one behind the other the way they
  are jumped share one drop, on the cells whose lip is drawn; a ledge whose
  far side reads higher (a mountain's top beside it) stays a berm on the
  ground it is jumped from; ground walled in by ledges alone, jumped
  into from one level and out of to a lower one, is a landing half way
  (`voxel_terraces.py`, "ledges between two levels"). `ledgecheck.py` must
  report 0 differences outside those.
- No per-map or per-cell exceptions in the code.
- No fractional levels: a piece is a whole top, face, band or corner. The
  one exception is ground between ledges (a landing), at half a level.
- No geometry outside the 16 px grid, no slants, hips or stretched pieces.
- No west/east side walls the drawing does not have: with the camera's
  perspective they read as dark slits.
- No "fix" that adds small pieces to improve a number: the simplest geometry
  that follows the drawing wins, even when a metric gets slightly worse.

## Tried and rejected

These were built, measured and dropped. They come back easily when a fault
looks local; do not reintroduce them.

- **One shared height lattice** (a point every 4 px shared by four cells,
  settled by neighbour rules such as "never rises southwards", column
  reconciliation, corner cones): one cell's rule deformed the next; slanted
  sides, bumps and wedges although every tile is straight. Replaced by the
  per-cell pieces above.
- **Fixed models per tile placed without counting levels**: the height of a
  rock mass cannot be read from a single tile; the levels must be counted
  over the whole map.
- **Corner lattices and planar triangles** (hips, ramps, cut corners): any
  piece that is not square reads as a deformation in the game.
- **Column-by-column refinement** (`refine()`): its per-column drops left
  wedges and side walls between neighbouring columns.
- **Fractional bands or faces** (`"S:8"`, a drop shared between tiles):
  neither face nor top, a twisted strip (on Route 111, 1.03 M deformed px
  against 24 k).
- **Flat on top and faces at the foot** when there are more tiles than
  levels: more holes than laying whole pieces from the top.
- **Reading a band head over the meadow (068) as a triangular corner**: the
  triangle stood up over the meadow.
- **Joining the ground through woods after the votes**, or not joining
  enclosed walkable pockets through trees: holes and side walls doubled.
- **Forcing the levels of one map** to resolve a contradiction: breaks dozens
  of votes and is an exception for one map.

## How to work on a fault

The loop below is the one that works. Metrics alone are not enough: they can
improve while the view stays full of faults.

1. **Locate the place.** From a screenshot or report, find the map and cell:
   the save's position (`scripts/emu/setpos.py` puts the emulator's player
   anywhere), or unique features (cave entrances: the `warp_events` of the
   map's `map.json`). Render the GBA drawing of the area.
2. **Mark every fault** on the screenshot, numbered by type: hole (the void),
   slit or dark side wall, deformed triangle or corner, tiny piece or teeth,
   stray pixel, stretched texture. Do not dismiss any as residual.
3. **Reproduce it with the engine's own code**:
   - `voxel_scene.py view LAYOUT X,Y out.png --relief R` (textured view over
     the shapes: each surface by its orientation, levels by colour) - it
     matches the emulator;
   - `voxel_scene.py probe` gives the cell behind a pixel, `voxel_scene.py
     cells` the piece and corners of each cell;
   - `voxel_view_scan.py LAYOUT --at X,Y --view v.png` (the console's whole
     scene, trees and buildings included) and `--box x0,y0,x1,y1 --overlay
     o.png` (the relief drawn in the GBA's own projection over the drawing).
   - `terrace_debug/pieces.py`, `trace.py`, `watch.py`, `votes.py` tell which
     rule put a piece there.
4. **Make the detector see it.** Run `voxel_view_scan.py` on the map. If a
   fault you marked is not reported, extend the detector before touching the
   geometry.
5. **Change one general rule**, preferring the simplest geometry that follows
   the drawing. Try it in minutes with `terrace_debug/trial.py`.
6. **Check regressions on every terraced map**:
   - `terrace_debug/regress.sh BASE.bin NEW.bin OUT` + `summary.py`: no class
     worse anywhere without an explanation;
   - `terrace_debug/rulediff.py VARIANT.py`: every cell whose piece changed,
     rendered both ways; a rule that changes an already good map is kept
     only if the change is clearly better;
   - `ledgecheck.py OLD.bin NEW.bin` = 0, `voxel_terraces.py LAYOUT --check`
     = `open edges: 0`.
7. **Build and look at the exact place** in the emulator (`scripts/emu`:
   `drive.ps1`, `grab.ps1`, `crop.ps1`), then on hardware. Make a
   before/after sheet: marked screenshot | new screenshot | GBA drawing.
8. **Report with numbers** what is fixed and what remains, per map.

## Checks

From `3ds_port/scripts` (`--relief X.bin` checks a trial relief instead of
`romfs/voxel/relief.bin`):

| Command | What it catches |
|---|---|
| `python voxel_view_scan.py LAYOUT [--sheet DIR] [--box x0,y0,x1,y1]` | **the first one to pass.** The console's whole scene from the game's cameras (yaw 0, pitch 34/40/46, both zooms): `hole`, `side`, `deformed` (always a fault), `flat` (rock drawn lying on a top), `standing` (a face or band above the ground behind it), `drawn-flat` / `drawn-raised` (the relief against the drawing) |
| `python voxel_shape_check.py LAYOUT [--at X,Y --view v.png]` | the relief alone: holes, deformed, side walls, tiny pieces |
| `python voxel_grid_check.py LAYOUT[,LAYOUT...] --holes` | every triangle a top, a vertical wall on the grid, or a band; 0 deformations |
| `python voxel_art_check.py LAYOUT` | every drawn pixel on a surface of its class; split seams |
| `python voxel_terraces.py LAYOUT --check` | `open edges: 0` |
| `python ledgecheck.py OLD.bin NEW.bin` | ledges and berry soil unchanged: 0 differences (but ledges between two levels) |

The full relief: `VOXEL_RELIEF_CACHE=build/relief_cache python3
scripts/gen_voxel_relief.py --output build/relief_new.bin` from
`3ds_port/` (about 6 minutes; the cache makes later runs faster).

## References

- Gen2Recomped's DRAMATIC_SHAPE mod builds Emerald in block columns on the
  grid, with heights per terrace set by convention or by hand: the height of
  a rock mass cannot be read from the drawing alone, which is why this method
  counts levels over the whole map.
- pokeplatinum's terrain is hand-made meshes plus collision plates; nothing
  is derived from the tileset.
