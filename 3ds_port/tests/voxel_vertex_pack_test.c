#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include "voxel_vertex_pack.h"

int main(void)
{
    VoxelPackStats stats = {0};
    VoxelVertex vertices[] = {
        {.x = -0.5f / 512, .y = 0.5f / 512, .z = -1.5f / 512, .u = 0.25f, .v = 0.75f, .shade = 1},
        {.x = 100, .y = -100, .shade = 3},
    };
    VoxelGpuVertex packed[2];
    VoxelPackBounds bounds = {{INT32_MAX, INT32_MAX, INT32_MAX}, {INT32_MIN, INT32_MIN, INT32_MIN}};

    VoxelPack_Vertices(vertices, 2, packed, &stats);
    assert(packed[0].x == 0 && packed[0].y == 1 && packed[0].z == -1);
    assert(packed[0].u == 0.25f && packed[0].v == 0.75f && packed[0].shade == 16384);
    assert(packed[1].x == INT16_MAX && packed[1].y == INT16_MIN && packed[1].shade == INT16_MAX);
    assert(stats.errors == 3 && stats.worst == 100 && stats.axis == 'p');
    VoxelPack_GrowBounds(&bounds, &packed[0]);
    VoxelPack_GrowBounds(&bounds, &packed[1]);
    assert(bounds.lo[0] == 0 && bounds.hi[0] == INT16_MAX);
    assert(bounds.lo[1] == INT16_MIN && bounds.hi[1] == 1);
    assert(bounds.lo[2] == -1 && bounds.hi[2] == 0);

    stats = (VoxelPackStats){0};
    for (int integer = -32768; integer <= 32767; ++integer)
    {
        float p = (float)integer / VOXEL_POS_SCALE;
        float shade = (float)integer / VOXEL_SHADE_SCALE;
        assert(VoxelPack_Quantise(p, VOXEL_POS_SCALE, &stats) == integer);
        assert(VoxelPack_Quantise(shade, VOXEL_SHADE_SCALE, &stats) == integer);
    }
    assert(stats.errors == 0);
    puts("PASS voxel vertex packing: GPU layout, rounding, UVs, saturation and chunk bounds");
    return 0;
}
