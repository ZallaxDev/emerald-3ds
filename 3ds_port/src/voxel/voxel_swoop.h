/*
 * The swoop between the voxel overworld and the 2D field: pure state, no
 * SDK. 3ds_video.c steps it once per top-screen frame, chooses the renderer
 * by it and lays its flash over the finished picture; ctr_voxel.c takes the
 * camera's share (VoxelCamera_Swoop).
 *
 * Voxel to 2D: the camera tilts up and dollies out to the 2D field's framing
 * (OUT), a flash of light goes up as it lands, and the 2D field is under the
 * flash as it dies away (FLASH_OUT). 2D to voxel is the reverse: the flash
 * (FLASH_IN), the world rises out of the map (RISE), and the camera settles
 * from the top-down pose to the player's (TILT_IN). A pop-up book.
 */
#ifndef CTR_VOXEL_SWOOP_H
#define CTR_VOXEL_SWOOP_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    VOXEL_SWOOP_IDLE,
    VOXEL_SWOOP_TILT_OUT,  /* the voxel world, camera tilting up to the 2D framing */
    VOXEL_SWOOP_FLATTEN,   /* ... the 2D field fading in over it (and the squash, if on) */
    VOXEL_SWOOP_FLASH_OUT, /* the 2D field under the flash */
    VOXEL_SWOOP_FLASH_IN,  /* the voxel world, flat and top-down, under the flash */
    VOXEL_SWOOP_RISE,      /* ... the 2D field fading out off it (and the rise, if on) */
    VOXEL_SWOOP_TILT_IN,   /* ... camera tilting down to the player's pose */
} VoxelSwoopPhase;

typedef struct
{
    VoxelSwoopPhase phase;
    unsigned frame; /* frames into the phase */
} VoxelSwoop;

/* What this frame draws, all 0..1. */
typedef struct
{
    float camera;   /* how far the camera is towards the top-down pose */
    float height;   /* the world's height, 1 as built, 0 flat on the map */
    float fade2d;   /* the 2D field laid over the voxel world, 0 none, 1 whole */
    float blur;     /* how far both pictures are softened through the crossfade */
    float flash;    /* the flat flash over everything */
    float bloom;    /* the radial bloom around the middle, over the flash */
    float whiteout; /* the voxel world's own light, towards white */
    bool cards;     /* the world draws its sprite cards; else the game's own
                     * sprites are laid over it where the 2D field puts them */
    bool shadows;   /* the cards' cast shadows (gone before the squash) */
    uint32_t colour; /* the flash's colour, 0xAABBGGRR as C2D_Color32 packs it */
} VoxelSwoopFrame;

/* Starts a swoop. `toVoxel`: the 2D field is leaving for the voxel world.
 * `canSwoop` (voxel to 2D only): the voxel world can still be drawn for the
 * camera's rise; without it (the map is gone) only the flash plays. */
void VoxelSwoop_Begin(VoxelSwoop *s, bool toVoxel, bool canSwoop);
/* Drops whatever plays: the frame is not a field frame. */
void VoxelSwoop_Cancel(VoxelSwoop *s);
/* The voxel world has gone from under a rising camera: straight to the flash. */
void VoxelSwoop_LoseVoxel(VoxelSwoop *s);
/* Whether the voxel world is the one to draw this frame, whatever the option
 * says: true through the camera's rise after the option went off. */
bool VoxelSwoop_ShowsVoxel(const VoxelSwoop *s);
bool VoxelSwoop_Active(const VoxelSwoop *s);
void VoxelSwoop_Frame(const VoxelSwoop *s, VoxelSwoopFrame *out);
/* The frame has been drawn: on to the next. */
void VoxelSwoop_Step(VoxelSwoop *s);

#endif
