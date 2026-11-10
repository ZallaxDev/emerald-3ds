#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "voxel_building.h"
#include "voxel_sign.h"
#include "voxel_regions.h"
#include "voxel_relief.h"

#ifndef SIGN_MASK_PATH
#define SIGN_MASK_PATH "build/voxel_lamps_fixture.bin"
#endif

static unsigned sRole = VOXEL_ROLE_SIGNPOST;

void VoxelBuilder_Quad(VoxelBuilder *builder, const VoxelVertex *first,
                       const VoxelVertex *second, const VoxelVertex *third,
                       const VoxelVertex *fourth)
{
    const VoxelVertex *corners[6] = {first, second, third, first, third, fourth};
    assert(builder->count + 6 <= builder->capacity);
    for (unsigned index = 0; index < 6; ++index)
    {
        VoxelVertex *vertex = &builder->vertices[builder->count++];
        *vertex = *corners[index];
        vertex->x -= builder->originX;
        vertex->z += builder->shift - builder->originZ;
        vertex->y += builder->base + builder->lift;
    }
}

void VoxelMesh_Top(VoxelBuilder *builder, float xpos, float zpos, float height, float inset,
                   float u0, float v0, float u1, float v1, float shade)
{
    (void)builder; (void)xpos; (void)zpos; (void)height; (void)inset;
    (void)u0; (void)v0; (void)u1; (void)v1; (void)shade;
}

bool VoxelMesh_MetatileUV(VoxelBuilder *builder, int metatileId,
                          float *u0, float *v0, float *u1, float *v1)
{
    (void)builder; (void)metatileId; (void)u0; (void)v0; (void)u1; (void)v1;
    return false;
}

bool VoxelMesh_TileUV(VoxelBuilder *builder, int xpos, int zpos,
                      float *u0, float *v0, float *u1, float *v1)
{
    (void)builder; (void)xpos; (void)zpos; (void)u0; (void)v0; (void)u1; (void)v1;
    return false;
}

const int16_t *VoxelRelief_Cell(const VoxelMapInstance *inst, int xpos, int zpos)
{
    (void)inst; (void)xpos; (void)zpos;
    return NULL;
}

/* These fixtures have no relief grid, depth map or drawn slopes. */
const int16_t *VoxelRelief_Depth(const VoxelMapInstance *inst, int x, int y)
{
    (void)inst; (void)x; (void)y;
    return NULL;
}

bool VoxelRelief_IsDrawn(const VoxelMapInstance *inst)
{
    (void)inst;
    return false;
}

bool VoxelRelief_IsSlope(const int16_t *grid)
{
    (void)grid;
    return false;
}

/* No lamp stands on a bridge's deck. */
bool VoxelBuildings_DeckAt(const VoxelMapInstance *inst, float worldX, float worldZ,
                           unsigned elevation, float *lift)
{
    (void)inst; (void)worldX; (void)worldZ; (void)elevation; (void)lift;
    return false;
}

float VoxelRelief_Base(const VoxelMapInstance *inst)
{
    (void)inst;
    return 2.0f;
}

float VoxelRelief_CellLift(const VoxelMapInstance *inst, int xpos, int zpos)
{
    (void)inst; (void)xpos; (void)zpos;
    return 0.5f;
}

float VoxelRelief_CellShift(const VoxelMapInstance *inst, int xpos, int zpos)
{
    (void)inst; (void)xpos; (void)zpos;
    return 0.25f;
}

unsigned VoxelRegions_RoleAt(unsigned layout, int xpos, int zpos)
{
    if (layout != 7)
        return VOXEL_ROLE_WALL;
    if (xpos == 4 && zpos == 4)
        return sRole;
    if (xpos == 5)
        return VOXEL_ROLE_WATER;
    return VOXEL_ROLE_FLOOR;
}

int main(void)
{
    uint16_t record[37] = {7, 4, 4};
    uint8_t header[8] = {'V', 'X', 'S', '3', 1, 0, 0, 0};
    uint16_t *head = record + 19;
    for (unsigned row = 0; row < 16; ++row)
        record[3 + row] = 0x0180;
    head[0] = 0x0080;
    for (unsigned row = 3; row < 12; ++row)
        head[row] = 0x1FF8;
    assert(VoxelSign_IsLantern(head));
    uint16_t wall[16] = {0xFFFF};
    wall[4] = 0xFFFF;
    assert(!VoxelSign_IsLantern(wall));
    head[4] = 0x0180;
    assert(!VoxelSign_IsLantern(head));
    head[4] = 0x1FF8;
    FILE *fixture = fopen(SIGN_MASK_PATH, "wb");
    assert(fixture != NULL);
    assert(fwrite(header, 1, sizeof(header), fixture) == sizeof(header));
    assert(fwrite(record, 1, sizeof(record), fixture) == sizeof(record));
    assert(fclose(fixture) == 0);
    VoxelSign_Init();
    VoxelMapInstance map = {.layoutId = 7, .width = 12, .height = 12,
                            .originX = -10, .originY = 20};
    VoxelVertex glassVertices[67], groundVertices[2049];
    memset(glassVertices, 0, sizeof(glassVertices));
    memset(groundVertices, 0, sizeof(groundVertices));
    glassVertices[66].shade = 17.0f;
    groundVertices[2048].shade = 19.0f;
    VoxelBuilder glass = {.vertices = glassVertices, .capacity = 66,
                          .originX = -10, .originZ = 20};
    VoxelBuilder ground = {.vertices = groundVertices, .capacity = 2048,
                           .originX = -10, .originZ = 20};
    VoxelSign_EmitNight(&glass, &ground, &map, -20, 10, 10, 40);
    assert(glass.count == 60 && ground.count > 0 && ground.count % 6 == 0);
    bool front = false, back = false;
    for (unsigned index = 0; index < glass.count; ++index)
    {
        const VoxelVertex *vertex = &glassVertices[index];
        assert(vertex->x >= 4.3125f && vertex->x <= 4.6875f);
        assert(vertex->y >= 3.8125f && vertex->y <= 4.125f);
        assert(vertex->shade == 1.0f);
        front |= fabsf(vertex->z - 5.0625f) < 0.00001f;
        back |= fabsf(vertex->z - 4.9375f) < 0.00001f;
    }
    assert(front && back);
    for (unsigned index = 0; index < ground.count; ++index)
    {
        const VoxelVertex *vertex = &groundVertices[index];
        assert(vertex->y == 2.5f);
        assert(vertex->shade >= 0.0f && vertex->shade <= 1.0f);
        assert(vertex->x <= 5.0f || vertex->x >= 6.0f);
    }
    assert(glassVertices[66].shade == 17.0f && groundVertices[2048].shade == 19.0f);
    glass.count = ground.count = 0;
    map.indoor = true;
    VoxelSign_EmitNight(&glass, &ground, &map, -20, 10, 10, 40);
    assert(glass.count == 0 && ground.count == 0);
    map.indoor = false;
    VoxelSign_EmitNight(&glass, &ground, &map, 0, 0, 1, 1);
    assert(glass.count == 0 && ground.count == 0);
    sRole = VOXEL_ROLE_WALL;
    VoxelSign_EmitNight(&glass, &ground, &map, -20, 10, 10, 40);
    assert(glass.count == 0 && ground.count == 0);
    sRole = VOXEL_ROLE_SIGNPOST;
    glass.capacity = 11;
    VoxelSign_EmitNight(&glass, &ground, &map, -20, 10, 10, 40);
    assert(glass.count == 0 && ground.count == 0);
    glass.capacity = 60;
    ground.capacity = 5;
    VoxelSign_EmitNight(&glass, &ground, &map, -20, 10, 10, 40);
    assert(glass.count == 60 && ground.count == 0);
    VoxelSign_Shutdown();
    assert(remove(SIGN_MASK_PATH) == 0);
    puts("PASS city lamps: glass rims, front/back, relief, connected maps, floor-only pools, bounds, interiors, capacity");
    return 0;
}
