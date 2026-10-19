/*
 * Free walking over the game's grid. See voxel_freemove.h.
 */
#include <math.h>
#include <string.h>

#include "voxel_freemove.h"

#define VOXEL_FM_EDGE 0.5f

static VoxelFreeMove sMove;

VoxelFreeMove *VoxelFreeMove_Get(void)
{
    return &sMove;
}

void VoxelFreeMove_Reset(VoxelFreeMove *fm)
{
    memset(fm, 0, sizeof(*fm));
}

static float Clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void VoxelFreeMove_Sync(VoxelFreeMove *fm, int gx, int gz)
{
    int dx, dz;

    if (!fm->have)
    {
        fm->have = true;
        fm->gx = gx;
        fm->gz = gz;
        fm->ox = fm->oz = 0.0f;
        return;
    }
    dx = gx - fm->gx;
    dz = gz - fm->gz;
    if (dx == 0 && dz == 0)
        return;
    fm->gx = gx;
    fm->gz = gz;
    {
        int adx = dx < 0 ? -dx : dx, adz = dz < 0 ? -dz : dz;

        if (adx <= 1 && adz <= 1)
        {
            /* The game's tile followed P: keep the same world point. */
            fm->ox -= (float)dx;
            fm->oz -= (float)dz;
        }
        else
        {
            /* A warp, a ledge's jump, a map's coordinates changing: the game
             * has put the player somewhere, which is where the picture goes
             * too, on its own for a moment. */
            fm->ox = fm->oz = 0.0f;
            fm->blend = 0.0f;
        }
    }
}

void VoxelFreeMove_Idle(VoxelFreeMove *fm, int gx, int gz)
{
    VoxelFreeMove_Sync(fm, gx, gz);
    fm->driving = false;
    fm->speed = 0.0f;
    fm->blend -= 1.0f / (float)VOXEL_FM_BLEND_FRAMES;
    if (fm->blend <= 0.0f)
    {
        fm->blend = 0.0f;
        fm->ox = fm->oz = 0.0f;
    }
}

bool VoxelFreeMove_Moving(const VoxelFreeMove *fm)
{
    return fm->driving && fm->speed > 0.001f;
}

int VoxelFreeMove_Step(VoxelFreeMove *fm, int gx, int gz, float vx, float vz,
                       VoxelFreeMoveBlocked blocked, void *context)
{
    float v[2], *o[2];
    float magnitude;
    int order[2] = {0, 1};
    int pressed = VOXEL_FM_NONE;

    VoxelFreeMove_Sync(fm, gx, gz);
    fm->driving = true;
    fm->blend += 1.0f / (float)VOXEL_FM_BLEND_FRAMES;
    if (fm->blend > 1.0f)
        fm->blend = 1.0f;

    v[0] = vx;
    v[1] = vz;
    o[0] = &fm->ox;
    o[1] = &fm->oz;
    magnitude = sqrtf(vx * vx + vz * vz);
    fm->speed = magnitude;
    if (magnitude <= 0.0001f)
    {
        fm->speed = 0.0f;
        return VOXEL_FM_NONE;
    }
    fm->heading = atan2f(vx, vz);
    fm->travel += magnitude;

    /* The axis with more of the walk first: it gets the step if both reach
     * an edge in the same frame. */
    if (fabsf(v[1]) > fabsf(v[0]))
    {
        order[0] = 1;
        order[1] = 0;
    }
    for (int k = 0; k < 2; ++k)
    {
        int axis = order[k];
        float m = v[axis], n;
        float side;

        if (m == 0.0f)
            continue;
        side = m > 0.0f ? 1.0f : -1.0f;
        n = *o[axis] + m;
        if (side * n > VOXEL_FM_EDGE)
        {
            int dir = axis == 0 ? (side > 0.0f ? VOXEL_FM_EAST : VOXEL_FM_WEST)
                                : (side > 0.0f ? VOXEL_FM_SOUTH : VOXEL_FM_NORTH);

            if (blocked != NULL && blocked(context, dir))
            {
                /* A wall (or a ledge the game must jump): stop at the face
                 * and slide. Aimed straight at it, the game is asked, to
                 * bump, to push, to take the warp that waits on that edge. */
                n = Clampf(n, -VOXEL_FM_EDGE, VOXEL_FM_EDGE);
                if (pressed == VOXEL_FM_NONE && fabsf(m) >= VOXEL_FM_ALIGNED * magnitude)
                    pressed = dir;
            }
            else if (pressed == VOXEL_FM_NONE)
            {
                /* Walkable: P keeps going; the game's tile is asked to follow. */
                pressed = dir;
            }
        }
        *o[axis] = n;
    }
    return pressed;
}
