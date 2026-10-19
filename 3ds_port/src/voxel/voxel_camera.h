/*
 * Follow camera for the voxel overworld: pure state and pure math.
 *
 * Ported from the MIT-licensed src/platform/voxel/voxel_camera.c of
 * pokeemerald-multiplatform (see NOTICE.md), minus gluPerspective/gluLookAt
 * and the SDL free-fly mode. Building the projection and view matrices is the
 * caller's job, so nothing in this module touches the GPU.
 */
#ifndef CTR_VOXEL_CAMERA_H
#define CTR_VOXEL_CAMERA_H

#include <stdbool.h>

/*
 * `x, y, z` is the eye and `lookX/Y/Z` what it is aimed at. On the orbit the
 * aim is the target (the player's ground point), the eye `distance` away at
 * `pitch`; with OPTIONS' CAMERA on 1ST or 3RD (voxel_freecam.h) the eye
 * stands in the player's head or on a boom behind it, and the target is only
 * the point the world is built and culled around (it follows the player at
 * once there). `pitch`, `yaw` and `distance` stay the orbit's own: the
 * battle's glide starts from them.
 */
typedef struct
{
    float x, y, z;
    float targetX, targetY, targetZ;
    float lookX, lookY, lookZ;
    float pitch;    /* degrees below the horizon */
    float yaw;      /* degrees around the vertical axis */
    float distance; /* distance from the target, adapted to the map size */
    float fov;      /* vertical field of view, degrees */
    float ground;   /* height, tiles, of the ground the player stands on */
    /* The free-roam rig (all 0 / false on the orbit): how far the camera is
     * from the orbit towards the head, 0..1; the pitch a card is sized for
     * and the bearing and share with which cards turn to face the eye, in
     * degrees and 0..1; the distance of the plane of zero parallax; the
     * player's own card left out (it would fill the lens from inside). */
    float rig;
    float cardPitch, cardYaw, cardBlend;
    float focusDistance;
    bool hidePlayer;
    /* The player's tile as last given (Snap, Update), the eye's own. */
    float playerX, playerZ;
} VoxelCamera;

/* The free-roam rig's near plane and how far it sees, tiles: the eye stands
 * half a tile from a wall, which the orbit's one-tile near plane would cut
 * away, and what is seen along the ground is much more than the orbit's
 * view of a small area (the haze takes the far end, ctr_voxel.c). */
#define VOXEL_CAMERA_FREE_NEAR 0.1f
#define VOXEL_CAMERA_FREE_REACH 22.0f

void VoxelCamera_Init(VoxelCamera *cam);
/* Places the camera on the player with no interpolation (map change, re-entry). */
void VoxelCamera_Snap(VoxelCamera *cam, float playerWorldX, float playerWorldZ);
void VoxelCamera_Update(VoxelCamera *cam, float playerWorldX, float playerWorldZ);
/* The height of the ground under the player (VoxelRelief_LiftAt): followed
 * as the player is, or taken at once with `snap`. Call before Snap/Update. */
void VoxelCamera_SetGround(VoxelCamera *cam, float ground, int snap);
/* Shifts the whole camera by a map-instance offset, keeping the framing. */
void VoxelCamera_Shift(VoxelCamera *cam, float dx, float dz);
/* Places the camera on a target of its own, with its own pitch and distance
 * rather than the player's options: the battle stage (ctr_voxel.c). */
void VoxelCamera_Frame(VoxelCamera *cam, float targetX, float targetZ, float ground,
                       float pitch, float distance);
/* A camera as the orbit's alone (the field's, kept for a battle to leave
 * from): the aim on the target, the orbit's field of view, no rig. */
void VoxelCamera_ToOrbit(VoxelCamera *cam);

/*
 * Billboards (voxel_entities.c): the bearing, in degrees, a card at (cx, cz)
 * turns to face the camera - the orbit's yaw, and in the free-roam rig the
 * bearing from the card to the eye - and the pitch it is sized for.
 */
float VoxelCamera_CardYaw(const VoxelCamera *cam, float cx, float cz);
float VoxelCamera_CardPitch(const VoxelCamera *cam);

/* The near plane's distance and the view's reach in tiles, for the eye as
 * the camera has it: the orbit's own (`orbitNear`, `orbitReach`) mixed
 * towards the head's by the rig (a wall beside the player is closer than the
 * orbit's one-tile near plane). */
float VoxelCamera_Near(const VoxelCamera *cam, float orbitNear);
float VoxelCamera_Reach(const VoxelCamera *cam, float orbitReach);
/* How much of the distance the haze takes: 0 on the orbit, 1 in the rig. */
float VoxelCamera_Rig(const VoxelCamera *cam);

#endif
