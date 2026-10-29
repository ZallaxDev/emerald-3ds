/*
 * Buildings modelled from their own drawing.
 *
 * voxel/buildings.bin is written by scripts/gen_voxel_buildings.py,
 * which reads every building off its art (voxel_building.py explains how) and
 * refuses to write a model that does not reproduce that art pixel for pixel
 * in the GBA's projection. The console does no modelling: it copies each
 * placed model's triangles into the chunk that holds its top-left cell, draws
 * the plain ground under every cell the model covers, and keeps the region
 * table's generic extrusion away from those cells.
 *
 * The models carry their own texture - the building's art with the ground
 * made transparent, paged by map - so they are drawn in their own
 * alpha-tested pass.
 */
#ifndef CTR_VOXEL_BUILDING_H
#define CTR_VOXEL_BUILDING_H

#include <stdbool.h>
#include <stdint.h>

#include "voxel_mesh_builder.h"
#include "voxel_world.h"

/* Reads the file. False, with everything left empty, if it is absent. */
bool VoxelBuildings_Init(void);
void VoxelBuildings_Shutdown(void);

/*
 * The current map (VoxelMapInstance.live) as the live grid draws it, not only
 * as its layout did: on a map a script swapped another layout in, where the
 * live grid shows more of the header layout's own models than of the
 * swapped-in one's (the Mirage Tower painted back on Route 111 for its fall)
 * the map's buildings are the header layout's. `drawing` gives a layout's
 * own metatiles (VoxelWorld_LayoutDrawing); NULL (host tools) keeps the
 * instance's own layout. WorldChanged: the live grid has changed, so the
 * answer is worked out again when next asked.
 */
void VoxelBuildings_SetDrawings(const uint16_t *(*drawing)(int layoutId, int *width, int *height));
void VoxelBuildings_WorldChanged(void);
/* The layout whose placements, page and animated runs `inst` draws. */
int VoxelBuildings_LayoutOf(const VoxelMapInstance *inst);
/* 0 while the current map stands as it was entered (and for other maps),
 * else a value of the layout it took: part of the current map's chunks'
 * signatures, so they are built again when it changes and only then. */
uint32_t VoxelBuildings_LiveSignature(const VoxelMapInstance *inst);

/*
 * Textures are paged by map: every layout's models are drawn from one page,
 * which the renderer loads when a map on screen needs it. -1 when the layout
 * places nothing.
 */
int VoxelBuildings_PageOf(const VoxelMapInstance *inst);
/* A page's texels, `width` x `height` (whole tiles of 8 rows), and the height
 * of the texture that samples them: `height` rounded up to a power of two,
 * its rows past `height` never read, so never allocated. */
bool VoxelBuildings_PageSize(unsigned page, unsigned *width, unsigned *height,
                             unsigned *texHeight);
/* Reads `count` of a page's RGBA5551 texels (PICA order) from texel `first`
 * into `dest`. */
bool VoxelBuildings_ReadPage(unsigned page, unsigned first, unsigned count, uint16_t *dest);
bool VoxelBuildings_PageHasGlow(unsigned page);
bool VoxelBuildings_ReadGlow(unsigned page, unsigned first, unsigned count, uint8_t *dest);

/*
 * Page texels drawn from tiles the game animates (Rustboro's fountain: its
 * jet). Each run is whole 8x8 blocks, contiguous in its page: `bytes` from
 * byte `offset` of page `page`'s texels. Its live texels are the drawing of
 * layout `layout` (a layout id), so they follow the tiles only while that
 * map's tilesets are the ones in BG VRAM. ComposeAnimRun writes the run's
 * blocks to `dest` - `bytes` of RGBA5551 in the page's order, graded -
 * from `liveTiles`, the 1024 tiles of BG VRAM as the game holds them.
 */
unsigned VoxelBuildings_AnimRunCount(void);
bool VoxelBuildings_AnimRun(unsigned run, unsigned *page, int *layout, unsigned *offset,
                            unsigned *bytes);
/* Whether a tile set in `dirty` (bit t: BG tile t) is one the run reads. */
bool VoxelBuildings_AnimRunDirty(unsigned run, const uint8_t dirty[128]);
void VoxelBuildings_ComposeAnimRun(unsigned run, const uint8_t *liveTiles, uint16_t *dest);

/* Tallest cell of any model, in tiles: a bound for shadow rays. */
float VoxelBuildings_MaxTop(void);
/* The tallest of the map's own models, in tiles over its base (0 for none). */
float VoxelBuildings_LayoutTop(const VoxelMapInstance *inst);

/*
 * World cell (x, y) of `inst`, when a model covers it: returns true and gives
 * the ground metatile to draw flat there and the model's tallest point over
 * the cell, in tiles (the lighting pass casts the shadow from it). Whatever
 * the map paints of its own in those cells arrives as the placement's ground
 * patches, drawn with the model - or, for a model found by its tiles (a rock
 * in the sea), the ground is the cell's own metatile, drawn from the atlas
 * so that its animation goes on: less the quarters the model stands for, a
 * ground variant (an atlas id past the real ones, voxel_atlas.h).
 */
bool VoxelBuildings_CellAt(const VoxelMapInstance *inst, int x, int y,
                           int *groundMetatile, float *top);
/*
 * A cell of a model it stands over nothing of, whose drawing carries the
 * model on its upper layer (a Sootopolis house's roof over the face of the
 * terrace behind it: scripts/gen_voxel_buildings.py bare_quarters): the
 * ground variant of the cell's own metatile less those quarters. The
 * relief stays there, drawn with it.
 */
bool VoxelBuildings_BareVariant(const VoxelMapInstance *inst, int x, int y, int *metatile);

/*
 * A bridge's deck (voxel_bridge.py): whoever stands at world (x, z) of
 * `inst` stands on it, `lift` tiles over the ground, when the cell is under
 * a deck and the walker is up there - a cell walked both over and under
 * (elevation 15) carries only a walker at the deck's own elevation; a ramp's
 * or a deck-only cell, anyone. False otherwise (the ground's own height).
 * The lift runs from the cell's north edge to its south edge (a ramp).
 */
bool VoxelBuildings_DeckAt(const VoxelMapInstance *inst, float worldX, float worldZ,
                           unsigned elevation, float *lift);

/*
 * The cell's footprint when the model covers only part of it (a railing's
 * line): 16 rows, bit x of row z set where its solid stands over that pixel.
 * NULL for a cell the model covers as a box, or does not cover.
 */
const uint16_t *VoxelBuildings_Footprint(const VoxelMapInstance *inst, int x, int y);

/*
 * Ground variant `i`: a layout, a metatile of the tileset that layout draws
 * it from, and the quarters of its upper layer (bit 2 * row + column) left
 * out. False past the last.
 */
bool VoxelBuildings_Variant(unsigned i, unsigned *layout, unsigned *metatile, unsigned *quarters);
/*
 * How VoxelBuildings_CellAt tells whether ground variant (layout, metatile)
 * is drawn from `inst`'s own tilesets, as the atlas composes it
 * (voxel_world.c sets it; NULL matches a variant by metatile alone).
 */
void VoxelBuildings_SetVariantFits(bool (*fits)(const VoxelMapInstance *inst, unsigned layout,
                                                unsigned metatile));

/* The map changed (setmetatile): what was worked out of the placements that
 * stand only while a cell holds a metatile is worked out again. */
void VoxelBuildings_MapChanged(void);

/*
 * The face of a model a walker in world cell (x, y) of `inst` stands before:
 * the southernmost upright face, looking south, of any model that reaches
 * the cell, crossing world x0..x1 at `height` tiles up. Its z in world tiles;
 * false when there is none. A piece of furniture's front, a building's wall.
 */
bool VoxelBuildings_FrontAt(const VoxelMapInstance *inst, int x, int y,
                            float x0, float x1, float height, float *z);

/* The wall a door in world cell (x, y) is drawn on: the face across the
 * middle of the cell, half a tile up. */
/*
 * A static model's triangles (positions in tiles from its top-left cell, uv
 * pixels of its drawing, drawn shades) and where its drawing lies on its
 * page: uv on the page are (ox + u) * su, 1 - (oy + v) * sv.
 */
bool VoxelBuildings_StaticModel(unsigned pageModel, const VoxelVertex **vertices,
                                unsigned *count, float *ox, float *oy, float *su, float *sv);

bool VoxelBuildings_DoorWall(const VoxelMapInstance *inst, int x, int y, float *z);

/* Appends every model whose top-left cell lies in [x0,x1) x [y0,y1), with
 * its placement's ground patches. */
void VoxelBuildings_EmitInstance(VoxelBuilder *builder, const VoxelMapInstance *inst,
                                 int x0, int y0, int x1, int y1);

/*
 * The same, at most `triangles` triangles at a time: a city's large models
 * are thousands of lit triangles, too many for one slice of a frame. Start
 * from a zeroed cursor and call again until it returns true; the triangles
 * come out in exactly EmitInstance's order.
 */
typedef struct
{
    unsigned placement, part;
    uint32_t vertex;
} VoxelBuildingCursor;

bool VoxelBuildings_EmitSome(VoxelBuilder *builder, const VoxelMapInstance *inst,
                             int x0, int y0, int x1, int y1, VoxelBuildingCursor *cursor,
                             unsigned triangles);

#endif
