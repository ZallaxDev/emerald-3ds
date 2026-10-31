/*
 * The Fly cinematic in the voxel overworld: pure state, no SDK. 3ds_video.c
 * steps it once per game frame beside the swoop, hands the camera's share to
 * ctr_voxel.c (CtrVoxel_SetFly) and lays the cloud cover over the finished
 * picture; the game's Fly (field_effect.c) asks it, through weak hooks in
 * 3ds_game_full.c, whether the bird still hovers and whether the landing
 * must wait.
 *
 * One swoop out, one swoop in. As the bird dives for the player the camera
 * draws in on them (DIVE); with the player aboard it leaves in a single
 * motion - out, up over the world with the cloud wisps streaming past, on up
 * into the deck, which closes over the picture (CLIMB). Inside the cloud
 * (CLOUD) the game flies off, fades, warps and builds the new map's world
 * unseen: the cloud is the load screen. Over the new map the camera comes
 * down out of the deck and settles onto the player's pose in one motion
 * (DESCEND), the camera coming in from behind the player along the way, as
 * the Pokemon flies in beside it and lands as it settles.
 */
#ifndef CTR_VOXEL_FLY_H
#define CTR_VOXEL_FLY_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    VOXEL_FLY_IDLE,
    VOXEL_FLY_DIVE,    /* the bird diving for the player, the camera drawing in */
    VOXEL_FLY_CLIMB,   /* the player aboard: out, up and into the deck */
    VOXEL_FLY_CLOUD,   /* inside: the game warps and the new world is built */
    VOXEL_FLY_DESCEND, /* the new map: down out of the deck onto the player's pose */
} VoxelFlyPhase;

typedef struct
{
    VoxelFlyPhase phase;
    unsigned frame;    /* frames into the phase */
    float dirX, dirZ;  /* unit direction of travel, world tiles (+X east, +Z south) */
    bool gone;         /* CLOUD: the old world has been seen to go (the warp) */
} VoxelFly;

/* What this frame draws, 0..1 unless said. */
typedef struct
{
    float lift;     /* how far the camera is from the player's pose to the high
                     * one; below 0 it is drawn in closer than the pose (the dive) */
    float rise;     /* ... and beyond the high pose, straight up into the deck */
    float travel;   /* where the view is along the way, in shares of the full
                     * travel: ahead of the player towards the destination
                     * (positive, the old map) or behind them, where they came
                     * from (negative, the new map); 0 on the player */
    float whiteout; /* the cloud cover over the whole picture */
    float wisps;    /* the strength of the wisp sheets in the scene */
    float dirX, dirZ;
    bool oldMap;    /* the camera leads towards the destination (old map) or
                     * trails behind the player (new map) */
    bool noFade;    /* the world ignores the palette fade (the new map's black
                     * fade-in is under the cloud, the world must show) */
} VoxelFlyFrame;

/* Starts the cinematic: the bird dives for the player. `dirX, dirZ` the way
 * to the destination, any length (zero: north). */
void VoxelFly_Begin(VoxelFly *f, float dirX, float dirZ);
void VoxelFly_Cancel(VoxelFly *f);
bool VoxelFly_Active(const VoxelFly *f);
/* The bird, with the player aboard, keeps hovering: until the cloud has
 * closed over the picture. */
bool VoxelFly_Hovers(const VoxelFly *f);
/* The landing waits: while inside the cloud (the new map's world unseen). */
bool VoxelFly_Busy(const VoxelFly *f);
/* The camera is coming down onto the new map: `share` 0..1 of the way down,
 * for the Pokemon flying in beside it to land as it settles. */
bool VoxelFly_Landing(const VoxelFly *f, float *share);

void VoxelFly_Frame(const VoxelFly *f, VoxelFlyFrame *out);
/* The frame has been drawn: on to the next. `fieldShown`: the voxel world
 * was drawn this frame; `fieldGone`: the old field has gone (the map was
 * loaded again, or there was no field at all), which inside the cloud marks
 * the warp. */
void VoxelFly_Step(VoxelFly *f, bool fieldShown, bool fieldGone);

#endif
