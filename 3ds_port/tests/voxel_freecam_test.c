/*
 * The free-roam camera (src/voxel/voxel_freecam.c): the attitude and its
 * clamps, the blend between the orbit and the head, the third-person boom
 * and its collision, the camera-relative steering and the turn tap.
 * SDK-free; run by `make verify`.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "3ds_input.h"
#include "voxel_freecam.h"

static int sFailures;

#define CHECK(cond) \
    do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++sFailures; } } while (0)
#define NEAR(a, b, eps) (fabsf((float)(a) - (float)(b)) <= (eps))

static const float PI = 3.14159265358979f;

/* A world with a wall: solid from x >= wallX up to wallTop, and the ground. */
typedef struct { float wallX, wallTop; unsigned calls; } Wall;

static bool WallBlocked(void *context, float x, float y, float z)
{
    Wall *w = context;

    (void)z;
    ++w->calls;
    if (y < 0.35f)
        return true;
    return x >= w->wallX && y < w->wallTop;
}

/* Runs the frames the game would: the tick, then the placement (which is
 * what sets the boom's wanted length, as ThirdPerson.place does). */
static VoxelFreeCamBlocked sSettleBlocked;
static void *sSettleContext;

static void Settle(VoxelFreeCam *fc, bool engaged, bool third, float seconds)
{
    static const float orbitEye[3] = {0, 9, 10}, orbitFocus[3] = {0, 0, 0};
    static const float head[3] = {0.0f, 1.4f, 0.0f};
    VoxelFreeCamView view;

    for (float t = 0.0f; t < seconds; t += 1.0f / 60.0f)
    {
        VoxelFreeCam_Tick(fc, 1.0f / 60.0f, engaged, third, 0.0f, true);
        VoxelFreeCam_Frame(fc, head, orbitEye, orbitFocus, 35.0f, sSettleBlocked, sSettleContext, &view);
    }
}

static void TestSteerMatchesInputThresholds(void)
{
    /* The steering has its own copy of the Circle Pad's thresholds; a stick
     * deflected just under ENTER must do nothing, just over must move. */
    CHECK(VoxelFreeCam_Steer(VOXEL_FC_DIR_NONE, 0.0f, 0.0f, CTR_CIRCLE_ENTER - 1) == VOXEL_FC_DIR_NONE);
    CHECK(VoxelFreeCam_Steer(VOXEL_FC_DIR_NONE, 0.0f, 0.0f, CTR_CIRCLE_ENTER + 1) == VOXEL_FC_DIR_SOUTH);
    /* Once moving it holds until LEAVE. */
    CHECK(VoxelFreeCam_Steer(VOXEL_FC_DIR_SOUTH, 0.0f, 0.0f, CTR_CIRCLE_LEAVE + 1) == VOXEL_FC_DIR_SOUTH);
    CHECK(VoxelFreeCam_Steer(VOXEL_FC_DIR_SOUTH, 0.0f, 0.0f, CTR_CIRCLE_LEAVE - 1) == VOXEL_FC_DIR_NONE);
}

static void TestSteerRotatesWithYaw(void)
{
    /* yaw 0 looks south: forward is south, right hand is west. */
    CHECK(VoxelFreeCam_Steer(0, 0.0f, 0.0f, 100.0f) == VOXEL_FC_DIR_SOUTH);
    CHECK(VoxelFreeCam_Steer(0, 0.0f, 100.0f, 0.0f) == VOXEL_FC_DIR_WEST);
    CHECK(VoxelFreeCam_Steer(0, 0.0f, 0.0f, -100.0f) == VOXEL_FC_DIR_NORTH);
    CHECK(VoxelFreeCam_Steer(0, 0.0f, -100.0f, 0.0f) == VOXEL_FC_DIR_EAST);
    /* looking east (yaw pi/2): forward is east, right is south. */
    CHECK(VoxelFreeCam_Steer(0, PI / 2, 0.0f, 100.0f) == VOXEL_FC_DIR_EAST);
    CHECK(VoxelFreeCam_Steer(0, PI / 2, 100.0f, 0.0f) == VOXEL_FC_DIR_SOUTH);
    /* looking north (yaw pi): forward is north. */
    CHECK(VoxelFreeCam_Steer(0, PI, 0.0f, 100.0f) == VOXEL_FC_DIR_NORTH);
    /* looking west (yaw -pi/2): forward is west. */
    CHECK(VoxelFreeCam_Steer(0, -PI / 2, 0.0f, 100.0f) == VOXEL_FC_DIR_WEST);
    /* The same wait the head's compass facing gives for the same yaw. */
    for (int i = 0; i < 360; i += 7)
    {
        float yaw = i * PI / 180.0f - PI;
        int facing = VoxelFreeCam_CompassFacing(yaw);
        int walk = VoxelFreeCam_Steer(0, yaw, 0.0f, 100.0f);

        CHECK(walk == facing);
    }
}

static void TestSteerHysteresis(void)
{
    /* Near a diagonal the axis already chosen is kept, as the Circle Pad's
     * own decoding does: yaw 45 degrees, forward leans on both axes. */
    float yaw = PI / 4.0f + 0.02f;   /* a hair towards east */
    int first = VoxelFreeCam_Steer(0, yaw, 0.0f, 100.0f);
    int kept = VoxelFreeCam_Steer(VOXEL_FC_DIR_SOUTH, yaw, 0.0f, 100.0f);

    CHECK(first == VOXEL_FC_DIR_EAST);
    CHECK(kept == VOXEL_FC_DIR_SOUTH);
    /* ... and past the hysteresis it does change. */
    CHECK(VoxelFreeCam_Steer(VOXEL_FC_DIR_SOUTH, PI / 2, 0.0f, 100.0f) == VOXEL_FC_DIR_EAST);
}

static void TestCompassFacing(void)
{
    CHECK(VoxelFreeCam_CompassFacing(0.0f) == VOXEL_FC_DIR_SOUTH);
    CHECK(VoxelFreeCam_CompassFacing(PI / 2) == VOXEL_FC_DIR_EAST);
    CHECK(VoxelFreeCam_CompassFacing(PI) == VOXEL_FC_DIR_NORTH);
    CHECK(VoxelFreeCam_CompassFacing(-PI / 2) == VOXEL_FC_DIR_WEST);
    CHECK(VoxelFreeCam_CompassFacing(PI / 4 - 0.01f) == VOXEL_FC_DIR_SOUTH);
    CHECK(VoxelFreeCam_CompassFacing(PI / 4 + 0.01f) == VOXEL_FC_DIR_EAST);
    for (int dir = 1; dir <= 4; ++dir)
        CHECK(VoxelFreeCam_CompassFacing(VoxelFreeCam_FacingAngle(dir)) == dir);
}

static void TestTurnTap(void)
{
    VoxelFreeCamTap tap = {0};

    /* Facing what the head looks at: nothing to do. */
    CHECK(VoxelFreeCam_TurnTap(&tap, true, VOXEL_FC_DIR_SOUTH, VOXEL_FC_DIR_SOUTH) == VOXEL_FC_DIR_NONE);
    /* Not free (mid-step, a script): never taps. */
    CHECK(VoxelFreeCam_TurnTap(&tap, false, VOXEL_FC_DIR_SOUTH, VOXEL_FC_DIR_EAST) == VOXEL_FC_DIR_NONE);
    /* Free: one frame of the wanted direction, then quiet. */
    CHECK(VoxelFreeCam_TurnTap(&tap, true, VOXEL_FC_DIR_SOUTH, VOXEL_FC_DIR_EAST) == VOXEL_FC_DIR_EAST);
    for (int i = 0; i < 6; ++i)
        CHECK(VoxelFreeCam_TurnTap(&tap, true, VOXEL_FC_DIR_SOUTH, VOXEL_FC_DIR_EAST) == VOXEL_FC_DIR_NONE);
    /* The turn did not take: it tries again. */
    CHECK(VoxelFreeCam_TurnTap(&tap, true, VOXEL_FC_DIR_SOUTH, VOXEL_FC_DIR_EAST) == VOXEL_FC_DIR_EAST);
    /* Once it faces the head, the cooldown is forgotten. */
    CHECK(VoxelFreeCam_TurnTap(&tap, true, VOXEL_FC_DIR_EAST, VOXEL_FC_DIR_EAST) == VOXEL_FC_DIR_NONE);
    CHECK(VoxelFreeCam_TurnTap(&tap, true, VOXEL_FC_DIR_EAST, VOXEL_FC_DIR_NORTH) == VOXEL_FC_DIR_NORTH);
}

static void TestLookClamps(void)
{
    VoxelFreeCam fc;

    VoxelFreeCam_Init(&fc);
    VoxelFreeCam_LookBy(&fc, 0.0f, 10.0f);
    CHECK(NEAR(fc.pitch, VOXEL_FC_PITCH_DOWN, 1e-6f));
    VoxelFreeCam_LookBy(&fc, 0.0f, -10.0f);
    CHECK(NEAR(fc.pitch, VOXEL_FC_PITCH_UP, 1e-6f));
    /* The yaw wraps to (-pi, pi]. */
    fc.yaw = 0.0f;
    VoxelFreeCam_LookBy(&fc, 3.0f * PI, 0.0f);
    CHECK(fc.yaw >= -PI - 1e-4f && fc.yaw <= PI + 1e-4f);
    CHECK(NEAR(fabsf(fc.yaw), PI, 1e-3f));
}

static void TestStickLook(void)
{
    VoxelFreeCam fc;
    float yaw;

    VoxelFreeCam_Init(&fc);
    fc.driving = true;
    VoxelFreeCam_Tick(&fc, 1.0f / 60.0f, true, false, 0.0f, true);
    yaw = fc.yaw;
    /* Inside the dead zone: nothing. */
    VoxelFreeCam_SetLook(&fc, VOXEL_FC_STICK_DEAD * 0.9f, 0.0f, 0.0f);
    Settle(&fc, true, false, 0.5f);
    CHECK(fc.yaw == yaw);
    /* Pushed right: the yaw decreases (a right turn), at the full rate. */
    VoxelFreeCam_SetLook(&fc, 1.0f, 0.0f, 0.0f);
    VoxelFreeCam_Tick(&fc, 0.1f, true, false, 0.0f, true);
    CHECK(NEAR(fc.yaw, yaw - VOXEL_FC_STICK_YAW * 0.1f, 1e-4f));
    /* Pushed up: the head tilts up (pitch decreases). */
    VoxelFreeCam_SetLook(&fc, 0.0f, 1.0f, 0.0f);
    float pitch = fc.pitch;
    VoxelFreeCam_Tick(&fc, 0.1f, true, false, 0.0f, true);
    CHECK(fc.pitch < pitch);
    /* Not driving (a menu is up): the sticks are ignored. */
    fc.driving = false;
    yaw = fc.yaw;
    VoxelFreeCam_SetLook(&fc, 1.0f, 0.0f, 1.0f);
    VoxelFreeCam_Tick(&fc, 0.1f, true, false, 0.0f, true);
    CHECK(fc.yaw == yaw);
}

static void TestEnteringTakesFacing(void)
{
    VoxelFreeCam fc;

    VoxelFreeCam_Init(&fc);
    VoxelFreeCam_Tick(&fc, 1.0f / 60.0f, true, false, PI / 2, true);
    CHECK(NEAR(fc.yaw, PI / 2, 1e-5f));
    CHECK(NEAR(fc.pitch, VOXEL_FC_PITCH_REST, 1e-5f));
    /* The yaw is the player's from then on, not reset every frame. */
    VoxelFreeCam_LookBy(&fc, 0.5f, 0.0f);
    VoxelFreeCam_Tick(&fc, 1.0f / 60.0f, true, false, 0.0f, true);
    CHECK(NEAR(fc.yaw, PI / 2 + 0.5f, 1e-5f));
}

static void TestBlend(void)
{
    VoxelFreeCam fc;
    float orbitEye[3] = {10.0f, 9.0f, 20.0f}, orbitFocus[3] = {10.0f, 0.0f, 10.0f};
    float head[3] = {10.0f, 1.4f, 10.0f};
    VoxelFreeCamView view;

    VoxelFreeCam_Init(&fc);
    /* Off: the orbit, untouched. */
    VoxelFreeCam_Tick(&fc, 1.0f / 60.0f, false, false, 0.0f, true);
    VoxelFreeCam_Frame(&fc, head, orbitEye, orbitFocus, 35.0f, NULL, NULL, &view);
    CHECK(NEAR(view.eye[1], 9.0f, 1e-5f) && NEAR(view.focus[2], 10.0f, 1e-5f));
    CHECK(NEAR(view.fov, 35.0f, 1e-5f) && view.eased == 0.0f && view.cardBlend == 0.0f);

    /* Not ready (no terrain yet): the blend waits. */
    VoxelFreeCam_Tick(&fc, 0.2f, true, false, 0.0f, false);
    CHECK(fc.blend == 0.0f);

    /* On: it dives into the head over BLEND_TIME. */
    Settle(&fc, true, false, VOXEL_FC_BLEND_TIME * 0.5f);
    CHECK(fc.blend > 0.4f && fc.blend < 0.6f);
    Settle(&fc, true, false, VOXEL_FC_BLEND_TIME);
    CHECK(fc.blend == 1.0f);
    VoxelFreeCam_Frame(&fc, head, orbitEye, orbitFocus, 35.0f, NULL, NULL, &view);
    CHECK(NEAR(view.eye[0], head[0], 1e-5f) && NEAR(view.eye[1], head[1], 1e-5f) && NEAR(view.eye[2], head[2], 1e-5f));
    CHECK(NEAR(view.fov, VOXEL_FC_FOV, 1e-4f));
    /* The aim is FOCUS_DIST along the look direction. */
    {
        float lx, ly, lz;

        VoxelFreeCam_LookDir(&fc, &lx, &ly, &lz);
        CHECK(NEAR(view.focus[0], head[0] + lx * VOXEL_FC_FOCUS_DIST, 1e-5f));
        CHECK(NEAR(view.focus[1], head[1] + ly * VOXEL_FC_FOCUS_DIST, 1e-5f));
        CHECK(NEAR(view.focus[2], head[2] + lz * VOXEL_FC_FOCUS_DIST, 1e-5f));
        CHECK(NEAR(lx * lx + ly * ly + lz * lz, 1.0f, 1e-5f));
    }
    CHECK(VoxelFreeCam_HidePlayer(&fc));

    /* Off again: it flies back out. */
    Settle(&fc, false, false, VOXEL_FC_BLEND_TIME + 0.1f);
    CHECK(fc.blend == 0.0f);
    VoxelFreeCam_Frame(&fc, head, orbitEye, orbitFocus, 35.0f, NULL, NULL, &view);
    CHECK(NEAR(view.eye[1], 9.0f, 1e-5f));
}

static void TestBoomOpenWorld(void)
{
    VoxelFreeCam fc;
    float orbitEye[3] = {0, 9, 10}, orbitFocus[3] = {0, 0, 0};
    float head[3] = {0.0f, 1.4f, 0.0f};
    VoxelFreeCamView view;
    float lx, ly, lz;

    VoxelFreeCam_Init(&fc);
    fc.yaw = 0.0f;
    fc.pitch = VOXEL_FC_PITCH_REST;
    Settle(&fc, true, true, 2.0f);
    CHECK(fc.out == 1.0f);
    VoxelFreeCam_Frame(&fc, head, orbitEye, orbitFocus, 35.0f, NULL, NULL, &view);
    VoxelFreeCam_LookDir(&fc, &lx, &ly, &lz);
    CHECK(NEAR(fc.len, VOXEL_FC_BOOM, 1e-4f));
    /* The eye is the pivot pushed back along the look, plus the shoulder:
     * looking south (+z) the eye stands north of the player, and the
     * shoulder (right hand = west) moves it a little west. */
    CHECK(view.eye[2] < head[2] - VOXEL_FC_BOOM * 0.9f);
    CHECK(view.eye[0] > -VOXEL_FC_SHOULDER - 1e-4f && view.eye[0] < 0.0f);
    /* It looks at the player: the head lies in front of the eye. */
    {
        float dx = head[0] - view.eye[0], dy = (head[1] + VOXEL_FC_PIVOT_LIFT) - view.eye[1], dz = head[2] - view.eye[2];
        float dot = (dx * lx + dy * ly + dz * lz) / sqrtf(dx * dx + dy * dy + dz * dz);

        CHECK(dot > 0.99f);
    }
    CHECK(VoxelFreeCam_ShowsPlayer(&fc));
    CHECK(!VoxelFreeCam_HidePlayer(&fc));

    /* The player's zoom: the boom follows it. */
    VoxelFreeCam_SetZoom(&fc, 0.5f);
    Settle(&fc, true, true, 3.0f);   /* the zoom eases exponentially (VOXEL_FC_ZOOM_TIME) */
    VoxelFreeCam_Frame(&fc, head, orbitEye, orbitFocus, 35.0f, NULL, NULL, &view);
    CHECK(NEAR(fc.want, VOXEL_FC_BOOM * 0.5f, 1e-4f));
    VoxelFreeCam_SetZoom(&fc, 100.0f);
    CHECK(fc.zoomGoal == VOXEL_FC_ZOOM_MAX);
    VoxelFreeCam_SetZoom(&fc, 0.0f);
    CHECK(fc.zoomGoal == VOXEL_FC_ZOOM_MIN);

    /* Back to 1ST: the boom draws in, and the card hides again. */
    Settle(&fc, true, false, 1.0f);
    VoxelFreeCam_Frame(&fc, head, orbitEye, orbitFocus, 35.0f, NULL, NULL, &view);
    CHECK(fc.out == 0.0f);
    CHECK(NEAR(view.eye[2], head[2], 1e-5f));
    CHECK(VoxelFreeCam_HidePlayer(&fc));
}

static void TestBoomCollision(void)
{
    VoxelFreeCam fc;
    float orbitEye[3] = {0, 9, 10}, orbitFocus[3] = {0, 0, 0};
    float head[3] = {0.0f, 1.4f, 0.0f};
    VoxelFreeCamView view;
    /* Looking east (yaw pi/2), the boom goes west. A wall stands 2 tiles west. */
    Wall wall = {-2.0f, 6.0f, 0};

    /* Looking west would put the eye east: the wall is at x <= -2 instead. */
    wall.wallX = 0.0f;
    VoxelFreeCam_Init(&fc);
    Settle(&fc, true, true, 2.0f);
    fc.yaw = -PI / 2;          /* look west: the eye goes east of the player */
    fc.pitch = 0.0f;
    /* A wall at x = 2, as tall as the boom. */
    wall.wallX = 2.0f;
    VoxelFreeCam_Frame(&fc, head, orbitEye, orbitFocus, 35.0f, WallBlocked, &wall, &view);
    CHECK(wall.calls > 0);
    CHECK(view.eye[0] < 2.0f - VOXEL_FC_PAD + 1e-3f);
    CHECK(view.eye[0] > 1.0f);   /* close to the wall, not squashed into the head */
    CHECK(fc.len < VOXEL_FC_BOOM - 1.0f);

    /* Over the wall: a low one is passed. */
    wall.wallTop = 0.5f;          /* below the pivot */
    fc.len = 0.0f;
    VoxelFreeCam_Tick(&fc, 1.0f, true, true, 0.0f, true);   /* a second of recovery */
    VoxelFreeCam_Frame(&fc, head, orbitEye, orbitFocus, 35.0f, WallBlocked, &wall, &view);
    CHECK(NEAR(fc.len, VOXEL_FC_BOOM, 1e-3f));

    /* Recovery is rationed: a camera in a wall comes back out at RETURN. */
    wall.wallTop = 6.0f;
    VoxelFreeCam_Frame(&fc, head, orbitEye, orbitFocus, 35.0f, WallBlocked, &wall, &view);
    float squeezed = fc.len;
    wall.wallTop = 0.5f;
    VoxelFreeCam_Tick(&fc, 0.1f, true, true, 0.0f, true);
    CHECK(NEAR(fc.len, squeezed + VOXEL_FC_RETURN * 0.1f, 1e-3f));

    /* Boxed in completely: the boom collapses into the head and the card
     * comes back out (it would fill the lens from inside). */
    wall.wallX = -100.0f;         /* solid everywhere ... */
    wall.wallTop = 6.0f;          /* ... as high as the pivot */
    VoxelFreeCam_Frame(&fc, head, orbitEye, orbitFocus, 35.0f, WallBlocked, &wall, &view);
    CHECK(fc.len < VOXEL_FC_SHOW_AT);
    CHECK(!VoxelFreeCam_ShowsPlayer(&fc));
    CHECK(VoxelFreeCam_HidePlayer(&fc));
    CHECK(view.eye[0] >= head[0] - 1e-4f);
}

static void TestReachBisects(void)
{
    /* The answer is the wall's own position (minus the pad), not the
     * sampling grid's. */
    Wall wall = {1.37f, 100.0f, 0};
    float pivot[3] = {0.0f, 5.0f, 0.0f};
    float reach = VoxelFreeCam_Reach(pivot, 1.0f, 0.0f, 0.0f, 4.0f, WallBlocked, &wall);

    CHECK(NEAR(reach, 1.37f - VOXEL_FC_PAD, 1.0f / 16.0f));
    CHECK(VoxelFreeCam_Reach(pivot, 1.0f, 0.0f, 0.0f, 4.0f, NULL, NULL) == 4.0f);
    CHECK(VoxelFreeCam_Reach(pivot, 1.0f, 0.0f, 0.0f, 0.0f, WallBlocked, &wall) == 0.0f);
}

static void TestCardYaw(void)
{
    float eye[3] = {5.0f, 1.0f, 8.0f};

    /* An eye due south of a card: yaw 0 (the orbit's default). */
    CHECK(NEAR(VoxelFreeCam_CardYaw(eye, 5.0f, 2.0f), 0.0f, 1e-6f));
    /* East of it: pi/2. */
    CHECK(NEAR(VoxelFreeCam_CardYaw(eye, 1.0f, 8.0f), PI / 2, 1e-6f));
    /* On top of it: no preference. */
    CHECK(VoxelFreeCam_CardYaw(eye, 5.0f, 8.0f) == 0.0f);
}

int main(void)
{
    TestSteerMatchesInputThresholds();
    TestSteerRotatesWithYaw();
    TestSteerHysteresis();
    TestCompassFacing();
    TestTurnTap();
    TestLookClamps();
    TestStickLook();
    TestEnteringTakesFacing();
    TestBlend();
    TestBoomOpenWorld();
    TestBoomCollision();
    TestReachBisects();
    TestCardYaw();
    if (sFailures != 0)
    {
        printf("voxel_freecam_test: %d failure(s)\n", sFailures);
        return 1;
    }
    printf("voxel_freecam_test: ok\n");
    return 0;
}
