/* The terrain of one map as the game builds it, dumped for the relief
 * checker (scripts/voxel_relief_lines.py): the actual C emitter run over the
 * whole layout with the shipped relief.bin and buildings.bin, every vertex
 * written out.
 *
 *     voxel_mesh_dump IN OUT
 *
 * IN (written by the checker): u16 layout id (1-based, gMapLayouts order),
 * u16 width, u16 height, then width*height u16 metatile ids.
 * OUT: u32 vertex count, then per vertex float x, y, z, u, v, shade.
 *
 * The atlas is a virtual one: id i sits in slot i of a 64x64 grid of slots,
 * so a triangle's UVs give back the atlas id it samples and the texel within
 * it (VOXEL_CUT_FIRST + k: cut variant k; VOXEL_METATILE_REAL + i: ground
 * variant i). Every cell is ground (the sea lies flush with it anyway). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "voxel_mesh_builder.h"
#include "voxel_regions.h"
#include "voxel_atlas.h"
#include "voxel_building.h"
#include "voxel_relief.h"
#include "voxel_sign.h"

#define GRID 64u

/* voxel_tree.c only compares a map's secondary tileset against its address */
const char gTileset_Dewford[1], gTileset_Fortree[1], gTileset_Mauville[1], gTileset_Lavaridge[1],
    gTileset_Rustboro[1], gTileset_Lilycove[1], gTileset_EverGrande[1],
    gTileset_BattleFrontierOutsideWest[1], gTileset_Mossdeep[1], gTileset_Fallarbor[1];

static VoxelMapInstance sMap;
static VoxelAtlasMap sAtlas;
static uint16_t *sCells;

unsigned VoxelRegions_RoleAt(unsigned id, int x, int y)
{
    (void)id; (void)x; (void)y;
    return VOXEL_ROLE_FLOOR;
}
bool VoxelWorld_UsesTreeSprites(const VoxelMapInstance *m) { (void)m; return false; }
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
    return VoxelWorld_GetInstanceAt(x, y) ? VOXEL_SHAPE_FLAT : VOXEL_SHAPE_VOID;
}
int VoxelWorld_BorderMetatile(int x, int y) { (void)x; (void)y; return -1; }
bool VoxelWorld_BorderIsWater(int x, int y) { (void)x; (void)y; return false; }
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

int main(int argc, char **argv)
{
    static VoxelVertex verts[3000000];
    VoxelBuilder b;
    FILE *in, *out;
    uint16_t head[3];
    uint32_t count;

    if (argc != 3 || (in = fopen(argv[1], "rb")) == NULL)
        return 2;
    if (fread(head, 2, 3, in) != 3)
        return 2;
    sMap.layoutId = head[0];
    sMap.width = head[1];
    sMap.height = head[2];
    sCells = malloc((size_t)head[1] * head[2] * 2);
    if (fread(sCells, 2, (size_t)head[1] * head[2], in) != (size_t)head[1] * head[2])
        return 2;
    fclose(in);
    for (unsigned i = 0; i < VOXEL_METATILE_IDS; ++i)
        sAtlas.slotOf[i] = (uint16_t)(i + 1);
    if (!VoxelRelief_Init())
        return 3;
    VoxelBuildings_Init();
    VoxelSign_Init();
    VoxelBuilder_Init(&b, verts, sizeof(verts) / sizeof(verts[0]));
    VoxelBuilder_SetAtlas(&b, &sAtlas);
    VoxelBuilder_SetOrigin(&b, 0, 0);
    VoxelMesh_BeginWindow(0, 0, sMap.width, sMap.height);
    VoxelMesh_EmitInstance(&b, &sMap, 0, 0, sMap.width, sMap.height);
    if ((out = fopen(argv[2], "wb")) == NULL)
        return 2;
    count = b.count;
    fwrite(&count, 4, 1, out);
    for (unsigned i = 0; i < b.count; ++i)
    {
        const VoxelVertex *v = &b.vertices[i];
        float f[6] = { v->x, v->y, v->z, v->u, v->v, v->shade };
        fwrite(f, 4, 6, out);
    }
    fclose(out);
    printf("%u vertices, %u dropped, %u uncovered\n", b.count, b.dropped, b.uncovered);
    return 0;
}
