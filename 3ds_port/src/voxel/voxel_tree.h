/* General tileset trees: ground trunk and a north-leaning cutout crown. */
#ifndef CTR_VOXEL_TREE_H
#define CTR_VOXEL_TREE_H

#include <stdint.h>
#include <stdbool.h>
#include "voxel_mesh_builder.h"

#define VOXEL_TREE_TEXTURE_DIM 128u
#define VOXEL_TREE_TEXTURE_PATH "romfs:/voxel/trees.rgba5551"

/* A small tree is one cell: its trunk, with the whole crown standing on it. */
#define VOXEL_TREE_SMALL 4

/* General IDs, plus the secondary tileset's own trees (Dewford's small one):
 * -1 for other art, the 2x2 quadrant (row*2+col) of a large tree, or
 * VOXEL_TREE_SMALL. secondaryTileset is the map's, as in VoxelMapInstance. */
int VoxelTree_Part(const void *secondaryTileset, int metatileId);
/* Remove the old canopy from the cell above a tree, leaving its ground. */
int VoxelTree_GroundMetatile(const void *secondaryTileset, int metatileId);

/*
 * A tree is built in two passes, so that a chunk's crowns are a range of
 * their own: the ground under the trees and the small trees' bases (TOPS),
 * then the crown cards (CROWNS). On the orbit the crowns are drawn as built,
 * leant towards the fixed view; in the free-roam rig (voxel_freecam.h) they
 * are drawn again each frame from their records, turned about their trunks to
 * face the eye (ctr_voxel.c), and the built ones are left out.
 */
typedef enum { VOXEL_TREE_TOPS, VOXEL_TREE_CROWNS } VoxelTreePass;

/*
 * What a crown card is, to draw it again turned: a card standing over the
 * pivot (a vertical axis, in the chunk's frame) with its corners as offsets
 * from it, at the heights the build gave it, in the texels it shows, and in
 * the light the build gave its corners. 32 bytes: a chunk of woods has a
 * few dozen. The offsets and heights are 1/512 tile, the vertex format's.
 */
typedef struct
{
    float px, pz;
    int16_t xl, xr;     /* left and right edges, from px */
    int16_t zt, zb;     /* top and bottom edges, from pz (the lean: both behind) */
    int16_t yt, yb;     /* heights, in the chunk's frame */
    uint8_t u0, v0, u1, v1; /* tree atlas texels: left, top, right, bottom */
    int16_t shade[4];   /* top left, top right, bottom right, bottom left; 1/16384 */
} VoxelCrown;

typedef struct VoxelCrownList
{
    VoxelCrown *items;
    unsigned capacity, count;
    bool overflow;      /* a card had no room: the chunk keeps its built crowns */
} VoxelCrownList;

/*
 * The two triangles of a recorded crown card, turned about its pivot by a
 * yaw whose sine and cosine are given (the convention of the billboards:
 * yaw 0 is the card as built, facing a camera to the south, and a card at
 * yaw `a` faces a camera at (sin a, cos a) from it), standing at offset
 * (ox, oz) tiles from the frame it was recorded in.
 */
void VoxelTree_CrownVertices(const VoxelCrown *crown, float sinYaw, float cosYaw,
                             float ox, float oz, VoxelVertex out[6]);

/* Appended after the ordinary terrain; these vertices use the tree texture.
 * In the crowns pass the builder's `crowns`, if set, also takes each card's record. */
void VoxelTree_EmitInstance(VoxelBuilder *builder, const VoxelMapInstance *inst,
                            int x0, int y0, int x1, int y1, VoxelTreePass pass);
void VoxelTree_EmitBorder(VoxelBuilder *builder, int x0, int y0, int x1, int y1,
                          VoxelTreePass pass);

#endif
