#ifndef CTR_FREECAM_H
#define CTR_FREECAM_H

/*
 * The free-roam camera (1ST / 3RD of OPTIONS' CAMERA) as the input layer
 * sees it. SDK-free: 3ds_input.c, 3ds_game_bridge.c and 3ds_video.c include
 * it; the implementation is src/voxel/voxel_camera.c over voxel_freecam.c.
 *
 * The camera steers the walk. With it engaged, the Circle Pad and the D-pad
 * mean "forward, back, left, right of where I look", turned by the camera's
 * yaw into one of the grid's four directions, and a player left standing
 * turns to face where the head looks. See voxel_freecam.h.
 */
#include <stdbool.h>

/* OPTIONS' CAMERA: 0 orbit (the default view), 1 first person, 2 third. */
enum { CTR_CAMERA_ORBIT = 0, CTR_CAMERA_FIRST = 1, CTR_CAMERA_THIRD = 2 };

/*
 * 1ST or 3RD is selected AND the voxel world is what the last frames drew
 * (not the 2D field while the world warms up, not a battle): the only time
 * the buttons mean anything different.
 */
bool CtrFreeCam_Engaged(void);
/* 1ST or 3RD is selected in OPTIONS, whether or not the world is drawn. */
bool CtrFreeCam_Selected(void);

/* OPTIONS' MOVE is FREE: with the camera engaged, the player walks in any
 * direction over walkable tiles (voxel_freemove.h) instead of by the grid's four. */
bool CtrFreeCam_FreeWalk(void);

/* The free walk entered another tile this frame: warps, steps and wild
 * encounters should run as they do at the end of a grid step. */
bool CtrFreeMove_TookStep(void);

/* The camera's bearing, radians: 0 looks south, pi/2 east. */
float CtrFreeCam_Yaw(void);

/* Whether the player's own input drives the camera this frame: the field is
 * on top and nothing owns the buttons (a script, a menu, a battle). */
void CtrFreeCam_SetDriving(bool driving);

/* The look input: the C-Stick's axes (-1..1, +y up) and the shoulder
 * buttons' turn (-1 left .. 1 right). Held until the next call. */
void CtrFreeCam_SetLook(float stickX, float stickY, float turn);

/*
 * The grid direction (the decompilation's DIR_*: 0 none, 1 south, 2 north,
 * 3 west, 4 east) the stick asks for, given `right` and `forward` in the
 * units of the Circle Pad (the D-pad is 128) and the direction asked the
 * frame before. DIR_NONE with the camera not engaged is not special: the
 * caller checks CtrFreeCam_Engaged first.
 */
int CtrFreeCam_Steer(int previous, float right, float forward);

/*
 * A standing player facing `facing` (DIR_*): the direction to tap this frame
 * to turn them to face where the head looks, or 0. `free` says a tap would
 * be taken as one (see voxel_freecam.h).
 */
int CtrFreeCam_TurnTap(bool free, int facing);

#endif
