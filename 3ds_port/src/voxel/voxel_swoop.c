/*
 * The swoop between the voxel overworld and the 2D field: a pop-up book.
 * See voxel_swoop.h; the camera's top-down pose is voxel_camera.c's
 * (VoxelCamera_Swoop) and the height squash is ctr_voxel.c's (CameraMatrices).
 *
 * Voxel to 2D: the camera tilts up to the top-down orthographic pose, whose
 * ground framing is the 2D field's 1:1 (TILT_OUT); the world then flattens
 * into the map (FLATTEN), a flash goes up as it lands, and the 2D field is
 * under the flash as it dies (FLASH_OUT). 2D to voxel is the reverse: the
 * flash (FLASH_IN), the world rising out of the map under the fading flash
 * (RISE), then the camera tilting down to the player's pose (TILT_IN).
 */

#include "voxel_swoop.h"
#include "voxel_camera.h"

/* ── Tunables, frames at 60 Hz ───────────────────────────────────────────────
 * Unhurried on purpose: the user likes the smooth swoop (about 1.25 s a way). */
/* The camera's tilt between the player's pose and the top-down one. */
#define VOXEL_SWOOP_TILT_FRAMES 50u
/* ... and going out to the 2D field, where a longer look was wanted. */
#define VOXEL_SWOOP_TILT_OUT_FRAMES 70u
/* The crossfade between the oblique voxel world and the 2D field (and, with
 * VOXEL_SWOOP_SQUASH, the world flattening into the map). It runs together
 * with the tilt, one motion: it starts this many frames into the tilt going
 * out, and the tilt starts this many frames into it coming in. */
#define VOXEL_SWOOP_HEIGHT_FRAMES 30u
/* Going out, the crossfade starts this far into the tilt (so the swoop is
 * seen first and the fade lands with it); coming in, the tilt starts this
 * far into the crossfade (so most of the swoop is seen sharp). */
#define VOXEL_SWOOP_OUT_FADE_LAG 40u
#define VOXEL_SWOOP_IN_TILT_LAG 10u
/* 1: the pop-up book, the world squashed to the ground under the crossfade;
 * 0: the world stands, the crossfade alone takes it to the 2D picture. */
#define VOXEL_SWOOP_SQUASH 0
/* The flash comes up over the last frames of the flattening, so it peaks as
 * the world lands flat on the 2D picture. */
#define VOXEL_SWOOP_FLASH_RISE 0u
/* The flash at full, the picture under it already the other renderer's. */
#define VOXEL_SWOOP_FLASH_HOLD 1u /* at least 1: the frame the renderer swaps */
/* The flash dying away over the new picture. */
#define VOXEL_SWOOP_FLASH_FADE 0u
/* The crossfade's softening: both pictures blurred this much (0..1, the
 * taps' own strength) at the middle of the fade, nothing at either end. */
#define VOXEL_SWOOP_BLUR 1.8f
/* The flash's strength, 0 for none (judged too much on top of the squash). */
#define VOXEL_SWOOP_FLASH 0.0f
/* The flash's colour: a warm white, as the 3D battle's landing. */
#define VOXEL_SWOOP_FLASH_R 255
#define VOXEL_SWOOP_FLASH_G 250
#define VOXEL_SWOOP_FLASH_B 235
/* Rising out of the map, the world's light starts this far towards white and
 * rings down as it stands up. */
#define VOXEL_SWOOP_WHITEOUT 0.0f /* 0 since the crossfade is now between two pictures of one colour */
/* The cards (object events, as billboards) squash to nothing, so below this
 * height the game's own 2D sprites stand in for them, where the 2D field
 * puts them: the camera is top-down and 1:1 by then, so they coincide. */
#define VOXEL_SWOOP_CARD_HEIGHT -1.0f /* -1: never; the cards lie down with the camera instead (voxel_entities.c) */
/* The cast shadows, upright silhouettes laid on the ground, are streaks from
 * above: gone once the camera has tilted this far towards top-down. */
#define VOXEL_SWOOP_SHADOW_TILT 0.3f

static float EaseOut(float x)
{
    float k;

    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;
    k = 1.0f - x;
    return 1.0f - k * k * k;
}

static float EaseInOut(float x)
{
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;
    return x < 0.5f ? 4.0f * x * x * x : 1.0f - 4.0f * (1.0f - x) * (1.0f - x) * (1.0f - x);
}

static float Ramp(unsigned frame, unsigned frames)
{
    return frames == 0 ? 1.0f : (float)frame / (float)frames;
}

static void Go(VoxelSwoop *s, VoxelSwoopPhase phase)
{
    s->phase = phase;
    s->frame = 0;
}

void VoxelSwoop_Begin(VoxelSwoop *s, bool toVoxel, bool canSwoop)
{
    Go(s, toVoxel ? VOXEL_SWOOP_FLASH_IN : canSwoop ? VOXEL_SWOOP_TILT_OUT : VOXEL_SWOOP_FLASH_OUT);
}

void VoxelSwoop_Cancel(VoxelSwoop *s)
{
    Go(s, VOXEL_SWOOP_IDLE);
}

void VoxelSwoop_LoseVoxel(VoxelSwoop *s)
{
    if (s->phase == VOXEL_SWOOP_TILT_OUT || s->phase == VOXEL_SWOOP_FLATTEN)
        Go(s, VOXEL_SWOOP_FLASH_OUT);
}

bool VoxelSwoop_ShowsVoxel(const VoxelSwoop *s)
{
    return s->phase == VOXEL_SWOOP_TILT_OUT || s->phase == VOXEL_SWOOP_FLATTEN;
}

bool VoxelSwoop_Active(const VoxelSwoop *s)
{
    return s->phase != VOXEL_SWOOP_IDLE;
}

void VoxelSwoop_Frame(const VoxelSwoop *s, VoxelSwoopFrame *out)
{
    unsigned f = s->frame;

    out->camera = 0.0f;
    out->height = 1.0f;
    out->fade2d = 0.0f;
    out->blur = 0.0f;
    out->flash = 0.0f;
    out->bloom = 0.0f;
    out->whiteout = 0.0f;
    out->cards = true;
    out->shadows = true;
    out->colour = (uint32_t)VOXEL_SWOOP_FLASH_R | ((uint32_t)VOXEL_SWOOP_FLASH_G << 8)
                | ((uint32_t)VOXEL_SWOOP_FLASH_B << 16) | 0xFF000000u;
    switch (s->phase)
    {
    case VOXEL_SWOOP_IDLE:
        break;
    case VOXEL_SWOOP_TILT_OUT:
    case VOXEL_SWOOP_FLATTEN:
        /* One motion: the tilt from frame 0 (already a step up: the frame
         * before was the player's pose), the crossfade from FADE_LAG. */
        out->camera = EaseInOut(Ramp(f + 1, VOXEL_SWOOP_TILT_OUT_FRAMES));
        out->fade2d = f >= VOXEL_SWOOP_OUT_FADE_LAG
                    ? EaseInOut(Ramp(f + 1 - VOXEL_SWOOP_OUT_FADE_LAG, VOXEL_SWOOP_HEIGHT_FRAMES)) : 0.0f;
        out->height = 1.0f - out->camera * (1.0f - VOXEL_SWOOP_HEIGHT);
        if (VOXEL_SWOOP_SQUASH) out->height *= 1.0f - out->fade2d;
        out->shadows = out->camera < VOXEL_SWOOP_SHADOW_TILT;
        out->cards = out->height >= VOXEL_SWOOP_CARD_HEIGHT;
        break;
    case VOXEL_SWOOP_FLASH_OUT:
        out->flash = 1.0f;
        if (f >= VOXEL_SWOOP_FLASH_HOLD)
        {
            float fade = 1.0f - EaseOut(Ramp(f - VOXEL_SWOOP_FLASH_HOLD, VOXEL_SWOOP_FLASH_FADE));

            out->flash = fade;
            out->bloom = fade;
        }
        break;
    case VOXEL_SWOOP_FLASH_IN:
        out->camera = 1.0f;
        out->fade2d = 1.0f;
        out->height = VOXEL_SWOOP_SQUASH ? 0.0f : VOXEL_SWOOP_HEIGHT;
        out->flash = 1.0f;
        out->shadows = false;
        out->cards = false;
        break;
    case VOXEL_SWOOP_RISE:
    case VOXEL_SWOOP_TILT_IN:
    {
        /* One motion: the crossfade from frame 0, the tilt from FADE_LAG. */
        float fade = 1.0f - EaseOut(Ramp(f, VOXEL_SWOOP_FLASH_FADE));
        float tilt = f >= VOXEL_SWOOP_IN_TILT_LAG
                   ? EaseInOut(Ramp(f + 1 - VOXEL_SWOOP_IN_TILT_LAG, VOXEL_SWOOP_TILT_FRAMES)) : 0.0f;

        out->camera = 1.0f - tilt;
        out->fade2d = 1.0f - EaseInOut(Ramp(f + 1, VOXEL_SWOOP_HEIGHT_FRAMES));
        out->height = 1.0f - out->camera * (1.0f - VOXEL_SWOOP_HEIGHT);
        if (VOXEL_SWOOP_SQUASH) out->height *= 1.0f - out->fade2d;
        out->shadows = out->camera < VOXEL_SWOOP_SHADOW_TILT;
        out->cards = out->height >= VOXEL_SWOOP_CARD_HEIGHT;
        out->flash = fade;
        out->bloom = fade;
        out->whiteout = VOXEL_SWOOP_WHITEOUT * out->fade2d;
        break;
    }
    }
    /* Softest halfway through the crossfade, sharp at both ends. */
    out->blur = VOXEL_SWOOP_BLUR * 4.0f * out->fade2d * (1.0f - out->fade2d);
    out->flash *= VOXEL_SWOOP_FLASH;
    out->bloom *= VOXEL_SWOOP_FLASH;
}

void VoxelSwoop_Step(VoxelSwoop *s)
{
    /* The merged phases' lengths: the later of the two motions' ends. */
    unsigned out = VOXEL_SWOOP_OUT_FADE_LAG + VOXEL_SWOOP_HEIGHT_FRAMES;
    unsigned in = VOXEL_SWOOP_IN_TILT_LAG + VOXEL_SWOOP_TILT_FRAMES;

    if (out < VOXEL_SWOOP_TILT_OUT_FRAMES) out = VOXEL_SWOOP_TILT_OUT_FRAMES;
    if (in < VOXEL_SWOOP_HEIGHT_FRAMES) in = VOXEL_SWOOP_HEIGHT_FRAMES;
    ++s->frame;
    switch (s->phase)
    {
    case VOXEL_SWOOP_IDLE:
        s->frame = 0;
        break;
    case VOXEL_SWOOP_TILT_OUT:
    case VOXEL_SWOOP_FLATTEN:
        if (s->frame >= out) Go(s, VOXEL_SWOOP_FLASH_OUT);
        break;
    case VOXEL_SWOOP_FLASH_OUT:
        if (s->frame >= VOXEL_SWOOP_FLASH_HOLD + VOXEL_SWOOP_FLASH_FADE) Go(s, VOXEL_SWOOP_IDLE);
        break;
    case VOXEL_SWOOP_FLASH_IN:
        if (s->frame >= VOXEL_SWOOP_FLASH_HOLD) Go(s, VOXEL_SWOOP_RISE);
        break;
    case VOXEL_SWOOP_RISE:
    case VOXEL_SWOOP_TILT_IN:
        if (s->frame >= in) Go(s, VOXEL_SWOOP_IDLE);
        break;
    }
}
