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

/*
 * The pose the swoop to the 2D field ends in (VoxelCamera_Swoop), degrees
 * below the horizon. The GBA's art is an oblique view, not a top-down one:
 * the ground is square-on, and a wall a tile high is drawn a tile tall in
 * the tiles above its base. An orthographic camera at 45 degrees with the
 * ground stretched back to 1:1 (ctr_voxel.c, CameraMatrices) is exactly
 * that: a tile of height shows as a tile north. Shared with ctr_voxel.c and
 * voxel_entities.c, which tilts the sprite cards to face this camera.
 */
#define VOXEL_SWOOP_PITCH 45.0f
/* The world's height at that pose, of its own: the GBA's trees and roofs
 * are far shorter than the models are at a true 45 degrees, so the world is
 * squashed towards the ground as the camera goes (ctr_voxel.c), and the
 * sprite cards sized to match the 2D sprites under it (voxel_entities.c). */
#define VOXEL_SWOOP_HEIGHT 0.55f

typedef struct
{
    float x, y, z;
    float targetX, targetY, targetZ;
    float pitch;    /* degrees below the horizon */
    float yaw;      /* degrees around the vertical axis */
    float distance; /* distance from the target, adapted to the map size */
    float fov;      /* vertical field of view, degrees */
    float ground;   /* height, tiles, of the ground the player stands on */
    /* The swoop's shift of the target (VoxelCamera_Swoop), taken off again
     * before the next follow step so it never feeds the damping. */
    float swoopX, swoopZ;
    /* How far the sprite cards have tilted back from upright to face the
     * swoop's end camera (VOXEL_SWOOP_PITCH), 0..1 with the swoop itself
     * (voxel_entities.c). */
    float lay;
} VoxelCamera;

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
/*
 * The swoop between the voxel world and the 2D field (voxel_swoop.c): blends
 * the camera just placed by Snap/Update from the player's pose (t = 0) to a
 * top-down pose whose ground framing is the 2D field's, 1:1 (t = 1): 25 by
 * 15 tiles with the player's tile half a tile above the middle, as the GBA
 * picture sits at FIELD_VIEW_OFFSET on the 400x240 view. Call after
 * Snap/Update; the field of view goes with it.
 */
void VoxelCamera_Swoop(VoxelCamera *cam, float t);

#endif
