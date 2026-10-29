/*
 * Terrain relief read off the drawing: cliffs, their stairs, rocks at sea.
 *
 * voxel/relief.bin is written by scripts/gen_voxel_relief.py, which
 * explains the model: every point (u, v) of the map's art is lifted to a
 * height h and stands at (u, h, v + h), which the GBA's 45-degree view sees as
 * the drawing itself. The file holds, for each lifted cell, its heights on a
 * 4-pixel lattice (5 x 5 points, in pixels, signed; a drawn mountain's in
 * units of 2 pixels, so that it can stand taller than a byte).
 *
 * Everything standing on a lifted cell is lifted with it: the same (0, h, h)
 * displacement applied to the terrain is applied to the trees, structures,
 * buildings and billboards on it, so they stand where the drawing shows them.
 *
 * A map as a whole stands at its base: the level its ground is at in the
 * world (Fortree up on the plateau Route 119 climbs to), everything on it
 * raised by it and, as the drawing has every point, drawn as far south: a
 * point stands as far south as it is high in the world, base and relief
 * (VoxelBuilder_StretchZ, VoxelRelief_ShiftAt). So two maps meeting at one
 * height meet in the same row, whatever their bases - Fortree's west edge
 * on its base and Route 119's plateau beside it, raised by its relief. The
 * cells' heights and depths are over the base; only VoxelRelief_LiftAt and
 * VoxelRelief_ShiftAt, a height and a depth in the world, count it in.
 */
#ifndef CTR_VOXEL_RELIEF_H
#define CTR_VOXEL_RELIEF_H

#include <stdbool.h>
#include <stdint.h>

#include "voxel_world.h"

#define VOXEL_RELIEF_SIDE 5  /* lattice points per cell side */

bool VoxelRelief_Init(void);
void VoxelRelief_Shutdown(void);

/* The level a map's ground stands at in the world, tiles (0 for a map the
 * file does not name), and its lift over that where it was placed. */
float VoxelRelief_Base(const VoxelMapInstance *inst);

/* Pixels to add to `b`'s lift for its edge to meet `a`'s at their seam, as
 * most of the seam's cells ask (see voxel_relief.c); 0 when they do not
 * touch or the seam asks no one step. */
int VoxelRelief_SeamStep(const VoxelMapInstance *a, const VoxelMapInstance *b);

/* The 25 heights (pixels, over the base) of a world cell, or NULL when it is
 * not lifted. */
const int16_t *VoxelRelief_Cell(const VoxelMapInstance *inst, int x, int y);

/* True for a map whose relief was read off its drawing: every blocked cell
 * of rock is a relief cell, and no structure stands on one. */
bool VoxelRelief_IsDrawn(const VoxelMapInstance *inst);

/*
 * The courses the border round a map stands, laid as its drawing lays it
 * (each block of it at (u, h, v + h), h the belt's height): a cave carved
 * out of the rock, whose edges stand at that height all round, goes on into
 * the rock past them without a seam. 0 for the border as every other map has
 * it (VoxelMesh_EmitBorder).
 */
unsigned VoxelRelief_Belt(const VoxelMapInstance *inst);

/* The cliff wall a map's cut cells hang most often (its mountains' face
 * tile), -1 if none: what a back's north face is drawn with when its own
 * record names no wall. */
int VoxelRelief_FaceTile(const VoxelMapInstance *inst);

/*
 * What a map's fixes fill past one of its edges with (relief_fixes.json
 * "edges", the editor's edge fills), in place of the border block: the
 * metatile (the map's own id), whether it is water, and where its top stands
 * (pixels over the map's base) when the fixes give it - else the belt follows
 * the map's edge as ever. `edge` is 1 west, 2 east, 4 north, 8 south; `along`
 * the row (west, east) or column (north, south) of the map's edge cell
 * nearest; `index` which of the stretch's 2x2 block (0-3, row major), the
 * game's border block's phase. False where nothing is set.
 */
typedef struct
{
    int metatile;
    bool water, hasTop;
    int top;
} VoxelEdgeFill;
bool VoxelRelief_EdgeFill(unsigned layoutId, unsigned edge, int along, unsigned index, VoxelEdgeFill *fill);
/* The fill past instance `ring`'s edges at a world cell outside it, if any. */
bool VoxelRelief_EdgeFillAt(const VoxelMapInstance *ring, int worldX, int worldY, VoxelEdgeFill *fill);
/* The i-th metatile of a map's edge fills, four a stretch (for its atlas),
 * false past the last. */
bool VoxelRelief_EdgeFills(unsigned layoutId, unsigned i, int *metatile);

/* True when the cell's lattice is not level: rock or stairs, drawn by the
 * relief itself and not by any structure. */
bool VoxelRelief_IsSlope(const int16_t *grid);

/* The 25 depths (pixels) the cell's points are moved south by, over its
 * map's base: its heights again, (u, h, v + h), the drawing seen at 45
 * degrees. NULL when the cell is not lifted. */
const int16_t *VoxelRelief_Depth(const VoxelMapInstance *inst, int x, int y);

/* Height in tiles over the base at the centre of a cell (0 when not lifted). */
float VoxelRelief_CellLift(const VoxelMapInstance *inst, int x, int y);

/* Its depth, in tiles, at the centre of a cell. */
float VoxelRelief_CellShift(const VoxelMapInstance *inst, int x, int y);

/* Height in tiles in the world at a world position, interpolated on the
 * lattice: the base of the map there and the relief over it. */
float VoxelRelief_LiftAt(float worldX, float worldZ);

/* Depth in tiles in the world at a world position, interpolated on the
 * lattice: the base of the map there and the relief over it. */
float VoxelRelief_ShiftAt(float worldX, float worldZ);

/*
 * A map's south seam ease (see voxel_relief.c): world z from `from` to `to`
 * is drawn longer, and past `to` moved `add` tiles south, so the map's edge
 * meets the next one's. False (and z unchanged) for a map with none.
 */
bool VoxelRelief_Stretch(const VoxelMapInstance *inst, float *from, float *to, float *add);
float VoxelRelief_StretchZ(const VoxelMapInstance *inst, float z);

/*
 * A drawn map's surface as it stands on its base, for the sun (the lighting's
 * rays): its height in tiles over the base at a world position, 0 off any
 * drawn map; the highest of it over a cell (and in `bottom` the lowest:
 * every SurfaceAt over the cell lies between the two); the highest of the
 * map.
 */
float VoxelRelief_SurfaceAt(float worldX, float worldZ);
float VoxelRelief_SurfaceTop(const VoxelMapInstance *inst, int x, int z, float *bottom);
float VoxelRelief_DrawnTop(const VoxelMapInstance *inst);

/*
 * Cut tiles: a cell of rock drawn over the ground behind it (the sea round a
 * ridge's end), whose lattice would lift that ground with the rock. It is
 * drawn twice - its whole tile flat at its foot, and its relief textured with
 * the variant that has the background clear (bit set in `rows`, a row of 16
 * pixels each, two bytes little endian). Returns the variant, -1 for a cell
 * that is not cut; `foot` in tiles over the base. `ground` is the metatile
 * of the ground it is drawn over (-1: none), which runs on under the rock
 * beside it: never drawn at 45 degrees, it is what shows through the clear
 * background from above, between the rock's outline and its hidden wall.
 *
 * VOXEL_RELIEF_NO_VARIANT is no cut: a cell under which the ground behind a
 * terrace's rim (`ground`) runs on, flat at `foot`, where the step down to
 * it leaves the view open from above. With VOXEL_RELIEF_GOES_ON in `sides`,
 * the rock of the cell north of it - a flank ending against this top's back
 * - goes on under it instead, one row on: what the camera finds past the
 * back when it looks down more steeply than the drawing.
 */
#define VOXEL_RELIEF_NO_VARIANT 0xFFFF
#define VOXEL_RELIEF_GOES_ON (2u << 8)
/* With VOXEL_RELIEF_GOES_ON_FACE too, that flank is a corner cut along its
 * rock (a band's head at the end of a rim): it goes on whole, drawn as the
 * band's side face, the metatile in `wall` (which is then no cliff). Cut, it
 * left a gap beside the corner that a camera steeper than the drawing saw
 * the ground below through. */
#define VOXEL_RELIEF_GOES_ON_FACE (32u << 8)
/* With VOXEL_RELIEF_GOES_ON_SOUTH instead, the rock that goes on under it is
 * the cell south of it, a row back: a bridge's deck over a band (Route
 * 120's ends over the slopes of its river's banks), whose row north is
 * another plank. Walled down as a block, the bank stood under the deck as a
 * vertical slab of rock. */
#define VOXEL_RELIEF_GOES_ON_SOUTH (64u << 8)
/*
 * `wall` (-1: none) says the cell has cliffs: wherever its west, east or
 * south edge stands over its neighbour's - a tile of rock reaching its own
 * edge in its own shape above ground further down than the tile goes - a
 * wall closes the step. West and east it is the rock's own edge column
 * drawn down; south, the mountain's face, this metatile
 * (VOXEL_RELIEF_NO_FACE: the map has none, no south cliff).
 *
 * With VOXEL_RELIEF_BLOCK_WALLS in `sides` (a hand-built block, see
 * relief_fixes.json) the west and east walls are the mountain's face too,
 * the same tile and the same way as the south one, whatever the cell's own
 * edge column holds: a block's four sides then read as one rock.
 */
#define VOXEL_RELIEF_NO_FACE 0xFFFE
#define VOXEL_RELIEF_BLOCK_WALLS (4u << 8)
/*
 * With VOXEL_RELIEF_THIN_DECK in `sides` (a bridge's deck over the water)
 * its walls go down only VOXEL_RELIEF_DECK_EDGE below the deck - the edge of
 * the planks - and the water runs on under it (`ground` at `foot`), seen
 * through the gap: walled down to the water it stood as a dam.
 */
#define VOXEL_RELIEF_THIN_DECK (8u << 8)

/* Its relief is drawn with its `wall`, not its own drawing: a tree's cell on
 * the mountain's slope (the crown is the tree model's; laid on the rock as
 * the cell's texture, a slab of leaves stood down Route 120's bank). */
#define VOXEL_RELIEF_FACE_TOP (16u << 8)
/* Its cut variant is a canopy cleared (a secondary tileset's crown of the
 * small tree south of it, over the rim of Route 120's plateau): drawn whole
 * on its relief, over a plate of `wall` - the ground the crown was drawn
 * over - a pixel under it; `ground` at `foot` is the ground run on under it,
 * as with no variant. The crown is the tree model's. */
#define VOXEL_RELIEF_CANOPY (128u << 8)
#define VOXEL_RELIEF_DECK_EDGE (4.0f / 16.0f)
int VoxelRelief_Cut(const VoxelMapInstance *inst, int x, int y, float *foot, int *ground,
                    int *wall, unsigned *sides);
/* Every cut record of a map's layout, in its own cells (x, y from the
 * layout's corner), with the ground and the wall it draws (-1: none): ids
 * the atlas must hold although a script may have changed the cell's own
 * metatile since the relief was read off the layout (Route 120's bridge,
 * its Kecleon gone). */
typedef void (*VoxelReliefCutVisit)(int x, int y, int ground, int wall, void *ctx);
void VoxelRelief_ForEachCut(const VoxelMapInstance *inst, VoxelReliefCutVisit visit, void *ctx);
unsigned VoxelRelief_CutCount(void);
bool VoxelRelief_CutVariant(unsigned i, unsigned *layout, unsigned *metatile,
                            const uint8_t **rows);
/*
 * A cut tile whose rock is a polygon of straight sides (the generator
 * straightens them: voxel_terraces.py straight_mask) is cut along its
 * 45-degree sides rather than by its texels, which leave a staircase where
 * the drawing has a straight diagonal. `count` sides a*X + b*Y <= c over the
 * tile's pixels (X, Y from 0 to 16), through the outer corners of the rock's
 * pixels beside them; none when the rock is no polygon (two pieces, a notch)
 * or its polygon has no 45-degree side. VoxelRelief_CutClear gives the
 * texels the tile's texture leaves clear: its background, less the halves of
 * background pixels those sides take in (drawn, or the cut would show them
 * as notches in the straight line).
 */
typedef struct
{
    int count;
    int8_t a[4], b[4];
    int16_t c[4];
} VoxelCutDiagonals;
void VoxelRelief_CutDiagonals(const uint8_t *rows, VoxelCutDiagonals *out);
void VoxelRelief_CutClear(const uint8_t *rows, uint16_t clear[16]);
/*
 * A background pixel VoxelRelief_CutClear keeps drawn shows only the part of
 * it inside the rock's polygon: it takes the colour of a rock pixel beside
 * it (west, east, north, south, then the corners), not the ground's (a fleck
 * of grass on the rock's edge). The index (y * 16 + x) of that pixel, or -1
 * when (x, y) is no such pixel or has no rock beside it.
 */
int VoxelRelief_CutFringe(const uint8_t *rows, const uint16_t clear[16], int x, int y);

#endif
