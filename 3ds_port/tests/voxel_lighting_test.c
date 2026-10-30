/* Real lighting/mesh code against a small mutable world, no SDK or GPU. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "voxel_tileset_fixture.h"

#include "voxel_lighting.h"
#include "voxel_atlas.h"
#include "voxel_regions.h"
#include "voxel_building.h"

static VoxelMapInstance sMaps[2] = {
    {.layoutId = 1, .width = 4, .height = 16},
    {.layoutId = 2, .originX = 4, .width = 12, .height = 16}
};
/* A modelled building three tiles tall, across the seam of two maps. */
#define HOUSE_TOP 3.0f
static bool sHousePresent = true, sTreePresent;
/* A railing along a row: layout 2's cell (8, 10), world (12, 10), its line
 * the cell's two southern pixel rows, 0.75 tall. */
static bool sRailPresent;
static const uint16_t sRailMask[16] = {[14] = 0xFFFF, [15] = 0xFFFF};
static int sOffsetX, sOffsetZ;
static unsigned sTileReads;

/* voxel_tree.c compares the map's secondary tileset with this object to
 * recognise Dewford's tree art; this test never builds trees, so an empty
 * tileset is enough for the link. */
const struct Tileset gTileset_Dewford = {0};
const struct Tileset gTileset_Fortree = {0};
const struct Tileset gTileset_Mauville = {0};
const struct Tileset gTileset_Lavaridge = {0};
const struct Tileset gTileset_Rustboro = {0};
const struct Tileset gTileset_Lilycove = {0};
const struct Tileset gTileset_EverGrande = {0};
const struct Tileset gTileset_BattleFrontierOutsideWest = {0};
const struct Tileset gTileset_Mossdeep = {0};
const struct Tileset gTileset_Fallarbor = {0};

unsigned VoxelWorld_InstanceCount(void) { return 2; }
const VoxelMapInstance *VoxelWorld_Instance(unsigned i) { return i < 2 ? &sMaps[i] : NULL; }
const VoxelMapInstance *VoxelWorld_GetInstanceAt(int x, int z)
{
    for (unsigned i = 0; i < 2; ++i)
        if (x >= sMaps[i].originX && x < sMaps[i].originX + sMaps[i].width
         && z >= sMaps[i].originY && z < sMaps[i].originY + sMaps[i].height)
            return &sMaps[i];
    return NULL;
}
bool VoxelWorld_UsesTreeSprites(const VoxelMapInstance *inst) { return inst != NULL; }
int VoxelWorld_GetMetatileId(int x, int z)
{
    int lx = x - sOffsetX, lz = z - sOffsetZ;
    ++sTileReads;
    if (VoxelWorld_GetInstanceAt(x, z) == NULL) return -1;
    if (sHousePresent && lx >= 4 && lx < 6 && lz >= 4 && lz < 6) return 2;
    if (sTreePresent && lx >= 8 && lx < 10 && lz >= 4 && lz < 6)
    {
        const int parts[] = {0x1D4, 0x1D5, 0x1DC, 0x1DD};
        return parts[(lz - 4) * 2 + lx - 8];
    }
    return 1;
}
unsigned VoxelRegions_RoleAt(unsigned layoutId, int x, int z)
{
    (void)layoutId; (void)x; (void)z;
    return VOXEL_ROLE_FLOOR;
}
/* The house stands in layout 2's first two columns, rows 4-5 - world 4-5. */
bool VoxelBuildings_CellAt(const VoxelMapInstance *inst, int x, int y,
                           int *groundMetatile, float *top)
{
    int lx = x - inst->originX, ly = y - inst->originY;

    if (sRailPresent && inst->layoutId == 2 && lx == 8 && ly == 10)
    {
        if (groundMetatile != NULL) *groundMetatile = 1;
        if (top != NULL) *top = 0.75f;
        return true;
    }
    if (!sHousePresent || inst->layoutId != 2 || lx < 0 || lx >= 2 || ly < 4 || ly >= 6)
        return false;
    if (groundMetatile != NULL) *groundMetatile = 1;
    if (top != NULL) *top = HOUSE_TOP;
    return true;
}
bool VoxelBuildings_BareVariant(const VoxelMapInstance *inst, int x, int y, int *metatile)
{
    (void)inst; (void)x; (void)y; (void)metatile;
    return false;
}
const uint16_t *VoxelBuildings_Footprint(const VoxelMapInstance *inst, int x, int y)
{
    if (sRailPresent && inst->layoutId == 2 && x - inst->originX == 8 && y - inst->originY == 10)
        return sRailMask;
    return NULL;
}
/* Raised to disable the ray ceiling, for the equivalence oracle. */
static float sCeilingOverride;
extern bool gVoxelLightingStepEveryPoint;
float VoxelBuildings_MaxTop(void)
{
    if (sCeilingOverride > 0.0f)
        return sCeilingOverride;
    return sHousePresent ? HOUSE_TOP : 0.0f;
}
float VoxelBuildings_LayoutTop(const VoxelMapInstance *inst)
{
    (void)inst;
    return VoxelBuildings_MaxTop();
}
/* No bridge deck in these scenes. */
bool VoxelBuildings_DeckAt(const VoxelMapInstance *inst, float worldX, float worldZ,
                           unsigned elevation, float *lift)
{
    (void)inst; (void)worldX; (void)worldZ; (void)elevation; (void)lift;
    return false;
}
VoxelVisualShape VoxelWorld_ClassifyTile(int x, int z)
{
    if (VoxelWorld_GetInstanceAt(x, z) == NULL) return VOXEL_SHAPE_VOID;
    if (x - sOffsetX == 2 && z - sOffsetZ == 2) return VOXEL_SHAPE_WATER;
    return VOXEL_SHAPE_FLAT;
}
uint32_t VoxelWorld_BlockHash(int x0, int z0, int x1, int z1)
{
    uint32_t hash = 2166136261u;
    for (int z = z0; z < z1; ++z)
        for (int x = x0; x < x1; ++x)
            hash = (hash ^ (uint32_t)VoxelWorld_GetMetatileId(x, z)) * 16777619u;
    return hash;
}
int VoxelWorld_BorderMetatile(int x, int z) { (void)x; (void)z; return -1; }
bool VoxelAtlas_IsVoid(const VoxelMapInstance *inst, int metatile) { (void)inst; (void)metatile; return false; }
bool VoxelWorld_BorderIsWater(int x, int z) { (void)x; (void)z; return false; }
void VoxelWorld_GetPlayerWorldCoords(float *x, float *z) { *x = *z = 0; }
void VoxelAtlas_SlotUV(unsigned slot, float *u0, float *v0, float *u1, float *v1)
{
    *u0 = (slot % 32) / 32.0f; *u1 = *u0 + 1.0f / 32;
    *v0 = 1.0f - (slot / 32) / 16.0f; *v1 = *v0 - 1.0f / 16;
}
void VoxelAtlas_SolidUV(VoxelSolidColor color, float *u0, float *v0, float *u1, float *v1)
{
    VoxelAtlas_SlotUV((unsigned)color, u0, v0, u1, v1);
}

static void Ground(VoxelBuilder *builder, int x, int z)
{
    VoxelMesh_Top(builder, (float)x, (float)z, 0, 0, 0, 1, 1, 0, 1);
}

#if CTR_VOXEL_LIGHTING
static void MovingSunTests(void)
{
    static const float directions[][2] = {
        {1.35f, 0.35f}, {-1.35f, 0.35f}, {0.0f, 0.55f},
        {1.0f, -0.55f}, {-1.0f, -0.55f}, {1.0f, 0.0f}, {0.0f, 0.0f}
    };
    uint32_t oldHash = VoxelLighting_Hash(0, 0, 4, 4);

    assert(VoxelLighting_SetSun(-1.0f, 0.55f, 1.0f, 101));
    assert(oldHash != VoxelLighting_Hash(0, 0, 4, 4));
    assert(VoxelLighting_Sample(2.5f, 0, 6.0f) < 0.8f);
    assert(VoxelLighting_Sample(6.5f, 0, 6.0f) == 1.0f);
    assert(VoxelLighting_Face(1, 0, 0) == 1.0f);
    assert(VoxelLighting_Face(-1, 0, 0) == VOXEL_AMBIENT);
    assert(!VoxelLighting_SetSun(-1.0f, 0.55f, 1.0f, 101));
    assert(VoxelLighting_SetSun(1.0f, 0.55f, 1.0f, 102));
    assert(VoxelLighting_Sample(6.5f, 0, 6.0f) < 0.8f);
    assert(VoxelLighting_Sample(2.5f, 0, 6.0f) == 1.0f);
    assert(VoxelLighting_Face(-1, 0, 0) == 1.0f);
    unsigned reads = sTileReads, rays = VoxelLighting_Rays();
    assert(VoxelLighting_SetSun(0.0f, 0.55f, 0.0f, 1));
    assert(VoxelLighting_Sample(6.5f, 0, 6.0f) == 1.0f);
    assert(VoxelLighting_Face(1, 0, 0) == 1.0f);
    assert(sTileReads == reads && VoxelLighting_Rays() == rays);
    assert(!VoxelLighting_SetSun(NAN, 0.55f, 1.0f, 103));
    assert(!VoxelLighting_SetSun(2.0f, 0.55f, 1.0f, 103));
    assert(!VoxelLighting_SetSun(0.0f, 0.55f, -1.0f, 103));

    sTreePresent = sRailPresent = true;
    float directionSamples[7][100];
    for (unsigned direction = 0; direction < sizeof(directions) / sizeof(directions[0]); ++direction)
    {
        VoxelLighting_SetSun(directions[direction][0], directions[direction][1], 1.0f, direction + 300);
        VoxelLighting_Reset();
        for (unsigned sample = 0; sample < 100; ++sample)
            directionSamples[direction][sample] = VoxelLighting_Sample(
                (float)(sample % 10) * 1.3f - 1.0f, 0.37f,
                (float)(sample / 10) * 1.3f - 1.0f);
    }
    for (unsigned direction = 0; direction < sizeof(directions) / sizeof(directions[0]); ++direction)
    {
        assert(VoxelLighting_SetSun(directions[direction][0], directions[direction][1], 1.0f, direction + 400));
        for (unsigned sample = 0; sample < 100; ++sample)
            assert(directionSamples[direction][sample] == VoxelLighting_Sample(
                (float)(sample % 10) * 1.3f - 1.0f, 0.37f,
                (float)(sample / 10) * 1.3f - 1.0f));
    }
    for (unsigned direction = 0; direction < sizeof(directions) / sizeof(directions[0]); ++direction)
    {
        assert(VoxelLighting_SetSun(directions[direction][0], directions[direction][1], 1.0f, direction + 200));
        for (unsigned height = 0; height < 3; ++height)
            for (unsigned row = 0; row < 45; ++row)
                for (unsigned column = 0; column < 60; ++column)
                {
                    float sampleX = (float)column * 0.27f - 1.0f;
                    float sampleZ = (float)row * 0.31f - 1.0f;
                    float sampleY = (float)height * 0.37f;
                    float optimized;

                    sCeilingOverride = 0.0f;
                    gVoxelLightingStepEveryPoint = false;
                    VoxelLighting_Reset();
                    optimized = VoxelLighting_Sample(sampleX, sampleY, sampleZ);
                    sCeilingOverride = 200.0f;
                    gVoxelLightingStepEveryPoint = true;
                    VoxelLighting_Reset();
                    assert(optimized == VoxelLighting_Sample(sampleX, sampleY, sampleZ));
                }
    }
    sTreePresent = sRailPresent = false;
    sCeilingOverride = 0.0f;
    gVoxelLightingStepEveryPoint = false;
    assert(VoxelLighting_SetSun(0.85f, 0.55f, 1.0f, 0));
    VoxelLighting_Reset();
    puts("PASS moving sun: both shadow directions, face lighting, zero axes, bounded rays, cache reuse, night, hash invalidation, 56700 oracle samples");
}
#endif

int main(void)
{
    VoxelVertex storage[2048];
    VoxelBuilder builder;
    VoxelBuilder_Init(&builder, storage, 2048);
    builder.lighting = true;
    VoxelMesh_BeginWindow(0, 0, 8, 8);
    Ground(&builder, 3, 3);
#if CTR_VOXEL_LIGHTING
    /* The sun is in the northwest: a building across a map/chunk boundary
     * casts southeast, but not northwest or onto its own roof. Shadows must
     * work before the caster's chunk has been meshed. */
    assert(VoxelLighting_Sample(6.5f, 0, 6.5f) < 0.8f);
    assert(VoxelLighting_Sample(3.5f, 0, 3.5f) == 1.0f);
    assert(VoxelLighting_Sample(4.5f, 3, 4.5f) == 1.0f);
    assert(VoxelLighting_Sample(3.9f, 0, 4.5f) > 0.9f);
    assert(VoxelLighting_Sample(3.9f, 0, 4.5f) < 1.0f); /* AO on the sunny side */
    /* Faces: the sun's, from the northwest - south and east in the ambient. */
    assert(VoxelLighting_Face(0, 1, 0) == 1.0f);
    assert(VoxelLighting_Face(0, 0, 1) == VOXEL_AMBIENT);
    assert(VoxelLighting_Face(1, 0, 0) == VOXEL_AMBIENT);
    assert(VoxelLighting_Face(-1, 0, 0) > 0.9f && VoxelLighting_Face(0, 0, -1) < 0.9f);
    /* Compare a cache-cold oracle with a densely reused sample grid, including
     * nearby but unequal coordinates sharing buckets and different heights. */
    float reference[3][25][25];
    for (int h = 0; h < 3; ++h)
        for (int z = 0; z < 25; ++z)
            for (int x = 0; x < 25; ++x)
            {
                VoxelLighting_Reset();
                reference[h][z][x] = VoxelLighting_Sample(x * 0.31f - 1, h * 0.4f, z * 0.31f - 1);
            }
    VoxelLighting_Reset();
    for (int pass = 0; pass < 2; ++pass)
        for (int h = 0; h < 3; ++h)
            for (int z = 0; z < 25; ++z)
                for (int x = 0; x < 25; ++x)
                    assert(reference[h][z][x] == VoxelLighting_Sample(x * 0.31f - 1, h * 0.4f, z * 0.31f - 1));
    /* Adjacent half-tile ground lattices should reuse their exact samples. */
    VoxelLighting_Reset();
    unsigned latticeStart = VoxelLighting_Rays();
    for (int pass = 0; pass < 2; ++pass)
        for (int z = 0; z < 16; ++z)
            for (int x = 0; x < 16; ++x)
                for (int dz = 0; dz < 3; ++dz)
                    for (int dx = 0; dx < 3; ++dx)
                        (void)VoxelLighting_Sample(x + dx * 0.5f, 0, z + dz * 0.5f);
    assert(VoxelLighting_Rays() - latticeStart <= 1200);
    printf("Lighting ground lattice: %u rays for 1089 unique points, 4608 queries\n",
           VoxelLighting_Rays() - latticeStart);
    /* Rays stop once they climb past the tallest caster on screen. With the
     * ceiling lifted out of reach every ray runs its full length, and must
     * reach the same answers. */
    sCeilingOverride = 200.0f;
    for (int h = 0; h < 3; ++h)
        for (int z = 0; z < 25; ++z)
            for (int x = 0; x < 25; ++x)
            {
                VoxelLighting_Reset();
                assert(reference[h][z][x] == VoxelLighting_Sample(x * 0.31f - 1, h * 0.4f, z * 0.31f - 1));
            }
    sCeilingOverride = 0.0f;
    /* A lone sample (a sprite's card) skips the blocks' bound: the same
     * answers, cold. */
    for (int h = 0; h < 3; ++h)
        for (int z = 0; z < 25; ++z)
            for (int x = 0; x < 25; ++x)
            {
                VoxelLighting_Reset();
                assert(reference[h][z][x] == VoxelLighting_SampleAlone(x * 0.31f - 1, h * 0.4f, z * 0.31f - 1));
            }
    VoxelLighting_Reset();
    /* Rays jump from cell to cell instead of visiting every point: the same
     * answers as the point-by-point march, over the whole reference grid. */
    gVoxelLightingStepEveryPoint = true;
    for (int h = 0; h < 3; ++h)
        for (int z = 0; z < 25; ++z)
            for (int x = 0; x < 25; ++x)
            {
                VoxelLighting_Reset();
                assert(reference[h][z][x] == VoxelLighting_Sample(x * 0.31f - 1, h * 0.4f, z * 0.31f - 1));
            }
    sTreePresent = true;
    for (int pass = 0; pass < 2; ++pass)
        for (int h = 0; h < 3; ++h)
            for (int z = 0; z < 40; ++z)
                for (int x = 0; x < 50; ++x)
                {
                    float got;

                    gVoxelLightingStepEveryPoint = false;
                    VoxelLighting_Reset();
                    got = VoxelLighting_Sample(x * 0.27f - 1, h * 0.37f, z * 0.19f - 1);
                    gVoxelLightingStepEveryPoint = true;
                    VoxelLighting_Reset();
                    assert(got == VoxelLighting_Sample(x * 0.27f - 1, h * 0.37f, z * 0.19f - 1));
                }
    sTreePresent = false;
    gVoxelLightingStepEveryPoint = false;
    VoxelLighting_Reset();
    assert(builder.count == 24 && builder.dropped == 0);
    for (unsigned i = 0; i < builder.count; ++i)
    {
        assert(storage[i].shade >= 0.60f && storage[i].shade <= 1.0f);
        assert(storage[i].u >= 0 && storage[i].u <= 1);
        assert(storage[i].v >= 0 && storage[i].v <= 1);
    }

    /* The receiving chunk's signature changes when a caster in its neighbour
     * changes, even though none of the receiver's own tiles changed. */
    uint32_t hash = VoxelLighting_Hash(0, 0, 4, 4);
    uint32_t local = VoxelWorld_BlockHash(0, 0, 4, 4);
    sHousePresent = false;
    VoxelLighting_Reset();
    assert(local == VoxelWorld_BlockHash(0, 0, 4, 4));
    assert(hash != VoxelLighting_Hash(0, 0, 4, 4));
    assert(VoxelLighting_Sample(6.5f, 0, 6.5f) == 1.0f);
    sHousePresent = true;
    VoxelLighting_Reset();

    /* Rebase into negative world coordinates: cached map-local geometry and
     * dependency hashes must remain the same. */
    VoxelVertex before[24];
    memcpy(before, storage, sizeof(before));
    sOffsetX = -20; sOffsetZ = -30;
    for (unsigned i = 0; i < 2; ++i)
    {
        sMaps[i].originX += sOffsetX;
        sMaps[i].originY += sOffsetZ;
    }
    assert(hash == VoxelLighting_Hash(-20, -30, -16, -26));
    /* The world moved: the renderer resets the caches on every rebase. */
    VoxelLighting_Reset();
    VoxelBuilder_Init(&builder, storage, 2048);
    builder.lighting = true;
    VoxelBuilder_SetOrigin(&builder, sOffsetX, sOffsetZ);
    VoxelMesh_BeginWindow(-20, -30, -12, -22);
    Ground(&builder, -17, -27);
    assert(builder.count == 24);
    for (unsigned i = 0; i < 24; ++i)
    {
        assert(fabsf(storage[i].x - before[i].x) < 0.00001f);
        assert(fabsf(storage[i].z - before[i].z) < 0.00001f);
        assert(fabsf(storage[i].shade - before[i].shade) < 0.00001f);
    }
    for (unsigned i = 0; i < 2; ++i)
    {
        sMaps[i].originX -= sOffsetX;
        sMaps[i].originY -= sOffsetZ;
    }
    sOffsetX = sOffsetZ = 0;
    VoxelLighting_Reset();

    /* A different meshing window cannot change a seam's lighting. */
    VoxelBuilder_Init(&builder, storage, 2048);
    builder.lighting = true;
    VoxelMesh_BeginWindow(3, 3, 4, 4);
    Ground(&builder, 3, 3);
    assert(memcmp(before, storage, sizeof(before)) == 0);

    /* Low capacity falls back to the original quad instead of losing ground. */
    VoxelBuilder_Init(&builder, storage, 6);
    builder.lighting = true;
    Ground(&builder, 3, 3);
    assert(builder.count == 6 && builder.dropped == 0);
    /* Nor half of one: with room for a triangle and a half, the second quad
     * goes in whole or not at all - never a sliver along its diagonal. */
    VoxelBuilder_Init(&builder, storage, 9);
    builder.lighting = true;
    Ground(&builder, 3, 3);
    Ground(&builder, 4, 3);
    assert(builder.count == 6 && builder.dropped == 2);

    /* Round tree proxies shade the floor southeast of the crown's centre and
     * disappear with the live tiles. */
    sTreePresent = true;
    VoxelLighting_Reset();
    assert(VoxelLighting_Sample(9.74f, 0, 5.13f) < 0.8f);
    sTreePresent = false;
    VoxelLighting_Reset();
    assert(VoxelLighting_Sample(9.74f, 0, 5.13f) == 1.0f);

    /* A railing casts from its line, not from its cell: the ground just
     * south-east of the line is in its shadow, the rest of its own cell -
     * north of the line - in the sun. */
    sRailPresent = true;
    VoxelLighting_Reset();
    assert(VoxelLighting_Sample(12.9f, 0, 11.1f) < 0.8f);
    assert(VoxelLighting_Sample(12.9f, 0, 10.5f) > 0.9f);
    sRailPresent = false;
    VoxelLighting_Reset();

    MovingSunTests();
    sMaps[1].indoor = true;
    VoxelLighting_Reset();
    assert(VoxelLighting_Sample(3.5f, 0, 3.5f) == 1.0f);
    puts("PASS lighting on: sample cache equivalence, ray ceiling, cell skipping, sun, AO, neighbour invalidation, rebase, seams, capacity, live trees, railing footprints, interiors");
#else
    assert(builder.count == 6 && builder.dropped == 0);
    for (unsigned i = 0; i < builder.count; ++i) assert(storage[i].shade == 1.0f);
    puts("PASS lighting off: original quad and colours");
#endif
    return 0;
}
