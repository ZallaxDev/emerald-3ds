/*
 * Host harness for the placements that stand only while a cell holds a
 * metatile (voxel_building.c PlacementLive, VXB9's conditions): loads the
 * buildings.bin tests/voxel_building_when_test.py wrote and prints, for each
 * metatile the switched cell is given, whether each of the two placements'
 * cells is a model's and how high - the one standing always, then the one a
 * script switches. Driven by that test, which judges what comes out.
 *
 *     voxel_building_when_test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "voxel_building.h"
#include "voxel_relief.h"

/* What the loader never calls here: the chunk builders'. */
void VoxelBuilder_Tri(VoxelBuilder *builder, const VoxelVertex *a, const VoxelVertex *b,
                      const VoxelVertex *c)
{
    (void)builder, (void)a, (void)b, (void)c;
}

void VoxelBuilder_Quad(VoxelBuilder *builder, const VoxelVertex *a, const VoxelVertex *b,
                       const VoxelVertex *c, const VoxelVertex *d)
{
    (void)builder, (void)a, (void)b, (void)c, (void)d;
}

float VoxelBuilder_StretchZ(const VoxelBuilder *builder, float z)
{
    (void)builder;
    return z;
}

float VoxelRelief_CellLift(const VoxelMapInstance *inst, int x, int y)
{
    (void)inst, (void)x, (void)y;
    return 0.0f;
}

float VoxelRelief_CellShift(const VoxelMapInstance *inst, int x, int y)
{
    (void)inst, (void)x, (void)y;
    return 0.0f;
}

/* The map: every cell 0x20A but the switched one, (15, 13) in the world. */
static int sSwitched = 0x20A;

int VoxelWorld_GetMetatileId(int worldX, int worldY)
{
    return worldX == 15 && worldY == 13 ? sSwitched : 0x20A;
}

static void Show(const VoxelMapInstance *inst, int x, int y)
{
    int ground = -1;
    float top = 0.0f;
    bool model = VoxelBuildings_CellAt(inst, x, y, &ground, &top);

    printf(" %d,%d:%s", x, y, model ? "model" : "map");
    if (model)
        printf("@%.2f", top);
}

int main(void)
{
    static const int states[] = { 0x20A, 0x222, 0x232 };
    VoxelMapInstance inst;

    memset(&inst, 0, sizeof(inst));
    inst.layoutId = 1;
    inst.originX = 10;
    inst.originY = 10;
    inst.width = 10;
    inst.height = 10;
    if (!VoxelBuildings_Init())
    {
        printf("not loaded\n");
        return 1;
    }
    for (unsigned i = 0; i < sizeof(states) / sizeof(states[0]); ++i)
    {
        sSwitched = states[i];
        VoxelBuildings_MapChanged();
        printf("0x%03X", states[i]);
        Show(&inst, 12, 13);
        Show(&inst, 15, 13);
        printf("\n");
    }
    VoxelBuildings_Shutdown();
    return 0;
}
