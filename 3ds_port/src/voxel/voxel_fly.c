/*
 * The Fly cinematic: see voxel_fly.h. The camera's poses and the wisp sheets
 * are ctr_voxel.c's (FlyCamera, DrawCloudWisps); the cloud cover over the
 * picture is 3ds_video.c's (DrawCloudCover).
 */

#include "voxel_fly.h"

#include <math.h>

/* ── Tunables, frames at 60 Hz ─────────────────────────────────────────── */
/* The bird's dive for the player (field_effect.c: 18 frames from the top of
 * its swoop to the player): the camera draws in over them. */
#define VOXEL_FLY_DIVE_FRAMES 18u
/* How far in, as a share of the pose's distance taken off (FlyCamera). */
#define VOXEL_FLY_DIP 0.0f /* 0: the camera waits on the pose (a dip was judged a jolt) */
/* The one motion out: from drawn in, up over the world and into the deck. */
#define VOXEL_FLY_CLIMB_FRAMES 60u
/* ... along it (0..1): where the wisps come in, where the rise into the deck
 * starts and where the cover closes. */
#define VOXEL_FLY_WISPS_FROM 0.10f
#define VOXEL_FLY_WISPS_OVER 0.30f
#define VOXEL_FLY_RISE_FROM 0.50f
#define VOXEL_FLY_COVER_FROM 0.70f
/* The bird is let go as the cover starts closing: its fly-off and the game's
 * fade to black (some 22 frames) play under the closing cloud. */
#define VOXEL_FLY_RELEASE_FROM 0.70f
/* Inside the cloud at least this long, whatever the game does (a beat of
 * white, and the bird's own fly-off and fade hidden). */
#define VOXEL_FLY_CLOUD_MIN 12u
/* ... and at most this long: a map the voxel world never draws (the option
 * off, a 2D fallback) ends the cinematic here. */
#define VOXEL_FLY_CLOUD_MAX 600u
/* The one motion in: out of the deck and down onto the player's pose, the
 * Pokemon flying in beside the camera to land as it settles. */
#define VOXEL_FLY_DESCEND_FRAMES 72u
#define VOXEL_FLY_UNCOVER_OVER 0.28f /* share of it over which the cover opens */
#define VOXEL_FLY_SINK_OVER 0.45f    /* ... over which the eye comes down out of the deck */
#define VOXEL_FLY_WISPS_GONE_FROM 0.45f
#define VOXEL_FLY_WISPS_GONE_OVER 0.35f

static float Smoother(float t)
{
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

static float Smooth(float t)
{
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

/* Smoothstep from `from` over `over` of a 0..1 way. */
static float Over(float u, float from, float over)
{
    return Smooth((u - from) / over);
}

static float Ramp(unsigned frame, unsigned frames)
{
    return frames == 0 ? 1.0f : (float)frame / (float)frames;
}

static void Go(VoxelFly *f, VoxelFlyPhase phase)
{
    f->phase = phase;
    f->frame = 0;
}

void VoxelFly_Begin(VoxelFly *f, float dirX, float dirZ, bool sky)
{
    float length = sqrtf(dirX * dirX + dirZ * dirZ);

    if (length < 0.001f)
    {
        dirX = 0.0f;
        dirZ = -1.0f;
    }
    else
    {
        dirX /= length;
        dirZ /= length;
    }
    f->dirX = dirX;
    f->dirZ = dirZ;
    f->gone = false;
    f->sky = sky;
    Go(f, VOXEL_FLY_DIVE);
}

void VoxelFly_Cancel(VoxelFly *f)
{
    Go(f, VOXEL_FLY_IDLE);
}

bool VoxelFly_Active(const VoxelFly *f)
{
    return f->phase != VOXEL_FLY_IDLE;
}

bool VoxelFly_Hovers(const VoxelFly *f)
{
    return f->phase == VOXEL_FLY_DIVE
        || (f->phase == VOXEL_FLY_CLIMB
            && (float)f->frame < VOXEL_FLY_RELEASE_FROM * (float)VOXEL_FLY_CLIMB_FRAMES);
}

bool VoxelFly_Busy(const VoxelFly *f)
{
    return f->phase == VOXEL_FLY_CLOUD || f->phase == VOXEL_FLY_SKY;
}

bool VoxelFly_Progress(const VoxelFly *f, float *share)
{
    float inside;

    switch (f->phase)
    {
    case VOXEL_FLY_DIVE:
        *share = 0.0f;
        return true;
    case VOXEL_FLY_CLIMB:
        *share = 0.3f * Ramp(f->frame, VOXEL_FLY_CLIMB_FRAMES);
        return true;
    case VOXEL_FLY_CLOUD:
        inside = Ramp(f->frame, 40u); /* the usual stay inside */
        *share = 0.3f + 0.45f * (inside < 1.0f ? inside : 1.0f);
        return true;
    case VOXEL_FLY_SKY:
        inside = Ramp(f->frame, VOXEL_FLY_SKY_FRAMES);
        *share = 0.3f + 0.45f * (inside < 1.0f ? inside : 1.0f);
        return true;
    case VOXEL_FLY_DESCEND:
        *share = 0.75f + 0.25f * Ramp(f->frame, VOXEL_FLY_DESCEND_FRAMES);
        return true;
    default:
        return false;
    }
}

bool VoxelFly_Sky(const VoxelFly *f, unsigned *frame)
{

    if (f->phase != VOXEL_FLY_SKY)
        return false;
    *frame = f->frame < VOXEL_FLY_SKY_FRAMES ? f->frame : VOXEL_FLY_SKY_FRAMES - 1;
    return true;
}

bool VoxelFly_Landing(const VoxelFly *f, float *share)
{
    if (f->phase != VOXEL_FLY_DESCEND)
        return false;
    *share = Ramp(f->frame, VOXEL_FLY_DESCEND_FRAMES);
    return true;
}

void VoxelFly_Frame(const VoxelFly *f, VoxelFlyFrame *out)
{
    unsigned n = f->frame;

    out->lift = 0.0f;
    out->rise = 0.0f;
    out->whiteout = 0.0f;
    out->wisps = 0.0f;
    out->travel = 0.0f;
    out->dirX = f->dirX;
    out->dirZ = f->dirZ;
    out->oldMap = true;
    out->noFade = false;
    switch (f->phase)
    {
    case VOXEL_FLY_IDLE:
        break;
    case VOXEL_FLY_DIVE:
        /* Drawing in with the bird, at full as it reaches the player. */
        out->lift = -VOXEL_FLY_DIP * Smooth(Ramp(n + 1, VOXEL_FLY_DIVE_FRAMES));
        break;
    case VOXEL_FLY_CLIMB:
    {
        /* One motion from drawn in to the high pose: already a step on at
         * frame 0 (the frame before was the dive's end). */
        float u = Ramp(n + 1, VOXEL_FLY_CLIMB_FRAMES);

        out->lift = -VOXEL_FLY_DIP + (1.0f + VOXEL_FLY_DIP) * Smoother(u);
        /* Under way towards the destination, gathering speed. */
        out->travel = u * u;
        out->wisps = Over(u, VOXEL_FLY_WISPS_FROM, VOXEL_FLY_WISPS_OVER);
        out->rise = Over(u, VOXEL_FLY_RISE_FROM, 1.0f - VOXEL_FLY_RISE_FROM);
        out->whiteout = Over(u, VOXEL_FLY_COVER_FROM, 1.0f - VOXEL_FLY_COVER_FROM);
        break;
    }
    case VOXEL_FLY_CLOUD:
        /* Over the old map still at the deck; the new map is built at the
         * player's own pose under the cover (the warm-up waits for every
         * chunk the view needs, and the high view needs the whole town: 3 s
         * of white against the usual quarter second), and the descent then
         * starts from the deck with the wide view's chunks drafted in as it
         * comes down. */
        out->lift = f->gone ? 0.0f : 1.0f;
        out->rise = out->lift;
        out->travel = out->lift;
        out->wisps = f->gone ? 0.0f : 1.0f;
        out->whiteout = 1.0f;
        out->oldMap = !f->gone;
        out->noFade = true;
        break;
    case VOXEL_FLY_SKY:
        /* The scene is drawn over the closed cover; under it the new map is
         * built at the player's own pose, as in CLOUD. */
        out->whiteout = 1.0f;
        out->oldMap = !f->gone;
        out->noFade = true;
        break;
    case VOXEL_FLY_DESCEND:
    {
        /* One motion from the deck down onto the pose. */
        float u = Ramp(n, VOXEL_FLY_DESCEND_FRAMES);

        out->lift = 1.0f - Smoother(u);
        /* Still under way: in from behind the player, slowing onto them. */
        out->travel = -(1.0f - u) * (1.0f - u);
        out->rise = 1.0f - Over(u, 0.0f, VOXEL_FLY_SINK_OVER);

        out->whiteout = 1.0f - Over(u, 0.0f, VOXEL_FLY_UNCOVER_OVER);
        out->wisps = 1.0f - Over(u, VOXEL_FLY_WISPS_GONE_FROM, VOXEL_FLY_WISPS_GONE_OVER);
        out->oldMap = false;
        out->noFade = true;
        break;
    }
    }
}

void VoxelFly_Step(VoxelFly *f, bool fieldShown, bool fieldGone)
{
    ++f->frame;
    switch (f->phase)
    {
    case VOXEL_FLY_IDLE:
        f->frame = 0;
        break;
    case VOXEL_FLY_DIVE:
        if (f->frame >= VOXEL_FLY_DIVE_FRAMES) Go(f, VOXEL_FLY_CLIMB);
        break;
    case VOXEL_FLY_CLIMB:
        if (fieldGone) f->gone = true;
        if (f->frame >= VOXEL_FLY_CLIMB_FRAMES) Go(f, f->sky ? VOXEL_FLY_SKY : VOXEL_FLY_CLOUD);
        break;
    case VOXEL_FLY_CLOUD:
        if (fieldGone) f->gone = true;
        if (f->gone && fieldShown && f->frame >= VOXEL_FLY_CLOUD_MIN)
            Go(f, VOXEL_FLY_DESCEND);
        else if (f->frame >= VOXEL_FLY_CLOUD_MAX)
            Go(f, VOXEL_FLY_IDLE);
        break;
    case VOXEL_FLY_SKY:
        if (fieldGone) f->gone = true;
        if (f->frame >= VOXEL_FLY_SKY_FRAMES && f->gone && fieldShown)
            Go(f, VOXEL_FLY_DESCEND);
        else if (f->frame >= VOXEL_FLY_SKY_FRAMES + VOXEL_FLY_CLOUD_MAX)
            Go(f, VOXEL_FLY_IDLE);
        break;
    case VOXEL_FLY_DESCEND:
        if (f->frame >= VOXEL_FLY_DESCEND_FRAMES) Go(f, VOXEL_FLY_IDLE);
        break;

    }
}
