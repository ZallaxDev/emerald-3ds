# Buildings modelled from their own drawing

`scripts/gen_voxel_buildings.py` turns every building described in
`scripts/voxel_building_specs.py` into a 3D model and writes them to
`voxel/buildings.bin` (an RGBA5551 texture in PICA order plus triangles in the
exact `VoxelVertex` format). On the console, `src/voxel/voxel_building.c`
copies the triangles into the chunk of the model's top-left cell and draws
flat ground under its cells.

## The principle

Emerald's art is a fixed oblique projection: a point (X, Y, Z) in pixels lands
at `u = X, v = Z − Y`, which is what an orthographic camera sees at 45°. So:

1. **Every texel is one pixel** seen at 45° square to its face: 1:1 on walls
   and floors, `cos b + sin b` along a roof slope of pitch `b`.
   `density_check` verifies it triangle by triangle. Nothing is stretched.
2. **The front is the drawing**: the generator renders the model at 45° and
   requires a pixel-exact match in the spec's `exact` zones (facade, eave,
   upper floor…). If it does not match, the build fails.
3. **Real depth**: the drawing is flatter than the building (a house with 4
   rows of collision drawn in 5 rows of art would be 2 cells deep). The model
   takes the real footprint and fills the missing depth with **more courses
   of the same tiles** or more of the same plaster column — never stretching.
4. **Sides and backs from the art itself**: hip roofs with courses laid
   parallel to each eave; side walls with the clean plaster column and the
   facade's corner posts.
5. **No ground**: a pixel belongs to the building if its upper layer is opaque
   or its lower-layer subtile is not one of the ground subtiles around it.
   Everything else is transparent and the cell gets the ground metatile.

## Modelling a new building

1. Find one copy in a layout and its rectangle of cells.
2. Read the art's bands row by row (see the docstring of
   `voxel_building_specs.py` for the Littleroot house): facade, eave (fascia),
   roof courses and their period, ridge teeth and cap, clean columns
   (`ROOF_COLUMNS`) without edge boards.
3. Describe the parts: `Prism` with `Proj` for walls at their drawn depth,
   `HipRoof` for roofs, `Strip` for coursed faces, `Tile` for plaster and posts.
4. Declare the `exact` zones and run:

   ```
   python scripts/gen_voxel_buildings.py --preview build/buildings
   ```

   It writes `*_ortho.png` (art | 45° render | differences) and perspective
   views from several cameras (`game`, `left`, `right`, `yaw35`, `yaw-50`,
   `high`) framed like the voxel renderer.
5. The generator finds every appearance of the same drawing in every layout
   with the same tilesets, so a building is modelled once.

## Reuse across Hoenn

- `match_rows` says which rows of the drawing must repeat cell for cell. If
  they all come from the primary tileset, the building matches in **any**
  town sharing that primary, whatever its secondary. That is how the Pokémon
  Center and the Mart (two specs) cover their 20 appearances.
- The spec points at a **canonical** copy (Petalburg's Center, Mauville's
  Mart): other towns paint things of their own in the top row (the path in
  Oldale, a tree crown, a cliff in Lilycove and Verdanturf, a sign in
  Lavaridge).
- Those cells are not lost: where the model has no geometry, the generator
  creates a **ground patch** from what the map paints there minus the
  building's pixels and lays it flat on the ground with the model. If the
  model does occupy the cell, the log says so.
- The ground under each placement is the most common walkable metatile around
  it in that map, which its atlas always contains.
- `--town LAYOUT_X` renders the whole layout with every model and the same
  rules as the console.

## Interiors: a tile modelled once is modelled everywhere

The rule for every room, now and in the future:

1. **Before modelling a room**, run the generator: its log lists, with
   `reuse: <piece> stands in <room> at x,y`, every piece already modelled
   in another room that this one repeats. A piece is reused, as it is, same
   model, no copy:
   - where **every cell of its rectangle draws exactly the same pixels**
     (whatever the tileset or metatile number); or, failing that,
   - for a thing standing in the room (not a wall), where **its own pixels
     are drawn again, pixel for pixel, on whatever floor**: the gyms' statue
     is modelled once in Rustboro and stands in all eight gyms. There it
     stands "bare" (a twin model without its first room's floor marks) and
     the room's own paint round it is laid as patches. Same-tile matches
     are placed first: two pieces drawn alike can stand at different depths.
   Shape a piece so that it does not claim floor pixels (the statue's
   rounded foot corners), or it will only match on its own room's floor.
   Anything like this (statues, plants, chairs, machines) must never be
   lifted again by hand in another map.
2. **The room's spec models only what is new.** Anything reused is left out:
   the generator treats its pixels as taken, and a piece of the spec that
   is left with none fails with `claims no pixel`, the signal that it was a
   duplicate and has to go.
3. Order counts: a piece belongs to the first room in `SPECS` that models
   it. Later rooms, and every indoor room nobody has modelled, take it.
   In a room with no spec, the known furniture stands and the rest of the
   drawing stays flat, laid over the floor as patches.
4. The proof is the same: `room ... 0 pixel(s) differ` for every room,
   reused pieces included.

This way Pokemon Center 2F uses the 1F stools, May's house uses Brendan's
fridge, sink, TVs, cabinet and chair seats (on her pink rug), the lab with the table uses the whole lab minus the
table, Lavaridge uses the Center's counter and machines, and House2 uses
House1's glass case, plant and east chair seats. Reuse also covers some rooms nobody has
modelled yet (Lanette's house, the League, Steven's house...).

Details that apply to every piece:

- A card (plant) is textured only with its own pixels, never with the floor
  behind its pot.
- What a piece hides of a mark (a rug under a chair) is drawn under it,
  when the row reaches the same mark on both sides; otherwise the floor
  shows through. Not behind a card: that floor can be seen.
- A piece's side (`side`) samples its own pixels, not the fill that the
  wall behind uses to complete itself.

## Free-form objects (hedges, walls)

A spec with `components` does not describe a building but a set of metatiles
(Petalburg's hedges). Every connected run of those metatiles, in every layout
of that tileset, becomes its own model:

- The art comes only from the run's cells, and the ground is separated
  **pixel by pixel** (foliage and grass share a subtile on the hedge's flank).
- `Relief` reads it column by column: in each column the drawing shows the
  object's top and, at its south end, its front of `height` rows. The top goes
  at that height and the front is vertical, both projected, so the drawing is
  reproduced exactly. Flanks and back are dressed with a piece of the drawn
  front.
- Cells of its rectangle that are not part of the run (the surrounding house)
  are marked with height 255 and the console does not claim them.
- A run is only placed where it was found: a short one also appears inside
  every long one.

Rustboro's railings add three rules:

- A long run is cut into 4×4-cell blocks, one model per repeated block. A
  block carries on into the next one: its top runs on past its seam with
  `height` rows of the next block's drawing, and no front or back stands at
  the seam. Without this, a railing running down a column stood up as a
  slab every four cells.
- `upper`: the railing is drawn whole on the upper layer. Only that layer is
  modelled, and the map paints the bottom layer under it (the ground with
  its grass edges). Otherwise those edges rose as pale slivers.
- `flank`: a railing running down a column is drawn as a line (its top). Its
  long sides carry the bars of the railing that runs along a row (metatile
  2A7), gaps included, instead of the dark line projected. That band is
  appended below the art and is never read as drawing.
- No flank stands where a block goes on into the block east or west of it.
- The tile list must hold every variant of the railing: the same railing
  over grass or a building's shade is another metatile.

Shadows: the lighting casts a model from one height per cell. A cell the
solid covers less than FOOTPRINT_FULL/256 of (a railing's line, a hedge's
end) also carries its footprint, 16×16 bits, and casts only from those
pixels. As a box, a railing darkened its whole cell and cast a block. The
ground patches under a model are lit like the terrain, so a shadow falls
across them too; the model keeps the shading it was drawn with.

Texture pages are packed bottom-left on a skyline, not on shelves. Two
pages of 512 KiB do not fit the 768 KiB VRAM block together: the page of a
room next to Rustboro (512 KiB) must stay at 256 KiB or less, or crossing
the door leaves one of them waiting. So a long, thin piece (a side wall the
room's full length) is split.

## Landmarks in files of their own

`scripts/voxel_specs_*.py` hold the landmarks whose art needs more than a
spec entry (a cleaned drawing, a texture of several bands, parts read off the
drawing); `voxel_building_specs.py` imports their `SPECS` at its end.

- `voxel_specs_small_landmarks.py`: Mt. Pyre's gravestones (one model, every
  grave on the first floor) and the Oceanic Museum's columns, display cases
  and the four exhibits upstairs, all as relief props.
- `voxel_specs_industrial_props.py`: the Space Center's computer
  desks (the laptop lifted off the desk), schematic table, empty desk and
  wall console; New Mauville's generator (plinth, body and round exhaust cap
  as three solids), equipment rack, green machine and power cabinet. Each
  piece is matched by its drawing, so a desk modelled once stands on both
  floors.
- `voxel_specs_frontier_aux.py`: the Battle Frontier's Ranking Hall and
  Exchange Service Corner (the raised service emblem on the roof, a canopy
  over the hall's entrance) and the Reception Gate's two wings.
- `voxel_specs_route_landmarks.py`: the Safari Zone's gate on Route 121 and
  its south entrance, and the Weather Institute with its two round roof
  housings (`WeatherRoofInstallations`; the drawing's own cream material
  covers their reverse flank).
- `voxel_specs_city_landmarks.py`: Lavaridge's gym and Sootopolis's seven
  houses, the peaked ones as an octagonal body under an octagonal cone.
- `voxel_specs_elevated_landmarks.py`: Fortree's treehouse, its leafy roof
  front a slope with the drawing projected at its natural density.

Three spec keys serve these models:

- `preserve_ground` with `own` (a set of the drawing's pixels, or a function
  of the art returning one): only those pixels are the model's; the map's own
  ground is patched under every cell it stands on (`placement_ground_policy`),
  so the cliff behind a Sootopolis house, the deck round the treehouse or the
  garden behind a Frontier hall stays terrain.
- `patch_omit_tiles`: metatiles whose colours a ground patch must not carry
  (water stays the animated ground's).
- `owned`: the cells of the rectangle the model reserves, when a corner of
  the rectangle holds only the surrounding terrain.

## Memory

The shared texture lives in VRAM (uploaded through the atlas staging buffer,
so at most 512×256). The generator fails if it is exceeded.

## Cost on Old 3DS

Littleroot house: 614 triangles (1,842 vertices, 43 KiB). Shared texture
256×128 (64 KiB of linear memory) for every model.
