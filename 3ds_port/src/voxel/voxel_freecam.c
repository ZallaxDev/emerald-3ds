/*
 * The free-roam camera. See voxel_freecam.h and NOTICE.md.
 *
 * Ported from the voxel mod's lib/FirstPerson.lua, lib/ThirdPerson.lua and
 * lib/FreeMove.lua; every comment that names a Lua function names it so the
 * two can be read side by side.
 */

#include <math.h>
#include <string.h>

#include "voxel_freecam.h"

#define PI_F 3.14159265358979f
#define DEG_TO_RAD (PI_F / 180.0f)

/* The Circle Pad's own decoding (3ds_input.h, 3ds_input_decode.c): the
 * steering keeps the same thresholds, so the stick feels as it does with the
 * camera off. The host test checks them against that header. */
#define STEER_ENTER 40.0f
#define STEER_LEAVE 28.0f
#define STEER_AXIS_HYSTERESIS 12.0f

static float Clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* FirstPerson.lua wrapPi. */
static float WrapPi(float a)
{
    float r = fmodf(a + PI_F, 2.0f * PI_F);

    if (r < 0.0f)
        r += 2.0f * PI_F;
    return r - PI_F;
}

/* FirstPerson.lua / ThirdPerson.lua ease. */
static float Ease(float t)
{
    return t * t * (3.0f - 2.0f * t);
}

void VoxelFreeCam_Init(VoxelFreeCam *fc)
{
    memset(fc, 0, sizeof(*fc));
    fc->yaw = 0.0f;
    fc->pitch = VOXEL_FC_PITCH_REST;
    fc->zoom = fc->zoomGoal = 1.0f;
}

void VoxelFreeCam_LookBy(VoxelFreeCam *fc, float dyaw, float dpitch)
{
    fc->yaw = WrapPi(fc->yaw + dyaw);
    fc->pitch = Clampf(fc->pitch + dpitch, VOXEL_FC_PITCH_UP, VOXEL_FC_PITCH_DOWN);
}

void VoxelFreeCam_SetLook(VoxelFreeCam *fc, float stickX, float stickY, float turn)
{
    fc->stickX = Clampf(stickX, -1.0f, 1.0f);
    fc->stickY = Clampf(stickY, -1.0f, 1.0f);
    fc->turn = Clampf(turn, -1.0f, 1.0f);
}

void VoxelFreeCam_SetZoom(VoxelFreeCam *fc, float zoom)
{
    fc->zoomGoal = Clampf(zoom, VOXEL_FC_ZOOM_MIN, VOXEL_FC_ZOOM_MAX);
}

/* The squared response with a dead zone: the first half of the throw aims
 * and the rest turns (FirstPerson.update's `curve`). */
static float Curve(float v)
{
    float a = fabsf(v);

    if (a < VOXEL_FC_STICK_DEAD)
        return 0.0f;
    a = (a - VOXEL_FC_STICK_DEAD) / (1.0f - VOXEL_FC_STICK_DEAD);
    return (v < 0.0f ? -1.0f : 1.0f) * a * a;
}

/* ThirdPerson.reachFor: the boom's full length at the player's zoom. */
static float ReachFor(const VoxelFreeCam *fc)
{
    return VOXEL_FC_BOOM * fc->zoom;
}

void VoxelFreeCam_Tick(VoxelFreeCam *fc, float dt, bool engaged, bool third,
                       float facingYaw, bool ready)
{
    float target, step, boomTarget;

    /* Entering the rung: the head starts looking the way the player faces,
     * pitched gently down - the reading pose of the flat game. */
    if (engaged && !fc->engagedBefore)
    {
        fc->yaw = WrapPi(facingYaw);
        fc->pitch = VOXEL_FC_PITCH_REST;
    }
    fc->engagedBefore = engaged;

    /* The blend, held at flat until there is terrain to dive into. */
    target = engaged ? 1.0f : 0.0f;
    if (target > fc->blend && fc->blend == 0.0f && !ready)
        target = 0.0f;
    step = dt / VOXEL_FC_BLEND_TIME;
    if (fc->blend < target)
        fc->blend = fc->blend + step < target ? fc->blend + step : target;
    else if (fc->blend > target)
        fc->blend = fc->blend - step > target ? fc->blend - step : target;

    /* ThirdPerson.update: the player's zoom first, so everything below
     * measures itself against the boom this frame wants. */
    if (fc->zoom != fc->zoomGoal)
    {
        float k = dt / VOXEL_FC_ZOOM_TIME < 1.0f ? dt / VOXEL_FC_ZOOM_TIME : 1.0f;
        float z = fc->zoom + (fc->zoomGoal - fc->zoom) * k;

        fc->zoom = fabsf(fc->zoomGoal - z) < 1e-4f ? fc->zoomGoal : z;
    }

    boomTarget = engaged && third ? 1.0f : 0.0f;
    if (fc->blend <= 0.0f)
    {
        /* Out of the head the extension snaps, so picking 3RD from an orbit
         * is one motion (the dive), not a dive and a slide. */
        fc->out = boomTarget;
        fc->len = ReachFor(fc) * boomTarget;
        fc->want = fc->len;
    }
    else
    {
        float boomStep = dt / VOXEL_FC_BOOM_TIME;

        if (fc->out < boomTarget)
            fc->out = fc->out + boomStep < boomTarget ? fc->out + boomStep : boomTarget;
        else if (fc->out > boomTarget)
            fc->out = fc->out - boomStep > boomTarget ? fc->out - boomStep : boomTarget;
    }
    /* The rationed recovery: Frame pulls `len` in to what the world allows at
     * once, and this is the only thing that lets it back out. */
    if (fc->len < fc->want)
    {
        float grown = fc->len + VOXEL_FC_RETURN * dt;

        fc->len = grown < fc->want ? grown : fc->want;
    }

    /* The look: the right stick is a rate, the shoulders a steady turn. The
     * yaw sign is negated, as in the Lua: yaw grows south -> east -> north,
     * which seen from behind the eye is a LEFT turn, so a push to the right
     * subtracts. */
    if (fc->driving)
    {
        float cy = Curve(fc->stickX), cp = Curve(fc->stickY);

        if (cy != 0.0f || cp != 0.0f)
            VoxelFreeCam_LookBy(fc, -cy * VOXEL_FC_STICK_YAW * dt, -cp * VOXEL_FC_STICK_PITCH * dt);
        if (fc->turn != 0.0f)
            VoxelFreeCam_LookBy(fc, -fc->turn * VOXEL_FC_STICK_YAW * 0.6f * dt, 0.0f);
    }
}

void VoxelFreeCam_LookDir(const VoxelFreeCam *fc, float *lx, float *ly, float *lz)
{
    float cp = cosf(fc->pitch);

    *lx = sinf(fc->yaw) * cp;
    *ly = -sinf(fc->pitch);
    *lz = cosf(fc->yaw) * cp;
}

float VoxelFreeCam_Eased(const VoxelFreeCam *fc)
{
    return Ease(Clampf(fc->blend, 0.0f, 1.0f));
}

float VoxelFreeCam_Extension(const VoxelFreeCam *fc)
{
    return Ease(Clampf(fc->out, 0.0f, 1.0f));
}

bool VoxelFreeCam_ShowsPlayer(const VoxelFreeCam *fc)
{
    return VoxelFreeCam_Extension(fc) > 0.0f && fc->len >= VOXEL_FC_SHOW_AT;
}

bool VoxelFreeCam_HidePlayer(const VoxelFreeCam *fc)
{
    if (VoxelFreeCam_ShowsPlayer(fc))
        return false;
    return VoxelFreeCam_Eased(fc) > 0.9f;
}

float VoxelFreeCam_Reach(const float pivot[3], float bx, float by, float bz, float want,
                         VoxelFreeCamBlocked blocked, void *context)
{
    float lo = 0.0f, hi = -1.0f;
    int steps;

    if (blocked == NULL || want <= 0.0f)
        return want > 0.0f ? want : 0.0f;
    steps = (int)ceilf(want / VOXEL_FC_STEP);
    for (int i = 1; i <= steps; ++i)
    {
        float t = i * VOXEL_FC_STEP < want ? i * VOXEL_FC_STEP : want;

        if (!blocked(context, pivot[0] + bx * t, pivot[1] + by * t, pivot[2] + bz * t))
            lo = t;
        else
        {
            hi = t;
            break;
        }
    }
    if (hi < 0.0f)
        return want;
    /* Bisect back into the gap between the refusal and the last clear
     * sample: the answer is the face's own position, not the sampling
     * grid's, and a wall is approached smoothly. */
    for (int i = 0; i < VOXEL_FC_REFINE; ++i)
    {
        float mid = (lo + hi) * 0.5f;

        if (!blocked(context, pivot[0] + bx * mid, pivot[1] + by * mid, pivot[2] + bz * mid))
            lo = mid;
        else
            hi = mid;
    }
    return lo - VOXEL_FC_PAD > 0.0f ? lo - VOXEL_FC_PAD : 0.0f;
}

void VoxelFreeCam_Frame(VoxelFreeCam *fc, const float head[3],
                        const float orbitEye[3], const float orbitFocus[3], float orbitFov,
                        VoxelFreeCamBlocked blocked, void *context, VoxelFreeCamView *view)
{
    float e = VoxelFreeCam_Eased(fc);
    float ext = VoxelFreeCam_Extension(fc);
    float lx, ly, lz, eye[3], aim[3], focus[3], rigDistance;

    VoxelFreeCam_LookDir(fc, &lx, &ly, &lz);
    focus[0] = head[0] + lx * VOXEL_FC_FOCUS_DIST;
    focus[1] = head[1] + ly * VOXEL_FC_FOCUS_DIST;
    focus[2] = head[2] + lz * VOXEL_FC_FOCUS_DIST;

    /* ThirdPerson.place: the eye walks back off the head along the very
     * direction it looks, as far as the world allows. With the boom fully in
     * this is exactly the first-person answer, so the two rungs are one rig
     * with one number between them. */
    if (ext <= 0.0f)
    {
        fc->want = fc->len = 0.0f;
        memcpy(eye, head, sizeof(eye));
        memcpy(aim, focus, sizeof(aim));
    }
    else
    {
        float lift = VOXEL_FC_PIVOT_LIFT * ext;
        float pivot[3] = {head[0], head[1] + lift, head[2]};
        float want = ReachFor(fc) * ext;
        float room, flat, sx = 0.0f, sz = 0.0f;

        fc->want = want;
        room = VoxelFreeCam_Reach(pivot, -lx, -ly, -lz, want, blocked, context);
        /* In instantly, out only as fast as Tick allows. */
        if (room < fc->len)
            fc->len = room;
        /* The rail offset, faded with how much boom survived: a camera
         * squeezed against a wall gives up its shoulder before its
         * distance. Right of the look, flat, and riding the zoom so it stays
         * the same fraction of the frame. */
        flat = sqrtf(lx * lx + lz * lz);
        if (flat > 1e-6f)
        {
            float s = VOXEL_FC_SHOULDER * fc->zoom * ext * (fc->len / (want > 1e-6f ? want : 1e-6f));

            sx = -lz / flat * s;
            sz = lx / flat * s;
        }
        eye[0] = pivot[0] - lx * fc->len + sx;
        eye[1] = pivot[1] - ly * fc->len;
        eye[2] = pivot[2] - lz * fc->len + sz;
        aim[0] = focus[0] + sx;
        aim[1] = focus[1] + lift;
        aim[2] = focus[2] + sz;
    }

    for (int i = 0; i < 3; ++i)
    {
        view->eye[i] = orbitEye[i] + (eye[i] - orbitEye[i]) * e;
        view->focus[i] = orbitFocus[i] + (aim[i] - orbitFocus[i]) * e;
    }
    view->fov = orbitFov + (VOXEL_FC_FOV - orbitFov) * e;
    /* The plane of zero parallax: the orbit's own distance, and in the rig
     * the boom's length and a little more, so the player is at the glass. */
    {
        float dx = orbitEye[0] - orbitFocus[0], dy = orbitEye[1] - orbitFocus[1], dz = orbitEye[2] - orbitFocus[2];
        float orbitDistance = sqrtf(dx * dx + dy * dy + dz * dz);

        rigDistance = fc->len + 3.0f;
        view->focusDistance = orbitDistance + (rigDistance - orbitDistance) * e;
    }
    view->cardPitch = fc->pitch > 0.0f ? fc->pitch / DEG_TO_RAD : 0.0f;
    view->cardBlend = e;
    view->eased = e;
}

float VoxelFreeCam_CardYaw(const float eye[3], float cardX, float cardZ)
{
    float dx = eye[0] - cardX, dz = eye[2] - cardZ;

    if (dx * dx + dz * dz < 1e-9f)
        return 0.0f;
    return atan2f(dx, dz);
}

/* ------- the walk ------- */

int VoxelFreeCam_CompassFacing(float yaw)
{
    float s = sinf(yaw), c = cosf(yaw);

    if (fabsf(s) > fabsf(c))
        return s > 0.0f ? VOXEL_FC_DIR_EAST : VOXEL_FC_DIR_WEST;
    return c > 0.0f ? VOXEL_FC_DIR_SOUTH : VOXEL_FC_DIR_NORTH;
}

float VoxelFreeCam_FacingAngle(int dir)
{
    switch (dir)
    {
    case VOXEL_FC_DIR_EAST: return PI_F * 0.5f;
    case VOXEL_FC_DIR_NORTH: return PI_F;
    case VOXEL_FC_DIR_WEST: return -PI_F * 0.5f;
    default: return 0.0f;
    }
}

void VoxelFreeCam_MoveWorld(float yaw, float right, float forward, float *wx, float *wz)
{
    float s = sinf(yaw), c = cosf(yaw);

    /* Forward is the flat look direction; strafe-right is its right hand:
     * cross(forward, up) with forward = (sin y, 0, cos y) lands on
     * (-cos y, 0, sin y), so facing south, your right hand points west. */
    *wx = -c * right + s * forward;
    *wz = s * right + c * forward;
}

int VoxelFreeCam_Steer(int previous, float yaw, float right, float forward)
{
    float wx, wz, ax, az, threshold;
    bool horizontal;

    VoxelFreeCam_MoveWorld(yaw, right, forward, &wx, &wz);
    threshold = previous != VOXEL_FC_DIR_NONE ? STEER_LEAVE : STEER_ENTER;
    if (wx * wx + wz * wz < threshold * threshold)
        return VOXEL_FC_DIR_NONE;
    ax = fabsf(wx);
    az = fabsf(wz);
    horizontal = ax > az;
    /* Keep the selected axis near diagonals; the opposite sign still
     * changes direction at once. */
    if (previous == VOXEL_FC_DIR_WEST || previous == VOXEL_FC_DIR_EAST)
        horizontal = az <= ax + STEER_AXIS_HYSTERESIS;
    else if (previous == VOXEL_FC_DIR_NORTH || previous == VOXEL_FC_DIR_SOUTH)
        horizontal = ax > az + STEER_AXIS_HYSTERESIS;
    if (horizontal)
        return wx < 0.0f ? VOXEL_FC_DIR_WEST : VOXEL_FC_DIR_EAST;
    /* North is -Z. */
    return wz < 0.0f ? VOXEL_FC_DIR_NORTH : VOXEL_FC_DIR_SOUTH;
}

#define VOXEL_FC_TAP_QUIET 6

int VoxelFreeCam_TurnTap(VoxelFreeCamTap *tap, bool free, int facing, int wanted)
{
    if (wanted == VOXEL_FC_DIR_NONE || facing == wanted)
    {
        tap->cooldown = 0;
        return VOXEL_FC_DIR_NONE;
    }
    if (!free)
        return VOXEL_FC_DIR_NONE;
    if (tap->cooldown > 0)
    {
        --tap->cooldown;
        return VOXEL_FC_DIR_NONE;
    }
    tap->cooldown = VOXEL_FC_TAP_QUIET;
    return wanted;
}
