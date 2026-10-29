# Trees of the General tileset

Original art for the voxel mode's trees, drawn for Emerald 3DS:

- `tree_trunk.png`: 32×32, opaque trunk and ground, on a 2×2-cell footprint.
- `tree_crown.png`: 32×36 RGBA, crown on a transparent background.
- `tree_small_trunk.png`: 16×16, trunk of the small tree on a single cell.
- `tree_small_crown.png`: 16×32 RGBA, crown of the small tree on a
  transparent background.
- `tree_2_small_trunk.png`, `tree_2_small_crown.png`: the same pair for
  Dewford Town's small tree, trunk on sand.
- `tree_small_trunk_jungle.png`, `tree_small_trunk_grass_road.png`,
  `tree_small_trunk_volcan.png`: 16×16, the small tree's trunk on Fortree's
  jungle floor, Route 111's pale grass and Route 112's volcanic soil. Their
  crown is `tree_small_crown.png`.
- `tree_crown_base_floor.png`: 32×32, opaque ground of the secret base's
  tree, on a 2×2-cell footprint.
- `tree_crown_base.png`, `tree_crown_base_open.png`: 32×32 RGBA, its crown
  closed and with the base's entrance opened (the same upper half).
- `tree_crown_mossdeep.png`: 32×32 RGBA, Mossdeep's round tree, trunk
  included; `tree_base_mossdeep.png`: 32×16, opaque, its trunk row's grass
  and shadow; `tree_base_water_mossdeep.png`: 32×16 RGBA, its roots and
  shadow on a transparent background, laid over the animated shallows.
- `tree_north_crown.png`: 32×32 RGBA, the leaning tree of Route 113,
  Fallarbor Town and Route 114; `tree_north_base_rock.png`,
  `tree_north_base_ash.png`: 32×16, its trunk row on the brown soil and on
  the ash.
- `tree_small_trunk_access.png`: 16×16, the small tree's trunk on the darker
  grass at a wood's way in (Route 104, Southern Island, Faraway Island,
  Route 112). Its crown is `tree_small_crown.png`.
- `tall_grass.png`: 16×32 RGBA, a tuft of long grass on a transparent
  background; `tall_grass_floor.png`: 16×16, opaque, the floor it grows on.

`scripts/gen_voxel_trees.py` packs them, unscaled, into
`voxel/trees.rgba5551`: a 128×128 RGBA5551 texture in PICA200 order (large
crown at 0,0; large trunk at 32,0; small crown at 32,32; small trunk at
48,32; Dewford's small crown at 64,0 and its trunk at 80,0; the jungle,
pale-grass and volcanic trunks at 48,48, 80,16 and 64,32; the secret base
tree's floor at 96,0, its crown at 0,64 and the opened one at 32,64;
Mossdeep's crown at 64,64 and the leaning tree's at 96,64, their trunk rows
at 64,96 (grass), 96,96 (water), 0,96 (soil) and 32,96 (ash); the long
grass's tuft at 80,32, its floor at 96,32 and the trunk at a wood's way in
at 112,32). It is an engine file and ships inside the 3DSX. The build regenerates
it whenever a source changes; it needs Pillow like the port's other graphics
tools.

The crown is a fixed plane tilted 50° from the ground, high end to the north.
The tilt and the anchor point are in `src/voxel/voxel_tree.c`. The plane's
base sits at −0.10 cells: the transparent margin sinks into the ground and the
visible leaves overlap the trunk. The renderer uses alpha test: transparent
pixels write neither colour nor depth.

They replace the regular metatiles `1D4–1D7`, `1DC–1DF`, `1E4–1E7`, `1F2–1F3`
(the lower row of a staggered wood, under the next tree's canopy) and
`1FC–1FD` (the upper row in front of a fence) of the **General** tileset, also in its repeated border. The tops `1C6–1C7` and
`1CE–1CF` get their background grass back. Special trees keep their art. Each
cell emits its own part of the trunk and the crown, so a tree can cross a
chunk or map boundary without being duplicated or cut when rebuilt.

Where a hedge, a roof, a fence or a ledge of the secondary tileset draws over
one row of a large tree (Petalburg, Oldale, Lilycove, Route 104's flower
shop, Route 116, the Safari Zone), that cell is not a tree part: it keeps its
own ground and model, and the tree's row in the next cell adds the missing
row of the crown over it, so the crown is not cut in two. Inside the map
only; across its edge the neighbour's own cells answer.

The small tree works the same way: the trunk cell (`016–017`, forest edge;
`0C6–0C7`, interior, where the next crown overlaps the trunk; `1F4–1F5`)
becomes the new trunk and raises the whole crown above it, a 1×2-cell plane at
50°, base at −0.10 and 0.825 cells from the north edge. The upper cells lose
the old crown and get their background back: `00E–00F` and `040` grass, `01D`
ledge edge, `025` tall grass, `02D` reflective water, `035`/`193` water, `0CE`
rock. `1EC–1ED` are the bottom row of a large tree. The secondary tilesets'
variants keep their art.

A small tree at the foot of a mountain (its crown's cell backed by a cliff or
shelf, two cells north of the trunk) stands up steeper, the plane's top at the
trunk cell's north edge: leant back at 50° it ran into the rock and came out of
it where the rock falls away, a corner's slope (Route 110, by New Mauville's
entrance). So does one whose north neighbour stands higher than its trunk (a
terrace's edge, a wood climbing terraces on Route 112 and 119): the higher
ground's top cut the leant crown across.

Dewford Town's small tree (its secondary tileset, so the ids only mean a tree
there) is replaced the same way with its own art: the trunk cells `23A` (on
the sand) and `243` (inside the wood, where the next crown overlaps it, also
the repeated border) become the trunk and carry the whole crown; the crown top
`239` gets the sand `124` back.

The General's small tree drawn by a secondary tileset over its own ground is
replaced the same way, with the General's crown and a trunk on that ground:
Fortree's `253` (jungle floor), Mauville's `226` (Route 111's pale grass) and
Lavaridge's `218` (Route 112 and Lavaridge Town), with `21A`, `3B8` and
`203` inside its woods, where the next crown overlaps the trunk (the
General's `0C6`, on the soil and on the dark rock). Fortree's `22A` is that
`0C6` with the crown drawn on both layers, round its tree houses: the
General's trunk on grass. The old crown its tileset
draws over the cell north of the trunk (over its ground, a mountain's rock or
anything else) is cut away by the relief (`scripts/voxel_terraces.py`,
`canopy_mask`), the ground it was drawn over laid under it. On maps the
relief does not read that way (Verdanturf, the Battle Frontier, Southern
Island, Ever Grande, Faraway Island) and over Route 112's dark rock,
`VoxelTree_GroundMetatile` swaps the tile for the plain ground it is drawn
over, the same tile without the crown.

The secret base's tree (`01E–01F` over `026–027`, `036–037` once
`secret_base.c` opens it) is a 2×2 tree like the large one, with its own
floor and a 32×32 crown, two cells long at the same 50°; its lower row takes
the opened crown when the base is open. Both are General ids, so it is
replaced on every map.

Fortree's tree houses, decks and bridges stand in the treetops
(`scripts/voxel_fortree.py`): the cells they are drawn on (`224-226`,
`22B-22F`, `233-237`, `23B-23F`, `243`, `245-247`, `256`) are that same
General trunk on grass, as `22A` is round them, so the wood fills the space
between the floor and the planks (the crowns stand 23 px, the planks 25).
The canopy over the next row's trunks (`244`, like the General's `00E-00F`)
and the cells of a palm (a model of its own) get the grass `001` back, the
crown's top over the path (`24B-24C`) the path `261`.

The same goes for the small tree at a wood's way in, drawn by a secondary
tileset on its path's darker grass: Rustboro's `307` (Route 104, Southern
Island, Faraway Island's entrance) and Lavaridge's `201` (Route 112) become
the trunk on `tree_small_trunk_access.png` with the General's crown.

Long grass (Route 119, Route 120, Faraway Island) is drawn the same way as a
small tree: every cell of it, the General's `015` and Fortree's south edge
`208` and `281–283`, lays `tall_grass_floor.png` and carries its tuft
`tall_grass.png`, a 1×2-cell card at the small crown's 50°, base and foot.
The tuft of the cell south of a walker stands in front of it up to its
shoulders, so only its head shows, as the GBA's grass covers the rest of it;
the game's flat picture of the grass under the walker
(`FLDEFFOBJ_LONG_GRASS`) is not laid on that floor. Long grass casts no
crown's shadow and a 3D battle stands on it as on any ground.

Two secondary tilesets draw 2×2 trees of their own, replaced with that art:
Mossdeep's round tree (Mossdeep City and Routes 124–129) and the leaning tree
of the Fallarbor tileset (Route 113, Fallarbor Town, Route 114, also in the
repeated border of all three). Only the trunk row is a tree part: Mossdeep's
`322–323` and `326–327` on grass, `2F8–2F9` in the shallows, Fallarbor's
`20E–20F` on the brown soil, `20C–20D` on the ash and `2CA–2CB` on the soil's
orange rim (drawn with the soil's trunk). Its cell lays the trunk row's ground
and carries the crown, 32×32 in two rows at the same 50°, the upper row added
over the cell north of it, across a map's edge too. That crown row's own cells
get the ground their lower layer is: Mossdeep's `31A–31B` grass `001`,
`31C–31D` the cliff's face `07C` (the old crown is not cast on the rock),
`31E–31F` `002`, `2F0–2F1` the shallows `19E`; Fallarbor's `204–205` ash
`218`, `206–207` soil `279`, `214–215` the rim `271`, `2C2–2C3` the brick
rock `269`, `2CC–2CD` water `170`. In a wood the trees are staggered and a
trunk's cell also holds the next tree's crown top (Mossdeep's `324–325` in
the water and `355–356` on grass; Fallarbor's `26B–26E`, `273–276`, and
`2CE–2CF` on the rim): it is the trunk, and the next tree's own trunk adds
that top.

A trunk in the shallows lays no ground of its own: the cell stays water
(`VoxelTree_LaysGround`), the ground pass lays the animated `19E`, and the
tree pass lays `tree_base_water_mossdeep.png` 0.02 over it, alpha tested, so
the water keeps moving between its roots. A crown row over water is water too.

The crown's foot stands a cell south of the trunk row's
north edge (1.15 for Mossdeep's on grass, 0.95 in the shallows), over the middle of its shadow. Against rock it stands up steeper,
as the small tree does: when the crown row is a cliff or a shelf, a mountain's
rock is right behind it, or the ground one or two rows north stands higher than
the trunk (the terrace over a cliff's face behind Mossdeep's woods, Fallarbor's
brick by its Pokemon Center). Leant back, its upper row went into that ground
and was cut across. Both columns of a tree stand alike.

