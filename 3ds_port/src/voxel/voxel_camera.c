/*
 * Follow camera for the voxel overworld. See voxel_camera.h and NOTICE.md.
 */

#include <math.h>

#include "3ds_field_view.h"
#include "3ds_platform.h"
#include "voxel_camera.h"
#include "voxel_relief.h"
#include "voxel_world.h"

#define VOXEL_DEG_TO_RAD (3.14159265358979323846f / 180.0f)
#define VOXEL_FOLLOW 0.15f
/* The follow camera's vertical field of view. */
#define VOXEL_CAMERA_FOV 35.0f

/* ── The swoop's end pose (VoxelCamera_Swoop; its pitch is in the header) ── */
/* The field of view stays the follow camera's; at the top the projection
 * itself goes orthographic (ctr_voxel.c, CameraMatrices), as the GBA's art
 * is, and the eye stands where the perspective ground framing is already the
 * 2D field's, so the two agree at every step of the blend. */
/* Half the 2D field's height in tiles: 240 px over 16 px a tile, halved. */
#define VOXEL_SWOOP_HALF_TILES 7.5f
/* Where the 2D field's middle lies from the camera's usual target: half a
 * tile right less the target's own half (VOXEL_TILE_MIDDLE) and a tile down (the tile's centre, then the picture's middle is
 * 8 px below the player's tile: FIELD_VIEW_OFFSET_Y 32 against (240-160)/2). */
/* Measured against the 2D frame in Azahar (2026-10-17): the voxel picture
 * sat 10 px high and 2 px left of it, hence not the 0.5 and 1.0 the tile
 * arithmetic says. */
#define VOXEL_SWOOP_SHIFT_X -0.125f /* 0.375 less the half tile the target has (VOXEL_TILE_MIDDLE) */
#define VOXEL_SWOOP_SHIFT_Z 0.4f
/* The picture's width over its height (CTR_GAME_WIDTH, CTR_GAME_HEIGHT). */
#define VOXEL_ASPECT (400.0f / 240.0f)
/*
 * How far the camera may stop short of the player at an edge, in parts of
 * its distance: up the picture, down it and to either side. The player walks
 * on towards the edge of the picture, but not into what the diorama blurs
 * (3ds_video.c), nor up to the top, far off and small; a little of what lies
 * past the edge shows instead, in the distance, where that blur is.
 */
#define VOXEL_EDGE_UP 0.4f
#define VOXEL_EDGE_DOWN 0.25f
#define VOXEL_EDGE_SIDE 0.45f
/* The player's card stands in the middle of its tile, half a tile east of the
 * position it is given by (VoxelEntities_GetPlayerWorldPos): the camera looks
 * there, or the player stood half a tile right of the middle of the picture.
 * North-south the framing is the GBA's own, the player a little low. */
#define VOXEL_TILE_MIDDLE 0.5f

void VoxelCamera_Init(VoxelCamera *cam)
{
    cam->x = 0.0f;
    cam->y = 8.0f;
    cam->z = 0.0f;
    cam->targetX = 0.0f;
    cam->targetY = 0.0f;
    cam->targetZ = 0.0f;
    cam->pitch = 40.0f;
    cam->yaw = 0.0f;
    cam->distance = 9.0f;
    cam->fov = VOXEL_CAMERA_FOV;
    cam->ground = 0.0f;
    cam->swoopX = cam->swoopZ = 0.0f;
    cam->lay = 0.0f;
}

void VoxelCamera_SetGround(VoxelCamera *cam, float ground, int snap)
{
    cam->ground = snap ? ground : cam->ground + (ground - cam->ground) * VOXEL_FOLLOW;
}

/* Places the eye for the current target, pitch, yaw and distance. */
static void Place(VoxelCamera *cam)
{
    float pitchRad = cam->pitch * VOXEL_DEG_TO_RAD;
    float yawRad = cam->yaw * VOXEL_DEG_TO_RAD;

    /* The eye sits south of the player (+Z) and above it, looking north. */
    cam->x = cam->targetX + sinf(yawRad) * cam->distance;
    cam->y = cam->ground + tanf(pitchRad) * cam->distance;
    cam->z = cam->targetZ + cosf(yawRad) * cam->distance;
    cam->targetY = cam->ground;
}

/* Wider maps are framed from further away, up to a fixed ceiling. Maps joined
 * by connections all take the ceiling: the player walks from one to the next
 * with no fade, and a 20x20 town (Oldale, Dewford) would otherwise pull the
 * camera in at once on crossing its edge. The pitch and the zoom are the
 * player's (bottom-screen options; 40 degrees, 100%). */
static void AdaptDistance(VoxelCamera *cam)
{
    int mapW = 0, mapH = 0;
    float scale = 5.0f;

    if (!VoxelWorld_MapIsConnected())
    {
        VoxelWorld_GetMapDimensions(&mapW, &mapH);
        scale = (float)(mapW > mapH ? mapW : mapH) * 0.2f;
        if (scale > 5.0f)
            scale = 5.0f;
    }
    cam->distance = (8.0f + scale) * 100.0f / (float)CtrSettings_VoxelZoom();
    cam->pitch = (float)CtrSettings_VoxelPitch();
}

static float Between(float value, float lo, float hi)
{
    return value < lo ? lo : value > hi ? hi : value;
}

/*
 * The edge of the map (3ds_field_view.h): where no other map joins it, the
 * camera stops short of the player so that the ground it shows ends at the
 * edge. How far it sees each way is taken where the target is, on its ground:
 * north to where the top of the picture meets it, south to the bottom, and
 * either side across the middle - the far corners of the picture, wider,
 * still show a little past a side. The point the camera looks at is where
 * the player is, or where it stops instead.
 */
static void StopAtEdges(const VoxelCamera *cam, float *x, float *z)
{
    const VoxelMapInstance *map = VoxelWorld_Instance(0);
    unsigned closed = VoxelWorld_ClosedEdges();
    float pitch = cam->pitch * VOXEL_DEG_TO_RAD, half = cam->fov * 0.5f * VOXEL_DEG_TO_RAD;
    float height = tanf(pitch) * cam->distance;
    float north, south, side;
    float playerX = *x, playerZ = *z;

    if (map == NULL || closed == 0)
        return;
    /* Looking at or over the horizon, the view has no northern end. */
    if (pitch - half < 0.02f)
        closed &= ~CTR_FIELD_EDGE_NORTH;
    north = pitch - half >= 0.02f ? height / tanf(pitch - half) - cam->distance : 0.0f;
    south = cam->distance - height / tanf(pitch + half);
    side = cam->distance / cosf(pitch) * tanf(half) * VOXEL_ASPECT;

    *x = CtrFieldView_Limit(*x, side, side, (float)map->originX, (float)(map->originX + map->width), closed & 3);
    /* (its rows as far south as its base is high: VoxelBuilder_StretchZ) */
    *z = CtrFieldView_Limit(*z, north, south, (float)map->originY + VoxelRelief_Base(map),
                            (float)(map->originY + map->height) + VoxelRelief_Base(map),
                            (closed >> 2) & 3);
    *x = Between(*x, playerX - VOXEL_EDGE_SIDE * cam->distance, playerX + VOXEL_EDGE_SIDE * cam->distance);
    *z = Between(*z, playerZ - VOXEL_EDGE_DOWN * cam->distance, playerZ + VOXEL_EDGE_UP * cam->distance);
}

/* Where the camera looks for the player there, and the view's shift for the
 * people it reaches (3ds_field_view.h). */
static void Aim(const VoxelCamera *cam, float playerWorldX, float playerWorldZ, float *x, float *z)
{
    *x = playerWorldX + VOXEL_TILE_MIDDLE;
    *z = playerWorldZ;
    StopAtEdges(cam, x, z);
    CtrFieldView_SetVoxelShift((int)floorf(*x - playerWorldX - VOXEL_TILE_MIDDLE + 0.5f),
                               (int)floorf(*z - playerWorldZ + 0.5f));
}

void VoxelCamera_Snap(VoxelCamera *cam, float playerWorldX, float playerWorldZ)
{
    cam->swoopX = cam->swoopZ = 0.0f;
    AdaptDistance(cam);
    Aim(cam, playerWorldX, playerWorldZ, &cam->targetX, &cam->targetZ);
    Place(cam);
}

void VoxelCamera_Update(VoxelCamera *cam, float playerWorldX, float playerWorldZ)
{
    float x, z;

    cam->targetX -= cam->swoopX;
    cam->targetZ -= cam->swoopZ;
    cam->swoopX = cam->swoopZ = 0.0f;
    AdaptDistance(cam);
    Aim(cam, playerWorldX, playerWorldZ, &x, &z);
    cam->targetX += (x - cam->targetX) * VOXEL_FOLLOW;
    cam->targetZ += (z - cam->targetZ) * VOXEL_FOLLOW;
    Place(cam);
}

void VoxelCamera_Shift(VoxelCamera *cam, float dx, float dz)
{
    cam->targetX += dx;
    cam->targetZ += dz;
    Place(cam);
}

void VoxelCamera_Swoop(VoxelCamera *cam, float t)
{
    /* Both poses as the eye's distance from the target along the line of
     * sight, so the dolly is one length eased, whatever the pitch does. */
    float pitch0 = cam->pitch, fov0 = VOXEL_CAMERA_FOV;
    float length0 = cam->distance / cosf(pitch0 * VOXEL_DEG_TO_RAD);
    /* The eye's distance at which the perspective ground framing is the 2D
     * field's: half its height of ground, seen at the pitch, fills half the
     * field of view. */
    float length1 = VOXEL_SWOOP_HALF_TILES * sinf(VOXEL_SWOOP_PITCH * VOXEL_DEG_TO_RAD)
                  / tanf(fov0 * 0.5f * VOXEL_DEG_TO_RAD);
    float pitch, length;

    if (t <= 0.0f)
    {
        cam->fov = fov0;
        cam->lay = 0.0f;
        return;
    }
    if (t > 1.0f) t = 1.0f;
    cam->lay = t;
    pitch = pitch0 + (VOXEL_SWOOP_PITCH - pitch0) * t;
    length = length0 + (length1 - length0) * t;
    cam->fov = fov0;
    cam->pitch = pitch;
    cam->distance = length * cosf(pitch * VOXEL_DEG_TO_RAD);
    cam->swoopX = VOXEL_SWOOP_SHIFT_X * t;
    cam->swoopZ = VOXEL_SWOOP_SHIFT_Z * t;
    cam->targetX += cam->swoopX;
    cam->targetZ += cam->swoopZ;
    Place(cam);
}

void VoxelCamera_Frame(VoxelCamera *cam, float targetX, float targetZ, float ground,
                       float pitch, float distance)
{
    cam->targetX = targetX;
    cam->targetZ = targetZ;
    cam->ground = ground;
    cam->pitch = pitch;
    cam->distance = distance;
    cam->lay = 0.0f;
    Place(cam);
}
