/* General tileset trees: ground trunk and a north-leaning cutout crown. */
#ifndef CTR_VOXEL_TREE_H
#define CTR_VOXEL_TREE_H

#include "voxel_mesh_builder.h"

#define VOXEL_TREE_TEXTURE_DIM 128u
#define VOXEL_TREE_TEXTURE_PATH "romfs:/voxel/trees.rgba5551"

/* A small tree is one cell: its trunk, with the whole crown standing on it. */
#define VOXEL_TREE_SMALL 4
/* Long grass (Route 119, 120): its own floor and a tuft standing on it, a
 * card leant back as a small tree's crown is. */
#define VOXEL_TREE_GRASS 5

/* General IDs, plus the secondary tileset's own trees (Dewford's small one):
 * -1 for other art, the 2x2 quadrant (row*2+col) of a large tree, or
 * VOXEL_TREE_SMALL or VOXEL_TREE_GRASS. secondaryTileset is the map's, as in
 * VoxelMapInstance. */
int VoxelTree_Part(const void *secondaryTileset, int metatileId);
/* The same at a world cell: this map's, the next map's or the border's. */
int VoxelTree_PartAt(int x, int y);
/* Remove the old canopy from the cell above a tree, leaving its ground. */
int VoxelTree_GroundMetatile(const void *secondaryTileset, int metatileId);
/* Does the tree pass lay a tree part's ground? Not for a trunk in water
 * (Mossdeep's shallows): the ground pass lays the water, animated, as
 * VoxelTree_GroundMetatile names it, and the tree its roots over it. */
bool VoxelTree_LaysGround(const void *secondaryTileset, int metatileId);

/*
 * Tall grass (the General's 00D, and Lavaridge's on Jagged Pass's soil): the
 * tile's own drawing stood up. The ground pass lays `*floor`, the ground the
 * grass grows on, and over it the tile's tufts on one low card a cell (cut
 * in two, each half stood up apart, the drawing's tufts - which run from
 * one half into the other - showed the cut from a battle's low camera),
 * drawn with the tile less its ground (voxel_atlas.h, VOXEL_TUFT_FIRST + the
 * index returned). -1 for any other metatile. The metatile is the one the
 * ground pass draws (VoxelTree_GroundMetatile's, under a crown's top).
 */
#define VOXEL_TUFT_COUNT 2
#define VOXEL_TUFT_ROWS 1
/* the card's height at its north edge, tiles: 6 px */
#define VOXEL_TUFT_HEIGHT 0.375f
int VoxelTree_Tuft(const void *secondaryTileset, int metatileId, int *floor);
/* Tuft `i`: its metatile, its floor, and the secondary tileset it is of
 * (NULL: the General's own, on every map of it). */
bool VoxelTree_TuftSource(unsigned i, const void **secondaryTileset, unsigned *metatile,
                          unsigned *floor);
/* The General's tall grass and the grass it grows on: what every tuft's
 * outline is read from (the atlas, TuftClear). */
#define VOXEL_TUFT_SHAPE 0x00D
#define VOXEL_TUFT_SHAPE_FLOOR 0x001

/* Appended after the ordinary terrain; these vertices use the tree texture. */
void VoxelTree_EmitInstance(VoxelBuilder *builder, const VoxelMapInstance *inst,
                            int x0, int y0, int x1, int y1);
void VoxelTree_EmitBorder(VoxelBuilder *builder, int x0, int y0, int x1, int y1);

/*
 * While a list is given, the crown cards are noted instead of emitted (long
 * grass is no crown): VoxelTree_EmitNotedCards emits them all at the end, in
 * one run after the trees' grounds, which the shadow map shades and the
 * crowns not (ctr_voxel.c). Each as it will be written (in the builder's
 * frame, lifted and drawn south as the card is): corners top left, top
 * right, bottom right, bottom left, and the foot of the tree it is part of
 * (all its cards share it) - HIGH's shadow map draws them turned to the sun
 * round it (TreeShadowCards). Past `capacity` a card is emitted at once.
 * NULL stops noting (the noted ones stay readable).
 */
typedef struct
{
    VoxelVertex corner[4];
    VoxelVertex foot;
    /* as emitted: the corners given, the builder's relief then */
    VoxelVertex raw[4];
    float lift, shift, base;
} VoxelTreeCard;
void VoxelTree_NoteCards(VoxelTreeCard *cards, unsigned capacity);
unsigned VoxelTree_CardsNoted(void);
const VoxelTreeCard *VoxelTree_NotedCards(void);
void VoxelTree_EmitNotedCards(VoxelBuilder *builder);

#endif
