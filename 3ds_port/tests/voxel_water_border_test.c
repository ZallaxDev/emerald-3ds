/* The belt of water past the last map is watertight against the map's edges
 * (issue #98): one plane for the whole belt even where the edge's water
 * changes level at a corner, a skirt from every edge not at that plane down
 * (or up) to it - the south edge too -, and a stood border cell beside a
 * lowered belt reaching down to it. Every check is on the emitted triangles:
 * points on the seam between a map edge and the belt must lie on some
 * triangle.
 *
 * Built like voxel_ground_mesh_test.c (same stubs, same objects linked), but
 * the builder is compiled into this file with the relief queries renamed to
 * the synthetic relief below; link everything voxel_ground_mesh_test links
 * except voxel_mesh_builder.o. No ROM assets: the relief, the map and its
 * border are all made up here. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "voxel_tileset_fixture.h"

#define VoxelRelief_Base      TestRelief_Base
#define VoxelRelief_Belt      TestRelief_Belt
#define VoxelRelief_Cell      TestRelief_Cell
#define VoxelRelief_Depth     TestRelief_Depth
#define VoxelRelief_CellLift  TestRelief_CellLift
#define VoxelRelief_CellShift TestRelief_CellShift
#define VoxelRelief_Cut       TestRelief_Cut
#include "voxel_mesh_builder.c"
#include "voxel_regions.h"

const struct Tileset gTileset_Dewford = {0};

#define W 6
#define H 6
#define BORDER_WATER 100
#define BORDER_STOOD 101

static VoxelMapInstance sMap = {.layoutId = 1, .width = W, .height = H};
static VoxelAtlasMap sAtlas;
static float sLift[H][W];
static bool sWater[H][W];
static bool sDrawn;           /* a drawn map: every point moved south by its height */
static int sStoodWestOf = -1000; /* border cells with x < this stand up; the rest are water */
static int16_t sGrid[H][W][VOXEL_RELIEF_SIDE * VOXEL_RELIEF_SIDE];
static int16_t sDepth[H][W][VOXEL_RELIEF_SIDE * VOXEL_RELIEF_SIDE];

static bool Inside(int x, int y)
{
    return x >= sMap.originX && y >= sMap.originY && x < sMap.originX + W && y < sMap.originY + H;
}

/* ── synthetic relief: every lifted cell level, at sLift ───────────────── */
float TestRelief_Base(const VoxelMapInstance *inst) { (void)inst; return 0.0f; }
unsigned TestRelief_Belt(const VoxelMapInstance *inst) { (void)inst; return 0u; }
float TestRelief_CellLift(const VoxelMapInstance *inst, int x, int y)
{
    (void)inst;
    return Inside(x, y) ? sLift[y - sMap.originY][x - sMap.originX] : 0.0f;
}
float TestRelief_CellShift(const VoxelMapInstance *inst, int x, int y)
{
    return sDrawn ? TestRelief_CellLift(inst, x, y) : 0.0f;
}
const int16_t *TestRelief_Cell(const VoxelMapInstance *inst, int x, int y)
{
    float lift = TestRelief_CellLift(inst, x, y);
    int16_t *g;

    if (!Inside(x, y) || lift == 0.0f)
        return NULL;
    g = sGrid[y - sMap.originY][x - sMap.originX];
    for (unsigned i = 0; i < VOXEL_RELIEF_SIDE * VOXEL_RELIEF_SIDE; ++i)
        g[i] = (int16_t)lroundf(lift * 16.0f);
    return g;
}
const int16_t *TestRelief_Depth(const VoxelMapInstance *inst, int x, int y)
{
    const int16_t *g = TestRelief_Cell(inst, x, y);
    int16_t *d;

    if (g == NULL)
        return NULL;
    d = sDepth[y - sMap.originY][x - sMap.originX];
    for (unsigned i = 0; i < VOXEL_RELIEF_SIDE * VOXEL_RELIEF_SIDE; ++i)
        d[i] = sDrawn ? g[i] : 0;
    return d;
}
int TestRelief_Cut(const VoxelMapInstance *inst, int x, int y, float *foot, int *ground,
                   int *wall, unsigned *sides)
{
    (void)inst; (void)x; (void)y;
    *foot = 0.0f; *ground = -1; *wall = -1; *sides = 0;
    return -1;
}

/* ── world stubs, as voxel_ground_mesh_test.c ──────────────────────────── */
unsigned VoxelRegions_RoleAt(unsigned id, int x, int y)
{
    (void)id; (void)x; (void)y;
    return VOXEL_ROLE_FLOOR;
}
bool VoxelWorld_UsesTreeSprites(const VoxelMapInstance *m) { (void)m; return false; }
const VoxelMapInstance *VoxelWorld_Instance(unsigned i) { return i == 0 ? &sMap : NULL; }
const VoxelMapInstance *VoxelWorld_GetInstanceAt(int x, int y) { return Inside(x, y) ? &sMap : NULL; }
int VoxelWorld_GetMetatileId(int x, int y)
{
    return Inside(x, y) ? (y - sMap.originY) * W + x - sMap.originX : 0;
}
VoxelVisualShape VoxelWorld_ClassifyTile(int x, int y)
{
    if (!Inside(x, y)) return VOXEL_SHAPE_VOID;
    return sWater[y - sMap.originY][x - sMap.originX] ? VOXEL_SHAPE_WATER : VOXEL_SHAPE_FLAT;
}
bool VoxelWorld_BorderIsWater(int x, int y) { (void)y; return x >= sStoodWestOf; }
int VoxelWorld_BorderMetatile(int x, int y)
{
    return VoxelWorld_BorderIsWater(x, y) ? BORDER_WATER : BORDER_STOOD;
}
void VoxelWorld_GetPlayerWorldCoords(float *x, float *z) { *x = 2; *z = 2; }
void VoxelAtlas_SlotUV(unsigned slot, float *u, float *v, float *ue, float *ve)
{
    *u = (slot % 32) / 32.0f; *ue = *u + 1.0f / 32;
    *v = 1 - (slot / 32) / 16.0f; *ve = *v - 1.0f / 16;
}
void VoxelAtlas_SolidUV(VoxelSolidColor c, float *u, float *v, float *ue, float *ve)
{
    VoxelAtlas_SlotUV((unsigned)c, u, v, ue, ve);
}

/* ── emitted geometry ──────────────────────────────────────────────────── */
#define CAP 30000
static VoxelVertex sGroundV[CAP], sBorderV[CAP];
static VoxelBuilder sGround, sBorder;

static void Reset(int layout)
{
    memset(sLift, 0, sizeof(sLift));
    memset(sWater, 0, sizeof(sWater));
    sDrawn = false;
    sStoodWestOf = -1000;
    sMap.indoor = false;
    sMap.layoutId = layout; /* a new map: the belt's plane is worked out again */
}

static void Build(void)
{
    VoxelBuilder_Init(&sGround, sGroundV, CAP);
    VoxelBuilder_SetAtlas(&sGround, &sAtlas);
    VoxelBuilder_Init(&sBorder, sBorderV, CAP);
    VoxelBuilder_SetAtlas(&sBorder, &sAtlas);
    VoxelMesh_BeginWindow(-3, -3, W + 3, H + 3);
    VoxelMesh_EmitInstance(&sGround, &sMap, 0, 0, W, H);
    VoxelMesh_EmitBorder(&sBorder, -3, -3, W + 3, H + 3);
    assert(!sGround.dropped && !sBorder.dropped);
}

static float Dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static bool OnTri(const VoxelVertex *t, const float p[3])
{
    float a[3] = {t[0].x, t[0].y, t[0].z};
    float e1[3] = {t[1].x - a[0], t[1].y - a[1], t[1].z - a[2]};
    float e2[3] = {t[2].x - a[0], t[2].y - a[1], t[2].z - a[2]};
    float w[3] = {p[0] - a[0], p[1] - a[1], p[2] - a[2]};
    float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                  e1[0] * e2[1] - e1[1] * e2[0]};
    float nn = Dot(n, n), d11, d12, d22, dw1, dw2, den, s, r;

    if (nn < 1e-10f || fabsf(Dot(w, n)) / sqrtf(nn) > 2e-3f)
        return false;
    d11 = Dot(e1, e1); d12 = Dot(e1, e2); d22 = Dot(e2, e2);
    dw1 = Dot(w, e1); dw2 = Dot(w, e2);
    den = d11 * d22 - d12 * d12;
    s = (d22 * dw1 - d12 * dw2) / den;
    r = (d11 * dw2 - d12 * dw1) / den;
    return s >= -2e-3f && r >= -2e-3f && s + r <= 1.0f + 2e-3f;
}

static bool Covered(float x, float y, float z)
{
    const float p[3] = {x, y, z};
    const VoxelBuilder *bs[2] = {&sGround, &sBorder};

    for (int k = 0; k < 2; ++k)
        for (unsigned i = 0; i + 2 < bs[k]->count; i += 3)
            if (OnTri(&bs[k]->vertices[i], p))
                return true;
    return false;
}

/*
 * The seam between every map edge facing a belt of water and the belt's
 * plane (level, shift south): sampled along the edge and from the edge's
 * height down (or up) to the plane, every sample on a triangle. Returns the
 * samples that were not.
 */
static unsigned Sweep(float plane, float planeShift)
{
    static const int kDir[4][2] = { {0, -1}, {0, 1}, {-1, 0}, {1, 0} };
    static const float kAlong[] = {0.0f, 0.125f, 0.375f, 0.5f, 0.625f, 0.875f, 1.0f};
    static const float kT[] = {0.25f, 0.5f, 0.75f, 1.0f};
    unsigned open = 0;

    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            for (int d = 0; d < 4; ++d)
            {
                int nx = x + kDir[d][0], ny = y + kDir[d][1];
                float lift = sLift[y][x], depth = sDrawn ? lift : 0.0f;

                if (Inside(nx, ny) || !VoxelWorld_BorderIsWater(nx, ny))
                    continue;
                if (fabsf(lift - plane) < 1e-4f && fabsf(depth - planeShift) < 1e-4f)
                    continue;
                for (unsigned a = 0; a < sizeof(kAlong) / sizeof(kAlong[0]); ++a)
                    for (unsigned t = 0; t < sizeof(kT) / sizeof(kT[0]); ++t)
                    {
                        float px, line, f = kT[t];

                        if (d < 2)
                        {
                            px = x + kAlong[a];
                            line = (float)y + (d == 1 ? 1.0f : 0.0f);
                        }
                        else
                        {
                            px = (float)x + (d == 3 ? 1.0f : 0.0f);
                            line = (float)y + kAlong[a];
                        }
                        if (!Covered(px, lift + (plane - lift) * f,
                                     line + depth + (planeShift - depth) * f))
                        {
                            if (open < 4)
                                printf("open seam: cell (%d,%d) side %d along %.3f t %.2f\n",
                                       x, y, d, kAlong[a], f);
                            ++open;
                        }
                    }
            }
    return open;
}

/* Ground triangles standing in the plane of the map's outline (a level map's
 * skirts): the top quads only touch it. */
static unsigned OutlineTriangles(void)
{
    unsigned n = 0;

    for (unsigned i = 0; i + 2 < sGround.count; i += 3)
    {
        const VoxelVertex *t = &sGroundV[i];
        bool sameX = fabsf(t[0].x - t[1].x) < 1e-4f && fabsf(t[0].x - t[2].x) < 1e-4f;
        bool sameZ = fabsf(t[0].z - t[1].z) < 1e-4f && fabsf(t[0].z - t[2].z) < 1e-4f;

        if ((sameX && (fabsf(t[0].x) < 1e-4f || fabsf(t[0].x - W) < 1e-4f))
         || (sameZ && (fabsf(t[0].z) < 1e-4f || fabsf(t[0].z - H) < 1e-4f)))
            ++n;
    }
    return n;
}

static void BorderAllAt(float level)
{
    assert(sBorder.count > 0);
    for (unsigned i = 0; i < sBorder.count; ++i)
        assert(fabsf(sBorderV[i].y - level) < 1e-4f);
}

int main(void)
{
    for (unsigned i = 0; i < 64; ++i) sAtlas.slotOf[i] = (uint16_t)(i + 1);
    sAtlas.slotOf[BORDER_WATER] = BORDER_WATER + 1;
    sAtlas.slotOf[BORDER_STOOD] = BORDER_STOOD + 1;

    /* A: uneven corner. The east edge's water at the ground, the south
     * edge's sea half a tile under it, meeting at the south-east corner.
     * Cell by cell from the nearest edge the belt stood at two levels side
     * by side with nothing between; now it lies on one plane, the lowest,
     * and every higher edge hangs a skirt down to it. */
    Reset(11);
    for (int x = 0; x < W; ++x) { sWater[H - 1][x] = true; sLift[H - 1][x] = -0.5f; }
    for (int y = 0; y < H - 1; ++y) sWater[y][W - 1] = true;
    Build();
    BorderAllAt(-0.5f);
    assert(Sweep(-0.5f, 0.0f) == 0);
    /* the belt cells either side of the corner's step meet edge to edge */
    assert(Covered(W + 0.5f, -0.5f, H - 1.0f) && Covered(W + 0.5f, -0.5f, H + 0.0f));

    /* A drawn: the same uneven corner, now with a nonzero belt shift.
     * Sample every seam and both sides of the shifted corner. */
    Reset(17);
    sDrawn = true;
    for (int x = 0; x < W; ++x) { sWater[H - 1][x] = true; sLift[H - 1][x] = -0.5f; }
    for (int y = 0; y < H - 1; ++y) sWater[y][W - 1] = true;
    Build();
    BorderAllAt(-0.5f);
    assert(Sweep(-0.5f, -0.5f) == 0);
    assert(Covered(W + 0.5f, -0.5f, H - 1.5f) && Covered(W + 0.5f, -0.5f, H - 0.5f));

    /* B: the south edge of a drawn map raised half a tile over a belt of
     * water at the ground. Its points stand half a tile south as well; the
     * skirt runs from them to the plane's edge, the south one included
     * (EmitSeamSkirt skipped it for a belt that stood in front). */
    Reset(12);
    sDrawn = true;
    for (int x = 0; x < W; ++x) sLift[H - 1][x] = 0.5f;
    Build();
    BorderAllAt(0.0f);
    assert(Sweep(0.0f, 0.0f) == 0);

    /* C: a stood belt is as it was - no skirt on the south edge, where its
     * own face stands, and the walls up to it on the others. */
    Reset(13);
    for (int x = 0; x < W; ++x) { sWater[H - 1][x] = true; sLift[H - 1][x] = -0.5f; }
    sStoodWestOf = 1000; /* no border cell is water */
    Build();
    {
        bool tall = false;

        for (unsigned i = 0; i < sBorder.count; ++i)
            tall |= fabsf(sBorderV[i].y - 2.0f) < 1e-4f;
        assert(tall);
        for (unsigned i = 0; i + 2 < sGround.count; i += 3)
            assert(!(sGroundV[i].z >= H - 1e-3f && sGroundV[i + 1].z >= H - 1e-3f
                     && sGroundV[i + 2].z >= H - 1e-3f));
    }

    /* D: indoors there is no belt and no skirt to it. */
    Reset(14);
    sMap.indoor = true;
    for (int x = 0; x < W; ++x) { sWater[H - 1][x] = true; sLift[H - 1][x] = -0.5f; }
    Build();
    assert(sBorder.count == 0);
    assert(OutlineTriangles() == 0);
    /* the same map outdoors does hang its walls up to the belt there */
    sMap.indoor = false; sMap.layoutId = 16;
    Build();
    assert(OutlineTriangles() > 0);

    /* E: the belt stands west of the map and is water elsewhere, its plane
     * lowered by the south edge's sea. The stood cell at the north-west
     * corner faces the water east of it: its face goes down to the plane. */
    Reset(15);
    for (int x = 0; x < W; ++x) { sWater[H - 1][x] = true; sLift[H - 1][x] = -0.5f; }
    sStoodWestOf = 0;
    Build();
    assert(Sweep(-0.5f, 0.0f) == 0);
    assert(Covered(0.0f, -0.25f, -0.5f));
    assert(Covered(0.0f, 1.0f, -0.5f)); /* and still stands its courses */

    /* E drawn: stood columns retain their position beside shifted water;
     * the water seams still reach the same displaced plane. */
    Reset(18);
    sDrawn = true;
    for (int x = 0; x < W; ++x) { sWater[H - 1][x] = true; sLift[H - 1][x] = -0.5f; }
    sStoodWestOf = 0;
    Build();
    assert(Sweep(-0.5f, -0.5f) == 0);
    assert(Covered(0.0f, -0.25f, -0.5f));
    assert(Covered(0.0f, 1.0f, -0.5f));
    assert(Covered(0.5f, -0.5f, -1.25f)); /* shifted north water top */

    puts("PASS water border: one plane at uneven corners, skirts on every side incl. south, "
         "drawn-map shift, stood belt unchanged, indoor untouched, stood faces down to the plane");
    return 0;
}
