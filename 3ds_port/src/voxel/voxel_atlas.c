/*
 * Metatile atlas builder. See voxel_atlas.h and NOTICE.md.
 */

/* Before global.h, which redefines abs() as a macro over stdlib's prototype. */
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "fieldmap.h"
#include "constants/rgb.h"
#include "port_platform.h"
#include "port_log.h"

#include "3ds_video.h"
#include "voxel_atlas.h"
#include "voxel_building.h"
#include "voxel_grade.h"
#include "voxel_relief.h"

/* Largest 4bpp tileset payload: 512 tiles of 32 bytes, doubled for headroom
 * because a compressed payload only declares its size at run time. */
#define VOXEL_TILE_SCRATCH 0x10000u

#define TILES_PER_TILESET 512u
#define SUBTILE_PX 8u

/* In texels; see VoxelAtlas_SlotUV. */
#define VOXEL_ATLAS_EDGE_INSET (1.0f / 32.0f)

void VoxelAtlas_SlotUV(unsigned slot, float *u0, float *v0, float *u1, float *v1)
{
    unsigned page = slot / VOXEL_ATLAS_MAX_SLOTS;
    slot %= VOXEL_ATLAS_MAX_SLOTS;
    float x = (float)(slot % VOXEL_ATLAS_COLUMNS) * VOXEL_ATLAS_SLOT;
    float y = (float)(slot / VOXEL_ATLAS_COLUMNS) * VOXEL_ATLAS_SLOT;

    /*
     * Row 0 in memory is v=1, the same convention the 2D compositor uses.
     * The slot's rows stop a thirty-second of a texel short of its north and
     * south edges. The slots touch, with no gutter, and while the camera
     * scrolls north or south a tile's edge passes through pixel centres now
     * and then: the sample there sits on v0 or v1 exactly, and a rounding
     * error in the interpolation (or, at v0, nearest sampling's choice of
     * the larger side) took a row of the slot above or below - a one-pixel
     * line of another metatile's art right along the tile edge. Inside the
     * slot no sample moves by more than that thirty-second.
     */
    *u0 = page * 2.0f + x / VOXEL_ATLAS_W;
    *u1 = page * 2.0f + (x + VOXEL_ATLAS_SLOT) / VOXEL_ATLAS_W;
    *v0 = 1.0f - (y + VOXEL_ATLAS_EDGE_INSET) / VOXEL_ATLAS_H;
    *v1 = 1.0f - (y + VOXEL_ATLAS_SLOT - VOXEL_ATLAS_EDGE_INSET) / VOXEL_ATLAS_H;
}

void VoxelAtlas_SolidUV(VoxelSolidColor color, float *u0, float *v0, float *u1, float *v1)
{
    VoxelAtlas_SlotUV(VOXEL_SOLID_BASE + (unsigned)color, u0, v0, u1, v1);
}

/*
 * BGR555, the same encoding the tilesets use, so one conversion path serves
 * both.
 *
 * Brighter than the reference's glColor3f triples, which run from 0.10 to
 * 0.40. Those were picked for a renderer that draws them at full brightness on
 * a monitor; here they sit next to metatile art on a 3DS panel, and 0.20 grey
 * reads as a hole in the room rather than as a television.
 */
static const uint16_t sSolidColors[VOXEL_SOLID_COUNT] = {
    [VOXEL_SOLID_WOOD]      = RGB2(18, 13, 8),  /* table sides  */
    [VOXEL_SOLID_WOOD_DARK] = RGB2(13, 9, 6),   /* table legs   */
    [VOXEL_SOLID_GREY]      = RGB2(14, 14, 15), /* set stand    */
    [VOXEL_SOLID_BLACK]     = RGB2(8, 8, 9),    /* set body     */
    [VOXEL_SOLID_SHEET]     = RGB2(26, 24, 22), /* bedding      */
};

static void FillSolidSlots(uint16_t *dest)
{
    for (unsigned c = 0; c < VOXEL_SOLID_COUNT; ++c)
    {
        unsigned slot = VOXEL_SOLID_BASE + c;
        unsigned baseX = (slot % VOXEL_ATLAS_COLUMNS) * VOXEL_ATLAS_SLOT;
        unsigned baseY = (slot / VOXEL_ATLAS_COLUMNS) * VOXEL_ATLAS_SLOT;
        uint16_t texel = VoxelGrade_RGBA5551(sSolidColors[c]);

        for (unsigned y = 0; y < VOXEL_ATLAS_SLOT; ++y)
            for (unsigned x = 0; x < VOXEL_ATLAS_SLOT; ++x)
                dest[CtrVideo_Texel(baseX + x, baseY + y, VOXEL_ATLAS_W)] = texel;
    }
}

struct AtlasSource
{
    const uint8_t *primaryTiles, *secondaryTiles;
    const uint16_t *primaryMetatiles, *secondaryMetatiles;
    const uint16_t *primaryPalettes, *secondaryPalettes;
    unsigned primaryCount, secondaryCount;
    /* Every colour a metatile entry can name, already converted to the
     * texture format, and whether it is black in the source. Index 0 of each
     * palette is the opaque backdrop layer 0 draws with. */
    uint16_t texel[16][16];
    bool black[16][16];
};

static uint16_t LookupColor(const struct AtlasSource *src, unsigned paletteId, unsigned colorIdx)
{
    const uint16_t *palette = paletteId < NUM_PALS_IN_PRIMARY
                            ? src->primaryPalettes : src->secondaryPalettes;

    if (palette == NULL)
        return 0;
    return palette[paletteId * 16 + colorIdx];
}

static void BuildColorTable(struct AtlasSource *src)
{
    uint16_t backdrop = src->primaryPalettes != NULL ? src->primaryPalettes[0] : 0;

    for (unsigned p = 0; p < 16; ++p)
        for (unsigned c = 0; c < 16; ++c)
        {
            uint16_t bgr15 = c == 0 ? backdrop : LookupColor(src, p, c);

            src->texel[p][c] = VoxelGrade_RGBA5551(bgr15);
            src->black[p][c] = (bgr15 & 0x7FFF) == 0;
        }
}

/*
 * Offset of each pixel of an 8x8 block inside the PICA's swizzled layout. A
 * subtile always starts on an 8-texel boundary, so it is one contiguous run of
 * 64 texels and only its first texel needs the full address computation.
 */
static uint8_t sMorton[64];

static void InitMorton(void)
{
    if (sMorton[63] != 0)
        return;
    for (unsigned i = 0; i < 64; ++i)
        sMorton[i] = (uint8_t)CtrVideo_Texel(i & 7, i / 8, 8);
}

/* The metatile's entry table, or NULL if this id has no composable data. */
static const uint16_t *MetatileEntries(const struct AtlasSource *src, unsigned m)
{
    if (m < NUM_METATILES_IN_PRIMARY)
    {
        if (src->primaryMetatiles == NULL || m >= src->primaryCount)
            return NULL;
        return src->primaryMetatiles + m * NUM_TILES_PER_METATILE;
    }
    else
    {
        unsigned local = m - NUM_METATILES_IN_PRIMARY;

        if (src->secondaryMetatiles == NULL || local >= src->secondaryCount)
            return NULL;
        return src->secondaryMetatiles + local * NUM_TILES_PER_METATILE;
    }
}

/*
 * What an atlas id is drawn from: a real metatile, whole, or a ground variant
 * (voxel_atlas.h) - the metatile of the variant's tileset less the quarters
 * of its upper layer a model stands for. buildings.bin names a variant's
 * tileset by a layout that draws it. False for a variant of another pair.
 */
static const struct Tileset *sVariantTileset[VOXEL_VARIANTS];
static bool sVariantsResolved;
static const struct Tileset *sCutTileset[VOXEL_CUTS];
static bool sCutsResolved;

static const struct Tileset *TilesetOf(unsigned layoutId, unsigned metatile)
{
    const struct MapLayout *layout = Port_GetMapLayoutById((u16)layoutId);

    if (layout == NULL)
        return NULL;
    return metatile < NUM_METATILES_IN_PRIMARY ? layout->primaryTileset : layout->secondaryTileset;
}

/* A cut tile (voxel_relief.h): its metatile and the rows of its background. */
static bool CutSourceOf(const struct Tileset *primary, const struct Tileset *secondary,
                        unsigned i, unsigned *metatile, const uint8_t **cut)
{
    unsigned layoutId;

    if (!sCutsResolved && VoxelRelief_CutCount() > 0)
    {
        for (unsigned k = 0; k < VOXEL_CUTS; ++k)
        {
            unsigned m;
            const uint8_t *rows;

            sCutTileset[k] = VoxelRelief_CutVariant(k, &layoutId, &m, &rows)
                           ? TilesetOf(layoutId, m) : NULL;
        }
        sCutsResolved = true;
    }
    if (i >= VOXEL_CUTS || !VoxelRelief_CutVariant(i, &layoutId, metatile, cut))
        return false;
    return sCutTileset[i] != NULL
        && sCutTileset[i] == (*metatile < NUM_METATILES_IN_PRIMARY ? primary : secondary);
}

static bool AtlasSourceOf(const struct Tileset *primary, const struct Tileset *secondary,
                          unsigned id, unsigned *metatile, unsigned *hidden, const uint8_t **cut)
{
    unsigned layoutId, quarters, i = id - VOXEL_METATILE_REAL;

    *cut = NULL;
    if (id < VOXEL_METATILE_REAL)
    {
        *metatile = id;
        *hidden = 0;
        return true;
    }
    if (id >= VOXEL_CUT_FIRST)
    {
        *hidden = 0;
        return CutSourceOf(primary, secondary, id - VOXEL_CUT_FIRST, metatile, cut);
    }
    if (!sVariantsResolved)
    {
        for (unsigned k = 0; k < VOXEL_VARIANTS; ++k)
        {
            unsigned m;
            const struct MapLayout *layout;

            sVariantTileset[k] = NULL;
            if (!VoxelBuildings_Variant(k, &layoutId, &m, &quarters))
                continue;
            layout = Port_GetMapLayoutById((u16)layoutId);
            if (layout != NULL)
                sVariantTileset[k] = m < NUM_METATILES_IN_PRIMARY
                                   ? layout->primaryTileset : layout->secondaryTileset;
        }
        /* only once the file is read: an atlas composed before it would
         * otherwise leave every variant unresolved for good */
        sVariantsResolved = VoxelBuildings_Variant(0, &layoutId, &i, &quarters);
        i = id - VOXEL_METATILE_REAL;
    }
    if (i >= VOXEL_VARIANTS || !VoxelBuildings_Variant(i, &layoutId, metatile, hidden))
        return false;
    return sVariantTileset[i] != NULL
        && sVariantTileset[i] == (*metatile < NUM_METATILES_IN_PRIMARY ? primary : secondary);
}

/*
 * Composes one metatile straight into its packed slot of the destination,
 * less the upper-layer quarters in `hidden`. Returns whether everything it
 * drew is black - what an interior is filled with outside its rooms (see
 * sVoid below). A quadrant whose tiles are missing draws nothing and does not
 * count as black.
 */
static bool ComposeAt(const struct AtlasSource *src, const uint16_t *entries,
                      uint16_t *dest, unsigned baseX, unsigned baseY, unsigned width,
                      unsigned hidden, const uint8_t *cut)
{
    bool black = true;

    for (unsigned layer = 0; layer < 2; ++layer)
    {
        for (unsigned quadrant = 0; quadrant < 4; ++quadrant)
        {
            uint16_t entry = entries[layer * 4 + quadrant];
            unsigned tileId = entry & 0x3FF;
            unsigned paletteId = (entry >> 12) & 0xF;
            unsigned flipX = (entry >> 10) & 1 ? SUBTILE_PX - 1 : 0;
            unsigned flipY = (entry >> 11) & 1 ? SUBTILE_PX - 1 : 0;
            const uint16_t *texels = src->texel[paletteId];
            const bool *blacks = src->black[paletteId];
            const uint8_t *tile;
            uint16_t *block;

            if (tileId < TILES_PER_TILESET)
                tile = src->primaryTiles != NULL ? src->primaryTiles + tileId * 32 : NULL;
            else
                tile = src->secondaryTiles != NULL
                     ? src->secondaryTiles + (tileId - TILES_PER_TILESET) * 32 : NULL;
            if (tile == NULL)
            {
                black = false;
                continue;
            }
            if (layer != 0 && (hidden >> quadrant) & 1u)
                continue;
            block = dest + CtrVideo_Texel(baseX + (quadrant % 2) * SUBTILE_PX,
                                          baseY + (quadrant / 2) * SUBTILE_PX, width);

            for (unsigned py = 0; py < SUBTILE_PX; ++py)
            {
                const uint8_t *row = tile + (py ^ flipY) * 4;
                const uint8_t *morton = &sMorton[py * SUBTILE_PX];

                for (unsigned px = 0; px < SUBTILE_PX; ++px)
                {
                    unsigned srcX = px ^ flipX;
                    unsigned colorIdx = (row[srcX / 2] >> ((srcX & 1) * 4)) & 0xF;

                    /* Layer 0 is the opaque base; layer 1 keys out index 0. */
                    if (layer != 0 && colorIdx == 0)
                        continue;
                    /* Index 0 of layer 0 is the backdrop, whatever palette. */
                    if (colorIdx == 0)
                    {
                        block[morton[px]] = src->texel[0][0];
                        black = black && src->black[0][0];
                    }
                    else
                    {
                        block[morton[px]] = texels[colorIdx];
                        black = black && blacks[colorIdx];
                    }
                }
            }
        }
    }
    /* a cut tile's background is clear: the alpha test leaves the ground
     * drawn flat under it to show (less what its straight diagonals take
     * in: VoxelRelief_CutClear) */
    if (cut != NULL)
    {
        uint16_t clear[16];

        VoxelRelief_CutClear(cut, clear);
        for (unsigned y = 0; y < VOXEL_ATLAS_SLOT && y < 16; ++y)
            for (unsigned x = 0; x < 16; ++x)
            {
                int from = VoxelRelief_CutFringe(cut, clear, (int)x, (int)y);

                if ((clear[y] >> x) & 1u)
                    dest[CtrVideo_Texel(baseX + x, baseY + y, width)] &= (uint16_t)~1u;
                else if (from >= 0)
                    /* kept for the polygon's edge: the rock's colour */
                    dest[CtrVideo_Texel(baseX + x, baseY + y, width)] =
                        dest[CtrVideo_Texel(baseX + (unsigned)(from % 16), baseY + (unsigned)(from / 16), width)];
            }
    }
    return black;
}

static bool ComposeMetatile(const struct AtlasSource *src, const uint16_t *entries,
                            uint16_t *dest, unsigned slot, unsigned hidden, const uint8_t *cut)
{
    return ComposeAt(src, entries, dest, (slot % VOXEL_ATLAS_COLUMNS) * VOXEL_ATLAS_SLOT,
                     (slot / VOXEL_ATLAS_COLUMNS) * VOXEL_ATLAS_SLOT, VOXEL_ATLAS_W, hidden, cut);
}

/*
 * The metatiles that draw nothing but black: what an interior is filled with
 * outside its rooms. Which id that is depends on the tileset pair - 0x001 in
 * the Building tileset, 0x201 in a house's, 0x208 in a gym's, 0x245 in Mirage
 * Tower - so it is read off the drawing when the pair's atlas is composed,
 * and remembered for the pairs most recently used - twice as many as there
 * are atlases.
 */
#define VOID_PAIRS 8
static struct
{
    const struct Tileset *primary, *secondary;
    uint8_t black[VOXEL_METATILE_IDS / 8];
    unsigned stamp;
} sVoid[VOID_PAIRS];
static unsigned sVoidStamp;

static uint8_t *VoidBits(const struct Tileset *primary, const struct Tileset *secondary,
                         bool claim)
{
    unsigned oldest = 0;

    for (unsigned i = 0; i < VOID_PAIRS; ++i)
    {
        if (sVoid[i].primary == primary && sVoid[i].secondary == secondary && primary != NULL)
        {
            /* Used, so kept: an interior whose atlas is still resident must
             * not lose its filler to pairs composed after it. */
            sVoid[i].stamp = ++sVoidStamp;
            return sVoid[i].black;
        }
        if (sVoid[i].stamp < sVoid[oldest].stamp)
            oldest = i;
    }
    if (!claim)
        return NULL;
    memset(&sVoid[oldest], 0, sizeof(sVoid[oldest]));
    sVoid[oldest].primary = primary;
    sVoid[oldest].secondary = secondary;
    sVoid[oldest].stamp = ++sVoidStamp;
    return sVoid[oldest].black;
}

bool VoxelAtlas_IsVoid(const VoxelMapInstance *inst, int metatile)
{
    const uint8_t *bits;

    if (inst == NULL || metatile < 0 || metatile >= (int)VOXEL_METATILE_IDS)
        return false;
    bits = VoidBits(inst->primaryTileset, inst->secondaryTileset, false);
    return bits != NULL && (bits[metatile >> 3] & (1u << (metatile & 7))) != 0;
}

/*
 * The composition, a piece at a time. A town's atlas is a few hundred
 * metatiles and two decompressed tilesets: 70-130 ms on an Old 3DS, which in
 * one go was the stall of every first sight of a new map. As a job it is
 * spread over frames against their budget - each tileset's tiles a step, then
 * a handful of metatiles a step - and composed into a map of its own, which
 * the caller swaps in with the upload so no chunk ever sees a half-built one.
 */
enum { ATLAS_LOAD_PRIMARY, ATLAS_LOAD_SECONDARY, ATLAS_COMPOSE, ATLAS_DONE };

static void JobSource(const VoxelAtlasJob *job, struct AtlasSource *src)
{
    /* Re-resolved every step: a payload the asset cache retires between
     * frames comes back on its next resolution. */
    src->primaryTiles = job->primaryLoaded ? job->primaryTiles : NULL;
    src->secondaryTiles = job->secondaryLoaded ? job->secondaryTiles : NULL;
    src->primaryMetatiles = Voxel_ResolveMetatiles(job->primary);
    src->secondaryMetatiles = Voxel_ResolveMetatiles(job->secondary);
    src->primaryPalettes = Voxel_ResolvePalettes(job->primary);
    src->secondaryPalettes = Voxel_ResolvePalettes(job->secondary);
    src->primaryCount = Voxel_MetatileCount(job->primary, NUM_METATILES_IN_PRIMARY);
    src->secondaryCount = Voxel_MetatileCount(job->secondary,
                                              NUM_METATILES_TOTAL - NUM_METATILES_IN_PRIMARY);
}

void VoxelAtlas_JobCancel(VoxelAtlasJob *job)
{
    free(job->primaryTiles);
    free(job->secondaryTiles);
    job->primaryTiles = job->secondaryTiles = NULL;
    job->active = false;
}

bool VoxelAtlas_JobBegin(VoxelAtlasJob *job, const VoxelMapInstance *inst, uint16_t *dest,
                         const VoxelAtlasMap *base)
{
    memset(job, 0, sizeof(*job));
    if (inst == NULL || dest == NULL)
        return false;
    job->primary = inst->primaryTileset;
    job->secondary = inst->secondaryTileset;
    job->mapGroup = inst->mapGroup;
    job->mapNum = inst->mapNum;
    job->dest = dest;
    job->extend = base != NULL;
    if (base != NULL)
        job->map = *base;
    /* Only the ids the maps on this tileset pair actually reference, as they
     * stand now: the maps on screen may change before the job ends, and the
     * next extension catches up with anything they add. */
    VoxelWorld_MarkUsedMetatiles(job->primary, job->secondary, job->used);
    /* and the pair's ground variants, a few slots: composed whether or not
     * a model on screen stands on them yet */
    for (unsigned id = VOXEL_METATILE_REAL; id < VOXEL_METATILE_IDS; ++id)
    {
        unsigned m, hidden;
        const uint8_t *cut;

        if (AtlasSourceOf(job->primary, job->secondary, id, &m, &hidden, &cut) && m < VOXEL_METATILE_REAL && job->used[m])
            job->used[id] = job->used[m];
    }
    /* An older in-flight chunk can request an extension after all of its
     * materials were already published. Do not decode both tilesets and
     * recompose/upload every existing page just to publish the same map.
     * Match the compose pass's retry rules, including derived absent ids. */
    if (base != NULL)
    {
        bool needed = false;
        for (unsigned id = 0; id < VOXEL_METATILE_IDS; ++id)
        {
            unsigned slot = base->slotOf[id];
            if (job->used[id] && (slot == 0 || slot == VOXEL_SLOT_PENDING
             || (slot == VOXEL_SLOT_ABSENT && id >= VOXEL_METATILE_REAL)))
            {
                needed = true;
                break;
            }
        }
        if (!needed)
        {
            job->ok = true;
            return false;
        }
    }
    job->primaryTiles = malloc(VOXEL_TILE_SCRATCH);
    job->secondaryTiles = malloc(VOXEL_TILE_SCRATCH);
    if (job->primaryTiles == NULL || job->secondaryTiles == NULL)
    {
        VoxelAtlas_JobCancel(job);
        PORT_LOG("[ERROR] VOXEL atlas: no heap for tileset scratch\n");
        return false;
    }
    job->active = true;
    memset(dest, 0, VOXEL_ATLAS_PIXELS * sizeof(uint16_t));
    FillSolidSlots(dest);
    InitMorton();
    job->priority = 2;
    job->phase = ATLAS_LOAD_PRIMARY;
    return true;
}

bool VoxelAtlas_JobStep(VoxelAtlasJob *job, unsigned metatiles)
{
    struct AtlasSource src;
    uint8_t *voidBits;

    if (!job->active || job->pageReady)
        return true;
    switch (job->phase)
    {
    case ATLAS_LOAD_PRIMARY:
    case ATLAS_LOAD_SECONDARY:
    {
        bool primary = job->phase == ATLAS_LOAD_PRIMARY;
        uint8_t *tiles = primary ? job->primaryTiles : job->secondaryTiles;
        if (!job->tileLoad.initialized) memset(tiles, 0, VOXEL_TILE_SCRATCH);
        if (!Voxel_LoadTilesStep(primary ? job->primary : job->secondary, tiles,
                                VOXEL_TILE_SCRATCH, &job->tileLoad, 512)) return false;
        if (primary) job->primaryLoaded = job->tileLoad.ok;
        else job->secondaryLoaded = job->tileLoad.ok;
        memset(&job->tileLoad, 0, sizeof(job->tileLoad));
        job->phase = primary ? ATLAS_LOAD_SECONDARY : ATLAS_COMPOSE;
        return false;
    }
    case ATLAS_COMPOSE:
        break;
    default:
        return true;
    }

    memset(&src, 0, sizeof(src));
    JobSource(job, &src);
    BuildColorTable(&src);
    voidBits = VoidBits(job->primary, job->secondary, true);

    for (; job->next < VOXEL_METATILE_IDS && metatiles > 0; ++job->next)
    {
        VoxelAtlasMap *map = &job->map;
        unsigned m = job->next, base, hidden, slot = map->slotOf[m];
        const uint16_t *entries;
        const uint8_t *cut;

        --metatiles; /* Bound lookups too, including unused and absent ids. */
        if (slot == VOXEL_SLOT_ABSENT && m >= VOXEL_METATILE_REAL && job->used[m])
            slot = map->slotOf[m] = 0;
        if (slot == VOXEL_SLOT_ABSENT)
            continue;
        if (job->priority == 2 ? job->used[m] != 2 : job->used[m] == 2)
            continue;
        if (!slot && !job->used[m])
            continue;
        entries = AtlasSourceOf(job->primary, job->secondary, m, &base, &hidden, &cut)
                ? MetatileEntries(&src, base) : NULL;
        if (entries == NULL)
        {
            map->slotOf[m] = VOXEL_SLOT_ABSENT;
            continue;
        }
        if (!slot || slot == VOXEL_SLOT_PENDING)
        {
            if (map->used >= VOXEL_ATLAS_PAGES * VOXEL_SOLID_BASE)
            {
                map->overflowed = true;
                map->slotOf[m] = VOXEL_SLOT_PENDING;
                continue;
            }
            slot = (map->used / VOXEL_SOLID_BASE) * VOXEL_ATLAS_MAX_SLOTS
                 + map->used % VOXEL_SOLID_BASE + 1u;
            map->slotOf[m] = (uint16_t)slot;
            ++map->used;
            ++job->added;
        }
        if ((slot - 1u) / VOXEL_ATLAS_MAX_SLOTS != job->page)
            continue;
        for (unsigned t = 0; t < NUM_TILES_PER_METATILE; ++t)
        {
            unsigned tile = entries[t] & 0x3FF;
            map->tileMask[job->page][tile >> 3] |= (uint8_t)(1u << (tile & 7));
        }
        voidBits[m >> 3] &= (uint8_t)~(1u << (m & 7));
        if (ComposeMetatile(&src, entries, job->dest,
                            (slot - 1u) % VOXEL_ATLAS_MAX_SLOTS, hidden, cut))
            voidBits[m >> 3] |= (uint8_t)(1u << (m & 7));
    }
    if (job->next < VOXEL_METATILE_IDS)
        return false;
    if (job->priority == 2)
    {
        job->priority = 1;
        job->next = 0;
        return false;
    }
    job->pageReady = true;
    job->ok = job->map.used != 0 && !job->map.overflowed;
    if (job->page + 1u >= VoxelAtlas_PageCount(&job->map))
    {
        VoxelAtlas_JobCancel(job);
        job->phase = ATLAS_DONE;
    }
    return true;
}

unsigned VoxelAtlas_PageCount(const VoxelAtlasMap *map)
{
    return (map->used + VOXEL_SOLID_BASE - 1u) / VOXEL_SOLID_BASE;
}

bool VoxelAtlas_JobNextPage(VoxelAtlasJob *job)
{
    if (!job->pageReady || !job->active)
        return false;
    ++job->page;
    job->priority = 2;
    job->next = 0;
    job->pageReady = false;
    memset(job->dest, 0, VOXEL_ATLAS_PIXELS * sizeof(uint16_t));
    FillSolidSlots(job->dest);
    return true;
}

unsigned VoxelAtlas_RefreshAnimatedPageRows(const VoxelMapInstance *inst, const VoxelAtlasMap *map,
                                        unsigned page, uint16_t *dest, const uint8_t *liveTiles,
                                        const uint8_t dirtyTiles[128], unsigned *rows)
{
    struct AtlasSource src;
    uint8_t *voidBits;
    unsigned changed = 0;

    if (rows != NULL)
        *rows = 0;
    if (inst == NULL || map == NULL || dest == NULL || liveTiles == NULL || dirtyTiles == NULL)
        return 0;
    memset(&src, 0, sizeof(src));
    /* The game has already applied TransferTilesetAnimsBuffer to logical GBA
     * VRAM. Both tilesets occupy the first 0x8000 bytes in 4bpp order. */
    src.primaryTiles = liveTiles;
    src.secondaryTiles = liveTiles + TILES_PER_TILESET * 32;
    src.primaryMetatiles = Voxel_ResolveMetatiles(inst->primaryTileset);
    src.secondaryMetatiles = Voxel_ResolveMetatiles(inst->secondaryTileset);
    src.primaryPalettes = Voxel_ResolvePalettes(inst->primaryTileset);
    src.secondaryPalettes = Voxel_ResolvePalettes(inst->secondaryTileset);
    src.primaryCount = Voxel_MetatileCount(inst->primaryTileset, NUM_METATILES_IN_PRIMARY);
    src.secondaryCount = Voxel_MetatileCount(inst->secondaryTileset,
                                             NUM_METATILES_TOTAL - NUM_METATILES_IN_PRIMARY);
    BuildColorTable(&src);
    InitMorton();
    voidBits = VoidBits(inst->primaryTileset, inst->secondaryTileset, true);
    for (unsigned m = 0; m < VOXEL_METATILE_IDS; ++m)
    {
        const uint16_t *entries;
        bool affected = false;
        unsigned slot = map->slotOf[m], base, hidden;
        const uint8_t *cut;

        if (slot == 0 || slot == VOXEL_SLOT_ABSENT || slot == VOXEL_SLOT_PENDING
         || (slot - 1u) / VOXEL_ATLAS_MAX_SLOTS != page)
            continue;
        if (!AtlasSourceOf(inst->primaryTileset, inst->secondaryTileset, m, &base, &hidden, &cut))
            continue;
        entries = MetatileEntries(&src, base);
        if (entries == NULL)
            continue;
        for (unsigned t = 0; t < NUM_TILES_PER_METATILE; ++t)
        {
            unsigned tile = entries[t] & 0x3FF;
            if (dirtyTiles[tile >> 3] & (1u << (tile & 7)))
            {
                affected = true;
                break;
            }
        }
        if (!affected)
            continue;
        if (voidBits != NULL)
            voidBits[m >> 3] &= (uint8_t)~(1u << (m & 7));
        if (ComposeMetatile(&src, entries, dest, (slot - 1u) % VOXEL_ATLAS_MAX_SLOTS, hidden, cut) && voidBits != NULL)
            voidBits[m >> 3] |= (uint8_t)(1u << (m & 7));
        if (rows != NULL)
            *rows |= 1u << (((slot - 1u) % VOXEL_ATLAS_MAX_SLOTS) / VOXEL_ATLAS_COLUMNS);
        ++changed;
    }
    return changed;
}

unsigned VoxelAtlas_RefreshAnimatedPage(const VoxelMapInstance *inst, const VoxelAtlasMap *map,
                                    unsigned page, uint16_t *dest, const uint8_t *liveTiles,
                                    const uint8_t dirtyTiles[128])
{
    return VoxelAtlas_RefreshAnimatedPageRows(inst, map, page, dest, liveTiles, dirtyTiles, NULL);
}

unsigned VoxelAtlas_RefreshAnimated(const VoxelMapInstance *inst, const VoxelAtlasMap *map,
                                    uint16_t *dest, const uint8_t *liveTiles,
                                    const uint8_t dirtyTiles[128])
{
    return VoxelAtlas_RefreshAnimatedPage(inst, map, 0, dest, liveTiles, dirtyTiles);
}

bool VoxelAtlas_Build(const VoxelMapInstance *inst, uint16_t *dest, VoxelAtlasMap *map,
                      bool extend)
{
    static VoxelAtlasJob job;

    if (!VoxelAtlas_JobBegin(&job, inst, dest, extend ? map : NULL))
    {
        if (!extend)
            memset(map, 0, sizeof(*map));
        return job.ok; /* An unchanged extension succeeds without touching dest. */
    }
    while (!VoxelAtlas_JobStep(&job, VOXEL_METATILE_IDS))
        ;
    *map = job.map;
    VoxelAtlas_JobCancel(&job); /* This convenience API returns page zero only. */
    return job.ok;
}

/*
 * The animated metatiles of a page, a few at a time and each into a little
 * block of its own - 16x16 texels in the PICA's order, which is the slot's two
 * runs of 256 bytes in the page, one after the other - instead of into a copy
 * of the whole page that had to be read back from VRAM first (a 256 KiB
 * invalidation and a GPU copy, 6-7 ms of an Old 3DS frame, every few seconds).
 * `work` holds the tiles to redraw for, `*cursor` where the walk over the
 * ids goes on; it ends when *cursor reaches VOXEL_METATILE_IDS.
 */
static unsigned AnimatedSlots(const VoxelMapInstance *inst, const VoxelAtlasMap *map,
                              unsigned page, const uint8_t *liveTiles,
                              const uint8_t work[128], unsigned *cursor,
                              VoxelAnimSlot *out, unsigned max, bool primaryOnly)
{
    struct AtlasSource src;
    uint8_t *voidBits;
    unsigned count = 0, m = *cursor;

    if (inst == NULL || map == NULL || liveTiles == NULL || work == NULL || out == NULL)
    {
        *cursor = VOXEL_METATILE_IDS;
        return 0;
    }
    memset(&src, 0, sizeof(src));
    src.primaryTiles = liveTiles;
    src.secondaryTiles = liveTiles + TILES_PER_TILESET * 32;
    src.primaryMetatiles = Voxel_ResolveMetatiles(inst->primaryTileset);
    src.secondaryMetatiles = Voxel_ResolveMetatiles(inst->secondaryTileset);
    src.primaryPalettes = Voxel_ResolvePalettes(inst->primaryTileset);
    src.secondaryPalettes = Voxel_ResolvePalettes(inst->secondaryTileset);
    src.primaryCount = Voxel_MetatileCount(inst->primaryTileset, NUM_METATILES_IN_PRIMARY);
    src.secondaryCount = Voxel_MetatileCount(inst->secondaryTileset,
                                             NUM_METATILES_TOTAL - NUM_METATILES_IN_PRIMARY);
    BuildColorTable(&src);
    InitMorton();
    voidBits = VoidBits(inst->primaryTileset, inst->secondaryTileset, true);
    for (; m < VOXEL_METATILE_IDS && count < max; ++m)
    {
        const uint16_t *entries;
        bool affected = false;
        unsigned slot = map->slotOf[m], base, hidden;
        const uint8_t *cut;

        if (slot == 0 || slot == VOXEL_SLOT_ABSENT || slot == VOXEL_SLOT_PENDING
         || (slot - 1u) / VOXEL_ATLAS_MAX_SLOTS != page)
            continue;
        if (!AtlasSourceOf(inst->primaryTileset, inst->secondaryTileset, m, &base, &hidden, &cut))
            continue;
        entries = MetatileEntries(&src, base);
        if (entries == NULL)
            continue;
        for (unsigned t = 0; t < NUM_TILES_PER_METATILE; ++t)
        {
            unsigned tile = entries[t] & 0x3FF;
            if (primaryOnly && tile >= TILES_PER_TILESET)
            {
                /* drawn with secondary tiles BG VRAM does not hold */
                affected = false;
                break;
            }
            if (work[tile >> 3] & (1u << (tile & 7)))
                affected = true;
            if (affected && !primaryOnly)
                break;
        }
        if (!affected)
            continue;
        if (voidBits != NULL)
            voidBits[m >> 3] &= (uint8_t)~(1u << (m & 7));
        memset(out[count].pixels, 0, sizeof(out[count].pixels));
        if (ComposeAt(&src, entries, out[count].pixels, 0, 0, VOXEL_ATLAS_SLOT, hidden, cut)
         && voidBits != NULL)
            voidBits[m >> 3] |= (uint8_t)(1u << (m & 7));
        out[count].slot = (slot - 1u) % VOXEL_ATLAS_MAX_SLOTS;
        ++count;
    }
    *cursor = m;
    return count;
}

unsigned VoxelAtlas_AnimatedSlots(const VoxelMapInstance *inst, const VoxelAtlasMap *map,
                                  unsigned page, const uint8_t *liveTiles,
                                  const uint8_t work[128], unsigned *cursor,
                                  VoxelAnimSlot *out, unsigned max)
{
    return AnimatedSlots(inst, map, page, liveTiles, work, cursor, out, max, false);
}

unsigned VoxelAtlas_SharedAnimatedSlots(const VoxelMapInstance *inst, const VoxelAtlasMap *map,
                                        unsigned page, const uint8_t *liveTiles,
                                        const uint8_t work[128], unsigned *cursor,
                                        VoxelAnimSlot *out, unsigned max)
{
    return AnimatedSlots(inst, map, page, liveTiles, work, cursor, out, max, true);
}
