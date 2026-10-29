/*
 * The sprite atlas's slots (voxel_entities.c), the part of it that needs
 * nothing of the game or the SDK, so a host test can check it on its own.
 *
 * Fitting a picture into a slot: a picture up to a slot wide is copied as it
 * is; one up to twice as wide (the S.S. Tidal, 96x40, the submarine's shadow,
 * 88x32) keeps every second column, and the card that shows it keeps the
 * picture's full width in the world.
 *
 * Handing the slots no object event holds to the other sprites on the map: the
 * ones the camera can see, in order of priority (a visible wild Pokemon before
 * a field effect, a field effect before the grass a step rustles), each kept
 * in the slot it had while it is still wanted.
 */
#ifndef CTR_VOXEL_SPRITE_SLOTS_H
#define CTR_VOXEL_SPRITE_SLOTS_H

#include <stdbool.h>
#include <stdint.h>

/* Texels across a slot that a picture `width` wide takes: itself up to the
 * slot's width, half of it (every second column) up to twice that, else 0
 * (it does not fit). */
unsigned VoxelSlots_AtlasWidth(unsigned width, unsigned slotDim);

/* Whether a picture fits a slot: at most twice the slot's width, at most its
 * height, whole tiles, and its source tiles in `maxBytes`. */
bool VoxelSlots_Fits(unsigned width, unsigned height, bool color256,
                     unsigned slotDim, unsigned maxBytes);

/* The picture's column that atlas column `atlasX` of `atlasWidth` shows, the
 * picture mirrored or not. */
unsigned VoxelSlots_SourceColumn(unsigned atlasX, unsigned atlasWidth, unsigned width, bool flipX);

/* The colour index at (x, y) of a picture `width` wide held as whole 8x8
 * tiles in rows (1D mapping), 4bpp or 8bpp. */
unsigned VoxelSlots_TilePixel(const uint8_t *tiles, unsigned width, unsigned x, unsigned y,
                              bool color256);

/*
 * A door's frame as field_door.c lays it (DrawCurrentDoorAnimFrame): a
 * column of two metatiles a cell across, the top one's four 4bpp tiles (TL TR
 * BL BR) then the bottom one's, 8 tiles a column and the columns one after
 * the other; the column's tile i in palette palettes[i]. The colour index at
 * (x, y) of its picture, 16 pixels a column across and 32 down, and in
 * `*palette` that pixel's palette.
 */
unsigned VoxelSlots_DoorPixel(const uint8_t *tiles, const uint8_t *palettes,
                              unsigned x, unsigned y, unsigned *palette);

/*
 * Where row `v` of a door's drawing goes against its wall at depth `wall`
 * (world tiles, south +z), by the GBA's 45-degree rule that put every model
 * where its drawing is: a row north of the wall stands up it, `wall - v`
 * high; one south of it lies on the ground in front, at its own depth.
 */
void VoxelSlots_DoorRow(float v, float wall, float *height, float *depth);

/* Lower first. */
enum
{
    VOXEL_SLOT_PRIORITY_WILD = 0,      /* a visible wild Pokemon */
    VOXEL_SLOT_PRIORITY_EFFECT = 1,    /* any other field effect */
    VOXEL_SLOT_PRIORITY_DECOR = 2      /* grass rustling on its tile */
};

typedef struct
{
    uint8_t id;          /* the sprite, < 255 */
    uint8_t priority;
    bool inRange;        /* the camera can see it: only then does it take a slot */
    int slot;            /* out: its slot, or -1 */
} VoxelSlotRequest;

/*
 * Gives `requests` (in any order) the slots of `slots` that `claimed` leaves:
 * as many as there are, in priority order, ties by their order in the array.
 * `owner[s]` is the id + 1 of the sprite slot s showed (0: none); a request
 * still wanted keeps its slot, one no longer wanted lets it go, and a new one
 * takes a free slot, from `firstShared` on before one under it. Every slot
 * whose owner changes is set in `reset` (its picture must be decoded again);
 * every slot given out is set in `claimed`. Returns how many in-range
 * requests were left without one.
 */
unsigned VoxelSlots_Assign(VoxelSlotRequest *requests, unsigned count, uint8_t *owner,
                           bool *claimed, bool *reset, unsigned slots, unsigned firstShared);

/*
 * How far a card whose feet stand at depth `cz` comes towards the camera to
 * be `clear` in front of a model's face at depth `front` (world tiles, south
 * is +z): a walker's card before furniture (voxel_entities.c, ModelPush).
 * False when nothing is needed - the face is
 * at least `clear` behind the feet - or when the face is more than `maxFace`
 * past them: that is no longer the furniture the walker stands at but a wall
 * or a model further on, and the card is never carried across it. The face
 * is bounded, not the push: a face right at the bound (a chair's front on
 * its cell's south edge, half a tile past a seated walker's feet) still gets
 * its clearance, so the push is at most maxFace + clear.
 */
bool VoxelSlots_ModelFacePush(float front, float cz, float clear, float maxFace, float *push);

#endif
