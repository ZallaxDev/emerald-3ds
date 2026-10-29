/*
 * Host harness for the building pages' animated texels (voxel_building.c
 * VoxelBuildings_ComposeAnimRun): composes every run of buildings.bin from a
 * BG VRAM image - the 1024 tiles of a map's tilesets, 32 bytes each, as the
 * game holds them at one frame of its animations - and writes each run as
 * u32 run, page, byte offset, bytes, then its texels. Driven by
 * tests/voxel_building_anim_test.py, which judges what comes out.
 *
 *     voxel_building_anim_test VRAM OUT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "voxel_building.h"
#include "voxel_relief.h"

/* What the loader never calls here: the chunk builders' and the world's. */
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

int VoxelWorld_GetMetatileId(int worldX, int worldY)
{
    (void)worldX, (void)worldY;
    return 0;
}

static void Put32(FILE *out, unsigned v)
{
    unsigned char b[4] = { v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF };
    fwrite(b, 1, 4, out);
}

int main(int argc, char **argv)
{
    static uint8_t vram[1024 * 32];
    static uint16_t texels[64 * 1024];
    FILE *in, *out;

    if (argc != 3)
    {
        fprintf(stderr, "usage: %s VRAM OUT\n", argv[0]);
        return 2;
    }
    in = fopen(argv[1], "rb");
    if (in == NULL || fread(vram, 1, sizeof(vram), in) != sizeof(vram))
    {
        fprintf(stderr, "%s: not a 32 KiB BG VRAM image\n", argv[1]);
        return 2;
    }
    fclose(in);
    if (!VoxelBuildings_Init())
    {
        fprintf(stderr, "buildings.bin did not load\n");
        return 1;
    }
    out = fopen(argv[2], "wb");
    if (out == NULL)
        return 2;
    for (unsigned run = 0; run < VoxelBuildings_AnimRunCount(); ++run)
    {
        unsigned page, offset, bytes;
        int layout;

        if (!VoxelBuildings_AnimRun(run, &page, &layout, &offset, &bytes)
         || bytes > sizeof(texels))
        {
            fprintf(stderr, "run %u unreadable\n", run);
            return 1;
        }
        memset(texels, 0xA5, sizeof(texels));
        VoxelBuildings_ComposeAnimRun(run, vram, texels);
        Put32(out, run);
        Put32(out, page);
        Put32(out, offset);
        Put32(out, bytes);
        fwrite(texels, 1, bytes, out);
    }
    fclose(out);
    printf("%u animated runs composed\n", VoxelBuildings_AnimRunCount());
    VoxelBuildings_Shutdown();
    return 0;
}
