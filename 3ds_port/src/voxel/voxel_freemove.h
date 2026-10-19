/*
 * Free walking for the free-roam camera (voxel_freecam.h): the player moves
 * in any direction at any speed, and slides along walls, over the game's own
 * grid.
 *
 * FreeMove.lua walks the player continuously. Here the decompilation keeps
 * what makes Emerald Emerald - collision, ledges, doors, warps, encounters,
 * trainers' sight and bumps - and this module walks P in any direction over
 * the walkable tiles as one area, not one tile at a time. The player has a
 * position P on the tile G the game has it on, an offset (ox, oz) from that
 * tile's centre, in tiles: the stick moves P freely, walls stop it and it
 * slides along them, and when P crosses into another walkable tile the game
 * is told so its tile can follow (without a grid walk). Everything that is
 * drawn (the player's card, the camera) stands at P.
 *
 * Pure state and math, no game types: voxel_entities.c reads the offset,
 * 3ds_game_bridge.c steps it once a frame with the game's collision, and the
 * host test (tests/voxel_freemove_test.c) drives it with a model of the game.
 */
#ifndef CTR_VOXEL_FREEMOVE_H
#define CTR_VOXEL_FREEMOVE_H

#include <stdbool.h>

/* The grid's four directions, as the decompilation's DIR_*. */
enum { VOXEL_FM_NONE = 0, VOXEL_FM_SOUTH = 1, VOXEL_FM_NORTH = 2, VOXEL_FM_WEST = 3, VOXEL_FM_EAST = 4 };

/* A direction counts as aimed at a wall when the stick's component along it
 * is this much of the stick: only then is the game asked for the step, so
 * that it bumps, pushes a boulder, takes an arrow warp - a diagonal slides. */
#define VOXEL_FM_ALIGNED 0.85f
/* Frames the picture takes to give way to the game's own, and back. */
#define VOXEL_FM_BLEND_FRAMES 8

typedef struct
{
    bool have;
    /* The tile the game has the player on, as last seen (map coordinates),
     * and the player's offset from its centre. */
    int gx, gz;
    float ox, oz;
    /* Where the player faces as a bearing (yaw: 0 south, pi/2 east), tiles
     * walked, and how far this frame: the card turns and walks by them. */
    float heading, travel, speed;
    /* 0..1: how much of P the picture shows (0 is the game's own). */
    float blend;
    /* Whether the player's own input drives this frame. */
    bool driving;
} VoxelFreeMove;

/*
 * Whether the game would refuse a step from the tile it is on in direction
 * `dir` (a VOXEL_FM_*): a wall, a ledge's wrong side, someone standing there.
 * Nonzero for refused. Asked of the game's own collision (bridge).
 */
typedef int (*VoxelFreeMoveBlocked)(void *context, int dir);

void VoxelFreeMove_Reset(VoxelFreeMove *fm);

/* The game's tile is (gx, gz) now (map coordinates): a step of one tile
 * moves the origin of the offset with it, so P stays where it was; anything
 * else (a warp, a ledge, a new map) puts the player back on the centre. Safe
 * to call as often as wanted; the renderer and the input both do. */
void VoxelFreeMove_Sync(VoxelFreeMove *fm, int gx, int gz);

/*
 * One frame of driving. (vx, vz) is the walk in tiles per frame, east and
 * south positive. Returns the direction the game's tile should follow, or
 * a wall to bump (VOXEL_FM_NONE for none). `blocked` may be NULL.
 */
int VoxelFreeMove_Step(VoxelFreeMove *fm, int gx, int gz, float vx, float vz,
                       VoxelFreeMoveBlocked blocked, void *context);

/* One frame with nobody driving (a script, a menu): the picture goes back to
 * the game's own. */
void VoxelFreeMove_Idle(VoxelFreeMove *fm, int gx, int gz);

/* Whether the player is walking: the card walks (and the head moves) with it. */
bool VoxelFreeMove_Moving(const VoxelFreeMove *fm);

/* The one the game and the renderer share. */
VoxelFreeMove *VoxelFreeMove_Get(void);

#endif
