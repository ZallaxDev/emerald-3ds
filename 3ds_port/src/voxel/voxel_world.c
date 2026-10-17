/*
 * Logical map view for the voxel overworld.
 *
 * Ported from the MIT-licensed src/platform/voxel/voxel_world.c of
 * pokeemerald-multiplatform (see NOTICE.md). The classification rules, the
 * instance layout and the "current map reads gBackupMapLayout, connections
 * read the static layout" split are that file's; what is new here is that
 * every payload goes through the asset layer instead of being dereferenced
 * directly, because the linker only kept stubs where the graphics used to be.
 */

#include "global.h"
#include "main.h"
#include "overworld.h"
#include "fieldmap.h"
#include "decompress.h"
#include "metatile_behavior.h"
#include "event_object_movement.h"
#include "field_player_avatar.h"
#include "field_camera.h"
#include "constants/map_types.h"
#include "constants/metatile_behaviors.h"
#include "port_platform.h"

#include "voxel_world.h"
#include "voxel_regions.h"
#include "voxel_tree.h"
#include "voxel_atlas.h"
#ifdef PLATFORM_3DS
#include "voxel_building.h"
#include "field_weather.h"
#include "palette.h"
#include "constants/field_weather.h"
#include "constants/weather.h"
#endif

extern const struct Tileset gTileset_General;
extern const struct Tileset gTileset_Fortree;
extern const struct Tileset gTileset_GenericBuilding;

#include <string.h>

static VoxelMapInstance sInstances[MAX_VOXEL_MAP_INSTANCES];
static unsigned sInstanceCount;

/*
 * Memo for the current batch. Small and linear on purpose: there are at most a
 * handful of distinct tilesets and layouts on screen, so a scan beats anything
 * cleverer, and every entry is dropped at VoxelWorld_BeginBatch.
 */
/* At least one per instance: past a full memo every lookup on the remaining
 * instances falls back to the asset binary search, per tile. */
#define VOXEL_MEMO_SLOTS (MAX_VOXEL_MAP_INSTANCES + 4)

struct VoxelMemo
{
    struct { const void *key, *value; } entry[VOXEL_MEMO_SLOTS];
    unsigned count;
};

static struct VoxelMemo sAttributeMemo, sMapMemo, sBorderMemo;

void VoxelWorld_BeginBatch(void)
{
    sAttributeMemo.count = 0;
    sMapMemo.count = 0;
    sBorderMemo.count = 0;
}

/* Returns true and fills *out when the key is already known. */
static bool MemoGet(const struct VoxelMemo *memo, const void *key, const void **out)
{
    for (unsigned i = 0; i < memo->count; ++i)
        if (memo->entry[i].key == key)
        {
            *out = memo->entry[i].value;
            return true;
        }
    return false;
}

/* A full memo simply stops caching; correctness never depends on it. */
static void MemoPut(struct VoxelMemo *memo, const void *key, const void *value)
{
    if (memo->count >= VOXEL_MEMO_SLOTS)
        return;
    memo->entry[memo->count].key = key;
    memo->entry[memo->count].value = value;
    ++memo->count;
}

/* ── Payload resolvers ──────────────────────────────────────────────────── */

const uint16_t *Voxel_ResolveMap(const void *layoutPtr)
{
    const struct MapLayout *layout = layoutPtr;
    const void *resolved;

    if (layout == NULL || layout->map == NULL)
        return NULL;
    if (MemoGet(&sMapMemo, layout->map, &resolved))
        return resolved;
    resolved = Port_ResolveAssetPointer(layout->map);
    MemoPut(&sMapMemo, layout->map, resolved);
    return resolved;
}

const uint16_t *Voxel_ResolveMetatiles(const void *tilesetPtr)
{
    const struct Tileset *tileset = tilesetPtr;

    if (tileset == NULL || tileset->metatiles == NULL)
        return NULL;
    return Port_ResolveAssetPointer(tileset->metatiles);
}

/*
 * Memoised: classification asks for this once per tile, and resolving it is a
 * binary search over every asset in the build plus a cache lookup. Without the
 * memo a mesh rebuild spends more time finding the attribute table than
 * reading it.
 */
const uint16_t *Voxel_ResolveAttributes(const void *tilesetPtr)
{
    const struct Tileset *tileset = tilesetPtr;
    const void *resolved;

    if (tileset == NULL || tileset->metatileAttributes == NULL)
        return NULL;
    if (MemoGet(&sAttributeMemo, tileset->metatileAttributes, &resolved))
        return resolved;
    resolved = Port_ResolveAssetPointer(tileset->metatileAttributes);
    MemoPut(&sAttributeMemo, tileset->metatileAttributes, resolved);
    return resolved;
}

const uint16_t *Voxel_ResolvePalettes(const void *tilesetPtr)
{
    const struct Tileset *tileset = tilesetPtr;

    if (tileset == NULL || tileset->palettes == NULL)
        return NULL;
    return (const uint16_t *)Port_ResolveAssetPointer(tileset->palettes);
}

unsigned Voxel_MetatileCount(const void *tilesetPtr, unsigned limit)
{
    const struct Tileset *tileset = tilesetPtr;
    u32 bytes;

    if (tileset == NULL || tileset->metatiles == NULL)
        return 0;
    /* A linked (non-stub) payload reports no size; trust the caller's limit. */
    bytes = Port_GetAssetSizeExact(tileset->metatiles);
    if (bytes == 0)
        return limit;
    bytes /= NUM_TILES_PER_METATILE * sizeof(u16);
    return bytes < limit ? (unsigned)bytes : limit;
}

/* ── Availability ───────────────────────────────────────────────────────── */

/* Return completion separately from success, allowing bounded decoding. */
bool Voxel_LoadTilesStep(const void *tilesetPtr, uint8_t *dest, uint32_t destSize,
                         VoxelTileLoad *load, unsigned bytes)
{
    const struct Tileset *tileset = tilesetPtr;
    const uint8_t *src;
    if (load->done) return true;
    if (!tileset || !tileset->tiles || !dest) goto failed;
    src = Port_ResolveAssetPointer(tileset->tiles);
    if (!src) goto failed;
    if (!load->initialized)
    {
        load->packedSize = Port_GetAssetSizeExact(tileset->tiles);
        if (!load->packedSize) goto failed;
        load->size = load->packedSize;
        if (tileset->isCompressed)
        {
            if (load->packedSize < 4 || src[0] != 0x10) goto failed;
            load->size = (uint32_t)src[1] | ((uint32_t)src[2] << 8) | ((uint32_t)src[3] << 16);
            load->source = 4;
        }
        if (!load->size || load->size > destSize) goto failed;
        load->initialized = true;
    }
    if (load->size > destSize) goto failed;
    if (!tileset->isCompressed)
    {
        unsigned count = load->size - load->written;
        if (count > bytes) count = bytes;
        memcpy(dest + load->written, src + load->written, count);
        load->written += count;
    }
    else while (bytes && load->written < load->size)
    {
        if (!load->remaining)
        {
            if (!load->bits)
            {
                if (load->source >= load->packedSize) goto failed;
                load->flags = src[load->source++];
                load->bits = 8;
            }
            bool match = (load->flags & 0x80) != 0;
            load->flags <<= 1;
            --load->bits;
            if (match)
            {
                if (load->packedSize - load->source < 2) goto failed;
                unsigned a = src[load->source++], b = src[load->source++];
                load->remaining = (a >> 4) + 3;
                load->distance = ((a & 15) << 8) + b + 1;
                if (load->distance > load->written) goto failed;
            }
            else
            {
                if (load->source >= load->packedSize) goto failed;
                dest[load->written++] = src[load->source++];
                --bytes;
                continue;
            }
        }
        dest[load->written] = dest[load->written - load->distance];
        ++load->written;
        --load->remaining;
        --bytes;
    }
    if (load->written == load->size) load->done = load->ok = true;
    return load->done;
failed:
    load->done = true;
    load->ok = false;
    return true;
}

bool VoxelWorld_IsMapAvailable(void)
{
    if (gMapHeader.mapLayout == NULL)
        return false;
    if (gSaveBlock1Ptr == NULL)
        return false;
    if (gBackupMapLayout.map == NULL)
        return false;
    return gMain.callback2 == CB2_Overworld || gMain.callback2 == CB2_OverworldBasic;
}

bool VoxelWorld_IsBattleMapAvailable(void)
{
    /* A battle leaves the map it was started from loaded, live grid and all:
     * the field picks up where it stopped when the battle is over. */
    return gMain.inBattle && gMapHeader.mapLayout != NULL && gSaveBlock1Ptr != NULL
        && gBackupMapLayout.map != NULL;
}

/* ── Instances ──────────────────────────────────────────────────────────── */

static void FillInstance(VoxelMapInstance *inst, const struct MapHeader *header,
                         int mapGroup, int mapNum, int originX, int originY)
{
    inst->header = header;
    inst->layout = header->mapLayout;
    inst->primaryTileset = header->mapLayout->primaryTileset;
    inst->secondaryTileset = header->mapLayout->secondaryTileset;
    inst->layoutId = header->mapLayoutId;
    inst->mapGroup = mapGroup;
    inst->mapNum = mapNum;
    inst->originX = originX;
    inst->originY = originY;
    inst->width = header->mapLayout->width;
    inst->height = header->mapLayout->height;
    inst->indoor = header->mapType == MAP_TYPE_INDOOR
                || header->mapType == MAP_TYPE_SECRET_BASE;
}

/*
 * Where the map across `conn` sits, from the map it leaves (at `from`'s
 * origin, of `from`'s size). False for a connection that is not a side.
 */
static bool ConnectionOrigin(const struct MapConnection *conn, const struct MapHeader *neighbour,
                             const VoxelMapInstance *from, int *originX, int *originY)
{
    switch (conn->direction)
    {
    case CONNECTION_NORTH:
        *originX = from->originX + conn->offset;
        *originY = from->originY - neighbour->mapLayout->height;
        return true;
    case CONNECTION_SOUTH:
        *originX = from->originX + conn->offset;
        *originY = from->originY + from->height;
        return true;
    case CONNECTION_WEST:
        *originX = from->originX - neighbour->mapLayout->width;
        *originY = from->originY + conn->offset;
        return true;
    case CONNECTION_EAST:
        *originX = from->originX + from->width;
        *originY = from->originY + conn->offset;
        return true;
    default:
        return false;
    }
}

/*
 * The map across each side connection of `from`. The current map's own are
 * taken as they are; one further away (`far`) only where it is not a map
 * already placed and covers none of their ground - the connections of
 * Hoenn's maps do not always agree with one another's.
 */
static void AddConnections(const VoxelMapInstance *from, bool far)
{
    const struct MapHeader *header = from->header;
    const struct MapConnection *conn;

    if (header == NULL || header->connections == NULL)
        return;
    conn = header->connections->connections;
    for (s32 i = 0; i < header->connections->count; ++i, ++conn)
    {
        const struct MapHeader *neighbour;
        int originX, originY;
        bool clash = false;

        if (sInstanceCount >= MAX_VOXEL_MAP_INSTANCES)
            return;
        if (conn->direction == CONNECTION_DIVE || conn->direction == CONNECTION_EMERGE)
            continue;
        neighbour = GetMapHeaderFromConnection(conn);
        if (neighbour == NULL || neighbour->mapLayout == NULL
         || !ConnectionOrigin(conn, neighbour, from, &originX, &originY))
            continue;
        for (unsigned k = 0; far && k < sInstanceCount && !clash; ++k)
        {
            const VoxelMapInstance *placed = &sInstances[k];

            clash = (placed->mapGroup == conn->mapGroup && placed->mapNum == conn->mapNum)
                 || (originX < placed->originX + placed->width
                     && originX + neighbour->mapLayout->width > placed->originX
                     && originY < placed->originY + placed->height
                     && originY + neighbour->mapLayout->height > placed->originY);
        }
        if (!clash)
            FillInstance(&sInstances[sInstanceCount++], neighbour,
                         conn->mapGroup, conn->mapNum, originX, originY);
    }
}

#ifdef PLATFORM_3DS
/* Ground variant (layout, metatile) is drawn from `inst`'s own tileset of
 * that metatile, the one the atlas composes it from (voxel_atlas.c
 * AtlasSourceOf). */
static bool VariantFits(const VoxelMapInstance *inst, unsigned layoutId, unsigned metatile)
{
    const struct MapLayout *layout = Port_GetMapLayoutById((u16)layoutId);

    if (layout == NULL)
        return false;
    return metatile < NUM_METATILES_IN_PRIMARY
         ? layout->primaryTileset == inst->primaryTileset
         : layout->secondaryTileset == inst->secondaryTileset;
}
#endif

/*
 * The current map, its connections, and theirs. The game loads only the first
 * two, which is all its 15 x 10 tiles of screen can reach; the 3D view sees
 * several times as far, and a map one crossing further away was missing from
 * it - the belt of border trees stood in its place, and on the crossing that
 * made it a neighbour it had to be built from nothing while the view showed
 * black where it lies. Placed from the start, it is built ahead of time like
 * any map in the ring, and the crossing only moves the coordinates. The set
 * depends on the current map alone, never on the view: a change to it is a
 * new epoch, and every chunk in view verified again.
 */
void VoxelWorld_BuildInstances(void)
{
    unsigned near;

#ifdef PLATFORM_3DS
    VoxelBuildings_SetVariantFits(VariantFits);
#endif
    sInstanceCount = 0;
    if (gMapHeader.mapLayout == NULL)
        return;

    /* Instance 0 is always the current map, at the origin. */
    FillInstance(&sInstances[0], &gMapHeader,
                 gSaveBlock1Ptr->location.mapGroup,
                 gSaveBlock1Ptr->location.mapNum, 0, 0);
    sInstanceCount = 1;
    AddConnections(&sInstances[0], false);
    near = sInstanceCount;
    for (unsigned i = 1; i < near; ++i)
        AddConnections(&sInstances[i], true);
}

unsigned VoxelWorld_InstanceCount(void)
{
    return sInstanceCount;
}

const VoxelMapInstance *VoxelWorld_Instance(unsigned index)
{
    return index < sInstanceCount ? &sInstances[index] : NULL;
}

const VoxelMapInstance *VoxelWorld_GetInstanceAt(int worldX, int worldY)
{
    /* The current map covers most lookups, so it is tested on its own. */
    if (sInstanceCount > 0
     && worldX >= 0 && worldX < sInstances[0].width
     && worldY >= 0 && worldY < sInstances[0].height)
        return &sInstances[0];

    for (unsigned i = 1; i < sInstanceCount; ++i)
    {
        const VoxelMapInstance *inst = &sInstances[i];
        int localX = worldX - inst->originX;
        int localY = worldY - inst->originY;

        if (localX >= 0 && localX < inst->width && localY >= 0 && localY < inst->height)
            return inst;
    }
    return NULL;
}

/* ── Block lookups ──────────────────────────────────────────────────────── */

static u16 GetRawBlock(int worldX, int worldY, const VoxelMapInstance **outInst)
{
    const VoxelMapInstance *inst = VoxelWorld_GetInstanceAt(worldX, worldY);
    int localX, localY;

    if (outInst != NULL)
        *outInst = inst;
    if (inst == NULL)
        return 0;

    localX = worldX - inst->originX;
    localY = worldY - inst->originY;

    if (inst == &sInstances[0])
    {
        /* The live layout, so scripted metatile changes are visible. */
        int bx = localX + MAP_OFFSET;
        int by = localY + MAP_OFFSET;

        if (bx >= 0 && bx < gBackupMapLayout.width
         && by >= 0 && by < gBackupMapLayout.height)
            return gBackupMapLayout.map[bx + gBackupMapLayout.width * by];
        return 0;
    }
    else
    {
        const uint16_t *map = Voxel_ResolveMap(inst->layout);

        if (map == NULL)
            return 0;
        return map[localY * inst->width + localX];
    }
}

int VoxelWorld_GetMetatileId(int worldX, int worldY)
{
    return GetRawBlock(worldX, worldY, NULL) & MAPGRID_METATILE_ID_MASK;
}

unsigned VoxelWorld_GetCollision(int worldX, int worldY)
{
    return (GetRawBlock(worldX, worldY, NULL) & MAPGRID_COLLISION_MASK) >> MAPGRID_COLLISION_SHIFT;
}

static u16 GetMetatileAttribute(const VoxelMapInstance *inst, int metatileId)
{
    const uint16_t *attributes;
    unsigned index;

    if (inst == NULL)
        return 0;
    if (metatileId < NUM_METATILES_IN_PRIMARY)
    {
        attributes = Voxel_ResolveAttributes(inst->primaryTileset);
        index = (unsigned)metatileId;
    }
    else
    {
        attributes = Voxel_ResolveAttributes(inst->secondaryTileset);
        index = (unsigned)metatileId - NUM_METATILES_IN_PRIMARY;
    }
    return attributes != NULL ? attributes[index] : 0;
}

unsigned VoxelWorld_GetMetatileBehavior(int worldX, int worldY)
{
    const VoxelMapInstance *inst = NULL;
    u16 block = GetRawBlock(worldX, worldY, &inst);

    return UNPACK_BEHAVIOR(GetMetatileAttribute(inst, block & MAPGRID_METATILE_ID_MASK));
}

bool VoxelWorld_IsVisibleReflectiveSurface(int worldX, int worldY)
{
    const VoxelMapInstance *inst = NULL;
    u16 block = GetRawBlock(worldX, worldY, &inst);
    unsigned metatileId, behavior;

    if (inst == NULL)
        return false;
    metatileId = block & MAPGRID_METATILE_ID_MASK;
    behavior = UNPACK_BEHAVIOR(GetMetatileAttribute(inst, metatileId));
    if (!MetatileBehavior_IsReflective(behavior) && !MetatileBehavior_IsIce(behavior))
        return false;
    /* A bridge's deck over a pond (Route 104's pier, the only one there is):
     * the GBA shows a walker's reflection under the bridge, in the water the
     * planks hide; here the deck is no water surface, and a reflection laid
     * on it was drawn over the planks. The water beside it still reflects. */
    if (behavior == MB_REFLECTION_UNDER_BRIDGE)
        return false;

    /* Fortree's 0x288-0x29A puddle family contains grass artwork despite its
     * MB_PUDDLE behavior. The original 2D ground effect treats it as nearby
     * water; a 3D surface reflection would instead be visible on the grass.
     * Blue puddle metatiles 0x274-0x27D retain their reflections. */
    if (behavior == MB_PUDDLE && inst->secondaryTileset == &gTileset_Fortree
     && metatileId >= 0x288 && metatileId <= 0x29A)
        return false;
    return true;
}

bool VoxelWorld_UsesTreeSprites(const VoxelMapInstance *inst)
{
    return inst != NULL && !inst->indoor && inst->primaryTileset == &gTileset_General;
}

/* ── Classification ─────────────────────────────────────────────────────── */

/*
 * What a cell is drawn as, when no model or relief covers it.
 *
 * Nothing here raises terrain. A cell stands up only when something was built
 * for it from its own drawing - a building (voxel_building.c), a tree
 * (voxel_tree.c), a sign (voxel_sign.c) or the relief of a map read off its
 * art (voxel_relief.c) - and those passes answer for their cells before this
 * is asked. What is left is ground, whatever its collision bit says: guessing
 * walls, roofs and cliffs from it is what put blocks where nothing stands.
 *
 * The exceptions are small things that are not terrain: water, which lies
 * flush with the ground, and indoor furniture.
 */
/*
 * The few behaviours that say what a cell looks like, asked through the
 * game's own predicates: water lies flush with the ground, counters and machines
 * stand up. Everything else says nothing (VOXEL_SHAPE_COUNT).
 */
static VoxelVisualShape BehaviorShape(u8 behavior)
{
    if (MetatileBehavior_IsSurfableWaterOrUnderwater(behavior)
     || MetatileBehavior_IsPuddle(behavior)
     || MetatileBehavior_IsShallowFlowingWater(behavior)
     || behavior == MB_HOT_SPRINGS
     || behavior == MB_REFLECTION_UNDER_BRIDGE)
        return VOXEL_SHAPE_WATER;
    if (MetatileBehavior_IsCounter(behavior))
        return VOXEL_SHAPE_COUNTER;
    if (MetatileBehavior_IsPC(behavior)
     || MetatileBehavior_IsSecretBasePC(behavior)
     || MetatileBehavior_IsPlayerRoomPCOn(behavior)
     || behavior == MB_SECRET_BASE_REGISTER_PC
     || behavior == MB_TELEVISION
     || behavior == MB_CABLE_BOX_RESULTS_1)
        return VOXEL_SHAPE_FURNITURE;
    return VOXEL_SHAPE_COUNT;
}

VoxelVisualShape VoxelWorld_ClassifyTile(int worldX, int worldY)
{
    const VoxelMapInstance *inst = NULL;
    u16 block = GetRawBlock(worldX, worldY, &inst);   /* one instance search for all three reads */
    int metatileId;
    VoxelVisualShape announced;

    if (inst == NULL)
        return VOXEL_SHAPE_VOID;

    metatileId = block & MAPGRID_METATILE_ID_MASK;
    if (VoxelWorld_UsesTreeSprites(inst)
     && (VoxelTree_Part(inst->secondaryTileset, metatileId) >= 0
         || VoxelTree_GroundMetatile(inst->secondaryTileset, metatileId) != metatileId))
        return VOXEL_SHAPE_FLAT;
    if (VoxelRegions_RoleAt((unsigned)inst->layoutId,
                            worldX - inst->originX, worldY - inst->originY)
        == VOXEL_ROLE_SIGNPOST)
        return VOXEL_SHAPE_SIGN;

    /* The cells whose behaviour says what they are: water and furniture. */
    announced = BehaviorShape(UNPACK_BEHAVIOR(GetMetatileAttribute(inst, metatileId)));
    if (announced != VOXEL_SHAPE_COUNT)
        return announced;

    if (inst->indoor)
    {
        /* The black filler outside the rooms: read off the drawing, since
         * its id is the tileset's (622 is the Pokemon Center's floor). */
        if (VoxelAtlas_IsVoid(inst, metatileId))
            return VOXEL_SHAPE_VOID;

        /*
         * Beds, tables and rugs announce nothing at all: Emerald gives them no
         * behaviour of their own, and they are not guessed from their ids.
         * The reference's list for the general indoor tileset names other
         * things in it: a rug's border stood up as a row of black television
         * sets and a bed (House1), the back wall's top row as mats, and a
         * table as two of its four cells. A room's furniture stands up when
         * the room is modelled from its drawing (voxel_building.h).
         */
    }

    return VOXEL_SHAPE_FLAT;
}

/* ── Queries used by the camera and the renderer ────────────────────────── */

void VoxelWorld_GetMapDimensions(int *width, int *height)
{
    if (gMapHeader.mapLayout != NULL)
    {
        if (width != NULL) *width = gMapHeader.mapLayout->width;
        if (height != NULL) *height = gMapHeader.mapLayout->height;
    }
    else
    {
        if (width != NULL) *width = 0;
        if (height != NULL) *height = 0;
    }
}

void VoxelWorld_GetPlayerWorldCoords(float *worldX, float *worldZ)
{
    const struct ObjectEvent *player = &gObjectEvents[gPlayerAvatar.objectEventId];
    float x = (float)(player->currentCoords.x - MAP_OFFSET);
    float z = (float)(player->currentCoords.y - MAP_OFFSET);

    if (sInstanceCount > 0)
    {
        x += (float)sInstances[0].originX;
        z += (float)sInstances[0].originY;
    }
    if (worldX != NULL) *worldX = x;
    if (worldZ != NULL) *worldZ = z;
}

VoxelWeatherClass VoxelWorld_Weather(void)
{
#ifdef PLATFORM_3DS
    switch (GetCurrentWeather())
    {
    case WEATHER_SUNNY:
    case WEATHER_DROUGHT:
        return VOXEL_WEATHER_SUN;
    case WEATHER_RAIN:
    case WEATHER_RAIN_THUNDERSTORM:
    case WEATHER_DOWNPOUR:
        return VOXEL_WEATHER_RAIN;
    case WEATHER_FOG_HORIZONTAL:
    case WEATHER_FOG_DIAGONAL:
    case WEATHER_UNDERWATER:
    case WEATHER_UNDERWATER_BUBBLES:
        return VOXEL_WEATHER_FOG;
    case WEATHER_SNOW:
    case WEATHER_VOLCANIC_ASH:
    case WEATHER_SANDSTORM:
        return VOXEL_WEATHER_PARTICLES;
    case WEATHER_SHADE:
    case WEATHER_SUNNY_CLOUDS:
        return VOXEL_WEATHER_SHADE;
    }
#endif
    return VOXEL_WEATHER_CLEAR;
}

/*
 * The fog's own blend: the sprites come in at EVA 12 (4 under water) over a
 * few frames and leave the same way, so the world's fog follows them.
 */
float VoxelWorld_FogDensity(void)
{
#ifdef PLATFORM_3DS
    float density;

    if (!gWeatherPtr->fogHSpritesCreated && !gWeatherPtr->fogDSpritesCreated)
        return 0.0f;
    density = (float)gWeatherPtr->currBlendEVA / 12.0f;
    return density > 1.0f ? 1.0f : density;
#else
    return 0.0f;
#endif
}

bool VoxelWorld_Underground(void)
{
    return gMapHeader.mapType == MAP_TYPE_UNDERGROUND;
}

bool VoxelWorld_MapIsConnected(void)
{
    return gMapHeader.connections != NULL && gMapHeader.connections->count > 0;
}

unsigned VoxelWorld_ClosedEdges(void)
{
    return GetMapClosedEdges();
}

/*
 * The fade the game has put on the background palettes, as one blend towards
 * a colour. The world's textures are baked from the tilesets' own palettes,
 * so a fade - which rewrites the shown palette every frame - never reaches
 * them; the renderer applies this instead. Sprites need nothing: they are
 * decoded from the shown palette, fade and all.
 *
 * The shown tileset colours are fitted against the unfaded ones as
 * shown = (1 - amount) * unfaded + amount * colour, least squares over the
 * three channels: exact for a fade towards black, white or the grey of the
 * battle intro, and close for anything else a script does. Only while a fade
 * is under way or has left the screen faded: at rest, a weather colour map
 * (rain darkening the palette) stays the lighting grade's job, as before.
 */
static float FadeChannel(unsigned color, unsigned shift)
{
    return (float)((color >> shift) & 31);
}

static void FadeFit(const u16 *shown, const u16 *unfaded, unsigned count,
                    float *amount, float rgb[3])
{
    float n = (float)count, cov = 0.0f, var = 0.0f;
    float su[3] = {0}, sf[3] = {0}, suu[3] = {0}, suf[3] = {0};

    for (unsigned i = 0; i < count; ++i)
        for (unsigned c = 0; c < 3; ++c)
        {
            float u = FadeChannel(unfaded[i], c * 5), f = FadeChannel(shown[i], c * 5);

            su[c] += u;
            sf[c] += f;
            suu[c] += u * u;
            suf[c] += u * f;
        }
    for (unsigned c = 0; c < 3; ++c)
    {
        cov += n * suf[c] - su[c] * sf[c];
        var += n * suu[c] - su[c] * su[c];
    }
    /* Colours all alike tell nothing of the slope: take them as the target. */
    float keep = var > 1.0f ? cov / var : 0.0f;

    if (keep < 0.0f) keep = 0.0f;
    if (keep > 1.0f) keep = 1.0f;
    *amount = 1.0f - keep;
    for (unsigned c = 0; c < 3; ++c)
    {
        float offset = (sf[c] - keep * su[c]) / n;
        float value = *amount > 0.01f ? offset / *amount : 0.0f;

        if (value < 0.0f) value = 0.0f;
        if (value > 31.0f) value = 31.0f;
        rgb[c] = value / 31.0f;
    }
}

bool VoxelWorld_ScreenFade(float *amount, float rgb[3])
{
    *amount = 0.0f;
    rgb[0] = rgb[1] = rgb[2] = 0.0f;
#ifdef PLATFORM_3DS
    {
        bool fading = gPaletteFade.active || gPaletteFade.y != 0
                   || (gWeatherPtr != NULL && gWeatherPtr->palProcessingState == WEATHER_PAL_STATE_SCREEN_FADING_IN)
                   || (gWeatherPtr != NULL && gWeatherPtr->palProcessingState == WEATHER_PAL_STATE_SCREEN_FADING_OUT);
        const u16 *shown = (const u16 *)PLTT, *unfaded = gPlttBufferUnfaded;
        unsigned count = NUM_PALS_TOTAL * 16;

        /*
         * In a battle the world stands in for the scenery, and fades as its
         * three palettes do (BG 2-4, LoadBattleTerrainGfx), whatever does it:
         * a palette fade, or a move blending the background towards a colour
         * without one. The battlers' palettes fade on their own.
         */
        if (gMain.inBattle)
        {
            shown += BG_PLTT_ID(2);
            unfaded += BG_PLTT_ID(2);
            count = 3 * 16;
            fading = true;
        }
        if (!fading || memcmp(shown, unfaded, count * sizeof(u16)) == 0)
            return false;
        FadeFit(shown, unfaded, count, amount, rgb);
        /* Below one step of the game's own 16 it is rounding, not a fade. */
        if (*amount < 1.0f / 32.0f)
            *amount = 0.0f;
    }
#endif
    return *amount > 0.0f;
}

void VoxelWorld_GetLocation(int *mapGroup, int *mapNum)
{
    if (mapGroup != NULL) *mapGroup = gSaveBlock1Ptr != NULL ? gSaveBlock1Ptr->location.mapGroup : -1;
    if (mapNum != NULL) *mapNum = gSaveBlock1Ptr != NULL ? gSaveBlock1Ptr->location.mapNum : -1;
}

static int sMaterialView[4], sMaterialMargin;
static bool sMaterialViewSet;

void VoxelWorld_SetMaterialView(const int rect[4], int margin)
{
    sMaterialViewSet = rect != NULL;
    if (rect != NULL) memcpy(sMaterialView, rect, sizeof(sMaterialView));
    sMaterialMargin = margin;
}

void VoxelWorld_MarkUsedMetatiles(const void *primaryTileset, const void *secondaryTileset,
                                  uint8_t *used)
{
    for (unsigned i = 0; i < sInstanceCount; ++i)
    {
        const VoxelMapInstance *inst = &sInstances[i];
        const struct MapLayout *layout = inst->layout;

        if (inst->primaryTileset != primaryTileset
         || inst->secondaryTileset != secondaryTileset)
            continue;

        int x0 = 0, y0 = 0, x1 = inst->width, y1 = inst->height;
        if (sMaterialViewSet)
        {
            if (x0 < sMaterialView[0] - sMaterialMargin - inst->originX)
                x0 = sMaterialView[0] - sMaterialMargin - inst->originX;
            if (y0 < sMaterialView[1] - sMaterialMargin - inst->originY)
                y0 = sMaterialView[1] - sMaterialMargin - inst->originY;
            if (x1 > sMaterialView[2] + sMaterialMargin - inst->originX)
                x1 = sMaterialView[2] + sMaterialMargin - inst->originX;
            if (y1 > sMaterialView[3] + sMaterialMargin - inst->originY)
                y1 = sMaterialView[3] + sMaterialMargin - inst->originY;
        }
        const uint16_t *map = i == 0 ? gBackupMapLayout.map : Voxel_ResolveMap(layout);
        if (map != NULL)
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x)
                {
                    int bx = i == 0 ? x + MAP_OFFSET : x;
                    int by = i == 0 ? y + MAP_OFFSET : y;
                    int stride = i == 0 ? gBackupMapLayout.width : inst->width;
                    if (i == 0 && (bx < 0 || by < 0 || bx >= stride || by >= gBackupMapLayout.height))
                        continue;
                    unsigned m = map[by * stride + bx] & MAPGRID_METATILE_ID_MASK;
                    unsigned priority = !sMaterialViewSet ||
                        (x + inst->originX >= sMaterialView[0] && x + inst->originX < sMaterialView[2]
                      && y + inst->originY >= sMaterialView[1] && y + inst->originY < sMaterialView[3]) ? 2u : 1u;
                    if (used[m] < priority) used[m] = priority;
                }
    }
    /* The belt is drawn with the current map's atlas, and from a neighbour's
     * border as well as its own (VoxelWorld_BorderMetatile): any border whose
     * metatiles these tilesets hold. */
    for (unsigned i = 0; i < sInstanceCount; ++i)
    {
        const VoxelMapInstance *inst = &sInstances[i];
        const struct MapLayout *layout = inst->layout;
        const uint16_t *border;
        bool secondary = false;

        if (inst->primaryTileset != primaryTileset || layout == NULL || layout->border == NULL)
            continue;
        border = Port_ResolveAssetPointer(layout->border);
        if (border == NULL)
            continue;
        for (unsigned t = 0; t < 4; ++t)
            secondary |= (border[t] & MAPGRID_METATILE_ID_MASK) >= NUM_METATILES_IN_PRIMARY;
        if (secondary && inst->secondaryTileset != secondaryTileset)
            continue;
        for (unsigned t = 0; t < 4; ++t)
            used[border[t] & MAPGRID_METATILE_ID_MASK] = 2;
    }
    /* The replacement removes canopy fringes even on maps that never used
     * their bare ground tile. Keep that material available in the atlas. */
    if (primaryTileset == &gTileset_General)
        for (int m = 0; m < NUM_METATILES_TOTAL; ++m)
        {
            int ground = VoxelTree_GroundMetatile(secondaryTileset, m);
            if (used[m] > used[ground])
                used[ground] = used[m];
        }
}

/*
 * A digest of the whole live grid, which is the only map data that changes
 * while it is on screen: connections are read from their static layouts. When
 * it is unchanged no chunk can have gone stale, so the per-chunk signatures
 * below need computing only on the frames where it moves - instead of every
 * visible chunk, every frame.
 *
 * Two words at a time and a multiply per word: a few thousand cycles for the
 * largest layout, far less than one chunk's signature.
 */
uint32_t VoxelWorld_LiveDigest(void)
{
    const u16 *map = gBackupMapLayout.map;
    uint32_t hash = 2166136261u;
    unsigned area, i;

    if (map == NULL || gBackupMapLayout.width <= 0 || gBackupMapLayout.height <= 0)
        return 0;
    area = (unsigned)gBackupMapLayout.width * (unsigned)gBackupMapLayout.height;
    hash = (hash ^ area) * 16777619u;
    hash = (hash ^ (uint32_t)(uintptr_t)map) * 16777619u;
    for (i = 0; i + 1 < area; i += 2)
        hash = (hash ^ ((uint32_t)map[i] | ((uint32_t)map[i + 1] << 16))) * 16777619u;
    if (i < area)
        hash = (hash ^ map[i]) * 16777619u;
    return hash;
}

uint32_t VoxelWorld_BlockHash(int x0, int y0, int x1, int y1)
{
    uint32_t hash = 2166136261u;
    uint16_t row[80];

    /* Resolve/copy a span per map and row instead of finding an instance and
     * resolving its payload for every tile of every overlapping light halo.
     * Reverse instance order preserves GetInstanceAt's first-match precedence. */
    if (x1 > x0 && x1 - x0 <= (int)(sizeof(row) / sizeof(row[0])))
    {
        for (int y = y0; y < y1; ++y)
        {
            memset(row, 0, (size_t)(x1 - x0) * sizeof(row[0]));
            for (unsigned i = sInstanceCount; i-- > 0;)
            {
                const VoxelMapInstance *inst = &sInstances[i];
                const uint16_t *map;
                int left = x0 > inst->originX ? x0 : inst->originX;
                int right = x1 < inst->originX + inst->width ? x1 : inst->originX + inst->width;
                if (left >= right || y < inst->originY || y >= inst->originY + inst->height)
                    continue;
                memset(row + left - x0, 0, (size_t)(right - left) * sizeof(row[0]));
                if (i == 0)
                {
                    int by = y - inst->originY + MAP_OFFSET;
                    if (by < 0 || by >= gBackupMapLayout.height)
                        continue;
                    if (left < inst->originX - MAP_OFFSET) left = inst->originX - MAP_OFFSET;
                    if (right > inst->originX + gBackupMapLayout.width - MAP_OFFSET)
                        right = inst->originX + gBackupMapLayout.width - MAP_OFFSET;
                    if (left >= right) continue;
                    map = gBackupMapLayout.map + by * gBackupMapLayout.width
                        + left - inst->originX + MAP_OFFSET;
                }
                else
                {
                    map = Voxel_ResolveMap(inst->layout);
                    if (map == NULL) continue;
                    map += (y - inst->originY) * inst->width + left - inst->originX;
                }
                memcpy(row + left - x0, map, (size_t)(right - left) * sizeof(row[0]));
            }
            for (int x = 0; x < x1 - x0; ++x)
                hash = (hash ^ row[x]) * 16777619u;
        }
        return hash;
    }

    /* FNV-1a over the raw blocks, not the metatile ids: collision and
     * elevation are part of what the classifier reads, so a change to them is
     * a change to the geometry even when the drawing is the same. */
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
        {
            hash ^= GetRawBlock(x, y, NULL);
            hash *= 16777619u;
        }
    return hash;
}

/*
 * The metatile Emerald tiles outside the map.
 *
 * A window reaches past the edge of the world on purpose - that is where the
 * connected maps are - but where no map reaches either, the renderer drew
 * nothing and the view ended in the clear colour. The 2D game does not: it
 * tiles the layout's 2x2 border block outwards for ever, which is why a town
 * looks like it sits in a wood rather than on a plate over a black sky.
 *
 * Whose border, and in which phase, is fieldmap.c's to say: the nearest map's
 * whose border the current tilesets draw, the same answer the 2D camera
 * gives, and the same from either side of a crossing.
 */
int VoxelWorld_BorderMetatile(int worldX, int worldY)
{
    const uint16_t *border;
    const void *source, *resolved;
    u8 index;

    if (sInstanceCount == 0 || sInstances[0].layout == NULL)
        return -1;
    source = MapGridGetCameraBorderAt(worldX - sInstances[0].originX,
                                      worldY - sInstances[0].originY, &index);
    if (source == NULL)
        return -1;
    if (!MemoGet(&sBorderMemo, source, &resolved))
    {
        resolved = Port_ResolveAssetPointer(source);
        MemoPut(&sBorderMemo, source, resolved);
    }
    border = resolved;
    if (border == NULL)
        return -1;
    return (int)(border[index] & MAPGRID_METATILE_ID_MASK);
}

bool VoxelWorld_BorderIsWater(int worldX, int worldY)
{
    int metatileId = VoxelWorld_BorderMetatile(worldX, worldY);

    return metatileId >= 0 && !sInstances[0].indoor
        && BehaviorShape(UNPACK_BEHAVIOR(GetMetatileAttribute(&sInstances[0], metatileId)))
           == VOXEL_SHAPE_WATER;
}

static unsigned AddPayload(const void **payloads, unsigned count, unsigned max, const void *ptr)
{
    if (ptr == NULL || count >= max)
        return count;
    for (unsigned i = 0; i < count; ++i)
        if (payloads[i] == ptr)
            return count;
    payloads[count] = ptr;
    return count + 1;
}

static unsigned AddMapPayloads(const void **payloads, unsigned count, unsigned max,
                               const struct MapLayout *layout)
{
    const struct Tileset *tilesets[2];

    if (layout == NULL)
        return count;
    tilesets[0] = layout->primaryTileset;
    tilesets[1] = layout->secondaryTileset;
    count = AddPayload(payloads, count, max, layout->border);
    for (unsigned t = 0; t < 2; ++t)
    {
        if (tilesets[t] == NULL)
            continue;
        count = AddPayload(payloads, count, max, tilesets[t]->metatileAttributes);
        count = AddPayload(payloads, count, max, tilesets[t]->metatiles);
        count = AddPayload(payloads, count, max, tilesets[t]->palettes);
        count = AddPayload(payloads, count, max, tilesets[t]->tiles);
    }
    return count;
}

/*
 * The pictures of a map's people. The game spawns an object event as the
 * camera comes within reach of it and reads its picture then: in a town full
 * of them, walking was one read off the card after another, each a frame the
 * CPU came back to too late. The first frame's picture holds them all.
 */
static unsigned AddObjectPayloads(const void **payloads, unsigned count, unsigned max,
                                  const struct MapHeader *header)
{
    const struct MapEvents *events = header != NULL ? header->events : NULL;

    if (events == NULL || events->objectEvents == NULL)
        return count;
    for (unsigned i = 0; i < events->objectEventCount && count < max; ++i)
    {
        const struct ObjectEventGraphicsInfo *info =
            GetObjectEventGraphicsInfo(events->objectEvents[i].graphicsId);

        if (info != NULL && info->images != NULL)
            count = AddPayload(payloads, count, max, info->images[0].data);
    }
    return count;
}

unsigned VoxelWorld_NearbyPayloads(const void **payloads, unsigned max)
{
    unsigned count = 0;

    /* The current map's, then its people, the maps around it, and theirs. */
    if (sInstanceCount > 0)
    {
        count = AddMapPayloads(payloads, count, max, sInstances[0].layout);
        count = AddObjectPayloads(payloads, count, max, sInstances[0].header);
    }
    for (unsigned i = 1; i < sInstanceCount; ++i)
        count = AddMapPayloads(payloads, count, max, sInstances[i].layout);
    for (unsigned i = 1; i < sInstanceCount; ++i)
        count = AddObjectPayloads(payloads, count, max, sInstances[i].header);
    for (unsigned i = 1; i < sInstanceCount; ++i)
    {
        const struct MapHeader *header = sInstances[i].header;
        const struct MapConnection *conn;

        if (header == NULL || header->connections == NULL)
            continue;
        conn = header->connections->connections;
        for (s32 c = 0; c < header->connections->count; ++c, ++conn)
        {
            const struct MapHeader *next;

            if (conn->direction == CONNECTION_DIVE || conn->direction == CONNECTION_EMERGE)
                continue;
            next = GetMapHeaderFromConnection(conn);
            if (next != NULL)
                count = AddMapPayloads(payloads, count, max, next->mapLayout);
        }
    }
    return count;
}

uint32_t VoxelWorld_PayloadSignature(void)
{
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < sInstanceCount; ++i)
    {
        const struct MapHeader *header = sInstances[i].header;
        const struct MapEvents *events = header != NULL ? header->events : NULL;
        hash = (hash ^ (uint32_t)(uintptr_t)sInstances[i].layout) * 16777619u;
        hash = (hash ^ (uint32_t)(uintptr_t)header) * 16777619u;
        if (events == NULL || events->objectEvents == NULL)
            continue;
        for (unsigned j = 0; j < events->objectEventCount; ++j)
        {
            const struct ObjectEventGraphicsInfo *info =
                GetObjectEventGraphicsInfo(events->objectEvents[j].graphicsId);
            const void *payload = info != NULL && info->images != NULL ? info->images[0].data : NULL;
            hash = (hash ^ (uint32_t)(uintptr_t)payload) * 16777619u;
        }
    }
    return hash;
}
