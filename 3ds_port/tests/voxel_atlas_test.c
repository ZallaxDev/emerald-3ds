/* The real atlas composer against the per-texel reference it replaced, on
 * random tiles, palettes, flips and layers: every texel, and the black-filler
 * verdict, must come out the same. */
#include <assert.h>
#include <stdio.h>
#include "../src/voxel/voxel_atlas.c"

#include "../src/3ds_video_decode.c"

/*
 * voxel_atlas.c's world and asset dependencies: a fake tileset pair for the
 * whole-atlas tests below, whose tiles, metatiles, palettes and referenced
 * ids are the arrays further down.
 */
unsigned char g_PortLogActive;
void Port_Log_Printf(const char *fmt, ...) { (void)fmt; }
const void *Port_ResolveAssetPointer(const void *p) { return p; }
static const int kPrimary = 1, kSecondary = 2;
static bool sDerived;
static const struct MapLayout sDerivedLayout = {
    .primaryTileset = (const struct Tileset *)&kPrimary,
    .secondaryTileset = (const struct Tileset *)&kSecondary
};
bool VoxelBuildings_Variant(unsigned i, unsigned *layout, unsigned *metatile, unsigned *quarters)
{
    if (!sDerived || i >= VOXEL_VARIANTS) return false;
    *layout = 1; *metatile = i; *quarters = 1; return true;
}
const struct MapLayout *Port_GetMapLayoutById(u16 id) { return id == 1 ? &sDerivedLayout : NULL; }
unsigned VoxelRelief_CutCount(void) { return sDerived ? VOXEL_CUTS : 0; }
bool VoxelRelief_CutVariant(unsigned i, unsigned *layout, unsigned *metatile, const uint8_t **rows)
{
    static const uint8_t cut[32] = {1};
    if (!sDerived || i >= VOXEL_CUTS) return false;
    *layout = 1; *metatile = i; *rows = cut; return true;
}
/* What voxel_relief.c gives a cut whose rock has no 45-degree side: its
 * background clear as drawn, and no pixel kept for the polygon's edge. */
/* sFringe: pixel (1, 0) of the background kept, with the colour of (1, 1). */
static bool sFringe;
void VoxelRelief_CutClear(const uint8_t *rows, uint16_t clear[16])
{
    for (int j = 0; j < 16; ++j)
        clear[j] = (uint16_t)((unsigned)rows[2 * j] | ((unsigned)rows[2 * j + 1] << 8));
    if (sFringe)
        clear[0] &= (uint16_t)~2u;
}
int VoxelRelief_CutFringe(const uint8_t *rows, const uint16_t clear[16], int x, int y)
{
    (void)rows; (void)clear;
    return sFringe && x == 1 && y == 0 ? 17 : -1;
}
static uint8_t sPrimaryTiles[512 * 32], sSecondaryTiles[512 * 32];
static uint16_t sPrimaryPalettes[16 * 16], sSecondaryPalettes[16 * 16];
static uint16_t sMetatiles[1024 * 8];
static uint8_t sUsed[1024];
void VoxelWorld_MarkUsedMetatiles(const void *p, const void *s, uint8_t *used)
{
    if (p == &kPrimary && s == &kSecondary)
        memcpy(used, sUsed, sizeof(sUsed));
}
bool Voxel_LoadTiles(const void *t, uint8_t *d, uint32_t n)
{
    if (t != &kPrimary && t != &kSecondary)
        return false;
    memcpy(d, t == &kPrimary ? sPrimaryTiles : sSecondaryTiles, n < 512 * 32 ? n : 512 * 32);
    return true;
}
const uint16_t *Voxel_ResolveMetatiles(const void *t)
{
    return t == &kPrimary ? sMetatiles : t == &kSecondary ? sMetatiles + 512 * 8 : NULL;
}
const uint16_t *Voxel_ResolvePalettes(const void *t)
{
    return t == &kPrimary ? sPrimaryPalettes : t == &kSecondary ? sSecondaryPalettes : NULL;
}
unsigned Voxel_MetatileCount(const void *t, unsigned limit) { (void)t; return limit; }

/* ── The composer as it was, texel by texel ─────────────────────────────── */

static void ReferenceCompose(const struct AtlasSource *src, const uint16_t *entries,
                             uint16_t *dest, unsigned slot)
{
    unsigned baseX = (slot % VOXEL_ATLAS_COLUMNS) * VOXEL_ATLAS_SLOT;
    unsigned baseY = (slot / VOXEL_ATLAS_COLUMNS) * VOXEL_ATLAS_SLOT;

    for (unsigned layer = 0; layer < 2; ++layer)
        for (unsigned quadrant = 0; quadrant < 4; ++quadrant)
        {
            unsigned dstX = (quadrant % 2) * SUBTILE_PX, dstY = (quadrant / 2) * SUBTILE_PX;
            uint16_t entry = entries[layer * 4 + quadrant];
            unsigned tileId = entry & 0x3FF, paletteId = (entry >> 12) & 0xF;
            bool flipX = (entry >> 10) & 1, flipY = (entry >> 11) & 1;
            const uint8_t *tiles = tileId < TILES_PER_TILESET ? src->primaryTiles : src->secondaryTiles;
            unsigned localTile = tileId < TILES_PER_TILESET ? tileId : tileId - TILES_PER_TILESET;

            if (tiles == NULL)
                continue;
            for (unsigned py = 0; py < SUBTILE_PX; ++py)
                for (unsigned px = 0; px < SUBTILE_PX; ++px)
                {
                    unsigned srcX = flipX ? (SUBTILE_PX - 1 - px) : px;
                    unsigned srcY = flipY ? (SUBTILE_PX - 1 - py) : py;
                    uint8_t packed = tiles[localTile * 32 + srcY * 4 + srcX / 2];
                    unsigned colorIdx = (srcX & 1) ? (packed >> 4) : (packed & 0xF);
                    uint16_t bgr15;

                    if (layer != 0 && colorIdx == 0)
                        continue;
                    if (layer == 0 && colorIdx == 0)
                        bgr15 = src->primaryPalettes != NULL ? src->primaryPalettes[0] : 0;
                    else
                        bgr15 = LookupColor(src, paletteId, colorIdx);
                    dest[CtrVideo_Texel(baseX + dstX + px, baseY + dstY + py, VOXEL_ATLAS_W)] =
                        VoxelGrade_RGBA5551(bgr15);
                }
        }
}

static bool ReferenceBlack(const struct AtlasSource *src, const uint16_t *entries)
{
    for (unsigned layer = 0; layer < 2; ++layer)
        for (unsigned quadrant = 0; quadrant < 4; ++quadrant)
        {
            uint16_t entry = entries[layer * 4 + quadrant];
            unsigned tileId = entry & 0x3FF, paletteId = (entry >> 12) & 0xF;
            const uint8_t *tiles = tileId < TILES_PER_TILESET ? src->primaryTiles : src->secondaryTiles;
            unsigned local = tileId < TILES_PER_TILESET ? tileId : tileId - TILES_PER_TILESET;

            if (tiles == NULL)
                return false;
            for (unsigned b = 0; b < 32; ++b)
                for (unsigned half = 0; half < 2; ++half)
                {
                    unsigned colorIdx = half ? (tiles[local * 32 + b] >> 4) : (tiles[local * 32 + b] & 0xF);
                    uint16_t bgr15;

                    if (colorIdx == 0)
                    {
                        if (layer != 0)
                            continue;
                        bgr15 = src->primaryPalettes != NULL ? src->primaryPalettes[0] : 0;
                    }
                    else
                        bgr15 = LookupColor(src, paletteId, colorIdx);
                    if ((bgr15 & 0x7FFF) != 0)
                        return false;
                }
        }
    return true;
}

static uint32_t sSeed = 12345;
static unsigned Random(void)
{
    sSeed = sSeed * 1103515245u + 12345u;
    return sSeed >> 8;
}

static uint16_t sNew[VOXEL_ATLAS_PIXELS], sOld[VOXEL_ATLAS_PIXELS];

/* A frame with no time left: VoxelAnim_Compose does one block. */
static bool NoTime(void)
{
    return false;
}

static void Fill(unsigned blackEvery)
{
    for (unsigned i = 0; i < sizeof(sPrimaryTiles); ++i)
    {
        sPrimaryTiles[i] = (uint8_t)Random();
        sSecondaryTiles[i] = (uint8_t)Random();
    }
    /* A few all-zero tiles, which only draw the backdrop and layer 1 keys out. */
    memset(sPrimaryTiles + 7 * 32, 0, 32);
    memset(sSecondaryTiles + 9 * 32, 0, 32);
    for (unsigned i = 0; i < 16 * 16; ++i)
    {
        sPrimaryPalettes[i] = (uint16_t)(Random() & 0xFFFF);
        sSecondaryPalettes[i] = (uint16_t)(Random() & 0xFFFF);
        if (blackEvery && i % blackEvery != 1)
        {
            sPrimaryPalettes[i] &= 0x8000;
            sSecondaryPalettes[i] &= 0x8000;
        }
    }
}

static void Run(struct AtlasSource *src, unsigned trials, unsigned *blacks)
{
    BuildColorTable(src);
    InitMorton();
    for (unsigned t = 0; t < trials; ++t)
    {
        uint16_t entries[8];
        unsigned slot = Random() % VOXEL_SOLID_BASE;
        bool black;

        for (unsigned e = 0; e < 8; ++e)
            entries[e] = (uint16_t)(Random() & 0xFFFF);
        if (t % 5 == 0)
            entries[Random() % 8] = (uint16_t)((Random() & 0xF000) | 7); /* blank tile */
        memset(sNew, 0x5A, sizeof(sNew));
        memset(sOld, 0x5A, sizeof(sOld));
        black = ComposeMetatile(src, entries, sNew, slot, 0, NULL);
        ReferenceCompose(src, entries, sOld, slot);
        assert(memcmp(sNew, sOld, sizeof(sNew)) == 0);
        assert(black == ReferenceBlack(src, entries));
        *blacks += black;
    }
}

/* ── Whole atlases: composed a slice at a time, exactly as in one go ────── */

static uint16_t sOneGo[VOXEL_ATLAS_PIXELS], sSliced[VOXEL_ATLAS_PIXELS];
static VoxelAnimPass sPass;

/* A pass's runs (of pair `pair`, on page `onPage`) copied into a page as the GPU copies them:
 * each two lines, the second a row of blocks after the first. The runs. */
static unsigned ApplyPass(const VoxelAnimPass *pass, uint16_t *page, unsigned pair, unsigned onPage)
{
    static uint16_t run[VOXEL_ANIM_RUN_MAX * 256];
    unsigned runs = 0;

    for (unsigned i = 0; i < pass->targets;)
    {
        unsigned n = VoxelAnim_Run(pass, i);
        unsigned line = n * 256u, offset = VoxelAnim_RunOffset(pass, i);

        assert(n >= 1 && n <= VOXEL_ANIM_RUN_MAX);
        if (pass->target[i].pair == pair && pass->target[i].page == onPage)
        {
            VoxelAnim_WriteRun(pass, i, n, run);
            memcpy((uint8_t *)page + offset, run, line);
            memcpy((uint8_t *)page + offset + (VOXEL_ATLAS_W / 8u) * 128u, (uint8_t *)run + line, line);
            ++runs;
        }
        i += n;
    }
    return runs;
}

static void Sliced(const VoxelMapInstance *inst, const VoxelAtlasMap *base, VoxelAtlasMap *out,
                   unsigned *added)
{
    static VoxelAtlasJob job;
    unsigned steps = 0;

    memset(sSliced, 0x5A, sizeof(sSliced));
    if (!VoxelAtlas_JobBegin(&job, inst, sSliced, base))
    {
        assert(base != NULL && job.ok && !job.active);
        *out = *base;
        *added = 0;
        return;
    }
    while (!VoxelAtlas_JobStep(&job, 1 + Random() % 9))
        ++steps;
    assert(steps > 20 && job.pageReady && job.ok);
    *out = job.map;
    *added = job.added;
    VoxelAtlas_JobCancel(&job);
}

static void WholeAtlases(void)
{
    VoxelMapInstance inst;
    VoxelAtlasMap oneGo, sliced, base;
    unsigned added;

    memset(&inst, 0, sizeof(inst));
    inst.primaryTileset = &kPrimary;
    inst.secondaryTileset = &kSecondary;
    Fill(0);
    for (unsigned i = 0; i < sizeof(sMetatiles) / sizeof(sMetatiles[0]); ++i)
        sMetatiles[i] = (uint16_t)(Random() & 0xFFFF);

    /* A fresh atlas of a map that references about a third of the ids. */
    for (unsigned m = 0; m < 1024; ++m)
        sUsed[m] = Random() % 3 == 0;
    memset(sOneGo, 0x5A, sizeof(sOneGo));
    assert(VoxelAtlas_Build(&inst, sOneGo, &oneGo, false));
    Sliced(&inst, NULL, &sliced, &added);
    assert(memcmp(sOneGo, sSliced, sizeof(sOneGo)) == 0);
    assert(memcmp(&oneGo, &sliced, sizeof(oneGo)) == 0);
    assert(added == oneGo.used && !oneGo.overflowed);

    /* The same atlas extended with more ids: nothing it had moves. */
    base = oneGo;
    for (unsigned m = 0; m < 1024; ++m)
        sUsed[m] = sUsed[m] || Random() % 4 == 0;
    assert(VoxelAtlas_Build(&inst, sOneGo, &oneGo, true));
    Sliced(&inst, &base, &sliced, &added);
    assert(memcmp(sOneGo, sSliced, sizeof(sOneGo)) == 0);
    assert(memcmp(&oneGo, &sliced, sizeof(oneGo)) == 0);
    assert(added == oneGo.used - base.used && added > 0);
    for (unsigned m = 0; m < 1024; ++m)
        assert(base.slotOf[m] == 0 || oneGo.slotOf[m] == base.slotOf[m]);

    /* Every real id: three pages, and no valid material becomes absent. */
    base = oneGo;
    memset(sUsed, 1, sizeof(sUsed));
    Sliced(&inst, &base, &sliced, &added);
    assert(!sliced.overflowed && sliced.used == 1024);
    for (unsigned m = 0; m < 1024; ++m)
        assert(sliced.slotOf[m] != 0 && sliced.slotOf[m] != VOXEL_SLOT_ABSENT);
    /* And extended again with nothing new, it adds nothing. */
    base = sliced;
    Sliced(&inst, &base, &sliced, &added);
    assert(added == 0 && memcmp(&base, &sliced, sizeof(base)) == 0);
}

static void PagedAtlases(void)
{
    static const unsigned sizes[] = {504, 505, 1024};
    static VoxelAtlasJob job;
    static uint16_t dest[VOXEL_ATLAS_PIXELS + 16];
    VoxelMapInstance inst = {0};
    struct AtlasSource src = {0};
    inst.primaryTileset = &kPrimary;
    inst.secondaryTileset = &kSecondary;
    Fill(0);
    for (unsigned i = 0; i < sizeof(sMetatiles) / sizeof(sMetatiles[0]); ++i)
        sMetatiles[i] = (uint16_t)Random();
    for (unsigned n = 0; n < sizeof(sizes) / sizeof(sizes[0]); ++n)
    {
        memset(sUsed, 0, sizeof(sUsed));
        memset(sUsed, 1, sizes[n]);
        for (unsigned i = VOXEL_ATLAS_PIXELS; i < VOXEL_ATLAS_PIXELS + 16; ++i)
            dest[i] = 0xCAFE;
        assert(VoxelAtlas_JobBegin(&job, &inst, dest, NULL));
        unsigned pages = 0;
        do
        {
            while (!VoxelAtlas_JobStep(&job, 7)) {}
            assert(job.ok && job.pageReady && job.map.used == sizes[n]);
            memset(sOld, 0, sizeof(sOld));
            FillSolidSlots(sOld);
            /* Job releases decoded tiles on its last page; fixture data is
             * independently available for the per-texel reference. */
            src.primaryTiles = sPrimaryTiles;
            src.secondaryTiles = sSecondaryTiles;
            src.primaryPalettes = sPrimaryPalettes;
            src.secondaryPalettes = sSecondaryPalettes;
            for (unsigned m = 0; m < sizes[n]; ++m)
            {
                unsigned slot = job.map.slotOf[m] - 1u;
                assert(job.map.slotOf[m] != VOXEL_SLOT_ABSENT);
                if (slot / VOXEL_ATLAS_MAX_SLOTS == job.page)
                    ReferenceCompose(&src, sMetatiles + m * 8u, sOld, slot % VOXEL_ATLAS_MAX_SLOTS);
            }
            assert(memcmp(dest, sOld, sizeof(sOld)) == 0);
            for (unsigned i = VOXEL_ATLAS_PIXELS; i < VOXEL_ATLAS_PIXELS + 16; ++i)
                assert(dest[i] == 0xCAFE);
            ++pages;
        } while (VoxelAtlas_JobNextPage(&job));
        assert(pages == (sizes[n] + 503u) / 504u && !job.active);
    }
}

/* Repeated extension requests must not decode or overwrite an unchanged atlas. */
static void UnchangedExtension(void)
{
    VoxelMapInstance inst = {.primaryTileset = &kPrimary, .secondaryTileset = &kSecondary};
    VoxelAtlasMap map;
    static VoxelAtlasJob job;
    memset(sUsed, 0, sizeof(sUsed));
    sUsed[1] = 2;
    Fill(0);
    assert(VoxelAtlas_Build(&inst, sOneGo, &map, false));
    memcpy(sSliced, sOneGo, sizeof(sOneGo));
    assert(!VoxelAtlas_JobBegin(&job, &inst, sSliced, &map));
    assert(!job.active && job.primaryTiles == NULL && job.secondaryTiles == NULL);
    assert(memcmp(sOneGo, sSliced, sizeof(sOneGo)) == 0);
    assert(VoxelAtlas_Build(&inst, sSliced, &map, true));
    assert(memcmp(sOneGo, sSliced, sizeof(sOneGo)) == 0);
    /* A newly required id must still schedule its extension. */
    sUsed[2] = 1;
    assert(VoxelAtlas_JobBegin(&job, &inst, sSliced, &map));
    while (!VoxelAtlas_JobStep(&job, 17)) {}
    assert(job.ok && job.map.slotOf[2] != 0 && job.map.slotOf[1] == map.slotOf[1]);
    /* Pending slots also require work, never the unchanged fast path. */
    map = job.map; map.slotOf[2] = VOXEL_SLOT_PENDING;
    assert(VoxelAtlas_JobBegin(&job, &inst, sSliced, &map));
    VoxelAtlas_JobCancel(&job);
}

static void AnimatedTiles(void)
{
    VoxelMapInstance inst = {0};
    VoxelAtlasMap map;
    uint8_t live[1024 * 32], dirty[128] = {0};
    struct AtlasSource src = {0};
    unsigned slot1, slot2;

    inst.primaryTileset = &kPrimary;
    inst.secondaryTileset = &kSecondary;
    memset(sUsed, 0, sizeof(sUsed));
    memset(sMetatiles, 0, sizeof(sMetatiles));
    sUsed[1] = sUsed[2] = 1;
    for (unsigned t = 0; t < 4; ++t)
    {
        sMetatiles[1 * 8 + t] = 432;
        sMetatiles[2 * 8 + t] = 10;
    }
    Fill(0);
    assert(VoxelAtlas_Build(&inst, sOneGo, &map, false));
    slot1 = map.slotOf[1] - 1;
    slot2 = map.slotOf[2] - 1;
    assert(slot1 != slot2);
    memcpy(sSliced, sOneGo, sizeof(sOneGo));
    memcpy(live, sPrimaryTiles, sizeof(sPrimaryTiles));
    memcpy(live + sizeof(sPrimaryTiles), sSecondaryTiles, sizeof(sSecondaryTiles));
    memset(live + 432 * 32, 0x11, 32);
    dirty[432 >> 3] = 1u << (432 & 7);
    assert(VoxelAtlas_RefreshAnimated(&inst, &map, sOneGo, live, dirty) == 1);
    for (unsigned y = 0; y < 16; ++y)
        for (unsigned x = 0; x < 16; ++x)
        {
            unsigned offset = CtrVideo_Texel((slot2 % VOXEL_ATLAS_COLUMNS) * 16 + x,
                                               (slot2 / VOXEL_ATLAS_COLUMNS) * 16 + y,
                                               VOXEL_ATLAS_W);
            assert(sOneGo[offset] == sSliced[offset]);
        }
    src.primaryTiles = live;
    src.secondaryTiles = live + sizeof(sPrimaryTiles);
    src.primaryPalettes = sPrimaryPalettes;
    src.secondaryPalettes = sSecondaryPalettes;
    src.primaryMetatiles = sMetatiles;
    src.secondaryMetatiles = sMetatiles + 512 * 8;
    memset(sOld, 0, sizeof(sOld));
    ReferenceCompose(&src, sMetatiles + 8, sOld, slot1);
    for (unsigned y = 0; y < 16; ++y)
        for (unsigned x = 0; x < 16; ++x)
        {
            unsigned offset = CtrVideo_Texel((slot1 % VOXEL_ATLAS_COLUMNS) * 16 + x,
                                               (slot1 / VOXEL_ATLAS_COLUMNS) * 16 + y,
                                               VOXEL_ATLAS_W);
            assert(sOneGo[offset] == sOld[offset]);
        }
    memset(dirty, 0, sizeof(dirty));
    assert(VoxelAtlas_RefreshAnimated(&inst, &map, sOneGo, live, dirty) == 0);

    /* The rows of slots a refresh wrote - all of the page a changed frame has
     * to upload: three animated metatiles in three rows, and every texel of
     * each inside its row's contiguous bytes. */
    {
        const unsigned animated[3] = {5, 40, 66};
        unsigned rows = 0, expected = 0;

        memset(sUsed, 0, sizeof(sUsed));
        memset(sMetatiles, 0, sizeof(sMetatiles));
        for (unsigned id = 1; id <= 70; ++id)
        {
            sUsed[id] = 1;
            for (unsigned t = 0; t < 4; ++t)
                sMetatiles[id * 8 + t] = (id == 5 || id == 40 || id == 66) ? 432 : 10;
        }
        Fill(0);
        assert(VoxelAtlas_Build(&inst, sOneGo, &map, false));
        for (unsigned i = 0; i < 3; ++i)
            expected |= 1u << (((map.slotOf[animated[i]] - 1u) % VOXEL_ATLAS_MAX_SLOTS) / VOXEL_ATLAS_COLUMNS);
        assert(expected == 7u); /* slots 4, 39, 65: rows 0, 1, 2 */
        memcpy(live, sPrimaryTiles, sizeof(sPrimaryTiles));
        memcpy(live + sizeof(sPrimaryTiles), sSecondaryTiles, sizeof(sSecondaryTiles));
        memset(live + 432 * 32, 0x11, 32);
        memset(dirty, 0, sizeof(dirty));
        dirty[432 >> 3] = 1u << (432 & 7);
        assert(VoxelAtlas_RefreshAnimatedPageRows(&inst, &map, 0, sOneGo, live, dirty, &rows) == 3);
        assert(rows == expected);
        for (unsigned i = 0; i < 3; ++i)
        {
            unsigned slot = (map.slotOf[animated[i]] - 1u) % VOXEL_ATLAS_MAX_SLOTS;
            unsigned row = slot / VOXEL_ATLAS_COLUMNS;

            for (unsigned y = 0; y < 16; ++y)
                for (unsigned x = 0; x < 16; ++x)
                {
                    unsigned bytes = 2u * CtrVideo_Texel((slot % VOXEL_ATLAS_COLUMNS) * 16 + x,
                                                         row * 16 + y, VOXEL_ATLAS_W);

                    assert(bytes >= row * VOXEL_ATLAS_ROW_BYTES
                        && bytes < (row + 1u) * VOXEL_ATLAS_ROW_BYTES);
                }
        }
        /* The same slots redrawn by a pass, its runs copied as the GPU does
         * (two lines a row of blocks apart) over a page that has them
         * wiped: exactly the page of the full refresh. */
        {
            unsigned runs;

            memcpy(sSliced, sOneGo, sizeof(sOneGo));
            for (unsigned i = 0; i < 3; ++i)
            {
                unsigned slot = (map.slotOf[animated[i]] - 1u) % VOXEL_ATLAS_MAX_SLOTS;

                for (unsigned y = 0; y < 16; ++y)
                    for (unsigned x = 0; x < 16; ++x)
                        sSliced[CtrVideo_Texel((slot % VOXEL_ATLAS_COLUMNS) * 16 + x,
                                               (slot / VOXEL_ATLAS_COLUMNS) * 16 + y, VOXEL_ATLAS_W)] = 0x1234;
            }
            VoxelAnim_Begin(&sPass, dirty);
            assert(VoxelAnim_AddAtlas(&sPass, 0, &inst, false, &map, live));
            assert(sPass.targets == 3);
            assert(VoxelAnim_Compose(&sPass, live, NULL));
            runs = ApplyPass(&sPass, sSliced, 0, 0);
            assert(runs == 3);
            assert(memcmp(sSliced, sOneGo, sizeof(sOneGo)) == 0);
            /* the same pass again, its slots kept: the same page */
            VoxelAnim_Again(&sPass);
            assert(sPass.composed == 0 && sPass.targets == 3);
            assert(VoxelAnim_Compose(&sPass, live, NULL));
            assert(sPass.kept);
            assert(ApplyPass(&sPass, sSliced, 0, 0) == 3);
            assert(memcmp(sSliced, sOneGo, sizeof(sOneGo)) == 0);
            /* another frame of the written tile is composed, not kept; its
             * first one, back again, is found kept: the same page */
            {
                uint8_t frame[32];

                memcpy(frame, live + 432 * 32, sizeof(frame));
                for (unsigned i = 0; i < sizeof(frame); ++i)
                    live[432 * 32 + i] = (uint8_t)(frame[i] ^ 0x11u);
                VoxelAnim_Again(&sPass);
                assert(VoxelAnim_Compose(&sPass, live, NULL) && !sPass.kept);
                ApplyPass(&sPass, sSliced, 0, 0);
                assert(memcmp(sSliced, sOneGo, sizeof(sOneGo)) != 0);
                memcpy(live + 432 * 32, frame, sizeof(frame));
                VoxelAnim_Again(&sPass);
                assert(VoxelAnim_Compose(&sPass, live, NULL) && sPass.kept);
                ApplyPass(&sPass, sSliced, 0, 0);
                assert(memcmp(sSliced, sOneGo, sizeof(sOneGo)) == 0);
            }
        }
        /* The atlas of a map beside the current one, sharing its primary:
         * the same three when they are drawn from primary tiles alone, and
         * one with a secondary tile in it (BG VRAM holds another map's) left
         * as it is - the current map's own pass still redraws it. */
        {
            VoxelAnim_Begin(&sPass, dirty);
            assert(VoxelAnim_AddAtlas(&sPass, 1, &inst, true, &map, live) && sPass.targets == 3);
            sMetatiles[66 * 8 + 5] = 600;
            VoxelAnim_Begin(&sPass, dirty);
            assert(VoxelAnim_AddAtlas(&sPass, 1, &inst, true, &map, live) && sPass.targets == 2);
            VoxelAnim_Begin(&sPass, dirty);
            assert(VoxelAnim_AddAtlas(&sPass, 0, &inst, false, &map, live) && sPass.targets == 3);
            sMetatiles[66 * 8 + 5] = 0;
        }
        memset(dirty, 0, sizeof(dirty));
        assert(VoxelAtlas_RefreshAnimatedPageRows(&inst, &map, 0, sOneGo, live, dirty, &rows) == 0
            && rows == 0);
        /* Animated slots a few apart in a row go as one run, the slots
         * between redrawn as they are; one far off is a run of its own. The
         * page, its animated and gap slots wiped, comes out as the full
         * refresh's. */
        {
            rows = 0;
            dirty[432 >> 3] = 1u << (432 & 7);
            memset(sMetatiles, 0, sizeof(sMetatiles));
            for (unsigned id = 1; id <= 70; ++id)
                for (unsigned t = 0; t < 4; ++t)
                    sMetatiles[id * 8 + t] = (id == 5 || id == 8 || id == 12 || id == 30) ? 432 : 10 + id + t;
            Fill(0);
            assert(VoxelAtlas_Build(&inst, sOneGo, &map, false));
            memcpy(live, sPrimaryTiles, sizeof(sPrimaryTiles));
            memcpy(live + sizeof(sPrimaryTiles), sSecondaryTiles, sizeof(sSecondaryTiles));
            memset(live + 432 * 32, 0x11, 32);
            memcpy(sSliced, sOneGo, sizeof(sOneGo));
            assert(VoxelAtlas_RefreshAnimatedPageRows(&inst, &map, 0, sOneGo, live, dirty, &rows) == 4);
            for (unsigned id = 5; id <= 30; ++id)
            {
                unsigned slot = (map.slotOf[id] - 1u) % VOXEL_ATLAS_MAX_SLOTS;

                if (id > 12 && id < 30)
                    continue;
                for (unsigned y = 0; y < 16; ++y)
                    for (unsigned x = 0; x < 16; ++x)
                        sSliced[CtrVideo_Texel((slot % VOXEL_ATLAS_COLUMNS) * 16 + x,
                                               (slot / VOXEL_ATLAS_COLUMNS) * 16 + y, VOXEL_ATLAS_W)] = 0x1234;
            }
            VoxelAnim_Begin(&sPass, dirty);
            assert(VoxelAnim_AddAtlas(&sPass, 0, &inst, false, &map, live));
            assert(sPass.targets == 4 + 5);
            assert(VoxelAnim_Compose(&sPass, live, NULL));
            assert(ApplyPass(&sPass, sSliced, 0, 0) == 2);
            assert(memcmp(sSliced, sOneGo, sizeof(sOneGo)) == 0);
        }
        /* A pass composed over several frames, the game writing the tile
         * again meanwhile: composed from the tiles as they were when it
         * began, and kept as that state. */
        {
            uint8_t first[32];

            for (unsigned i = 0; i < 32; ++i)
                live[432 * 32 + i] = first[i] = (uint8_t)(0x33u ^ i);
            memcpy(sOld, sOneGo, sizeof(sOneGo));
            VoxelAnim_Begin(&sPass, dirty);
            assert(VoxelAnim_AddAtlas(&sPass, 0, &inst, false, &map, live));
            assert(VoxelAnim_Compose(&sPass, live, NULL));
            ApplyPass(&sPass, sOld, 0, 0);
            memcpy(sSliced, sOneGo, sizeof(sOneGo));
            VoxelAnim_Begin(&sPass, dirty);
            assert(VoxelAnim_AddAtlas(&sPass, 0, &inst, false, &map, live) && sPass.blocks > 1);
            assert(!VoxelAnim_Compose(&sPass, live, NoTime));
            memset(live + 432 * 32, 0x44, 32);
            assert(VoxelAnim_Compose(&sPass, live, NULL) && !sPass.kept);
            ApplyPass(&sPass, sSliced, 0, 0);
            assert(memcmp(sSliced, sOld, sizeof(sOld)) == 0);
            memcpy(live + 432 * 32, first, sizeof(first));
            VoxelAnim_Again(&sPass);
            assert(VoxelAnim_Compose(&sPass, live, NULL) && sPass.kept);
        }
    }
}

/* All real ids plus building/cut variants: fourth page and live animation. */
static void DerivedPages(void)
{
    VoxelMapInstance inst = {.primaryTileset = &kPrimary, .secondaryTileset = &kSecondary};
    static VoxelAtlasJob job;
    uint8_t live[1024 * 32], dirty[128] = {0};
    sDerived = true;
    sVariantsResolved = sCutsResolved = false;
    Fill(0);
    memset(sPrimaryTiles, 0, 32); /* transparent upper layer */
    memset(sMetatiles, 0, sizeof(sMetatiles));
    for (unsigned m = 0; m < 1024; ++m)
        for (unsigned q = 0; q < 4; ++q) sMetatiles[m * 8 + q] = 432;
    memset(sUsed, 1, sizeof(sUsed));
    sUsed[1023] = 2; /* Visible material precedes all prefetch ids. */
    memcpy(live, sPrimaryTiles, sizeof(sPrimaryTiles));
    memcpy(live + sizeof(sPrimaryTiles), sSecondaryTiles, sizeof(sSecondaryTiles));
    memset(live + 432 * 32, 0x11, 32);
    dirty[432 >> 3] = 1u << (432 & 7);
    assert(VoxelAtlas_JobBegin(&job, &inst, sOneGo, NULL));
    unsigned pages = 0;
    do
    {
        while (!VoxelAtlas_JobStep(&job, 17)) {}
        assert(job.ok && !job.map.overflowed && job.map.used == VOXEL_METATILE_IDS);
        assert(job.map.slotOf[1023] == 1);
        unsigned expected = job.map.used - job.page * VOXEL_SOLID_BASE;
        if (expected > VOXEL_SOLID_BASE) expected = VOXEL_SOLID_BASE;
        assert(VoxelAtlas_RefreshAnimatedPage(&inst, &job.map, job.page, sOneGo, live, dirty) == expected);
        struct AtlasSource src = {.primaryTiles = live, .secondaryTiles = live + sizeof(sPrimaryTiles),
            .primaryPalettes = sPrimaryPalettes, .secondaryPalettes = sSecondaryPalettes};
        memset(sOld, 0, sizeof(sOld)); FillSolidSlots(sOld);
        for (unsigned m = 0; m < VOXEL_METATILE_IDS; ++m)
        {
            unsigned slot = job.map.slotOf[m] - 1u;
            assert(slot / VOXEL_ATLAS_MAX_SLOTS < VOXEL_ATLAS_PAGES);
            if (slot / VOXEL_ATLAS_MAX_SLOTS != job.page) continue;
            unsigned local = slot % VOXEL_ATLAS_MAX_SLOTS;
            unsigned base = m < 1024 ? m : m < VOXEL_CUT_FIRST ? m - 1024 : m - VOXEL_CUT_FIRST;
            ReferenceCompose(&src, sMetatiles + base * 8, sOld, local);
            if (m >= VOXEL_CUT_FIRST)
                sOld[CtrVideo_Texel(local % VOXEL_ATLAS_COLUMNS * 16,
                                     local / VOXEL_ATLAS_COLUMNS * 16, VOXEL_ATLAS_W)] &= (uint16_t)~1u;
        }
        assert(memcmp(sOneGo, sOld, sizeof(sOld)) == 0);
        /* A pass over this page's ids alone - ground variants, cut tiles and
         * all, the sea being in every one of them - writes each slot it took
         * as the page has it. Cut tiles take blocks of their own, so it is
         * full before the last ones: those wait. */
        {
            static VoxelAtlasMap one;
            unsigned taken = 0;

            one = job.map;
            for (unsigned m = 0; m < VOXEL_METATILE_IDS; ++m)
                if ((one.slotOf[m] - 1u) / VOXEL_ATLAS_MAX_SLOTS != job.page)
                    one.slotOf[m] = 0;
            VoxelAnim_Begin(&sPass, dirty);
            VoxelAnim_AddAtlas(&sPass, 2, &inst, false, &one, live);
            assert(sPass.targets > 0 && sPass.targets <= expected);
            assert(sPass.full == (sPass.targets < expected));
            assert(VoxelAnim_Compose(&sPass, live, NULL));
            memcpy(sSliced, sOld, sizeof(sOld));
            for (unsigned i = 0; i < sPass.targets; ++i, ++taken)
            {
                unsigned slot = sPass.target[i].slot;

                assert(sPass.target[i].page == job.page && (i == 0 || slot > sPass.target[i - 1].slot));
                for (unsigned y = 0; y < 16; ++y)
                    for (unsigned x = 0; x < 16; ++x)
                        sSliced[CtrVideo_Texel((slot % VOXEL_ATLAS_COLUMNS) * 16 + x,
                                               (slot / VOXEL_ATLAS_COLUMNS) * 16 + y, VOXEL_ATLAS_W)] = 0x1234;
            }
            /* slots side by side: a run to each row of the page they fill */
            assert(ApplyPass(&sPass, sSliced, 2, job.page) <= (taken + 31u) / 32u + 1u);
            assert(memcmp(sSliced, sOld, sizeof(sOld)) == 0);
        }
        ++pages;
    } while (VoxelAtlas_JobNextPage(&job));
    assert(pages == 4 && !job.active);
    /* A cut tile over the moving sea: passed over while its rock (all but
     * the clear corner) is drawn opaque over the sea by its upper layer,
     * redrawn once that layer is clear and the sea shows through it. */
    {
        static VoxelAtlasMap one;

        memset(&one, 0, sizeof(one));
        one.slotOf[VOXEL_CUT_FIRST + 3] = 1;
        memset(live + 10 * 32, 0x22, 32);
        for (unsigned q = 0; q < 4; ++q)
        {
            sMetatiles[3 * 8 + q] = 432;
            sMetatiles[3 * 8 + 4 + q] = 10;
        }
        VoxelAnim_Begin(&sPass, dirty);
        assert(VoxelAnim_AddAtlas(&sPass, 0, &inst, false, &one, live) && sPass.targets == 0);
        sMetatiles[3 * 8 + 4 + 2] = 0;
        VoxelAnim_Begin(&sPass, dirty);
        assert(VoxelAnim_AddAtlas(&sPass, 0, &inst, false, &one, live) && sPass.targets == 1);
    }
    sDerived = false;
}

/* A cut tile as its kept plan draws it, met once and again: its clear
 * pixel without alpha, its fringe pixel the colour of the rock beside. */
static void CutPlans(const struct AtlasSource *src)
{
    static const uint16_t entries[8] = {5 | 2 << 12, 6, 7 | 1 << 10, 8, 0, 0, 0, 0};
    uint8_t rows[32] = {0x03};
    uint16_t plain[256], cut[256];

    InitMorton();
    sFringe = true;
    for (unsigned round = 0; round < 2; ++round)
    {
        memset(plain, 0, sizeof(plain));
        memset(cut, 0, sizeof(cut));
        ComposeAt(src, entries, plain, 0, 0, 16, 0, NULL);
        ComposeAt(src, entries, cut, 0, 0, 16, 0, rows);
        plain[CtrVideo_Texel(0, 0, 16)] &= (uint16_t)~1u;
        plain[CtrVideo_Texel(1, 0, 16)] = plain[CtrVideo_Texel(1, 1, 16)];
        assert(memcmp(plain, cut, sizeof(cut)) == 0);
    }
    sFringe = false;
}

bool Voxel_LoadTilesStep(const void *t, uint8_t *d, uint32_t n, VoxelTileLoad *load, unsigned bytes)
{
    (void)bytes;
    load->ok = Voxel_LoadTiles(t, d, n);
    load->done = load->initialized = true;
    return true;
}

int main(void)
{
    struct AtlasSource src;
    unsigned blacks = 0;

    memset(&src, 0, sizeof(src));
    src.primaryTiles = sPrimaryTiles;
    src.secondaryTiles = sSecondaryTiles;
    src.primaryPalettes = sPrimaryPalettes;
    src.secondaryPalettes = sSecondaryPalettes;

    Fill(0);
    BuildColorTable(&src);
    CutPlans(&src);
    Run(&src, 2000, &blacks);
    /* Mostly black, then all black: the verdict is exercised both ways. */
    Fill(16);
    Run(&src, 2000, &blacks);
    Fill(1);
    Run(&src, 2000, &blacks);
    assert(blacks != 0);
    /* Missing secondary tiles and palettes. */
    src.secondaryTiles = NULL;
    src.secondaryPalettes = NULL;
    Run(&src, 2000, &blacks);
    src.primaryPalettes = NULL;
    Run(&src, 1000, &blacks);
    WholeAtlases();
    AnimatedTiles();
    PagedAtlases();
    VoxelGrade_Init();
    WholeAtlases();
    AnimatedTiles();
    PagedAtlases();
    DerivedPages();
    UnchangedExtension();
    printf("PASS voxel atlas: block composer equals per-texel reference (%u black), "
           "sliced atlases equal one-go ones, fresh and extended; unchanged extensions skipped, selective animation, four pages with variants and visible priority\n", blacks);
    return 0;
}
