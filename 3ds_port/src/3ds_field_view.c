/*
 * The part of the overworld on screen, for the map objects (3ds_field_view.h).
 */
#include "3ds_field_view.h"
#include <math.h>
#include <string.h>

#ifndef CTR_FIELD_VIEW_HOST
#include "3ds_platform.h"
#endif

#define DEG_TO_RAD (3.14159265358979323846f / 180.0f)

/*
 * The 2D field. The camera puts the player's metatile at x 192-208 of the
 * 400-pixel screen and at y 96-112 of its 240 (16px above the centre, see
 * FIELD_VIEW_OFFSET_Y): 12 columns either side, 6 rows above and 8 below.
 * A person is two metatiles tall, so one standing in the row under the
 * bottom edge shows its head; one 32 pixels wide shows past the side edge.
 */
#define FLAT_NORTH 6.0f
#define FLAT_SOUTH 9.0f
#define FLAT_HALF_WIDTH 13.0f

/*
 * The voxel camera (voxel_camera.c): a 35-degree vertical field of view on
 * the 400x240 screen, looking down at the player's pitch from a horizontal
 * distance of (8 + scale) / zoom, the scale growing with the map up to 5.
 * The largest is taken: further out, the camera sees further.
 */
#define VOXEL_FOV 35.0f
#define VOXEL_ASPECT (400.0f / 240.0f)
#define VOXEL_DISTANCE 13.0f
/* Ground this far below the player's is seen further off (a terrace down
 * the slope, the beach under a cliff). */
#define VOXEL_GROUND_DROP 1.0f
/* How tall a person stands, for the rows behind the player: the card's head
 * is on screen after its feet have left it. */
#define VOXEL_PERSON_HEIGHT 2.0f
/* The voxel world leaves out the cards further than 24 tiles from the
 * camera's target (voxel_entities.c), which trails the player by a step. */
#define VOXEL_REACH_MAX 22.0f

void CtrFieldView_Flat(CtrFieldView *view)
{
    view->north = FLAT_NORTH;
    view->south = FLAT_SOUTH;
    view->farHalfWidth = FLAT_HALF_WIDTH;
    view->nearHalfWidth = FLAT_HALF_WIDTH;
    view->shiftX = view->shiftY = 0;
}

static float Clamp(float value, float lo, float hi)
{
    return value < lo ? lo : value > hi ? hi : value;
}

void CtrFieldView_Voxel(CtrFieldView *view, float pitchDegrees, float zoomPercent)
{
    float pitch = pitchDegrees * DEG_TO_RAD;
    float halfFov = VOXEL_FOV * 0.5f * DEG_TO_RAD;
    float tanX = tanf(halfFov) * VOXEL_ASPECT;
    float distance = VOXEL_DISTANCE * 100.0f / (zoomPercent > 1.0f ? zoomPercent : 100.0f);
    float height = tanf(pitch) * distance;
    float farHeight = height + VOXEL_GROUND_DROP;
    float nearHeight = height - VOXEL_PERSON_HEIGHT;
    float upper = pitch - halfFov, lower = pitch + halfFov;
    float farGround, nearGround;

    /* The top edge of the picture meets the ground (or reaches the bound,
     * looking at or over the horizon). */
    farGround = upper > 0.01f ? farHeight / tanf(upper) : distance + VOXEL_REACH_MAX;
    /* The bottom edge meets a person's head. */
    nearGround = nearHeight > 0.0f ? nearHeight / tanf(lower) : 0.0f;

    view->north = Clamp(farGround - distance, 0.0f, VOXEL_REACH_MAX);
    view->south = Clamp(distance - nearGround, 0.0f, VOXEL_REACH_MAX);
    /* Half the width of the picture at a point is its depth along the view
     * times the tangent of half the horizontal field of view; a card is a
     * metatile wider than the tile it stands on. */
    view->farHalfWidth = Clamp((farGround * cosf(pitch) + farHeight * sinf(pitch)) * tanX + 1.0f,
                               1.0f, VOXEL_REACH_MAX);
    view->nearHalfWidth = Clamp((nearGround * cosf(pitch) + nearHeight * sinf(pitch)) * tanX + 1.0f,
                                1.0f, VOXEL_REACH_MAX);
    view->shiftX = view->shiftY = 0;
}

float CtrFieldView_Outside(const CtrFieldView *view, int dx, int dy)
{
    float outside = 0.0f;
    float span = view->north + view->south;
    float t, halfWidth, side;

    dx -= view->shiftX;
    dy -= view->shiftY;
    if (dy < -view->north)
        outside = -view->north - dy;
    else if (dy > view->south)
        outside = dy - view->south;
    /* The width at this row, between the far edge's and the near edge's. */
    t = span > 0.0f ? Clamp(((float)dy + view->north) / span, 0.0f, 1.0f) : 0.0f;
    halfWidth = view->farHalfWidth + (view->nearHalfWidth - view->farHalfWidth) * t;
    side = fabsf((float)dx) - halfWidth;
    return side > outside ? side : outside;
}

float CtrFieldView_Score(const CtrFieldView *view, int dx, int dy)
{
    return 2.0f * CtrFieldView_Outside(view, dx, dy) + sqrtf((float)(dx * dx + dy * dy));
}

void CtrFieldView_Window(const CtrFieldView *view, int margin, int *west, int *east, int *north, int *south)
{
    float halfWidth = view->farHalfWidth > view->nearHalfWidth ? view->farHalfWidth : view->nearHalfWidth;
    int sideways = (int)ceilf(halfWidth) + margin;

    *west = sideways - view->shiftX;
    *east = sideways + view->shiftX;
    *north = (int)ceilf(view->north) + margin - view->shiftY;
    *south = (int)ceilf(view->south) + margin + view->shiftY;
}

float CtrFieldView_Limit(float at, float before, float after, float low, float high, unsigned closed)
{
    float least = low + before, most = high - after;

    /* Both ends closed and the map narrower than the view: in the middle. */
    if ((closed & CTR_FIELD_EDGE_LOW) && (closed & CTR_FIELD_EDGE_HIGH) && least > most)
        return (low + high + before - after) * 0.5f;
    if ((closed & CTR_FIELD_EDGE_LOW) && at < least)
        return least;
    if ((closed & CTR_FIELD_EDGE_HIGH) && at > most)
        return most;
    return at;
}

#ifndef CTR_FIELD_VIEW_HOST
static CtrFieldView sView;
static unsigned sGeneration;
static int sKey = -1;
static int sFlatShiftX, sFlatShiftY, sVoxelShiftX, sVoxelShiftY;

void CtrFieldView_SetFlatShift(int x, int y)
{
    sFlatShiftX = x;
    sFlatShiftY = y;
}

void CtrFieldView_SetVoxelShift(int x, int y)
{
    sVoxelShiftX = x;
    sVoxelShiftY = y;
}

const CtrFieldView *CtrFieldView_Current(void)
{
    /* The voxel option rather than whether this map is drawn in 3D: the
     * objects are spawned while a map loads, before its world exists. A map
     * the voxel world does not draw is shown in 2D within the larger view. */
#if CTR_VOXEL_ENABLED
    bool voxel = CtrSettings_Voxel();
    int pitch = CtrSettings_VoxelPitch(), zoom = CtrSettings_VoxelZoom();
#else
    bool voxel = false;
    int pitch = 0, zoom = 0;
#endif
    int key = voxel ? 1 + ((pitch & 0xFF) << 1) + ((zoom & 0x3FF) << 9) : 0;
    int shiftX = voxel ? sVoxelShiftX : sFlatShiftX, shiftY = voxel ? sVoxelShiftY : sFlatShiftY;

    if (key != sKey || shiftX != sView.shiftX || shiftY != sView.shiftY)
    {
        CtrFieldView view;

        if (voxel)
            CtrFieldView_Voxel(&view, (float)pitch, (float)zoom);
        else
            CtrFieldView_Flat(&view);
        view.shiftX = shiftX;
        view.shiftY = shiftY;
        if (sKey < 0 || memcmp(&view, &sView, sizeof(view)) != 0)
            ++sGeneration;
        sView = view;
        sKey = key;
    }
    return &sView;
}

unsigned CtrFieldView_Generation(void)
{
    CtrFieldView_Current();
    return sGeneration;
}
#endif
