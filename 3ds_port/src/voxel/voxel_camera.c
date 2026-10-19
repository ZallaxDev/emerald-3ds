/*
 * Follow camera for the voxel overworld. See voxel_camera.h and NOTICE.md.
 */

#include <math.h>
#include <string.h>

#include "3ds_platform.h"
#include "3ds_extras.h"
#include "3ds_freecam.h"
#include "voxel_building.h"
#include "voxel_camera.h"
#include "voxel_entities.h"
#include "voxel_freecam.h"
#include "voxel_relief.h"
#include "voxel_world.h"

#define VOXEL_DEG_TO_RAD (3.14159265358979323846f / 180.0f)
#define VOXEL_FOLLOW 0.15f
/* The orbit's field of view (VoxelCamera_Init). */
#define VOXEL_ORBIT_FOV 35.0f
/* A frame is 1/60 s: the world is updated once per shown frame. */
#define VOXEL_FRAME_DT (1.0f / 60.0f)
/* Frames since the last Update before the rig stops counting as drawn. */
#define VOXEL_RIG_STALE_FRAMES 4u

/* The one free-roam camera (voxel_freecam.h) and when the world was last
 * drawn from it. */
static VoxelFreeCam sRig;
static bool sRigReady;
static uint64_t sLastUpdate;
static bool sUpdated;

void VoxelCamera_Init(VoxelCamera *cam)
{
    cam->x = 0.0f;
    cam->y = 8.0f;
    cam->z = 0.0f;
    cam->targetX = 0.0f;
    cam->targetY = 0.0f;
    cam->targetZ = 0.0f;
    cam->lookX = cam->lookY = cam->lookZ = 0.0f;
    cam->pitch = 40.0f;
    cam->yaw = 0.0f;
    cam->distance = 9.0f;
    cam->fov = VOXEL_ORBIT_FOV;
    cam->ground = 0.0f;
    cam->rig = 0.0f;
    cam->cardPitch = 40.0f;
    cam->cardYaw = 0.0f;
    cam->cardBlend = 0.0f;
    cam->focusDistance = 0.0f;
    cam->hidePlayer = false;
    cam->playerX = cam->playerZ = 0.0f;
    if (!sRigReady)
    {
        VoxelFreeCam_Init(&sRig);
        sRigReady = true;
    }
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
    /* On the orbit the eye looks at the target; the free-roam rig moves the
     * aim afterwards (ApplyRig). */
    cam->lookX = cam->targetX;
    cam->lookY = cam->targetY;
    cam->lookZ = cam->targetZ;
    cam->fov = VOXEL_ORBIT_FOV;
    cam->rig = 0.0f;
    cam->cardPitch = cam->pitch;
    cam->cardYaw = cam->yaw;
    cam->cardBlend = 0.0f;
    cam->focusDistance = 0.0f;
    cam->hidePlayer = false;
}

/* ------- the free-roam rig (1ST / 3RD) ------- */

/* OPTIONS' CAMERA (3ds_extras.c "voxel_camera"). */
static int SelectedMode(void)
{
    int mode = CtrExtras_Get("voxel_camera");

    return mode == CTR_CAMERA_FIRST || mode == CTR_CAMERA_THIRD ? mode : CTR_CAMERA_ORBIT;
}

/*
 * Whether the eye may not stand at a point (ThirdPerson.lua `occupied`): the
 * ground, anything modelled that stands on it, and a blocked cell up to head
 * height. Off every map is refused too: the border is drawn there, and the
 * camera has no business in it.
 */
static bool EyeBlocked(void *context, float x, float y, float z)
{
    int cx = (int)floorf(x), cz = (int)floorf(z);
    const VoxelMapInstance *inst = VoxelWorld_GetInstanceAt(cx, cz);
    float ground, top;

    (void)context;
    if (inst == NULL)
        return true;
    ground = VoxelRelief_LiftAt(x, z);
    if (y < ground + VOXEL_FC_PAD)
        return true;
    /* A modelled building or piece of furniture: its own height. */
    if (VoxelBuildings_CellAt(inst, cx, cz, NULL, &top) && top > 0.0f && y < ground + top)
        return true;
    /* The short furniture of the world is passed over, a wall is not. */
    if (VoxelWorld_GetCollision(cx, cz) != 0 && y < ground + VOXEL_FC_CLEAR)
        return true;
    return false;
}

/* The rig's tick and placement over the orbit Place() just made. */
static void ApplyRig(VoxelCamera *cam, bool engaged)
{
    VoxelFreeCamView view;
    float head[3], orbitEye[3], orbitLook[3];
    float yaw = VoxelFreeCam_FacingAngle(VoxelEntities_PlayerFacing());
    bool third = SelectedMode() == CTR_CAMERA_THIRD;

    /* The existing zoom (OPTIONS, 90-120%) is the boom's: a closer zoom is a
     * shorter boom. */
    VoxelFreeCam_SetZoom(&sRig, 100.0f / (float)CtrSettings_VoxelZoom());
    VoxelFreeCam_Tick(&sRig, VOXEL_FRAME_DT, engaged, third, yaw, true);
    if (sRig.blend <= 0.0f)
        return;
    head[0] = cam->playerX + 0.5f;
    head[1] = cam->ground + VOXEL_FC_EYE_HEIGHT;
    head[2] = cam->playerZ + 0.5f;
    orbitEye[0] = cam->x; orbitEye[1] = cam->y; orbitEye[2] = cam->z;
    orbitLook[0] = cam->lookX; orbitLook[1] = cam->lookY; orbitLook[2] = cam->lookZ;
    VoxelFreeCam_Frame(&sRig, head, orbitEye, orbitLook, VOXEL_ORBIT_FOV, EyeBlocked, NULL, &view);
    cam->x = view.eye[0]; cam->y = view.eye[1]; cam->z = view.eye[2];
    cam->lookX = view.focus[0]; cam->lookY = view.focus[1]; cam->lookZ = view.focus[2];
    cam->fov = view.fov;
    cam->rig = view.eased;
    cam->focusDistance = view.focusDistance;
    cam->cardPitch = cam->pitch + (view.cardPitch - cam->pitch) * view.eased;
    cam->cardBlend = view.cardBlend;
    cam->hidePlayer = VoxelFreeCam_HidePlayer(&sRig);
}

bool CtrFreeCam_Selected(void)
{
    return SelectedMode() != CTR_CAMERA_ORBIT;
}

bool CtrFreeCam_FreeWalk(void)
{
    return CtrExtras_Get("voxel_move") != 0;
}

bool CtrFreeCam_Engaged(void)
{
    return sUpdated && SelectedMode() != CTR_CAMERA_ORBIT
        && CtrPlatform_FrameCount() - sLastUpdate <= VOXEL_RIG_STALE_FRAMES;
}

float CtrFreeCam_Yaw(void)
{
    return sRig.yaw;
}

void CtrFreeCam_SetDriving(bool driving)
{
    sRig.driving = driving;
}

void CtrFreeCam_SetLook(float stickX, float stickY, float turn)
{
    VoxelFreeCam_SetLook(&sRig, stickX, stickY, turn);
}

int CtrFreeCam_Steer(int previous, float right, float forward)
{
    return VoxelFreeCam_Steer(previous, sRig.yaw, right, forward);
}

int CtrFreeCam_TurnTap(bool free, int facing)
{
    static VoxelFreeCamTap tap;

    return VoxelFreeCam_TurnTap(&tap, free, facing, VoxelFreeCam_CompassFacing(sRig.yaw));
}

void VoxelCamera_ToOrbit(VoxelCamera *cam)
{
    /* The orbit's eye again, from the target, pitch, yaw and distance the
     * orbit kept all along (Place also clears the rig's fields). */
    Place(cam);
}

float VoxelCamera_CardYaw(const VoxelCamera *cam, float cx, float cz)
{
    float eye[3], toEye, delta;

    if (cam->cardBlend <= 0.0f)
        return cam->yaw;
    eye[0] = cam->x; eye[1] = cam->y; eye[2] = cam->z;
    toEye = VoxelFreeCam_CardYaw(eye, cx, cz) / VOXEL_DEG_TO_RAD;
    /* From the orbit's plane to the eye by the short way round. */
    delta = fmodf(toEye - cam->yaw + 540.0f, 360.0f) - 180.0f;
    return cam->yaw + delta * cam->cardBlend;
}

float VoxelCamera_CardPitch(const VoxelCamera *cam)
{
    return cam->cardPitch;
}

float VoxelCamera_Near(const VoxelCamera *cam, float orbitNear)
{
    return orbitNear + (VOXEL_CAMERA_FREE_NEAR - orbitNear) * cam->rig;
}

float VoxelCamera_Reach(const VoxelCamera *cam, float orbitReach)
{
    return orbitReach + (VOXEL_CAMERA_FREE_REACH - orbitReach) * cam->rig;
}

float VoxelCamera_Rig(const VoxelCamera *cam)
{
    return cam->rig;
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

void VoxelCamera_Snap(VoxelCamera *cam, float playerWorldX, float playerWorldZ)
{
    cam->targetX = playerWorldX;
    cam->targetZ = playerWorldZ;
    cam->playerX = playerWorldX;
    cam->playerZ = playerWorldZ;
    AdaptDistance(cam);
    Place(cam);
    ApplyRig(cam, SelectedMode() != CTR_CAMERA_ORBIT);
    sLastUpdate = CtrPlatform_FrameCount();
    sUpdated = true;
}

void VoxelCamera_Update(VoxelCamera *cam, float playerWorldX, float playerWorldZ)
{
    /* The orbit trails the player; the head does not - the world is built
     * around the point the camera follows, which in the rig is the player's
     * own, to the extent the rig has taken over. */
    float follow = VOXEL_FOLLOW + (1.0f - VOXEL_FOLLOW) * VoxelFreeCam_Eased(&sRig);

    cam->playerX = playerWorldX;
    cam->playerZ = playerWorldZ;
    cam->targetX += (playerWorldX - cam->targetX) * follow;
    cam->targetZ += (playerWorldZ - cam->targetZ) * follow;
    AdaptDistance(cam);
    Place(cam);
    ApplyRig(cam, SelectedMode() != CTR_CAMERA_ORBIT);
    sLastUpdate = CtrPlatform_FrameCount();
    sUpdated = true;
}

void VoxelCamera_Shift(VoxelCamera *cam, float dx, float dz)
{
    cam->targetX += dx;
    cam->targetZ += dz;
    cam->playerX += dx;
    cam->playerZ += dz;
    Place(cam);
    /* The rig is placed again at the next Update, from the shifted player. */
}

void VoxelCamera_Frame(VoxelCamera *cam, float targetX, float targetZ, float ground,
                       float pitch, float distance)
{
    cam->targetX = targetX;
    cam->targetZ = targetZ;
    cam->ground = ground;
    cam->pitch = pitch;
    cam->distance = distance;
    Place(cam);
}
