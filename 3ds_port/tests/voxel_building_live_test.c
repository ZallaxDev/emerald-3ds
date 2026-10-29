/*
 * What stands on the current map follows the live grid (voxel_building.h,
 * VoxelBuildings_SetDrawings): the models of the map's header layout the
 * live grid shows - the Mirage Tower painted back on Route 111 without it -
 * stand. A synthetic
 * buildings.bin of two models on a 6 x 6 map, its layout drawn two ways.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "voxel_building.h"

#define W 6
#define H 6
#define WITH_TOWER 4    /* the header's layout: the tower and a house */
#define NO_TOWER 5      /* swapped in by a script: the house alone */
#define TOWER_TOP 100
#define TOWER_FOOT 101
#define HOUSE_W 200
#define HOUSE_E 201

static uint16_t sWith[W * H], sWithout[W * H], sLive[W * H];

static const uint16_t *Drawing(int layoutId, int *width, int *height)
{
    *width = W;
    *height = H;
    return layoutId == WITH_TOWER ? sWith : layoutId == NO_TOWER ? sWithout : NULL;
}

int VoxelWorld_GetMetatileId(int x, int y)
{
    assert(x >= 0 && y >= 0 && x < W && y < H);
    return sLive[y * W + x] & 0x3FF;
}

/* What voxel_building.c links against and these checks never reach. */
float VoxelRelief_CellLift(const VoxelMapInstance *inst, int x, int y)
{ (void)inst; (void)x; (void)y; return 0.0f; }
float VoxelRelief_CellShift(const VoxelMapInstance *inst, int x, int y)
{ (void)inst; (void)x; (void)y; return 0.0f; }
float VoxelBuilder_StretchZ(const VoxelBuilder *b, float z) { (void)b; return z; }
void VoxelBuilder_Quad(VoxelBuilder *b, const VoxelVertex *v0, const VoxelVertex *v1,
                       const VoxelVertex *v2, const VoxelVertex *v3)
{ (void)b; (void)v0; (void)v1; (void)v2; (void)v3; }
void VoxelBuilder_Tri(VoxelBuilder *b, const VoxelVertex *v0, const VoxelVertex *v1,
                      const VoxelVertex *v2)
{ (void)b; (void)v0; (void)v1; (void)v2; }
void VoxelGrade_Texels(uint16_t *texels, unsigned count) { (void)texels; (void)count; }

static void Put16(FILE *f, unsigned v) { fputc(v & 0xFF, f); fputc((v >> 8) & 0xFF, f); }
static void Put32(FILE *f, uint32_t v) { Put16(f, v & 0xFFFF); Put16(f, v >> 16); }

/* VXB8 (voxel_building.c): a tower one cell wide and two tall, a house two
 * wide; the tower and the house on WITH_TOWER, the house on NO_TOWER. */
static void WriteFixture(const char *path)
{
    FILE *f = fopen(path, "wb");

    assert(f != NULL);
    fwrite("VXB8", 1, 4, f);
    Put16(f, 1); Put16(f, 2); Put16(f, 2); Put16(f, 3); Put16(f, 4); Put16(f, 0);
    Put32(f, 0); Put16(f, 0); Put16(f, 0);
    Put16(f, 8); Put16(f, 8); Put32(f, 0);                          /* page */
    fputc(1, f); fputc(2, f); Put16(f, 0); Put32(f, 0); Put32(f, 0); Put32(f, 0); /* tower */
    fputc(2, f); fputc(1, f); Put16(f, 0); Put32(f, 0); Put32(f, 0); Put32(f, 2); /* house */
    Put16(f, 0); Put16(f, 0); Put16(f, 0); Put16(f, 0);
    Put16(f, 1); Put16(f, 0); Put16(f, 0); Put16(f, 0);
    /* placements, by layout: layout, pageModel, x, y, ground, extras */
    Put16(f, WITH_TOWER); Put16(f, 0); Put16(f, 1); Put16(f, 0); Put16(f, 1); Put16(f, 0); Put32(f, 0);
    Put16(f, WITH_TOWER); Put16(f, 1); Put16(f, 3); Put16(f, 3); Put16(f, 1); Put16(f, 0); Put32(f, 0);
    Put16(f, NO_TOWER); Put16(f, 1); Put16(f, 3); Put16(f, 3); Put16(f, 1); Put16(f, 0); Put32(f, 0);
    fputc(16, f); fputc(32, f); fputc(16, f); fputc(16, f);         /* heights */
    for (int i = 0; i < 4; ++i)
        Put16(f, 0xFFFF);                                           /* footprints */
    for (int i = 0; i < 4; ++i)
        fputc(0, f);                                                /* quarters */
    fclose(f);
}

static bool Stands(const VoxelMapInstance *inst, int x, int y)
{
    return VoxelBuildings_CellAt(inst, x, y, NULL, NULL);
}

static void Paint(uint16_t top, uint16_t foot)
{
    sLive[0 * W + 1] = top;
    sLive[1 * W + 1] = foot;
    VoxelBuildings_WorldChanged();
}

int main(void)
{
    VoxelMapInstance map = {.layoutId = NO_TOWER, .headerLayoutId = WITH_TOWER, .live = true,
                            .width = W, .height = H};
    VoxelMapInstance neighbour = map;
    uint32_t without, with;

    for (int i = 0; i < W * H; ++i)
        sWith[i] = sWithout[i] = 1;
    sWith[0 * W + 1] = TOWER_TOP;
    sWith[1 * W + 1] = TOWER_FOOT;
    sWith[3 * W + 3] = sWithout[3 * W + 3] = HOUSE_W;
    sWith[3 * W + 4] = sWithout[3 * W + 4] = HOUSE_E;
    memcpy(sLive, sWithout, sizeof(sLive));
    WriteFixture(VOXEL_BUILDINGS_PATH);
    assert(VoxelBuildings_Init());

    /* Host tools read no live grid: the layout's placements as they are. */
    assert(!Stands(&map, 1, 0) && Stands(&map, 3, 3));
    VoxelBuildings_SetDrawings(Drawing);

    /* The swapped layout as it was drawn: its house, no tower. */
    assert(VoxelBuildings_LayoutOf(&map) == NO_TOWER);
    assert(!Stands(&map, 1, 0) && !Stands(&map, 1, 1));
    assert(Stands(&map, 3, 3) && Stands(&map, 4, 3));
    without = VoxelBuildings_LiveSignature(&map);
    /* as it was entered: its chunks, built as a neighbour's, stay */
    assert(without == 0);

    /* A script paints the tower back (Route111_EventScript_ShowTemporary-
     * MirageTower): the header layout's tower stands, with the house. */
    Paint(TOWER_TOP, TOWER_FOOT);
    assert(VoxelBuildings_LayoutOf(&map) == WITH_TOWER);
    assert(Stands(&map, 1, 0) && Stands(&map, 1, 1) && Stands(&map, 3, 3));
    with = VoxelBuildings_LiveSignature(&map);
    assert(with != without);

    /* ... and paints it out again as it crumbles. */
    Paint(1, 1);
    assert(VoxelBuildings_LayoutOf(&map) == NO_TOWER);
    assert(!Stands(&map, 1, 0) && Stands(&map, 3, 3));
    assert(VoxelBuildings_LiveSignature(&map) == without);

    /* A write that leaves every model as it was moves nothing. */
    sLive[5 * W + 5] = 7;
    VoxelBuildings_WorldChanged();
    assert(VoxelBuildings_LiveSignature(&map) == without);

    /* The map entered with the tower, as its header names it: a script that
     * rewrites a model's cells animates it (the Department Store's elevator
     * lights, the lottery's computer), and it stands; nothing moves. */
    map.layoutId = WITH_TOWER;
    Paint(TOWER_TOP, TOWER_FOOT);
    assert(VoxelBuildings_LayoutOf(&map) == WITH_TOWER && Stands(&map, 1, 0));
    assert(VoxelBuildings_LiveSignature(&map) == 0);
    Paint(7, 8);
    assert(Stands(&map, 1, 0) && Stands(&map, 1, 1) && Stands(&map, 3, 3));
    assert(VoxelBuildings_LiveSignature(&map) == 0);

    /* A neighbour reads its layout's drawing, never the live grid. */
    neighbour.layoutId = WITH_TOWER;
    neighbour.live = false;
    assert(Stands(&neighbour, 1, 0));
    assert(VoxelBuildings_LiveSignature(&neighbour) == 0);

    VoxelBuildings_Shutdown();
    puts("PASS live buildings: the header layout's models a script paints back stand, animated models stay, neighbours unchanged");
    return 0;
}
