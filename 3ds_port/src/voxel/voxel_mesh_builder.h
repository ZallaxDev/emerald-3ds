/*
 * CPU geometry for the voxel overworld.
 *
 * The reference renderer emits its geometry with glBegin/glVertex/glEnd. The
 * PICA200 has no immediate mode and one draw call per quad would be absurd, so
 * the geometric formulas are kept and only the emission changes: every shape
 * writes triangles into a caller-owned vertex array, which ctr_voxel.c then
 * draws as one array per material.
 */
#ifndef CTR_VOXEL_MESH_BUILDER_H
#define CTR_VOXEL_MESH_BUILDER_H

#include <stdbool.h>
#include <stdint.h>

#include "voxel_world.h"

typedef struct
{
    float x, y, z;
    float u, v;
    float shade;
} VoxelVertex;

/*
 * A model drawn from its own mesh (VoxelBuildings_StaticModel) rather than
 * copied into the chunk: where the chunk places it, in the chunk's frame
 * (after the origin, the relief's lift and the map's base), and its box.
 */
typedef struct
{
    unsigned pageModel;
    bool lit;
    float x, y, z;          /* its top-left cell's corner, tiles */
    float w, d, top;        /* its extent: cells across, cells deep, tiles up */
} VoxelStaticDraw;

typedef struct
{
    VoxelVertex *vertices;
    unsigned capacity;
    unsigned count;
    unsigned dropped; /* triangles refused for lack of room; never silent */
    /*
     * Subtracted from every vertex, so a chunk's geometry is written in its
     * map's own coordinates and the map's position in the world becomes a
     * translation at draw time. Crossing a border moves every map on screen;
     * without this it would also invalidate every chunk of them, which is the
     * one moment there is least time to rebuild anything.
     */
    float originX, originZ;
    /*
     * Where each metatile id lives in the bound atlas. A script that places an
     * id the atlas was never built with shows up here as `uncovered`, which is
     * the renderer's cue to rebuild the atlas once - rather than sampling an
     * empty slot forever, or rebuilding it every frame just in case.
     */
    const struct VoxelAtlasMap *atlas;
    unsigned uncovered;
    /*
     * Terrain relief (voxel_relief.h): whatever is emitted while this is set
     * stands on a cell lifted by `lift` tiles, and is displaced as the cell
     * is, by (0, lift, shift): shift is the lift again but on a drawn map
     * (VoxelRelief_Depth). Zero for everything on level ground.
     */
    float lift, shift;
    /* The base of the map being built (VoxelRelief_Base): everything emitted
     * is raised by it and drawn as far south (VoxelBuilder_StretchZ). */
    float base;
    /* Its south seam's ease (VoxelRelief_Stretch): world z from stretchFrom
     * to stretchTo drawn longer, past it moved stretchAdd south. stretchAdd
     * zero: none. */
    float stretchFrom, stretchTo, stretchAdd;
    bool lighting; /* opt-in for outdoor chunks and border receivers */
    bool artShaded; /* relief: its drawing is its own; lit like any face */
    bool lightingRefine; /* ground subdivision; disabled for the fixed-size belt */
    /* A tree's crown card: a rounded volume, not the plane it is drawn on. */
    bool rounded;
    /* The relief's lattice: each vertex's shade already holds its face term,
     * worked out a tile across (see EmitRelief), not its quad's. */
    bool vertexFace;
    /* When not negative, the light every corner gets instead of its own
     * sample: one sample for a small object drawn as many faces. */
    float lightingConstant;
    /* Where static models are noted instead of emitted (NULL: emitted). */
    VoxelStaticDraw *statics;
    unsigned staticCapacity, staticCount;
} VoxelBuilder;

void VoxelBuilder_Init(VoxelBuilder *builder, VoxelVertex *storage, unsigned capacity);
void VoxelBuilder_SetAtlas(VoxelBuilder *builder, const struct VoxelAtlasMap *atlas);
/* World tile the emitted coordinates are measured from. Zero is world space. */
void VoxelBuilder_SetOrigin(VoxelBuilder *builder, int originX, int originZ);
/* The map being built's ease, if it has one (VoxelRelief_Stretch). */
void VoxelBuilder_SetStretch(VoxelBuilder *builder, const VoxelMapInstance *inst);
/* A world z (after the shift) where the builder's map draws it: on its
 * eased rows, and as far south as its base is high - a point stands as far
 * south as it is high in the world, (u, h, v + h), so two maps meeting at one
 * height meet in the same row whatever their bases. */
float VoxelBuilder_StretchZ(const VoxelBuilder *builder, float z);
void VoxelBuilder_Tri(VoxelBuilder *builder, const VoxelVertex *a,
                      const VoxelVertex *b, const VoxelVertex *c);
/* A B C / A C D, the winding the reference quads already use. */
void VoxelBuilder_Quad(VoxelBuilder *builder, const VoxelVertex *a, const VoxelVertex *b,
                       const VoxelVertex *c, const VoxelVertex *d);

/* Largest rectangle side the internal bookkeeping supports. */
#define VOXEL_WINDOW_MAX 80

/*
 * How far north of its own edge a chunk build reads, and so how far its
 * signature has to hash: what stands on a cell can depend on the rows above
 * it (a lamp's lantern on its post, a tree's crown on its trunk). Generous on
 * purpose; it is the margin the old column pass used.
 */
#define VOXEL_CHUNK_MARGIN_NORTH 8

/*
 * Opens a build over the world-tile rectangle [x0,x1) x [y0,y1): sizes the
 * classification cache to it. The rectangle is a chunk plus the margin its
 * columns and face culling read beyond its edges.
 */
void VoxelMesh_BeginWindow(int x0, int y0, int x1, int y1);

/*
 * Face primitives and window bookkeeping, shared with the other passes so they
 * emit through the same geometry as everything else. All coordinates are world
 * tiles; `inset` shrinks the face towards the tile centre.
 */
void VoxelMesh_Top(VoxelBuilder *b, float wx, float wz, float h, float inset,
                   float u0, float v0, float u1, float v1, float shade);
/* The cell's relief drawn with u0..v1 (a tree's trunk on a slope). */
void VoxelMesh_ReliefGround(VoxelBuilder *b, const VoxelMapInstance *inst, int x, int y,
                            float u0, float v0, float u1, float v1);
/* A tree's cell drawn as the rock under it: the ground pass's, not the tree
 * pass's. */
bool VoxelMesh_TreeOnRock(const VoxelMapInstance *inst, int x, int y);
void VoxelMesh_South(VoxelBuilder *b, float wx, float wz, float yBottom, float yTop,
                     float inset, float u0, float v0, float u1, float v1);
void VoxelMesh_North(VoxelBuilder *b, float wx, float wz, float yBottom, float yTop,
                     float inset, float u0, float v0, float u1, float v1);
void VoxelMesh_West(VoxelBuilder *b, float wx, float wz, float yBottom, float yTop,
                    float inset, float u0, float v0, float u1, float v1);
void VoxelMesh_East(VoxelBuilder *b, float wx, float wz, float yBottom, float yTop,
                    float inset, float u0, float v0, float u1, float v1);

/* Classification, cached for the duration of the open window. */
VoxelVisualShape VoxelMesh_Classify(int x, int y);
/* UVs of a tile's own metatile in the bound atlas; false if it has none. */
bool VoxelMesh_TileUV(VoxelBuilder *b, int x, int y,
                      float *u0, float *v0, float *u1, float *v1);
/* UVs of a metatile id in the bound atlas; false if it has none. */
bool VoxelMesh_MetatileUV(VoxelBuilder *b, int metatileId,
                          float *u0, float *v0, float *u1, float *v1);

/*
 * Emits one instance's ground over the world-tile rectangle [x0,x1) x
 * [y0,y1), clipped to the instance: every cell flat or on its relief, with the
 * signs and furniture that stand on it. Classification comes from
 * VoxelWorld_ClassifyTile. Trees and modelled buildings are passes of their
 * own.
 *
 * Nothing carries over between calls, so the rectangle may be a chunk and the
 * result is the same geometry the whole view would have produced.
 */
void VoxelMesh_EmitInstance(VoxelBuilder *builder, const VoxelMapInstance *inst,
                            int x0, int y0, int x1, int y1);

/*
 * The same build a row at a time, for a caller that spreads one chunk over
 * several frames: every ground row in turn gives exactly what
 * VoxelMesh_EmitInstance gives. The rows must already be clipped to the
 * instance.
 */
void VoxelMesh_EmitGroundRow(VoxelBuilder *builder, const VoxelMapInstance *inst,
                             int x0, int x1, int y);

/*
 * The draft of a ground cell (ctr_voxel.c, DraftChunk): the cell's own
 * drawing laid flat at its level, unlit, with nothing standing on it - no
 * relief, tree, model or shadow. A chunk of them costs a few hundred
 * microseconds where its full build costs 10-30 ms, so a square the view
 * needs and has not got yet can show the ground at once and the real
 * geometry replaces it when it is done.
 *
 * DraftSlot says where the cell's drawing is in the atlas (the packed slot,
 * page*512 + slot), or -1 for a cell that draws nothing; ids the atlas has
 * not met are counted in `*uncovered`. It reads no builder and nothing the
 * running build owns. DraftCell writes the cell's two triangles to `b`
 * (six vertices), which the caller points at a buffer of its own.
 */
int VoxelMesh_DraftSlot(const VoxelMapInstance *inst, const struct VoxelAtlasMap *atlas,
                        int x, int y, unsigned *uncovered);
void VoxelMesh_DraftCell(VoxelBuilder *b, const VoxelMapInstance *inst, int x, int y, int slot);

/* Stands the current map border block up over every tile of [x0,x1) x [y0,y1)
 * that belongs to no map, so the view does not end in the clear colour.
 * Instance 0 only, and nothing indoors, where the filler is the clear colour. */
void VoxelMesh_EmitBorder(VoxelBuilder *builder, int x0, int y0, int x1, int y1);

/* The level a cell of the belt of land stands at, in tiles over instance 0's
 * base, and its depth: its map edge's (false, both 0: at the base). */
bool VoxelMesh_BorderLevel(int x, int y, float *level, float *shift);

/* settings.txt voxel_border_edge: the belt at its edge's level (default) or
 * at the base, as it was. */
void VoxelMesh_SetBorderFollowsEdge(bool on);

#endif
