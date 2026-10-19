/*
 * Host test of the free walking (voxel_freemove.c) against a model of the
 * game's grid: a step is asked for by pressing a direction, the game turns
 * the player first when it faces another way, then steps - the tile changes
 * at once and the step takes `stepFrames` before the next one can start -
 * and refuses walls. What is checked is what the player sees: P = tile +
 * offset moves smoothly, never enters a wall, slides along one, and does not
 * jitter or step on its own.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "voxel_freemove.h"

#define W 24
#define H 24

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct
{
    unsigned char wall[H][W];
    int gx, gz;
    int facing;
    int busy;       /* frames until the next step may start */
    int stepFrames; /* the game's frames per tile */
    int steps;      /* steps the game took */
    int presses;    /* frames a direction was pressed */
    int bumps;      /* presses the game refused */
    int turnFrames; /* frames of walking on the spot to turn to a new heading */
    bool autoFace;  /* the bridge tells the game it already faces the heading */
    int turns;      /* turns the game made */
} Game;

static bool IsWall(const Game *g, int x, int z)
{
    return x < 0 || z < 0 || x >= W || z >= H || g->wall[z][x];
}

static void Delta(int dir, int *dx, int *dz)
{
    *dx = *dz = 0;
    if (dir == VOXEL_FM_SOUTH) *dz = 1;
    else if (dir == VOXEL_FM_NORTH) *dz = -1;
    else if (dir == VOXEL_FM_WEST) *dx = -1;
    else if (dir == VOXEL_FM_EAST) *dx = 1;
}

static int Blocked(void *context, int dir)
{
    const Game *g = context;
    int dx, dz;

    Delta(dir, &dx, &dz);
    return IsWall(g, g->gx + dx, g->gz + dz);
}

/* The game's frame given the direction pressed. */
static void GameFrame(Game *g, int dir)
{
    int dx, dz;

    if (g->busy > 0)
        --g->busy;
    if (dir == VOXEL_FM_NONE)
        return;
    ++g->presses;
    if (g->busy > 0)
        return;
    if (g->autoFace)
        g->facing = dir;
    if (g->facing != dir)
    {
        g->facing = dir; /* the turn, on the spot */
        g->busy = g->turnFrames > 0 ? g->turnFrames : 1;
        ++g->turns;
        return;
    }
    Delta(dir, &dx, &dz);
    if (IsWall(g, g->gx + dx, g->gz + dz))
    {
        ++g->bumps;
        return;
    }
    g->gx += dx;
    g->gz += dz;
    ++g->steps;
    g->busy = g->stepFrames;
}

typedef struct
{
    VoxelFreeMove fm;
    Game game;
    float px, pz;     /* P as the renderer shows it */
    float maxJump;    /* the largest move of P in one frame */
    float maxBack;    /* the largest move against the walk */
    int wallFrames;   /* frames P was inside a wall tile */
} Run;

static void Begin(Run *r, int gx, int gz, int stepFrames)
{
    memset(r, 0, sizeof(*r));
    VoxelFreeMove_Reset(&r->fm);
    r->game.gx = gx;
    r->game.gz = gz;
    r->game.facing = VOXEL_FM_SOUTH;
    r->game.stepFrames = stepFrames;
    VoxelFreeMove_Sync(&r->fm, gx, gz);
    r->px = (float)gx;
    r->pz = (float)gz;
}

/* One frame at the walk (vx, vz) in tiles a frame. */
static int Frame(Run *r, float vx, float vz)
{
    int dir = VoxelFreeMove_Step(&r->fm, r->game.gx, r->game.gz, vx, vz, Blocked, &r->game);
    float x, z, jump;

    GameFrame(&r->game, dir);
    VoxelFreeMove_Sync(&r->fm, r->game.gx, r->game.gz);
    x = (float)r->game.gx + r->fm.ox;
    z = (float)r->game.gz + r->fm.oz;
    jump = sqrtf((x - r->px) * (x - r->px) + (z - r->pz) * (z - r->pz));
    if (jump > r->maxJump)
        r->maxJump = jump;
    r->px = x;
    r->pz = z;
    /* P inside a wall's tile is a fault (a tile's centre is its coordinate,
     * its edges 0.5 either side; on the edge is not inside). */
    for (int j = (int)floorf(z) - 1; j <= (int)floorf(z) + 2; ++j)
        for (int i = (int)floorf(x) - 1; i <= (int)floorf(x) + 2; ++i)
            if (IsWall(&r->game, i, j) && fabsf(x - (float)i) < 0.499f && fabsf(z - (float)j) < 0.499f)
            {
                ++r->wallFrames;
                j = 1000;
                break;
            }
    return dir;
}

static void TestStraightWalk(void)
{
    Run r;
    float last;

    Begin(&r, 2, 10, 16);
    last = r.px;
    for (int f = 0; f < 16 * 6; ++f)
    {
        Frame(&r, 1.0f / 16.0f, 0.0f);
        CHECK(r.px >= last - 1e-4f, "walk east went back: %f -> %f at frame %d", last, r.px, f);
        last = r.px;
    }
    CHECK(r.maxJump < 1.0f / 16.0f + 0.002f, "P jumped %f in a frame walking", r.maxJump);
    CHECK(r.px > 2.0f + 5.5f, "walked only to %f in six tiles' time", r.px);
    CHECK(r.game.steps >= 5, "the game stepped %d times", r.game.steps);
    CHECK(fabsf(r.pz - 10.0f) < 1e-4f, "drifted in z: %f", r.pz);
    CHECK(r.wallFrames == 0, "in a wall");
}

static void TestRun(void)
{
    Run r;

    /* The game's run: 8 frames a tile; P at that speed keeps up. */
    Begin(&r, 2, 10, 8);
    for (int f = 0; f < 8 * 8; ++f)
        Frame(&r, 1.0f / 8.0f, 0.0f);
    CHECK(r.maxJump < 1.0f / 8.0f + 0.002f, "P jumped %f running", r.maxJump);
    CHECK(r.px > 2.0f + 6.0f, "ran only to %f", r.px);
    /* P faster than the game takes steps is held back by it, not run ahead. */
    Begin(&r, 2, 10, 16);
    for (int f = 0; f < 16 * 4; ++f)
        Frame(&r, 1.0f / 6.0f, 0.0f);
    CHECK(r.px < 2.0f + 4.0f + 1.2f, "P ran ahead of the game: %f", r.px);
    CHECK(r.maxJump < 1.0f / 6.0f + 0.002f, "P jumped %f held back", r.maxJump);
}

static void TestSlowAnalog(void)
{
    Run r;

    /* A light tilt: slower than a step, so P moves inside tiles and the game
     * steps as it crosses their edges, with nothing in between. */
    Begin(&r, 2, 10, 16);
    for (int f = 0; f < 200; ++f)
        Frame(&r, 1.0f / 40.0f, 0.0f);
    CHECK(fabsf(r.px - (2.0f + 200.0f / 40.0f)) < 0.05f, "slow walk ended at %f, wanted %f", r.px, 2.0f + 5.0f);
    CHECK(r.maxJump < 1.0f / 40.0f + 0.002f, "P jumped %f at a slow walk", r.maxJump);
}

static void TestDiagonal(void)
{
    Run r;
    int steps;

    /* Free direction: south-east at 45 degrees, in a room. */
    Begin(&r, 3, 3, 16);
    for (int f = 0; f < 16 * 5; ++f)
        Frame(&r, 0.7f / 16.0f, 0.7f / 16.0f);
    CHECK(r.maxJump < 1.0f / 16.0f + 0.002f, "P jumped %f on a diagonal", r.maxJump);
    /* The game takes one step at a time, so the two axes share its pace:
     * a diagonal is slower along each axis (about half) than a straight walk. */
    CHECK(fabsf((r.px - 3.0f) - (r.pz - 3.0f)) < 0.6f, "the diagonal bent: %f, %f", r.px, r.pz);
    CHECK(r.px > 3.0f + 2.2f && r.pz > 3.0f + 2.2f, "diagonal got only to %f, %f", r.px, r.pz);
    CHECK(r.wallFrames == 0, "in a wall");
    steps = r.game.steps;
    CHECK(steps >= 4, "the diagonal took %d steps", steps);
}

/* The real game turns on the spot (about 8 frames, the step ignored) when
 * pressed a heading it does not face and it was not already moving - and
 * the free walk presses only at an edge, so every change of axis on a
 * diagonal was a turn with P held at the edge. The bridge sets the heading
 * first (autoFace): no turn, and the walk's pace is the game's. */
static void TestTurnStall(void)
{
    Run a, b;

    Begin(&a, 3, 3, 16);
    a.game.turnFrames = 8;
    a.game.autoFace = false;
    Begin(&b, 3, 3, 16);
    b.game.turnFrames = 8;
    b.game.autoFace = true;
    for (int f = 0; f < 16 * 6; ++f)
    {
        Frame(&a, 0.7f / 16.0f, 0.7f / 16.0f);
        Frame(&b, 0.7f / 16.0f, 0.7f / 16.0f);
    }
    printf("diagonal, 96 frames: with turns %d turns reached %.2f,%.2f; without %d turns reached %.2f,%.2f\n",
           a.game.turns, a.px, a.pz, b.game.turns, b.px, b.pz);
    CHECK(b.game.turns == 0, "the game still turned %d times", b.game.turns);
    CHECK(b.px + b.pz > a.px + a.pz, "no faster without the turns");
    CHECK(b.maxJump < 1.0f / 16.0f + 0.002f, "P jumped %f", b.maxJump);
    CHECK(b.wallFrames == 0, "in a wall");
}

static void TestWallSlide(void)
{
    Run r;
    float maxX = 0.0f;

    /* A wall down the column x = 8: walking south-east at it, P stops at its
     * face and slides south along it. */
    Begin(&r, 4, 4, 16);
    for (int z = 0; z < H; ++z)
        r.game.wall[z][8] = 1;
    for (int f = 0; f < 16 * 12; ++f)
    {
        Frame(&r, 0.6f / 16.0f, 0.8f / 16.0f);
        if (r.px > maxX)
            maxX = r.px;
    }
    CHECK(maxX <= 7.5f + 1e-3f, "P went into the wall: x %f", maxX);
    CHECK(maxX > 7.3f, "P stopped short of the wall: x %f", maxX);
    CHECK(r.pz > 4.0f + 6.0f, "P did not slide along the wall: z %f", r.pz);
    CHECK(r.wallFrames == 0, "in a wall for %d frames", r.wallFrames);
    /* Not aimed at the wall (37 degrees off): the game is never asked to bump. */
    CHECK(r.game.bumps == 0, "bumped %d times sliding", r.game.bumps);
}

static void TestAimedAtWall(void)
{
    Run r;

    /* Straight at a wall: the game is asked, to bump (and a warp or a boulder
     * would act); P stops at the edge. */
    Begin(&r, 4, 4, 16);
    for (int z = 0; z < H; ++z)
        r.game.wall[z][8] = 1;
    for (int f = 0; f < 16 * 8; ++f)
        Frame(&r, 1.0f / 16.0f, 0.0f);
    CHECK(r.px <= 7.5f + 1e-3f && r.px > 7.4f, "stopped at %f", r.px);
    CHECK(r.game.bumps > 0, "the game was not asked for the step into the wall");
    CHECK(r.wallFrames == 0, "in a wall");
}

static void TestCorner(void)
{
    Run r;

    /* Walls to the east and to the south: a corner; P ends in it, in neither. */
    Begin(&r, 4, 4, 16);
    for (int z = 0; z < H; ++z)
        r.game.wall[z][7] = 1;
    for (int x = 0; x < W; ++x)
        r.game.wall[7][x] = 1;
    for (int f = 0; f < 16 * 10; ++f)
        Frame(&r, 0.7f / 16.0f, 0.7f / 16.0f);
    CHECK(r.px <= 6.5f + 1e-3f && r.pz <= 6.5f + 1e-3f, "corner: %f, %f", r.px, r.pz);
    CHECK(r.px > 6.3f && r.pz > 6.3f, "corner not reached: %f, %f", r.px, r.pz);
    CHECK(r.wallFrames == 0, "in a wall");
}

static void TestStopAndReverse(void)
{
    Run r;
    int steps, beforeStops;

    Begin(&r, 5, 10, 16);
    for (int f = 0; f < 40; ++f)
        Frame(&r, 1.0f / 16.0f, 0.0f);
    steps = r.game.steps;
    beforeStops = r.game.presses;
    /* Let go: the game finishes the step it began and P stands. */
    for (int f = 0; f < 120; ++f)
        Frame(&r, 0.0f, 0.0f);
    CHECK(r.game.steps <= steps + 1, "the game kept walking after the stick was let go: %d -> %d", steps, r.game.steps);
    CHECK(r.game.presses <= beforeStops + 1, "pressed after the stick was let go");
    {
        float x = r.px;

        for (int f = 0; f < 60; ++f)
            Frame(&r, 0.0f, 0.0f);
        CHECK(fabsf(r.px - x) < 1e-5f, "P drifted standing: %f -> %f", x, r.px);
        CHECK(r.game.steps <= steps + 1, "the game stepped on its own");
    }
    /* Back the way it came. */
    {
        float x = r.px;

        for (int f = 0; f < 16 * 3; ++f)
            Frame(&r, -1.0f / 16.0f, 0.0f);
        CHECK(r.px < x - 2.5f, "reversing moved only %f", x - r.px);
        CHECK(r.maxJump < 1.0f / 16.0f + 0.002f, "P jumped %f reversing", r.maxJump);
    }
    CHECK(r.wallFrames == 0, "in a wall");
}

static void TestTeleportAndIdle(void)
{
    Run r;

    Begin(&r, 5, 5, 16);
    for (int f = 0; f < 40; ++f)
        Frame(&r, 1.0f / 16.0f, 0.0f);
    CHECK(r.fm.blend >= 1.0f, "blend %f", r.fm.blend);
    /* A warp: the game puts the player elsewhere; the picture goes with it. */
    r.game.gx = 15;
    r.game.gz = 15;
    VoxelFreeMove_Sync(&r.fm, 15, 15);
    CHECK(r.fm.ox == 0.0f && r.fm.oz == 0.0f, "offset kept over a warp: %f %f", r.fm.ox, r.fm.oz);
    CHECK(r.fm.blend == 0.0f, "blend kept over a warp: %f", r.fm.blend);
    /* Nobody driving: the picture hands back to the game within the blend. */
    for (int f = 0; f < 30; ++f)
        Frame(&r, 1.0f / 16.0f, 0.0f);
    for (int f = 0; f < VOXEL_FM_BLEND_FRAMES + 1; ++f)
        VoxelFreeMove_Idle(&r.fm, r.game.gx, r.game.gz);
    CHECK(r.fm.blend == 0.0f, "blend %f after idling", r.fm.blend);
    CHECK(r.fm.ox == 0.0f && r.fm.oz == 0.0f, "offset kept idling");
    CHECK(!VoxelFreeMove_Moving(&r.fm), "moving idle");
}

static void TestHeading(void)
{
    Run r;

    Begin(&r, 5, 5, 16);
    Frame(&r, 0.0f, 0.05f); /* south */
    CHECK(fabsf(r.fm.heading) < 1e-4f, "south heading %f", r.fm.heading);
    Frame(&r, 0.05f, 0.0f); /* east */
    CHECK(fabsf(r.fm.heading - 1.5707963f) < 1e-4f, "east heading %f", r.fm.heading);
    Frame(&r, -0.05f, 0.0f); /* west */
    CHECK(fabsf(r.fm.heading + 1.5707963f) < 1e-4f, "west heading %f", r.fm.heading);
    CHECK(VoxelFreeMove_Moving(&r.fm), "not moving");
    Frame(&r, 0.0f, 0.0f);
    CHECK(!VoxelFreeMove_Moving(&r.fm), "moving at rest");
}

static void TestRandomWalk(void)
{
    Run r;
    unsigned seed = 12345u;

    /* Whatever the stick does, P is never in a wall and never jumps. */
    Begin(&r, 10, 10, 16);
    for (int z = 0; z < H; ++z)
        for (int x = 0; x < W; ++x)
            if (x == 0 || z == 0 || x == W - 1 || z == H - 1 || ((x * 7 + z * 13) % 11 == 0 && (x != 10 || z != 10)))
                r.game.wall[z][x] = 1;
    for (int f = 0; f < 20000; ++f)
    {
        {
            static float va, vm;

            if (f % 90 == 0)
            {
                seed = seed * 1664525u + 1013904223u;
                va = (float)((seed >> 8) & 0xFFFF) / 65535.0f * 6.2831853f;
                seed = seed * 1664525u + 1013904223u;
                vm = ((seed >> 8) & 0xFF) < 40 ? 0.0f : (float)((seed >> 8) & 0xFF) / 255.0f / 8.0f;
            }
            Frame(&r, sinf(va) * vm, cosf(va) * vm);
        }
    }
    CHECK(r.wallFrames == 0, "random walk was in a wall for %d frames", r.wallFrames);
    CHECK(r.maxJump < 0.13f, "random walk jumped %f", r.maxJump);
    CHECK(r.game.steps > 100, "random walk took only %d steps", r.game.steps);
}

int main(void)
{
    TestStraightWalk();
    TestRun();
    TestSlowAnalog();
    TestDiagonal();
    TestTurnStall();
    TestWallSlide();
    TestAimedAtWall();
    TestCorner();
    TestStopAndReverse();
    TestTeleportAndIdle();
    TestHeading();
    TestRandomWalk();
    if (failures)
    {
        printf("voxel_freemove_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("voxel_freemove_test: ok\n");
    return 0;
}
