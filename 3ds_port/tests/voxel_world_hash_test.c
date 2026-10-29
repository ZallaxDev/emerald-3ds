/* Include the real world implementation so fixtures can arrange overlapping
 * instances and compare its batched hash with its scalar raw-block reader. */
#include <assert.h>
#include <stdint.h>
/* field_camera.h declares it for the 3DS build only (PLATFORM_3DS). */
uint32_t GetMapClosedEdges(void);
#include "../src/voxel/voxel_world.c"

struct MapHeader gMapHeader;
struct BackupMapLayout gBackupMapLayout;
struct SaveBlock1 *gSaveBlock1Ptr;
struct Main gMain;
struct ObjectEvent gObjectEvents[OBJECT_EVENTS_COUNT];
struct PlayerAvatar gPlayerAvatar;
const struct Tileset gTileset_General = {0};
static u16 sFortreeAttributes[280] = {
    [628 - NUM_METATILES_IN_PRIMARY] = MB_PUDDLE,
    [657 - NUM_METATILES_IN_PRIMARY] = MB_PUDDLE,
};
const struct Tileset gTileset_Fortree = {.metatileAttributes = sFortreeAttributes};
bool8 MetatileBehavior_IsReflective(u8 b) { return b == MB_PUDDLE || b == MB_POND_WATER; }
bool8 MetatileBehavior_IsIce(u8 b) { return b == MB_ICE; }
bool8 MetatileBehavior_IsSurfableWaterOrUnderwater(u8 b)
{ return b == MB_POND_WATER || b == MB_OCEAN_WATER || b == MB_DEEP_WATER; }
bool8 MetatileBehavior_IsPuddle(u8 b) { return b == MB_PUDDLE; }
bool8 MetatileBehavior_IsShallowFlowingWater(u8 b)
{ return b == MB_SHALLOW_WATER || b == MB_STAIRS_OUTSIDE_ABANDONED_SHIP || b == MB_SHOAL_CAVE_ENTRANCE; }
bool8 MetatileBehavior_IsCounter(u8 b) { return b == MB_COUNTER; }
bool8 MetatileBehavior_IsPC(u8 b) { return b == MB_PC; }
bool8 MetatileBehavior_IsSecretBasePC(u8 b) { return b == MB_SECRET_BASE_PC; }
bool8 MetatileBehavior_IsPlayerRoomPCOn(u8 b) { return b == MB_PLAYER_ROOM_PC_ON; }
static const struct ObjectEventGraphicsInfo *sDynamicInfo;
const struct ObjectEventGraphicsInfo *GetObjectEventGraphicsInfo(u8 id)
{ return id == 1 ? sDynamicInfo : NULL; }
const struct Tileset gTileset_GenericBuilding = {0};
bool VoxelAtlas_IsVoid(const VoxelMapInstance *inst, int metatile) { (void)inst; (void)metatile; return false; }

const void *Port_ResolveAssetPointer(const void *p) { return p; }
static const void *sSizedAsset;
static u32 sSizedBytes;
u32 Port_GetAssetSizeExact(const void *p) { return p == sSizedAsset ? sSizedBytes : 0; }
void LZDecompressWram(const u32 *src, void *dst) { (void)src; (void)dst; }
void CB2_Overworld(void) {}
void CB2_OverworldBasic(void) {}
/* The maps of the instance fixture (InstanceTests), by number; none else. */
#define FIXTURE_GROUP 30
#define FIXTURE_MAPS 10
static struct MapHeader sFixtureHeaders[FIXTURE_MAPS];
static bool sFixtureMaps;
const struct MapHeader *const GetMapHeaderFromConnection(const struct MapConnection *c)
{
    return sFixtureMaps && c->mapGroup == FIXTURE_GROUP && c->mapNum < FIXTURE_MAPS
         ? &sFixtureHeaders[c->mapNum] : NULL;
}
unsigned VoxelRegions_RoleAt(unsigned id, int x, int y)
{ (void)id; (void)x; (void)y; return VOXEL_ROLE_FLOOR; }
VoxelVisualShape Voxel_BehaviorShape(unsigned b) { (void)b; return VOXEL_SHAPE_COUNT; }
int VoxelTree_Part(const void *secondaryTileset, int metatileId)
{ (void)secondaryTileset; (void)metatileId; return -1; }
int VoxelTree_GroundMetatile(const void *secondaryTileset, int metatileId)
{ (void)secondaryTileset; return metatileId; }
bool VoxelRelief_IsDrawn(const VoxelMapInstance *inst) { (void)inst; return false; }
bool VoxelRelief_EdgeFillAt(const VoxelMapInstance *ring, int worldX, int worldY, VoxelEdgeFill *fill)
{ (void)ring; (void)worldX; (void)worldY; (void)fill; return false; }
bool VoxelRelief_EdgeFills(unsigned layoutId, unsigned i, int *metatile)
{ (void)layoutId; (void)i; (void)metatile; return false; }
/* The row's seam between maps 3 and 4 asks map 4 a level up (Hoenn's loops
 * do not close: voxel_relief.c VoxelRelief_SeamStep). */
static bool sSeamStep;
int VoxelRelief_SeamStep(const VoxelMapInstance *a, const VoxelMapInstance *b)
{
    if (!sSeamStep)
        return 0;
    if (a->mapNum == 3 && b->mapNum == 4)
        return 16;
    return a->mapNum == 4 && b->mapNum == 3 ? -16 : 0;
}
void VoxelRelief_ForEachCut(const VoxelMapInstance *inst, VoxelReliefCutVisit visit, void *ctx)
{ (void)inst; (void)visit; (void)ctx; }
const struct MapLayout *Port_GetMapLayoutById(u16 mapLayoutId) { (void)mapLayoutId; return NULL; }
const u16 *MapGridGetCameraBorderAt(int x, int y, u8 *index)
{ (void)x; (void)y; *index = 0; return NULL; }
u32 GetMapClosedEdges(void) { return 0; }
/* The layout swaps' conditions (sLayoutSwaps): no fixture map is one of theirs. */
bool8 FlagGet(u16 id) { (void)id; return FALSE; }
bool8 IsMirageIslandPresent(void) { return FALSE; }

static void Check(int x0, int y0, int x1, int y1)
{
    uint32_t expected = 2166136261u;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            expected = (expected ^ GetRawBlock(x, y, NULL)) * 16777619u;
    assert(VoxelWorld_BlockHash(x0, y0, x1, y1) == expected);
}

static void MaterialAndPayloadTests(void)
{
    static u16 grid[32 * 32];
    struct MapLayout layout = {.primaryTileset = &gTileset_General};
    memset(grid, 0, sizeof(grid));
    grid[MAP_OFFSET * 32 + MAP_OFFSET] = 11;
    grid[MAP_OFFSET * 32 + MAP_OFFSET + 2] = 12;
    grid[MAP_OFFSET * 32 + MAP_OFFSET + 14] = 600;
    gBackupMapLayout.map = grid;
    gBackupMapLayout.width = gBackupMapLayout.height = 32;
    sInstanceCount = 1;
    sInstances[0] = (VoxelMapInstance){.layout = &layout, .width = 16, .height = 16,
        .primaryTileset = &gTileset_General};
    uint8_t used[VOXEL_METATILE_IDS] = {0};
    int view[4] = {0, 0, 2, 2};
    VoxelWorld_SetMaterialView(view, 1);
    VoxelWorld_MarkUsedMetatiles(&gTileset_General, NULL, used);
    assert(used[11] == 2 && used[12] == 1 && used[600] == 0);
    view[0] = 14; view[2] = 16;
    memset(used, 0, sizeof(used));
    VoxelWorld_SetMaterialView(view, 1);
    VoxelWorld_MarkUsedMetatiles(&gTileset_General, NULL, used);
    assert(used[600] == 2 && used[11] == 0);
    VoxelWorld_SetMaterialView(NULL, 0);
    static const u8 artA, artB;
    struct SpriteFrameImage images = {.data = &artA};
    struct ObjectEventGraphicsInfo info = {.images = &images};
    struct ObjectEventTemplate object = {.graphicsId = 1};
    struct MapEvents events = {.objectEventCount = 1, .objectEvents = &object};
    struct MapHeader header = {.events = &events};
    sInstances[0].header = &header; sDynamicInfo = &info;
    uint32_t signature = VoxelWorld_PayloadSignature();
    assert(signature == VoxelWorld_PayloadSignature());
    images.data = &artB;
    assert(signature != VoxelWorld_PayloadSignature());
    sDynamicInfo = NULL; sInstanceCount = 0;
}

static void TileLoadTests(void)
{
    const uint8_t packed[] = {0x10,19,0,0,0x40,'A',0xF0,0};
    struct Tileset tileset = {.isCompressed = true, .tiles = (const void *)packed};
    uint8_t dest[1026], raw[1025];
    VoxelTileLoad load = {0};
    sSizedAsset = packed; sSizedBytes = sizeof(packed);
    memset(dest, 0xCA, sizeof(dest));
    for (unsigned i = 0; i < 19; ++i)
    {
        assert(Voxel_LoadTilesStep(&tileset, dest, 19, &load, 1) == (i == 18));
        assert(load.written == i + 1 && dest[i] == 'A' && dest[i + 1] == 0xCA);
    }
    assert(load.ok);
    load = (VoxelTileLoad){0};
    assert(Voxel_LoadTilesStep(&tileset, dest, 18, &load, 512) && !load.ok);
    load = (VoxelTileLoad){0}; sSizedBytes = 7;
    assert(Voxel_LoadTilesStep(&tileset, dest, 19, &load, 512) && !load.ok);
    const uint8_t invalid[] = {0x10,3,0,0,0x80,0,0};
    tileset.tiles = (const void *)invalid; sSizedAsset = invalid; sSizedBytes = sizeof(invalid);
    load = (VoxelTileLoad){0};
    assert(Voxel_LoadTilesStep(&tileset, dest, 19, &load, 512) && !load.ok && !load.written);
    memset(raw, 0x3B, sizeof(raw)); memset(dest, 0xCA, sizeof(dest));
    tileset.tiles = (const void *)raw; tileset.isCompressed = false;
    sSizedAsset = raw; sSizedBytes = sizeof(raw); load = (VoxelTileLoad){0};
    assert(!Voxel_LoadTilesStep(&tileset, dest, 1025, &load, 512) && load.written == 512);
    assert(!Voxel_LoadTilesStep(&tileset, dest, 1025, &load, 512) && load.written == 1024);
    assert(Voxel_LoadTilesStep(&tileset, dest, 1025, &load, 512) && load.ok);
    assert(!memcmp(raw, dest, 1025) && dest[1025] == 0xCA);
    sSizedAsset = NULL; sSizedBytes = 0;
    puts("PASS tile streaming: bounded literal/match/raw writes, overlap, truncation and bounds");
}

/* A layout a script swaps in (SetCurrentMapLayout) changes the save block's
 * id and the header's layout pointer, not the header's id: the current map's
 * instance takes the save block's, which keys its relief and buildings. */
static void LayoutSwapTest(void)
{
    struct SaveBlock1 save = {0};
    struct MapLayout swapped = {.width = 20, .height = 30};

    gSaveBlock1Ptr = &save;
    gMapHeader = (struct MapHeader){.mapLayout = &swapped, .mapLayoutId = 10};
    save.mapLayoutId = 11;
    VoxelWorld_BuildInstances();
    assert(VoxelWorld_InstanceCount() == 1);
    assert(VoxelWorld_Instance(0)->layoutId == 11);
    assert(VoxelWorld_Instance(0)->layout == &swapped);
    assert(VoxelWorld_Instance(0)->width == 20 && VoxelWorld_Instance(0)->height == 30);
    save.mapLayoutId = 10;
    VoxelWorld_BuildInstances();
    assert(VoxelWorld_Instance(0)->layoutId == 10);
    gSaveBlock1Ptr = NULL;
    gMapHeader = (struct MapHeader){0};
    sInstanceCount = 0;
    puts("PASS layout swap: the current map's instance keys on the save block's layout id");
}

/* A row of maps 20 tiles wide, each joined to the next going east. */
static struct SaveBlock1 sSave;
static struct MapLayout sRowLayout = {.width = 20, .height = 10};
static struct MapConnection sRowLinks[FIXTURE_MAPS][2];
static struct MapConnections sRowConnections[FIXTURE_MAPS];

static void EnterRowMap(unsigned num)
{
    gMapHeader = sFixtureHeaders[num];
    sSave.location.mapGroup = FIXTURE_GROUP;
    sSave.location.mapNum = (u8)num;
    sSave.mapLayoutId = gMapHeader.mapLayoutId;
    gSaveBlock1Ptr = &sSave;
    VoxelWorld_BuildInstances();
}

static const VoxelMapInstance *FindRowMap(unsigned num)
{
    for (unsigned i = 0; i < VoxelWorld_InstanceCount(); ++i)
        if (VoxelWorld_Instance(i)->mapNum == (int)num)
            return VoxelWorld_Instance(i);
    return NULL;
}

static void InstanceTests(void)
{
    VoxelMapInstance before[MAX_VOXEL_MAP_INSTANCES];
    unsigned count;

    for (unsigned k = 0; k < FIXTURE_MAPS; ++k)
    {
        unsigned n = 0;

        if (k > 0)
            sRowLinks[k][n++] = (struct MapConnection){CONNECTION_WEST, 0, FIXTURE_GROUP, (u8)(k - 1)};
        if (k + 1 < FIXTURE_MAPS)
            sRowLinks[k][n++] = (struct MapConnection){CONNECTION_EAST, 0, FIXTURE_GROUP, (u8)(k + 1)};
        sRowConnections[k] = (struct MapConnections){(s32)n, sRowLinks[k]};
        sFixtureHeaders[k] = (struct MapHeader){.mapLayout = &sRowLayout,
                                                .connections = &sRowConnections[k],
                                                .mapLayoutId = 1, .mapType = MAP_TYPE_ROUTE};
    }
    sFixtureMaps = true;

    /* From map 2: its neighbours, theirs, and the one past them within
     * reach (map 5, 40 tiles off); map 6, 60 off, is out of reach. */
    EnterRowMap(2);
    assert(VoxelWorld_Instance(0)->mapNum == 2 && VoxelWorld_Instance(0)->originX == 0);
    for (unsigned k = 0; k <= 5; ++k)
    {
        const VoxelMapInstance *inst = FindRowMap(k);

        assert(inst != NULL && inst->originX == ((int)k - 2) * 20 && inst->originY == 0);
    }
    assert(FindRowMap(6) == NULL && VoxelWorld_InstanceCount() == 6);
    /* Two crossings off are kept however far they lie: wider maps. */
    sRowLayout.width = 70;
    EnterRowMap(2);
    assert(FindRowMap(0) != NULL && FindRowMap(4) != NULL && FindRowMap(5) == NULL);
    sRowLayout.width = 20;

    /* Walking east into map 3: every map placed from map 2 that lies within
     * reach of map 3 is still there, where it was (one map further west). */
    EnterRowMap(2);
    count = VoxelWorld_InstanceCount();
    memcpy(before, sInstances, sizeof(before));
    EnterRowMap(3);
    for (unsigned i = 0; i < count; ++i)
    {
        const VoxelMapInstance *inst = FindRowMap((unsigned)before[i].mapNum);
        /* tiles between it and map 3, at 20-40 */
        int gap = before[i].originX >= 40 ? before[i].originX - 40
                : before[i].originX < 0 ? -before[i].originX : 0;

        if (gap <= VOXEL_WORLD_REACH)
            assert(inst != NULL && inst->originX == before[i].originX - 20);
    }
    /* Walking back west: map 0, now two crossings behind, stays. */
    EnterRowMap(1);
    assert(FindRowMap(0) != NULL && FindRowMap(3) != NULL && FindRowMap(4) != NULL);

    /* A seam that asks a step: the maps past it stand as high as it asks,
     * each placed from the one it was reached by. */
    sSeamStep = true;
    EnterRowMap(2);
    assert(VoxelWorld_Instance(0)->lift == 0);
    assert(FindRowMap(3)->lift == 0 && FindRowMap(4)->lift == 16 && FindRowMap(5)->lift == 16);
    /* Across the seam nothing moves: map 4 keeps the lift it was seen at,
     * and the maps behind it theirs. */
    EnterRowMap(3);
    EnterRowMap(4);
    assert(VoxelWorld_Instance(0)->lift == 16);
    assert(FindRowMap(3)->lift == 0 && FindRowMap(2)->lift == 0 && FindRowMap(5)->lift == 16);
    /* Through a warp (nothing on screen), the lift it was last seen at. */
    sInstanceCount = 0;
    EnterRowMap(4);
    assert(VoxelWorld_Instance(0)->lift == 16 && FindRowMap(3)->lift == 0);
    sInstanceCount = 0;
    EnterRowMap(2);
    assert(VoxelWorld_Instance(0)->lift == 0 && FindRowMap(4)->lift == 16);
    sSeamStep = false;
    sFixtureMaps = false;
    sInstanceCount = 0;
}

int main(void)
{
    u16 live[32 * 32], a[16 * 16], b[16 * 16];
    struct MapLayout layouts[3] = {{.map = a}, {.map = b}, {.map = NULL}};
    for (unsigned i = 0; i < 32 * 32; ++i) live[i] = (u16)(i * 7919u);
    for (unsigned i = 0; i < 16 * 16; ++i)
    { a[i] = (u16)(i * 31u); b[i] = (u16)(i * 97u); }
    gBackupMapLayout.map = live;
    gBackupMapLayout.width = gBackupMapLayout.height = 32;
    sInstanceCount = 4;
    sInstances[0] = (VoxelMapInstance){.width = 16, .height = 16};
    sInstances[1] = (VoxelMapInstance){.layout = &layouts[0], .originX = -8,
        .originY = -4, .width = 16, .height = 16};
    sInstances[2] = (VoxelMapInstance){.layout = &layouts[1], .originX = 12,
        .originY = 10, .width = 16, .height = 16};
    sInstances[3] = (VoxelMapInstance){.layout = &layouts[2], .originX = -12,
        .originY = 2, .width = 16, .height = 16};
    VoxelWorld_BeginBatch();
    for (int y = -16; y < 30; y += 3)
        for (int x = -20; x < 30; x += 3)
        {
            Check(x, y, x + 18, y + 26);
            Check(x, y, x + 80, y + 2);
            Check(x, y, x + 81, y + 2); /* scalar fallback */
            Check(x, y, x, y);
        }
    /* Incomplete live grids must zero their overlap rather than expose a
     * lower-priority neighbour; collision/elevation edits change the hash. */
    gBackupMapLayout.width = gBackupMapLayout.height = 12;
    Check(-20, -16, 40, 32);
    uint32_t before = VoxelWorld_BlockHash(0, 0, 4, 4);
    live[MAP_OFFSET * 12 + MAP_OFFSET] ^= 0xFC00;
    assert(before != VoxelWorld_BlockHash(0, 0, 4, 4));
    Check(0, 0, 4, 4);
    /* The live digest sees the same edits, and only them. */
    uint32_t digest = VoxelWorld_LiveDigest();
    assert(digest == VoxelWorld_LiveDigest());
    live[3] ^= 1;
    assert(digest != VoxelWorld_LiveDigest());
    live[3] ^= 1;
    assert(digest == VoxelWorld_LiveDigest());
    sInstanceCount = 0;
    Check(-20, -20, 40, 40);
    /* Both cells have MB_PUDDLE, but Fortree's 657 is drawn as grass. */
    gBackupMapLayout.width = gBackupMapLayout.height = 12;
    live[MAP_OFFSET + MAP_OFFSET * 12] = 657;
    live[MAP_OFFSET + 1 + MAP_OFFSET * 12] = 628;
    sInstanceCount = 1;
    sInstances[0] = (VoxelMapInstance){.width = 2, .height = 1,
                                     .secondaryTileset = &gTileset_Fortree};
    VoxelWorld_BeginBatch();
    assert(!VoxelWorld_IsVisibleReflectiveSurface(0, 0));
    assert(VoxelWorld_IsVisibleReflectiveSurface(1, 0));
    MaterialAndPayloadTests();
    TileLoadTests();
    LayoutSwapTest();
    InstanceTests();
    puts("PASS instances: maps within reach of the current one, kept across a crossing; "
         "a seam's step lifts the maps past it, kept across a crossing and a warp");
    puts("PASS world hash: scalar equivalence, live digest, overlaps, negative origins, missing maps, live edits, clipping; Fortree puddle art");
    return 0;
}
