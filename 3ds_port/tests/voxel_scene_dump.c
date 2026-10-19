/* What the console builds for one map, chunk by chunk, dumped for the
 * previews and checks of scripts/voxel_engine_view.py: the engine's own code
 * (voxel_mesh_builder.c, voxel_tree.c, voxel_building.c, voxel_lighting.c,
 * voxel_relief.c, voxel_sign.c) over the shipped data (romfs/voxel/*.bin),
 * each chunk built as ctr_voxel.c JobStep builds it - ground row by row,
 * trees, models - into a builder of the console's capacity
 * (VOXEL_CHUNK_SCRATCH), so whatever the console would refuse is refused
 * here too and counted.
 *
 *     voxel_scene_dump IN OUT
 *
 * IN (written by the script): u16 layout id (1-based, gMapLayouts order),
 * u16 width, u16 height, u16 flags (1 indoor, 2 the General primary
 * tileset, 4 Dewford's secondary), width*height u16 metatile ids, then
 * width*height u8 visual shapes (voxel_world.c ClassifyTile as the script
 * reads the behaviours: VoxelVisualShape).
 *
 * OUT: u32 chunks; per chunk: i16 cx, cy, u32 count, dropped, terrainCount,
 * buildingFirst, statics, i32 page; per static: u32 pageModel, u32 lit,
 * f32 x, y, z (world tiles); then count vertices of f32 x, y, z, u, v,
 * shade, positions in world tiles. Terrain uv are slots of a virtual atlas
 * (id i in slot i of a 64x64 grid), trees' of the tree atlas, models' of
 * their page.
 *
 * On stderr: the time the ground rows took (they cast the shadow rays, the
 * phase that makes chunks slow on the console), for before/after timings;
 * with -DSCENE_LIGHT_COUNTERS also the rays' steps and relief tests. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "voxel_mesh_builder.h"
#include "voxel_regions.h"
#include "voxel_atlas.h"
#include "voxel_building.h"
#include "voxel_relief.h"
#include "voxel_sign.h"
#include "voxel_tree.h"
#if CTR_VOXEL_LIGHTING
#include "voxel_lighting.h"
#endif

#ifdef _WIN32
#include <windows.h>
static double Seconds(void)
{
    LARGE_INTEGER t, f;

    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return (double)t.QuadPart / (double)f.QuadPart;
}
#else
#include <time.h>
static double Seconds(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + t.tv_nsec * 1e-9;
}
#endif

#define GRID 64u
/* ctr_voxel.c: VOXEL_CHUNK, VOXEL_CHUNK_SCRATCH (with lighting),
 * VOXEL_CHUNK_STATICS */
#define CHUNK 8
#if CTR_VOXEL_LIGHTING
#define SCRATCH (8192u + 64u * 18u)
#else
#define SCRATCH 8192u
#endif
#define STATICS 32u

/* voxel_tree.c compares a map's secondary tileset against its address */
const char gTileset_Dewford[1];
static const char sGeneral[1], sOther[1];

static VoxelMapInstance sMap;
static VoxelAtlasMap sAtlas;
static uint16_t *sCells;
static uint8_t *sShapes;

bool VoxelWorld_UsesTreeSprites(const VoxelMapInstance *m)
{
    return m != NULL && !m->indoor && m->primaryTileset == sGeneral;
}
const VoxelMapInstance *VoxelWorld_Instance(unsigned i) { return i == 0 ? &sMap : NULL; }
const VoxelMapInstance *VoxelWorld_GetInstanceAt(int x, int y)
{
    return x >= 0 && y >= 0 && x < sMap.width && y < sMap.height ? &sMap : NULL;
}
int VoxelWorld_GetMetatileId(int x, int y)
{
    if (!VoxelWorld_GetInstanceAt(x, y))
        return -1;
    return sCells[y * sMap.width + x];
}
VoxelVisualShape VoxelWorld_ClassifyTile(int x, int y)
{
    int id;

    if (!VoxelWorld_GetInstanceAt(x, y))
        return VOXEL_SHAPE_VOID;
    /* voxel_world.c: a tree's cell is flat, a sign by its region, then the
     * behaviour's shape (the script's) */
    id = VoxelWorld_GetMetatileId(x, y);
    if (VoxelWorld_UsesTreeSprites(&sMap)
     && (VoxelTree_Part(sMap.secondaryTileset, id) >= 0
         || VoxelTree_GroundMetatile(sMap.secondaryTileset, id) != id))
        return VOXEL_SHAPE_FLAT;
    if (VoxelRegions_RoleAt((unsigned)sMap.layoutId, x, y) == VOXEL_ROLE_SIGNPOST)
        return VOXEL_SHAPE_SIGN;
    return (VoxelVisualShape)sShapes[y * sMap.width + x];
}
int VoxelWorld_BorderMetatile(int x, int y) { (void)x; (void)y; return -1; }
bool VoxelWorld_BorderIsWater(int x, int y) { (void)x; (void)y; return false; }
unsigned VoxelWorld_InstanceCount(void) { return 1; }
/* the lighting caches what it casts by this: the map never changes here */
uint32_t VoxelWorld_BlockHash(int x0, int y0, int x1, int y1)
{
    uint32_t h = 2166136261u;

    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            h = (h ^ (uint32_t)VoxelWorld_GetMetatileId(x, y)) * 16777619u;
    return h;
}
void VoxelWorld_GetPlayerWorldCoords(float *x, float *z) { *x = 0; *z = 0; }
void VoxelAtlas_SlotUV(unsigned slot, float *u, float *v, float *ue, float *ve)
{
    *u = (slot % GRID) / (float)GRID; *ue = *u + 1.0f / GRID;
    *v = (slot / GRID) / (float)GRID; *ve = *v + 1.0f / GRID;
}
void VoxelAtlas_SolidUV(VoxelSolidColor c, float *u, float *v, float *ue, float *ve)
{
    VoxelAtlas_SlotUV(GRID * GRID - 1 - (unsigned)c, u, v, ue, ve);
}

static void Put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }

int main(int argc, char **argv)
{
    static VoxelVertex verts[SCRATCH];
    VoxelStaticDraw statics[STATICS];
    FILE *in, *out;
    uint16_t head[4];
    uint32_t chunks = 0, total = 0, dropped = 0;
    long countAt;
    double groundSeconds = 0.0;

    if (argc != 3 || (in = fopen(argv[1], "rb")) == NULL)
        return 2;
    if (fread(head, 2, 4, in) != 4)
        return 2;
    sMap.layoutId = head[0];
    sMap.width = head[1];
    sMap.height = head[2];
    sMap.indoor = (head[3] & 1) != 0;
    sMap.primaryTileset = (head[3] & 2) ? sGeneral : sOther;
    sMap.secondaryTileset = (head[3] & 4) ? gTileset_Dewford : sOther;
    sCells = malloc((size_t)head[1] * head[2] * 2);
    sShapes = malloc((size_t)head[1] * head[2]);
    if (fread(sCells, 2, (size_t)head[1] * head[2], in) != (size_t)head[1] * head[2]
     || fread(sShapes, 1, (size_t)head[1] * head[2], in) != (size_t)head[1] * head[2])
        return 2;
    fclose(in);
    for (unsigned i = 0; i < VOXEL_METATILE_IDS; ++i)
        sAtlas.slotOf[i] = (uint16_t)(i + 1);
    if (!VoxelRelief_Init())
        return 3;
    VoxelRegions_Init();
    VoxelBuildings_Init();
    VoxelSign_Init();
    if ((out = fopen(argv[2], "wb")) == NULL)
        return 2;
    countAt = ftell(out);
    Put32(out, 0);
    for (int cy = 0; cy * CHUNK < sMap.height; ++cy)
        for (int cx = 0; cx * CHUNK < sMap.width; ++cx)
        {
            int x0 = cx * CHUNK, y0 = cy * CHUNK, x1 = x0 + CHUNK, y1 = y0 + CHUNK;
            VoxelBuilder b;
            VoxelBuildingCursor cursor = { 0, 0, 0 };
            uint32_t terrainCount, buildingFirst;
            int16_t c[2] = { (int16_t)cx, (int16_t)cy };
            int page;

            if (x1 > sMap.width) x1 = sMap.width;
            if (y1 > sMap.height) y1 = sMap.height;
            VoxelBuilder_Init(&b, verts, SCRATCH);
            b.statics = statics;
            b.staticCapacity = STATICS;
            VoxelBuilder_SetAtlas(&b, &sAtlas);
            VoxelBuilder_SetOrigin(&b, x0, y0);
            b.base = VoxelRelief_Base(&sMap);
#if CTR_VOXEL_LIGHTING
            b.lighting = !sMap.indoor;
            b.lightingRefine = true;
#endif
            VoxelMesh_BeginWindow(x0 - 1, y0 - VOXEL_CHUNK_MARGIN_NORTH, x1 + 1, y1 + 1);
            {
                double start = Seconds();

                for (int row = y0; row < y1; ++row)
                    for (int col = x0; col < x1; ++col)
                        VoxelMesh_EmitGroundRow(&b, &sMap, col, col + 1, row);
                groundSeconds += Seconds() - start;
            }
            terrainCount = b.count;
            for (int pass = 0; pass < 2; ++pass)
                for (int row = y0; row < y1; ++row)
                    for (int col = x0; col < x1; ++col)
                        VoxelTree_EmitInstance(&b, &sMap, col, row, col + 1, row + 1,
                                               pass == 0 ? VOXEL_TREE_TOPS : VOXEL_TREE_CROWNS);
            buildingFirst = b.count;
            while (!VoxelBuildings_EmitSome(&b, &sMap, x0, y0, x1, y1, &cursor, 0xFFFFFFFFu))
                ;
            page = VoxelBuildings_PageOf(&sMap);
            fwrite(c, 2, 2, out);
            Put32(out, b.count);
            Put32(out, b.dropped);
            Put32(out, terrainCount);
            Put32(out, buildingFirst);
            Put32(out, b.staticCount);
            fwrite(&page, 4, 1, out);
            for (unsigned i = 0; i < b.staticCount; ++i)
            {
                float p[3] = { statics[i].x + x0, statics[i].y, statics[i].z + y0 };
                Put32(out, statics[i].pageModel);
                Put32(out, statics[i].lit);
                fwrite(p, 4, 3, out);
            }
            for (unsigned i = 0; i < b.count; ++i)
            {
                const VoxelVertex *v = &b.vertices[i];
                float f[6] = { v->x + x0, v->y, v->z + y0, v->u, v->v, v->shade };
                fwrite(f, 4, 6, out);
            }
            ++chunks;
            total += b.count;
            dropped += b.dropped;
        }
    fseek(out, countAt, SEEK_SET);
    Put32(out, chunks);
    fclose(out);
    printf("%u chunks, %u vertices, %u triangles dropped\n", chunks, total, dropped);
    fprintf(stderr, "ground %.3f ms", groundSeconds * 1000.0);
#if CTR_VOXEL_LIGHTING
    fprintf(stderr, ", %u rays", VoxelLighting_Rays());
#ifdef SCENE_LIGHT_COUNTERS
    fprintf(stderr, ", %u steps, %u surface tests", VoxelLighting_Steps(),
            VoxelLighting_SurfaceTests());
#endif
#endif
    fprintf(stderr, "\n");
    return 0;
}
