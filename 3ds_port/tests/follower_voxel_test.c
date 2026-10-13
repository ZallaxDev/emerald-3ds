/* Real follower billboard/shadow emission, with flat terrain and lighting fixtures. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../src/voxel/voxel_entities.c"

static VoxelMapInstance sMap = {.originX = 10, .originY = -20};
static int32_t sX = (MAP_OFFSET + 3) * 16 + 8, sY = (MAP_OFFSET + 5) * 16 + 4;
static bool sVisible = true;
static int sPad = 8;
static float sGround = 0.25f, sLight = 1.0f;

bool CtrFollower_GetPosition(const struct Sprite *sprite, int32_t *x, int32_t *y, int *pad)
{
    (void)sprite;
    *x = sX; *y = sY; *pad = sPad;
    return sVisible;
}
unsigned VoxelWorld_InstanceCount(void) { return 1; }
const VoxelMapInstance *VoxelWorld_Instance(unsigned index) { assert(index == 0); return &sMap; }
const VoxelMapInstance *VoxelWorld_GetInstanceAt(int x, int y) { (void)x; (void)y; return &sMap; }
const int16_t *VoxelRelief_Cell(const VoxelMapInstance *map, int x, int y)
{ (void)map; (void)x; (void)y; return NULL; }
bool VoxelRelief_IsSlope(const int16_t *grid) { (void)grid; return false; }
float VoxelRelief_LiftAt(float x, float z) { (void)x; (void)z; return sGround; }
float VoxelRelief_ShiftAt(float x, float z) { (void)x; (void)z; return 0; }
float VoxelLighting_Sample(float x, float y, float z) { (void)x; (void)y; (void)z; return sLight; }
bool VoxelBuildings_CellAt(const VoxelMapInstance *map, int x, int y, int *metatile, float *top)
{ (void)map; (void)x; (void)y; (void)metatile; (void)top; return false; }
bool VoxelBuildings_FrontAt(const VoxelMapInstance *map, int x, int y, float x0, float x1, float height, float *z)
{ (void)map; (void)x; (void)y; (void)x0; (void)x1; (void)height; (void)z; return false; }
void VoxelBuilder_Quad(VoxelBuilder *builder, const VoxelVertex *a, const VoxelVertex *b,
                       const VoxelVertex *c, const VoxelVertex *d)
{
    assert(builder->count + 6 <= builder->capacity);
    const VoxelVertex vertices[] = {*a, *b, *c, *a, *c, *d};
    memcpy(builder->vertices + builder->count, vertices, sizeof(vertices));
    builder->count += 6;
}

static void Reset(VoxelBuilder *body, VoxelBuilder *shadow) { body->count = shadow->count = 0; }
int main(void)
{
    VoxelVertex bodyVertices[6], shadowVertices[6];
    VoxelBuilder body = {.vertices = bodyVertices, .capacity = 6};
    VoxelBuilder shadow = {.vertices = shadowVertices, .capacity = 6};
    VoxelSpriteSlot slot = {.width = 32, .height = 32};
    struct Sprite sprite = {0};
    VoxelCamera camera = {.targetX = 14, .targetZ = -14.25f};
    VoxelDoor door = {0};
    assert(EmitFollower(&body, &shadow, &slot, 0, &sprite, &camera, &door, 1, 0, 1));
    assert(body.count == 6 && slot.footPad == 8);
    assert(fabsf(bodyVertices[0].y - sGround) < 0.0001f);
    assert(fabsf(bodyVertices[2].y - sGround - 1.5f) < 0.0001f);
    assert(fabsf((bodyVertices[0].x + bodyVertices[1].x) / 2 - 14) < 0.0001f);
    assert(fabsf(bodyVertices[0].z + 14.25f) < 0.0001f);
#if CTR_VOXEL_LIGHTING
    assert(shadow.count == 6);
    assert(fabsf((shadowVertices[0].x + shadowVertices[1].x) / 2 - 14) < 0.0001f);
    assert(fabsf(shadowVertices[0].z - bodyVertices[0].z) < 0.0001f);
    assert(fabsf(shadowVertices[0].y - sGround - VOXEL_CAST_SHADOW_LIFT) < 0.0001f);
    assert(shadowVertices[0].v == bodyVertices[0].v && shadowVertices[0].shade > 0);
#else
    assert(shadow.count == 0);
#endif
    /* Terrain raises both feet and shadow; changing species' padding keeps the same foot position. */
    Reset(&body, &shadow); sGround = 0.75f; sPad = 4;
    assert(EmitFollower(&body, &shadow, &slot, 0, &sprite, &camera, &door, 1, 0, 1));
    assert(fabsf(bodyVertices[0].y - sGround) < 0.0001f);
    assert(fabsf(bodyVertices[2].y - sGround - 1.75f) < 0.0001f);
    Reset(&body, &shadow); sMap.indoor = true;
    assert(EmitFollower(&body, &shadow, &slot, 0, &sprite, &camera, &door, 1, 0, 1));
    assert(body.count == 6 && shadow.count == 0);
    Reset(&body, &shadow); sMap.indoor = false; sLight = 0.2f;
    assert(EmitFollower(&body, &shadow, &slot, 0, &sprite, &camera, &door, 1, 0, 1));
    assert(body.count == 6 && shadow.count == 0);
    Reset(&body, &shadow); sVisible = false;
    assert(!EmitFollower(&body, &shadow, &slot, 0, &sprite, &camera, &door, 1, 0, 1));
    assert(body.count == 0 && shadow.count == 0);
    sVisible = true; camera.targetX = 100;
    assert(EmitFollower(&body, &shadow, &slot, 0, &sprite, &camera, &door, 1, 0, 1));
    assert(body.count == 0 && shadow.count == 0);
    puts("PASS follower voxel: mapped position, transparent feet, terrain, matching cast shadow, indoor/dark/hidden");
    return 0;
}
