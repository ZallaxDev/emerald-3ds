/*
 * Reader and emitter for voxel/buildings.bin (game data). See voxel_building.h;
 * scripts/gen_voxel_buildings.py writes the file and documents the models.
 *
 * Layout (little endian):
 *   "VXB9", u16 pages, models, pageModels, placements, heightBytes, masks,
 *   u32 vertices, u16 variants, u16 decks, u16 conditions, u16 0 ("VXB8":
 *   no conditions, and neither field)
 *   pages       x 8:  u16 w, h; u32 file offset of its RGBA5551 texels (h
 *                     the rows it holds, whole tiles of 8: see TakePage;
 *                     h's bit 15: w*h L8 window-glass mask bytes follow
 *                     the texels, in the same PICA order)
 *   models      x 16: u8 w, h; u16 ground; u32 firstVertex, vertexCount, heights
 *                     (vertexCount's top bit: a static model, drawn from its
 *                     own mesh, never copied into a chunk)
 *   pageModels  x 8:  u16 model, page; i16 ox, oy (pixels)
 *   placements  x 16: u16 layout, pageModel, x, y, ground, extraCount;
 *                     u32 extraFirst (sorted by layout)
 *   heightBytes       one byte per model cell, pixels; 255 = not the model's
 *   padding to 2, u16 footprint per model cell: the index of its mask, or
 *                     0xFFFF for a cell cast as a box
 *   masks x 32        16 u16 rows; bit x of row z: the solid stands over that
 *                     pixel of the cell (a railing's line, not its cell)
 *   quarters          u8 per height byte: of its own metatile's upper layer,
 *                     what the model stands for (bit 2 * row + column)
 *   padding to 2, variants x 6: u16 layout, metatile; u8 quarters; u8 0 -
 *                     the ground variants (voxel_atlas.h)
 *   decks x 8:        u16 layout; u8 x, y, top, bottom, elevation, 0 - a
 *                     bridge's deck over a cell (VoxelBuildings_DeckAt),
 *                     sorted by layout, y, x
 *   conditions x 6:   u16 placement, metatile; u8 x, y - that placement
 *                     stands only while the cell (x, y) of its rectangle
 *                     holds that metatile: a beam a script switches on and
 *                     off (setmetatile), a switch it presses
 *   padding to 4, vertices x 24: float x, y, z, u, v, shade (tiles, relative
 *                     to the top-left cell; u, v in pixels of the model's own
 *                     drawing, or of its page for a placement's ground patches)
 *   the pages' texels, read only when a map on screen needs them
 *   then, optional, the texels that follow the game's tile animations (a
 *   fountain's jet) and, last in the file, u32 that table's offset, "ANIM":
 *     u16 runs, palettes; u32 lives, statics
 *     runs     x 20: u16 page, layout; u32 the run's first byte in the page;
 *                    u16 8x8 blocks, lives; u32 first live, first static
 *     palettes x 32: sixteen RGBA5551 colours
 *     lives    x 8:  u16 texel (in the run, PICA order), tile (BG VRAM,
 *                    0-1023); u8 pixel (row * 8 + column), palette;
 *                    u16 bg (RGBA5551: that colour is drawn clear)
 *     statics  x 2:  the runs' texels as the page holds them, ungraded
 */
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* Host tests define PORT_LOG away rather than link the console's logger. */
#ifndef PORT_LOG
#include "port_log.h"
#endif

#include "voxel_atlas.h"
#include "voxel_building.h"
#include "voxel_file.h"
#include "voxel_grade.h"
#include "voxel_lighting.h"
#include "voxel_relief.h"

#ifndef VOXEL_BUILDINGS_PATH
#define VOXEL_BUILDINGS_PATH "voxel/buildings.bin"
#endif

typedef struct
{
    uint16_t w, h;          /* h: the rows the file holds */
    uint16_t texHeight;     /* the texture's: h rounded up to a power of two */
    uint32_t offset;
    bool glow;
} BuildingPage;

typedef struct
{
    uint8_t w, h;
    uint16_t ground;
    uint32_t firstVertex, vertexCount, heights;
    bool isStatic;
    float top;      /* its tallest cell, tiles */
} BuildingModel;

typedef struct
{
    uint16_t model, page;
    int16_t ox, oy;
} BuildingPageModel;

typedef struct
{
    uint16_t layout, pageModel, x, y, ground, extraCount;
    uint32_t extraFirst;
    /* It stands only while its cell (whenX, whenY) holds metatile `when`
     * (NO_CONDITION: always). */
    uint16_t when;
    uint8_t whenX, whenY;
} BuildingPlacement;

#define NO_CONDITION 0xFFFFu

static BuildingPage *sPages;
static unsigned sPageCount;
static BuildingModel *sModels;
static unsigned sModelCount;
static BuildingPageModel *sPageModels;
static unsigned sPageModelCount;
static BuildingPlacement *sPlacements;
static unsigned sPlacementCount;
static uint8_t *sHeights;
static uint16_t *sFootprints;   /* one per height byte */
static uint16_t *sMasks;        /* 16 rows each */
static unsigned sMaskCount;
static VoxelVertex *sVertices;
static unsigned sVertexCount;
static uint8_t *sQuarters;      /* one per height byte */
static uint8_t *sVariants;      /* 6 bytes each */
static unsigned sVariantCount;
static uint8_t *sDecks;         /* 8 bytes each */
static unsigned sDeckCount;
/* A placement whose cells keep their own metatile (gen_voxel_buildings.py
 * OWN_GROUND). */
#define OWN_GROUND 0xFFFFu
/* Whether ground variant (layout, metatile) is drawn from `inst`'s tilesets:
 * set by voxel_world.c, which knows the game's layouts. Unset (host tools),
 * a variant is told by its metatile and quarters alone. */
static bool (*sVariantFits)(const VoxelMapInstance *inst, unsigned layout, unsigned metatile);
static float sMaxTop;
/* Kept open for the page reads, which come one slice at a time: opening the
 * file again for every slice cost a RomFS path lookup per slice. */
static FILE *sPageFile;
/* The placements last looked up: a build or a shadow ray asks about every
 * cell it touches, nearly always of the layout it asked about last. */
static int sLastLayout = -1;
static unsigned sLastFirst, sLastCount;

/*
 * The current map as it stands. Its placements were found in its layout's
 * drawing, but a script that swapped another layout in may draw the first
 * one's models back (setmetatile): the Mirage Tower is painted on Route 111
 * without it for the moments before it crumbles, and painted out again
 * after. Where the live grid shows more of the header layout's own models
 * than of the swapped-in one's, the map's buildings are the header layout's
 * (sLive.chosen). A model whose cells a script rewrites otherwise stands:
 * the game animates some that way (the Department Store's elevator lights,
 * the lottery's computer). Worked out again only when the grid has changed
 * (VoxelBuildings_WorldChanged), and only on a map whose layout was swapped.
 */
#define LIVE_METATILE_MASK 0x3FFu
static const uint16_t *(*sDrawingOf)(int layoutId, int *width, int *height);
static struct
{
    bool valid;
    int layout, header, originX, originY;
    int chosen;
    uint32_t signature;
} sLive;

/* An upright face looking south: its span in world x and its z (FrontAt). */
typedef struct
{
    float lo, hi, z;
} FrontFace;

/*
 * A walker's card asks for the faces of the cells under it every frame, and
 * a model is read triangle by triangle - thousands of them for a Pokemon
 * Center - so a cell's faces are found once and kept. The models never
 * change; a cell is known by its layout, the layout's origin (a crossing
 * moves it) and the height asked about. A cell with more faces than an entry
 * holds is read again each time, as before.
 */
#define FRONT_SETS 64u
#define FRONT_FACES 16u

typedef struct
{
    bool valid;
    int layout, originX, originY, x, y;
    float height;
    unsigned count;
    FrontFace face[FRONT_FACES];
} FrontCell;

static FrontCell sFrontCells[FRONT_SETS];
/* Said once: a chunk with more static models than its list holds. */
static bool sStaticsFullLogged;

/*
 * The page texels drawn from tiles the game animates (Rustboro's fountain:
 * the jet). A run is whole 8x8 blocks along a row of blocks of one page, so
 * contiguous in it; its live texels are recomposed from the tiles the game
 * has just written, over the rest of its blocks as the page holds them.
 */
typedef struct
{
    uint16_t page, layout;
    uint32_t offset;            /* bytes into the page's texels */
    uint16_t blocks, lives;
    uint32_t firstLive, firstStatic;
} AnimRun;

typedef struct
{
    uint16_t texel, tile;
    uint8_t pixel, palette;
    uint16_t bg;
} AnimLive;

static AnimRun *sAnimRuns;
static unsigned sAnimRunCount;
static AnimLive *sAnimLives;
static unsigned sAnimLiveCount;
static uint16_t *sAnimPalettes;     /* sixteen colours each */
static unsigned sAnimPaletteCount;
static uint16_t *sAnimStatics;
static unsigned sAnimStaticCount;

/*
 * The placements of a layout by the rows they cover: bit i of band b is set
 * when placement i's rectangle reaches a row of b (rows >> shift). A town has
 * up to some two hundred placements - Slateport's fences, stalls and posts -
 * and every cell a build or a ray asks about walked them all; a band holds a
 * tenth of them. The bits are read lowest first, so the first placement that
 * owns a cell still answers. Built when a layout is first asked about, for a
 * few layouts at a time (a ray crosses into the map beside), and dropped with
 * the placements. A layout of more placements than the bits hold is walked
 * whole, as before.
 */
#define BAND_SLOTS 3u
#define BAND_COUNT 32u
#define BAND_WORDS 8u   /* placements: 32 bits a word */
typedef struct
{
    int layout;         /* layout id + 1; 0: empty */
    unsigned shift;
    int rows;           /* past the last row any placement covers */
    unsigned age;
    uint32_t bits[BAND_COUNT][BAND_WORDS];
} PlacementBands;
static PlacementBands sBands[BAND_SLOTS];
static unsigned sBandClock;

static unsigned U16(const uint8_t *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static uint32_t U32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static bool ReadRows(FILE *file, unsigned count, unsigned size, uint8_t *row,
                     bool (*take)(unsigned, const uint8_t *))
{
    for (unsigned i = 0; i < count; ++i)
        if (fread(row, 1, size, file) != size || !take(i, row))
            return false;
    return true;
}

static unsigned sHeightBytes;
static void FreeCellGrids(void);

/*
 * A page is a power of two wide but holds only the rows it uses, in whole
 * tiles of 8. The GPU samples it as a texture the next power of two tall:
 * the rows past the page are never stored nor streamed, and never read - no
 * face's uv reaches them. They still take their VRAM (ctr_voxel.c
 * PageBytes): an emulator's texture cache claims the texture's whole range.
 */
static bool TakePage(unsigned i, const uint8_t *r)
{
    unsigned w = U16(r), h = U16(r + 2) & 0x7FFFu, th = 8;

    while (th < h)
        th <<= 1;
    sPages[i].w = (uint16_t)w;
    sPages[i].h = (uint16_t)h;
    sPages[i].texHeight = (uint16_t)th;
    sPages[i].glow = (U16(r + 2) & 0x8000u) != 0;
    sPages[i].offset = U32(r + 4);
    return w >= 8 && w <= 1024 && (w & (w - 1)) == 0 && h > 0 && h % 8 == 0 && th <= 1024;
}

static bool TakeModel(unsigned i, const uint8_t *r)
{
    BuildingModel *m = &sModels[i];
    m->w = r[0];
    m->h = r[1];
    m->ground = (uint16_t)U16(r + 2);
    m->firstVertex = U32(r + 4);
    m->vertexCount = U32(r + 8) & 0x7FFFFFFFu;
    m->isStatic = (U32(r + 8) & 0x80000000u) != 0;
    m->heights = U32(r + 12);
    m->top = 0.0f;
    /* Subtractive bounds: a sum of two u32s from the file can wrap. */
    return m->vertexCount <= sVertexCount && m->firstVertex <= sVertexCount - m->vertexCount
        && (unsigned)m->w * m->h <= sHeightBytes && m->heights <= sHeightBytes - (unsigned)m->w * m->h;
}

static bool TakePageModel(unsigned i, const uint8_t *r)
{
    BuildingPageModel *pm = &sPageModels[i];
    pm->model = (uint16_t)U16(r);
    pm->page = (uint16_t)U16(r + 2);
    pm->ox = (int16_t)U16(r + 4);
    pm->oy = (int16_t)U16(r + 6);
    return pm->model < sModelCount && pm->page < sPageCount;
}

static bool TakePlacement(unsigned i, const uint8_t *r)
{
    BuildingPlacement *p = &sPlacements[i];
    p->layout = (uint16_t)U16(r);
    p->pageModel = (uint16_t)U16(r + 2);
    p->x = (uint16_t)U16(r + 4);
    p->y = (uint16_t)U16(r + 6);
    p->ground = (uint16_t)U16(r + 8);
    p->extraCount = (uint16_t)U16(r + 10);
    p->extraFirst = U32(r + 12);
    p->when = NO_CONDITION;
    return p->pageModel < sPageModelCount
        && p->extraCount <= sVertexCount && p->extraFirst <= sVertexCount - p->extraCount;
}

/* The animated texels' table, when the file ends with one; true with none. */
static bool ReadAnims(FILE *file)
{
    uint8_t tail[8], head[12], row[20];
    long size, at;

    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 8
     || fseek(file, size - 8, SEEK_SET) != 0 || fread(tail, 1, sizeof(tail), file) != sizeof(tail))
        return false;
    if (memcmp(tail + 4, "ANIM", 4) != 0)
        return true;
    at = (long)U32(tail);
    if (at < 0 || at + (long)sizeof(head) > size - 8 || fseek(file, at, SEEK_SET) != 0
     || fread(head, 1, sizeof(head), file) != sizeof(head))
        return false;
    sAnimRunCount = U16(head);
    sAnimPaletteCount = U16(head + 2);
    sAnimLiveCount = U32(head + 4);
    sAnimStaticCount = U32(head + 8);
    if (sAnimRunCount == 0)
        return true;
    /* Counts bounded by the bytes left, so the sizes below cannot wrap. */
    if (sAnimLiveCount > (unsigned long)(size - at) / 8u
     || sAnimStaticCount > (unsigned long)(size - at) / sizeof(uint16_t))
        return false;
    sAnimRuns = malloc(sAnimRunCount * sizeof(*sAnimRuns));
    sAnimPalettes = malloc(sAnimPaletteCount * 16u * sizeof(uint16_t) + 1);
    sAnimLives = malloc(sAnimLiveCount * sizeof(*sAnimLives) + 1);
    sAnimStatics = malloc(sAnimStaticCount * sizeof(uint16_t) + 1);
    if (!sAnimRuns || !sAnimPalettes || !sAnimLives || !sAnimStatics)
        return false;
    for (unsigned i = 0; i < sAnimRunCount; ++i)
    {
        AnimRun *r = &sAnimRuns[i];

        if (fread(row, 1, 20, file) != 20)
            return false;
        r->page = (uint16_t)U16(row);
        r->layout = (uint16_t)U16(row + 2);
        r->offset = U32(row + 4);
        r->blocks = (uint16_t)U16(row + 8);
        r->lives = (uint16_t)U16(row + 10);
        r->firstLive = U32(row + 12);
        r->firstStatic = U32(row + 16);
        if (r->page >= sPageCount || r->blocks == 0
         || r->blocks * 128u > (uint32_t)sPages[r->page].w * sPages[r->page].h * 2u
         || r->offset > (uint32_t)sPages[r->page].w * sPages[r->page].h * 2u - r->blocks * 128u
         || r->lives > sAnimLiveCount || r->firstLive > sAnimLiveCount - r->lives
         || r->blocks * 64u > sAnimStaticCount || r->firstStatic > sAnimStaticCount - r->blocks * 64u)
            return false;
    }
    /* Little-endian u16s, the console's own order. */
    if (fread(sAnimPalettes, 16u * sizeof(uint16_t), sAnimPaletteCount, file) != sAnimPaletteCount)
        return false;
    for (unsigned i = 0; i < sAnimLiveCount; ++i)
    {
        AnimLive *l = &sAnimLives[i];

        if (fread(row, 1, 8, file) != 8)
            return false;
        l->texel = (uint16_t)U16(row);
        l->tile = (uint16_t)U16(row + 2);
        l->pixel = row[4];
        l->palette = row[5];
        l->bg = (uint16_t)U16(row + 6);
        if (l->tile >= 1024u || l->pixel >= 64u || l->palette >= sAnimPaletteCount)
            return false;
    }
    if (fread(sAnimStatics, sizeof(uint16_t), sAnimStaticCount, file) != sAnimStaticCount)
        return false;
    for (unsigned i = 0; i < sAnimRunCount; ++i)
        for (unsigned k = 0; k < sAnimRuns[i].lives; ++k)
            if (sAnimLives[sAnimRuns[i].firstLive + k].texel >= sAnimRuns[i].blocks * 64u)
                return false;
    return true;
}

static void FreeAnims(void)
{
    free(sAnimRuns);
    free(sAnimLives);
    free(sAnimPalettes);
    free(sAnimStatics);
    sAnimRuns = NULL;
    sAnimLives = NULL;
    sAnimPalettes = NULL;
    sAnimStatics = NULL;
    sAnimRunCount = sAnimLiveCount = sAnimPaletteCount = sAnimStaticCount = 0;
}

bool VoxelBuildings_Init(void)
{
    uint8_t header[28], row[16];
    unsigned conditions = 0;
    bool conditioned;
    long offset;
    FILE *file;
    bool ok = false;

    VoxelBuildings_Shutdown();
    file = VoxelFile_Open(VOXEL_BUILDINGS_PATH);
    if (file == NULL)
    {
        PORT_LOG("[VIDEO] VOXEL buildings: %s absent, houses stay extruded\n",
                VOXEL_BUILDINGS_PATH);
        return false;
    }
    if (fread(header, 1, 24, file) != 24)
        goto done;
    conditioned = memcmp(header, "VXB9", 4) == 0;
    if (!conditioned && memcmp(header, "VXB8", 4) != 0)
        goto done;
    if (conditioned)
    {
        if (fread(header + 24, 1, 4, file) != 4)
            goto done;
        conditions = U16(header + 24);
    }
    sPageCount = U16(header + 4);
    sModelCount = U16(header + 6);
    sPageModelCount = U16(header + 8);
    sPlacementCount = U16(header + 10);
    sHeightBytes = U16(header + 12);
    sMaskCount = U16(header + 14);
    sVertexCount = U32(header + 16);
    sVariantCount = U16(header + 20);
    sDeckCount = U16(header + 22);

    /* A u32 count times the record size wraps a 32-bit size_t. */
    if (sVertexCount > 0x7FFFFFFFu / sizeof(VoxelVertex))
        goto done;
    sPages = malloc(sPageCount * sizeof(*sPages) + 1);
    sModels = malloc(sModelCount * sizeof(*sModels) + 1);
    sPageModels = malloc(sPageModelCount * sizeof(*sPageModels) + 1);
    sPlacements = malloc(sPlacementCount * sizeof(*sPlacements) + 1);
    sHeights = malloc(sHeightBytes + 1);
    sFootprints = malloc(sHeightBytes * sizeof(uint16_t) + 1);
    sMasks = malloc(sMaskCount * 16 * sizeof(uint16_t) + 1);
    sVertices = malloc(sVertexCount * sizeof(VoxelVertex) + 1);
    sQuarters = malloc(sHeightBytes + 1);
    sVariants = malloc(sVariantCount * 6u + 1);
    sDecks = malloc(sDeckCount * 8u + 1);
    if (!sPages || !sModels || !sPageModels || !sPlacements || !sHeights || !sVertices
     || !sFootprints || !sMasks || !sQuarters || !sVariants || !sDecks)
        goto done;
    if (!ReadRows(file, sPageCount, 8, row, TakePage)
     || !ReadRows(file, sModelCount, 16, row, TakeModel)
     || !ReadRows(file, sPageModelCount, 8, row, TakePageModel)
     || !ReadRows(file, sPlacementCount, 16, row, TakePlacement))
        goto done;
    if (fread(sHeights, 1, sHeightBytes, file) != sHeightBytes)
        goto done;
    if ((sHeightBytes & 1) != 0 && fseek(file, 1, SEEK_CUR) != 0)
        goto done;
    /* Little-endian u16s, the console's own order. */
    if (fread(sFootprints, sizeof(uint16_t), sHeightBytes, file) != sHeightBytes
     || fread(sMasks, 16 * sizeof(uint16_t), sMaskCount, file) != sMaskCount
     || fread(sQuarters, 1, sHeightBytes, file) != sHeightBytes
     || ((sHeightBytes & 1) != 0 && fseek(file, 1, SEEK_CUR) != 0)
     || fread(sVariants, 6, sVariantCount, file) != sVariantCount
     || fread(sDecks, 8, sDeckCount, file) != sDeckCount)
        goto done;
    for (unsigned i = 0; i < conditions; ++i)
    {
        BuildingPlacement *p;

        if (fread(row, 1, 6, file) != 6 || U16(row) >= sPlacementCount)
            goto done;
        p = &sPlacements[U16(row)];
        p->when = (uint16_t)U16(row + 2);
        p->whenX = row[4];
        p->whenY = row[5];
    }
    for (unsigned i = 0; i < sHeightBytes; ++i)
        if (sFootprints[i] != 0xFFFF && sFootprints[i] >= sMaskCount)
            goto done;
    offset = ftell(file);
    if (offset < 0 || fseek(file, (4 - (offset & 3)) & 3, SEEK_CUR) != 0)
        goto done;
    /* The file's vertex record is VoxelVertex exactly: six little-endian
     * floats, which is also the console's layout. */
    if (fread(sVertices, sizeof(VoxelVertex), sVertexCount, file) != sVertexCount)
        goto done;
    if (!ReadAnims(file))
        goto done;
    sMaxTop = 0.0f;
    for (unsigned i = 0; i < sHeightBytes; ++i)
        if (sHeights[i] != 0xFF && sHeights[i] / 16.0f > sMaxTop)
            sMaxTop = sHeights[i] / 16.0f;
    for (unsigned i = 0; i < sModelCount; ++i)
    {
        BuildingModel *m = &sModels[i];

        for (unsigned c = 0; c < (unsigned)m->w * m->h; ++c)
            if (sHeights[m->heights + c] != 0xFF && sHeights[m->heights + c] / 16.0f > m->top)
                m->top = sHeights[m->heights + c] / 16.0f;
    }
    ok = true;

done:
    fclose(file);
    if (!ok)
    {
        PORT_LOG("[ERROR] VOXEL buildings: %s is truncated or malformed\n",
                VOXEL_BUILDINGS_PATH);
        VoxelBuildings_Shutdown();
        return false;
    }
    PORT_LOG("[VIDEO] VOXEL buildings: %u models on %u pages, %u placements, %u vertices, "
            "%u animated runs\n", sModelCount, sPageCount, sPlacementCount, sVertexCount,
            sAnimRunCount);
    return true;
}

void VoxelBuildings_Shutdown(void)
{
    free(sPages);
    free(sModels);
    free(sPageModels);
    free(sPlacements);
    free(sHeights);
    free(sFootprints);
    free(sMasks);
    free(sVertices);
    free(sQuarters);
    free(sVariants);
    sLive.valid = false;
    FreeAnims();
    free(sDecks);
    sQuarters = NULL;
    sVariants = NULL;
    sVariantCount = 0;
    sDecks = NULL;
    sDeckCount = 0;
    sPages = NULL;
    sModels = NULL;
    sPageModels = NULL;
    sPlacements = NULL;
    sHeights = NULL;
    sFootprints = NULL;
    sMasks = NULL;
    sMaskCount = 0;
    sVertices = NULL;
    sPageCount = sModelCount = sPageModelCount = sPlacementCount = sVertexCount = 0;
    sLastLayout = -1;
    memset(sBands, 0, sizeof(sBands));
    FreeCellGrids();
    memset(sFrontCells, 0, sizeof(sFrontCells));
    sMaxTop = 0.0f;
    if (sPageFile != NULL)
    {
        fclose(sPageFile);
        sPageFile = NULL;
    }
}

float VoxelBuildings_MaxTop(void)
{
    return sMaxTop;
}

bool VoxelBuildings_PageSize(unsigned page, unsigned *width, unsigned *height,
                             unsigned *texHeight)
{
    if (page >= sPageCount)
        return false;
    *width = sPages[page].w;
    *height = sPages[page].h;
    *texHeight = sPages[page].texHeight;
    return true;
}

bool VoxelBuildings_ReadPage(unsigned page, unsigned first, unsigned count, uint16_t *dest)
{
    if (page >= sPageCount || first > (unsigned)sPages[page].w * sPages[page].h
     || count > (unsigned)sPages[page].w * sPages[page].h - first)
        return false;
    if (sPageFile == NULL)
    {
        sPageFile = VoxelFile_Open(VOXEL_BUILDINGS_PATH);
        if (sPageFile == NULL)
            return false;
        /* Slices go straight into the caller's buffer, never through stdio's. */
        setvbuf(sPageFile, NULL, _IONBF, 0);
    }
    if (fseek(sPageFile, (long)(sPages[page].offset + first * sizeof(uint16_t)), SEEK_SET) != 0
     || fread(dest, sizeof(uint16_t), count, sPageFile) != count)
        return false;
    /* On the reading thread, off the render thread's frame. */
    VoxelGrade_Texels(dest, count);
    return true;
}

unsigned VoxelBuildings_AnimRunCount(void)
{
    return sAnimRunCount;
}

bool VoxelBuildings_AnimRun(unsigned run, unsigned *page, int *layout, unsigned *offset,
                            unsigned *bytes)
{
    if (run >= sAnimRunCount)
        return false;
    *page = sAnimRuns[run].page;
    *layout = sAnimRuns[run].layout;
    *offset = sAnimRuns[run].offset;
    *bytes = sAnimRuns[run].blocks * 128u;
    return true;
}

bool VoxelBuildings_AnimRunDirty(unsigned run, const uint8_t dirty[128])
{
    const AnimRun *r;

    if (run >= sAnimRunCount)
        return false;
    r = &sAnimRuns[run];
    for (unsigned k = 0; k < r->lives; ++k)
    {
        unsigned tile = sAnimLives[r->firstLive + k].tile;

        if (dirty[tile >> 3] & (1u << (tile & 7)))
            return true;
    }
    return false;
}

void VoxelBuildings_ComposeAnimRun(unsigned run, const uint8_t *liveTiles, uint16_t *dest)
{
    const AnimRun *r;
    unsigned count;

    if (run >= sAnimRunCount)
        return;
    r = &sAnimRuns[run];
    count = r->blocks * 64u;
    memcpy(dest, sAnimStatics + r->firstStatic, count * sizeof(uint16_t));
    for (unsigned k = 0; k < r->lives; ++k)
    {
        const AnimLive *l = &sAnimLives[r->firstLive + k];
        /* 4bpp, eight rows of four bytes, the left pixel in the low nibble */
        uint8_t b = liveTiles[l->tile * 32u + (l->pixel >> 3) * 4u + ((l->pixel & 7u) >> 1)];
        unsigned index = (l->pixel & 1u) ? (unsigned)(b >> 4) : (unsigned)(b & 15u);
        uint16_t c = index != 0 ? sAnimPalettes[l->palette * 16u + index] : 0;

        dest[l->texel] = c == l->bg ? 0 : c;
    }
    /* graded as the page's own texels were when they were read */
    VoxelGrade_Texels(dest, count);
}

bool VoxelBuildings_PageHasGlow(unsigned page)
{
    return page < sPageCount && sPages[page].glow;
}

bool VoxelBuildings_ReadGlow(unsigned page, unsigned first, unsigned count, uint8_t *dest)
{
    unsigned pixels;

    if (!VoxelBuildings_PageHasGlow(page) || sPageFile == NULL)
        return false;
    pixels = (unsigned)sPages[page].w * sPages[page].h;
    if (first > pixels || count > pixels - first)
        return false;
    return fseek(sPageFile, (long)(sPages[page].offset + pixels * 2u + first), SEEK_SET) == 0
        && fread(dest, 1, count, sPageFile) == count;
}

/* The placements of one layout, which the generator sorted by layout. */
static const BuildingPlacement *PlacementsOf(int layout, unsigned *count)
{
    unsigned lo = 0, hi = sPlacementCount, n = 0;

    *count = 0;
    if (sPlacementCount == 0)
        return NULL;
    if (layout != sLastLayout)
    {
        while (lo < hi)
        {
            unsigned mid = (lo + hi) / 2;

            if ((int)sPlacements[mid].layout < layout)
                lo = mid + 1;
            else
                hi = mid;
        }
        while (lo + n < sPlacementCount && (int)sPlacements[lo + n].layout == layout)
            ++n;
        sLastLayout = layout;
        sLastFirst = lo;
        sLastCount = n;
    }
    *count = sLastCount;
    return sLastCount ? &sPlacements[sLastFirst] : NULL;
}

void VoxelBuildings_SetDrawings(const uint16_t *(*drawing)(int layoutId, int *width, int *height))
{
    if (drawing != sDrawingOf)
        sLive.valid = false;
    sDrawingOf = drawing;
}

void VoxelBuildings_WorldChanged(void)
{
    sLive.valid = false;
}

/*
 * Of the cells within the map that placement `p`'s model stands on
 * (`covered`), those the live grid draws as `drawing` does (`live`), and
 * those `other` - a drawing of the same size, or NULL - draws the same
 * (`twin`).
 */
static void Tally(const VoxelMapInstance *inst, const BuildingPlacement *p,
                  const uint16_t *drawing, const uint16_t *other, int width, int height,
                  unsigned *covered, unsigned *live, unsigned *twin)
{
    const BuildingModel *m = &sModels[sPageModels[p->pageModel].model];

    *covered = *live = *twin = 0;
    for (int cy = 0; cy < m->h; ++cy)
        for (int cx = 0; cx < m->w; ++cx)
        {
            int x = p->x + cx, y = p->y + cy;
            unsigned own;

            if (x >= width || y >= height
             || sHeights[m->heights + (unsigned)cy * m->w + (unsigned)cx] == 0xFF)
                continue;
            own = drawing[(size_t)y * width + x] & LIVE_METATILE_MASK;
            ++*covered;
            if ((unsigned)VoxelWorld_GetMetatileId(inst->originX + x, inst->originY + y) == own)
                ++*live;
            if (other != NULL && (other[(size_t)y * width + x] & LIVE_METATILE_MASK) == own)
                ++*twin;
        }
}

/* How many of `layout`'s placements the live grid shows (most of their
 * cells drawn as `drawing` draws them) where `other` draws something else. */
static unsigned ShownAlone(const VoxelMapInstance *inst, int layout, const uint16_t *drawing,
                           const uint16_t *other, int width, int height)
{
    unsigned count, shown = 0;
    const BuildingPlacement *p = PlacementsOf(layout, &count);

    for (unsigned i = 0; p != NULL && i < count; ++i)
    {
        unsigned covered, live, twin;

        Tally(inst, &p[i], drawing, other, width, height, &covered, &live, &twin);
        if (covered != 0 && 2u * live > covered && 2u * twin <= covered)
            ++shown;
    }
    return shown;
}

static void RefreshLive(const VoxelMapInstance *inst)
{
    const uint16_t *own, *other;
    int width = 0, height = 0, otherWidth = 0, otherHeight = 0;

    sLive.valid = true;
    sLive.layout = inst->layoutId;
    sLive.header = inst->headerLayoutId;
    sLive.originX = inst->originX;
    sLive.originY = inst->originY;
    sLive.chosen = inst->layoutId;
    sLive.signature = 0;
    if (inst->headerLayoutId == inst->layoutId)
        return;
    own = sDrawingOf(inst->layoutId, &width, &height);
    other = sDrawingOf(inst->headerLayoutId, &otherWidth, &otherHeight);
    if (own == NULL || other == NULL || otherWidth != width || otherHeight != height)
        return;
    if (ShownAlone(inst, inst->headerLayoutId, other, own, width, height)
        > ShownAlone(inst, inst->layoutId, own, other, width, height))
    {
        /* 0 while the map stands as it was entered: its chunks, built as a
         * neighbour's before the crossing, stay as they are */
        sLive.chosen = inst->headerLayoutId;
        sLive.signature = (2166136261u ^ (uint32_t)sLive.chosen) * 16777619u;
    }
}

/* The layout whose placements `inst` stands: its own, or for the current
 * map the drawing the live grid shows (sLive). */
static int BuildingLayout(const VoxelMapInstance *inst)
{
    if (!inst->live || sDrawingOf == NULL || sModels == NULL)
        return inst->layoutId;
    if (!sLive.valid || sLive.layout != inst->layoutId || sLive.header != inst->headerLayoutId
     || sLive.originX != inst->originX || sLive.originY != inst->originY)
        RefreshLive(inst);
    return sLive.chosen;
}

static const BuildingPlacement *LayoutPlacements(const VoxelMapInstance *inst, unsigned *count)
{
    *count = 0;
    if (inst == NULL || sPlacementCount == 0)
        return NULL;
    return PlacementsOf(BuildingLayout(inst), count);
}

int VoxelBuildings_LayoutOf(const VoxelMapInstance *inst)
{
    return inst == NULL ? -1 : BuildingLayout(inst);
}

uint32_t VoxelBuildings_LiveSignature(const VoxelMapInstance *inst)
{
    if (inst == NULL || !inst->live || sDrawingOf == NULL || sModels == NULL)
        return 0;
    BuildingLayout(inst);
    return sLive.signature;
}

/* Whether a placement stands as the map is now: one a script switches
 * (setmetatile) only while its cell holds the metatile it was built for. */
static bool PlacementLive(const VoxelMapInstance *inst, const BuildingPlacement *p)
{
    return p->when == NO_CONDITION
        || VoxelWorld_GetMetatileId(inst->originX + p->x + p->whenX,
                                    inst->originY + p->y + p->whenY) == (int)p->when;
}

/* A few layouts' answers kept: the lighting asks once per map on screen at
 * every reset of its caches. */
#define LAYOUT_TOPS 8u

float VoxelBuildings_LayoutTop(const VoxelMapInstance *inst)
{
    static int sTopLayout[LAYOUT_TOPS]; /* layout + 1; 0 is empty */
    static float sTop[LAYOUT_TOPS];
    static unsigned sTopNext;
    unsigned count;
    const BuildingPlacement *p;
    float top = 0.0f;

    int layout;

    if (inst == NULL || sModels == NULL)
        return 0.0f;
    layout = BuildingLayout(inst);
    for (unsigned i = 0; i < LAYOUT_TOPS; ++i)
        if (sTopLayout[i] == layout + 1)
            return sTop[i];
    p = PlacementsOf(layout, &count);
    for (unsigned i = 0; i < count; ++i)
    {
        const BuildingModel *m = &sModels[sPageModels[p[i].pageModel].model];

        for (unsigned k = 0; k < (unsigned)m->w * m->h; ++k)
            if (sHeights[m->heights + k] != 0xFF && sHeights[m->heights + k] / 16.0f > top)
                top = sHeights[m->heights + k] / 16.0f;
    }
    sTopLayout[sTopNext] = layout + 1;
    sTop[sTopNext] = top;
    sTopNext = (sTopNext + 1) % LAYOUT_TOPS;
    return top;
}

int VoxelBuildings_PageOf(const VoxelMapInstance *inst)
{
    unsigned count;
    const BuildingPlacement *p = LayoutPlacements(inst, &count);

    return p ? (int)sPageModels[p[0].pageModel].page : -1;
}

/* The bands of the layout `p` and `count` are the placements of (NULL when
 * they do not fit), from a slot or built into the least recently used. */
static const PlacementBands *LayoutBands(const VoxelMapInstance *inst,
                                         const BuildingPlacement *p, unsigned count)
{
    PlacementBands *b = &sBands[0];
    int rows = 0, layout = BuildingLayout(inst);

    if (count > BAND_WORDS * 32u)
        return NULL;
    for (unsigned s = 0; s < BAND_SLOTS; ++s)
    {
        if (sBands[s].layout == layout + 1)
        {
            sBands[s].age = ++sBandClock;
            return &sBands[s];
        }
        if (sBands[s].age < b->age)
            b = &sBands[s];
    }
    for (unsigned i = 0; i < count; ++i)
    {
        int end = (int)p[i].y + sModels[sPageModels[p[i].pageModel].model].h;

        if (end > rows)
            rows = end;
    }
    b->layout = layout + 1;
    b->age = ++sBandClock;
    b->rows = rows;
    b->shift = 0;
    while (rows > 0 && (unsigned)(rows - 1) >> b->shift >= BAND_COUNT)
        ++b->shift;
    memset(b->bits, 0, sizeof(b->bits));
    for (unsigned i = 0; i < count; ++i)
    {
        const BuildingModel *m = &sModels[sPageModels[p[i].pageModel].model];

        if (m->h == 0)
            continue;
        for (unsigned band = p[i].y >> b->shift; band <= (p[i].y + m->h - 1u) >> b->shift; ++band)
            b->bits[band][i / 32u] |= 1u << (i % 32u);
    }
    return b;
}

/*
 * A layout of more placements than the bands hold (Route 110's cycling
 * road: a model a cell, voxel_bridge.py): which placement stands over each
 * of its cells, worked out once, for a few layouts at a time; NO_CELL for
 * none. The first placement that owns a cell answers, as in the bands.
 */
#define CELL_GRIDS 3u
#define NO_CELL 0xFFFFu

typedef struct
{
    int layout, width, height;
    uint16_t *cells;
} CellGrid;

static CellGrid sCellGrids[CELL_GRIDS];
static unsigned sCellGridNext;

/*
 * The placements whose rectangle reaches each row of a layout, in placement
 * order, for a few layouts at a time: the faces of a cell (CollectFronts) are
 * looked for among those alone. Walked whole, Route 110's thousand placements
 * were read for every cell under a walker the front cache had not seen - all
 * of them after a crossing (VoxelBuildings_MapChanged): sprites=4-5 ms on an
 * Old 3DS on the crossing frame (port.log, 2026-10-25).
 */
#define ROW_INDEXES 3u

typedef struct
{
    int layout;          /* layout id + 1; 0: empty */
    int rows;            /* past the last row any placement covers */
    uint32_t *start;     /* rows + 1 offsets into `placements` */
    uint16_t *placements;
} RowIndex;

static RowIndex sRowIndexes[ROW_INDEXES];
static unsigned sRowIndexNext;

static void FreeRowIndex(RowIndex *r)
{
    free(r->start);
    free(r->placements);
    r->start = NULL;
    r->placements = NULL;
    r->layout = 0;
}

static void FreeCellGrids(void)
{
    for (unsigned i = 0; i < CELL_GRIDS; ++i)
    {
        free(sCellGrids[i].cells);
        sCellGrids[i].cells = NULL;
        sCellGrids[i].layout = -1;
    }
    sCellGridNext = 0;
    for (unsigned i = 0; i < ROW_INDEXES; ++i)
        FreeRowIndex(&sRowIndexes[i]);
    sRowIndexNext = 0;
}

/* The row index of the layout `p` and `count` are the placements of; NULL
 * when it cannot be had (the placements are then walked whole). */
static const RowIndex *RowIndexOf(const VoxelMapInstance *inst, const BuildingPlacement *p,
                                  unsigned count)
{
    int layout = BuildingLayout(inst);
    RowIndex *r;
    uint32_t total = 0;
    int rows = 0;

    for (unsigned i = 0; i < ROW_INDEXES; ++i)
        if (sRowIndexes[i].layout == layout + 1)
            return &sRowIndexes[i];
    if (count >= NO_CELL)
        return NULL;
    for (unsigned i = 0; i < count; ++i)
    {
        int h = sModels[sPageModels[p[i].pageModel].model].h;

        if ((int)p[i].y + h > rows)
            rows = (int)p[i].y + h;
        total += (uint32_t)h;
    }
    r = &sRowIndexes[sRowIndexNext];
    sRowIndexNext = (sRowIndexNext + 1) % ROW_INDEXES;
    FreeRowIndex(r);
    r->start = calloc((size_t)rows + 1u, sizeof(uint32_t));
    r->placements = malloc((total ? total : 1u) * sizeof(uint16_t));
    if (r->start == NULL || r->placements == NULL)
    {
        FreeRowIndex(r);
        return NULL;
    }
    /* counted per row, offsets, then filled in placement order */
    for (unsigned i = 0; i < count; ++i)
        for (int y = p[i].y; y < (int)p[i].y + sModels[sPageModels[p[i].pageModel].model].h; ++y)
            ++r->start[y + 1];
    for (int y = 0; y < rows; ++y)
        r->start[y + 1] += r->start[y];
    {
        uint32_t *fill = malloc((size_t)rows * sizeof(uint32_t));

        if (fill == NULL)
        {
            FreeRowIndex(r);
            return NULL;
        }
        memcpy(fill, r->start, (size_t)rows * sizeof(uint32_t));
        for (unsigned i = 0; i < count; ++i)
            for (int y = p[i].y; y < (int)p[i].y + sModels[sPageModels[p[i].pageModel].model].h; ++y)
                r->placements[fill[y]++] = (uint16_t)i;
        free(fill);
    }
    r->rows = rows;
    r->layout = layout + 1;
    return r;
}

static const CellGrid *CellGridOf(const VoxelMapInstance *inst, const BuildingPlacement *p,
                                  unsigned count)
{
    size_t size = (size_t)inst->width * inst->height;
    int layout = BuildingLayout(inst);
    CellGrid *g;

    if (count >= NO_CELL)
        return NULL;
    /* Looked up first: this runs for every cell the lighting reads, and a
     * layout has a grid only if none of its placements comes and goes. */
    for (unsigned i = 0; i < CELL_GRIDS; ++i)
        if (sCellGrids[i].cells != NULL && sCellGrids[i].layout == layout
         && sCellGrids[i].width == inst->width && sCellGrids[i].height == inst->height)
            return &sCellGrids[i];
    /* a grid worked out once cannot follow a placement that comes and goes */
    for (unsigned i = 0; i < count; ++i)
        if (p[i].when != NO_CONDITION)
            return NULL;
    g = &sCellGrids[sCellGridNext];
    sCellGridNext = (sCellGridNext + 1) % CELL_GRIDS;
    free(g->cells);
    g->layout = -1;
    g->cells = malloc(size * sizeof(uint16_t));
    if (g->cells == NULL)
        return NULL;
    memset(g->cells, 0xFF, size * sizeof(uint16_t));
    for (unsigned i = 0; i < count; ++i)
    {
        const BuildingModel *m = &sModels[sPageModels[p[i].pageModel].model];

        for (int cy = 0; cy < m->h; ++cy)
            for (int cx = 0; cx < m->w; ++cx)
            {
                int x = p[i].x + cx, y = p[i].y + cy;
                uint16_t *cell;

                /* A hedge's rectangle holds the house it runs round. */
                if (x >= inst->width || y >= inst->height
                 || sHeights[m->heights + (unsigned)cy * m->w + (unsigned)cx] == 0xFF)
                    continue;
                cell = &g->cells[(size_t)y * inst->width + x];
                if (*cell == NO_CELL)
                    *cell = (uint16_t)i;
            }
    }
    g->layout = layout;
    g->width = inst->width;
    g->height = inst->height;
    return g;
}

/* The model cell over world cell (x, y): the index of its height byte, or
 * -1. The first placement that owns the cell answers. */
static long ModelCell(const VoxelMapInstance *inst, int x, int y, unsigned *placement)
{
    unsigned count;
    const BuildingPlacement *p = LayoutPlacements(inst, &count);
    const PlacementBands *bands;
    const CellGrid *g;
    int lx, ly;

    if (p == NULL)
        return -1;
    lx = x - inst->originX;
    ly = y - inst->originY;
    bands = LayoutBands(inst, p, count);
    if (bands != NULL)
    {
        const uint32_t *band;

        /* No placement reaches a row out of [0, rows). */
        if (ly < 0 || ly >= bands->rows)
            return -1;
        band = bands->bits[(unsigned)ly >> bands->shift];
        for (unsigned w = 0; w < BAND_WORDS; ++w)
            for (uint32_t bits = band[w]; bits != 0; bits &= bits - 1u)
            {
                unsigned i = w * 32u + (unsigned)__builtin_ctz(bits);
                const BuildingModel *m = &sModels[sPageModels[p[i].pageModel].model];
                int cx = lx - p[i].x, cy = ly - p[i].y;
                unsigned k;

                if (cx < 0 || cy < 0 || cx >= m->w || cy >= m->h)
                    continue;
                k = m->heights + (unsigned)cy * m->w + (unsigned)cx;
                if (sHeights[k] == 0xFF || !PlacementLive(inst, &p[i]))
                    continue;
                if (placement != NULL)
                    *placement = i;
                return (long)k;
            }
        return -1;
    }
    g = lx >= 0 && ly >= 0 && lx < inst->width && ly < inst->height
      ? CellGridOf(inst, p, count) : NULL;
    if (g != NULL)
    {
        const BuildingModel *m;
        unsigned i = g->cells[(size_t)ly * inst->width + lx];

        if (i == NO_CELL)
            return -1;
        m = &sModels[sPageModels[p[i].pageModel].model];
        if (placement != NULL)
            *placement = i;
        return (long)(m->heights + (unsigned)(ly - p[i].y) * m->w + (unsigned)(lx - p[i].x));
    }
    for (unsigned i = 0; i < count; ++i)
    {
        const BuildingModel *m = &sModels[sPageModels[p[i].pageModel].model];
        int cx = lx - p[i].x, cy = ly - p[i].y;
        unsigned k;

        if (cx < 0 || cy < 0 || cx >= m->w || cy >= m->h)
            continue;
        /* A hedge's rectangle holds the house it runs round. */
        k = m->heights + (unsigned)cy * m->w + (unsigned)cx;
        if (sHeights[k] == 0xFF || !PlacementLive(inst, &p[i]))
            continue;
        if (placement != NULL)
            *placement = i;
        return (long)k;
    }
    return -1;
}

bool VoxelBuildings_CellAt(const VoxelMapInstance *inst, int x, int y,
                           int *groundMetatile, float *top)
{
    unsigned i, count;
    long k = ModelCell(inst, x, y, &i);

    if (k < 0)
        return false;
    if (groundMetatile != NULL)
    {
        *groundMetatile = LayoutPlacements(inst, &count)[i].ground;
        if (*groundMetatile == (int)OWN_GROUND)
        {
            /* the cell's own drawing, less what the model stands for */
            unsigned own = (unsigned)VoxelWorld_GetMetatileId(x, y);

            *groundMetatile = (int)own;
            /* The generator keeps one variant per tileset, metatile and
             * quarters: the same metatile id of another secondary tileset
             * (Slateport's 0x22B, Route 111's) is another variant, one the
             * atlas of these tilesets never composes (AtlasSourceOf). */
            for (unsigned v = 0; sQuarters[k] != 0 && v < sVariantCount && v < VOXEL_VARIANTS; ++v)
                if (U16(sVariants + 6u * v + 2) == own && sVariants[6u * v + 4] == sQuarters[k]
                 && (sVariantFits == NULL || sVariantFits(inst, U16(sVariants + 6u * v), own)))
                {
                    *groundMetatile = (int)(VOXEL_METATILE_REAL + v);
                    break;
                }
        }
    }
    if (top != NULL)
        *top = sHeights[k] / 16.0f;
    return true;
}

bool VoxelBuildings_BareVariant(const VoxelMapInstance *inst, int x, int y, int *metatile)
{
    unsigned i;
    long k = ModelCell(inst, x, y, &i);
    unsigned own;

    /* a cell of the model it stands over nothing of, with quarters to
     * leave out: a relief_bare house's roof drawn over the terrace face */
    if (k < 0 || sHeights[k] != 0 || sQuarters[k] == 0)
        return false;
    own = (unsigned)VoxelWorld_GetMetatileId(x, y);
    for (unsigned v = 0; v < sVariantCount && v < VOXEL_VARIANTS; ++v)
        if (U16(sVariants + 6u * v + 2) == own && sVariants[6u * v + 4] == sQuarters[k]
         && (sVariantFits == NULL || sVariantFits(inst, U16(sVariants + 6u * v), own)))
        {
            *metatile = (int)(VOXEL_METATILE_REAL + v);
            return true;
        }
    return false;
}

bool VoxelBuildings_DeckAt(const VoxelMapInstance *inst, float worldX, float worldZ,
                           unsigned elevation, float *lift)
{
    int x, y;
    unsigned lo = 0, hi = sDeckCount, key;

    if (inst == NULL || sDeckCount == 0)
        return false;
    x = (int)floorf(worldX) - inst->originX;
    y = (int)floorf(worldZ) - inst->originY;
    if (x < 0 || y < 0 || x > 255 || y > 255)
        return false;
    key = ((unsigned)BuildingLayout(inst) << 16) | ((unsigned)y << 8) | (unsigned)x;
    while (lo < hi)
    {
        unsigned mid = (lo + hi) / 2;
        const uint8_t *d = sDecks + 8u * mid;
        unsigned k = (U16(d) << 16) | ((unsigned)d[3] << 8) | d[2];

        if (k < key)
            lo = mid + 1;
        else if (k > key)
            hi = mid;
        else
        {
            float f = worldZ - floorf(worldZ);

            /* a cell walked both over and under: up there only at the
             * deck's own elevation */
            if (d[6] != 0 && d[6] != elevation)
                return false;
            *lift = (d[4] + (d[5] - (float)d[4]) * f) / 16.0f;
            return true;
        }
    }
    return false;
}

void VoxelBuildings_SetVariantFits(bool (*fits)(const VoxelMapInstance *inst, unsigned layout,
                                                unsigned metatile))
{
    sVariantFits = fits;
}

bool VoxelBuildings_Variant(unsigned i, unsigned *layout, unsigned *metatile, unsigned *quarters)
{
    if (i >= sVariantCount)
        return false;
    *layout = U16(sVariants + 6u * i);
    *metatile = U16(sVariants + 6u * i + 2);
    *quarters = sVariants[6u * i + 4];
    return true;
}

const uint16_t *VoxelBuildings_Footprint(const VoxelMapInstance *inst, int x, int y)
{
    long k = ModelCell(inst, x, y, NULL);

    if (k < 0 || sFootprints[k] == 0xFFFF)
        return NULL;
    return &sMasks[(unsigned)sFootprints[k] * 16u];
}

/*
 * The upright faces looking south in world cell (x, y) that cross `height`:
 * each one's span in world x and its z in world tiles. Every model that
 * reaches the cell, not the first one whose rectangle holds it: a Pokemon
 * Center's PC stands in a cell of the counter's rectangle, and the counter
 * has no face there. Returns how many there are, writing at most `max`.
 */
static unsigned CollectFronts(const VoxelMapInstance *inst, int x, int y, float height,
                              FrontFace *out, unsigned max)
{
    unsigned count;
    const BuildingPlacement *p = LayoutPlacements(inst, &count);
    const RowIndex *rows = p != NULL ? RowIndexOf(inst, p, count) : NULL;
    int row = y - inst->originY;
    uint32_t n = count, first = 0;
    unsigned found = 0;

    /* Only the placements that reach the cell's row, in the same order. */
    if (rows != NULL)
    {
        if (row < 0 || row >= rows->rows)
            return 0;
        first = rows->start[row];
        n = rows->start[row + 1] - first;
    }
    for (uint32_t t = 0; p != NULL && t < n; ++t)
    {
        unsigned i = rows != NULL ? rows->placements[first + t] : t;
        const BuildingModel *m = &sModels[sPageModels[p[i].pageModel].model];
        const VoxelVertex *v = &sVertices[m->firstVertex];
        int lx = x - inst->originX - p[i].x, ly = y - inst->originY - p[i].y;
        float left = (float)(inst->originX + p[i].x), north = (float)ly;

        if (lx < 0 || ly < 0 || lx >= m->w || ly >= m->h || !PlacementLive(inst, &p[i]))
            continue;
        for (uint32_t k = 0; k + 2 < m->vertexCount; k += 3)
        {
            const VoxelVertex *a = &v[k], *b = &v[k + 1], *c = &v[k + 2];
            float lo = a->x < b->x ? a->x : b->x, hi = a->x > b->x ? a->x : b->x;
            float y0 = a->y < b->y ? a->y : b->y, y1 = a->y > b->y ? a->y : b->y;

            if (c->x < lo) lo = c->x;
            if (c->x > hi) hi = c->x;
            if (c->y < y0) y0 = c->y;
            if (c->y > y1) y1 = c->y;
            /* upright, in the cell, at the height */
            if (a->z != b->z || a->z != c->z || a->z < north - 0.01f || a->z > north + 1.01f
             || y0 > height || y1 < height)
                continue;
            /* and looking south: a counter's back is not in front of the
             * nurse behind it (a face winds outwards) */
            if ((b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x) <= 0.0f)
                continue;
            {
                FrontFace face = {lo + left, hi + left, (float)(inst->originY + p[i].y) + a->z};
                unsigned j;

                /* A face at the same depth that meets one already found
                 * is the same front for FrontAt: one span. */
                for (j = 0; j < found && j < max; ++j)
                    if (out[j].z == face.z && face.lo <= out[j].hi && face.hi >= out[j].lo)
                    {
                        if (face.lo < out[j].lo) out[j].lo = face.lo;
                        if (face.hi > out[j].hi) out[j].hi = face.hi;
                        break;
                    }
                if (j < found && j < max)
                    continue;
                if (found < max)
                    out[found] = face;
                ++found;
            }
        }
    }
    return found;
}

void VoxelBuildings_MapChanged(void)
{
    /* a placement a script switched comes or goes with its faces */
    memset(sFrontCells, 0, sizeof(sFrontCells));
}

static const FrontCell *FrontCellAt(const VoxelMapInstance *inst, int x, int y, float height)
{
    int layout = BuildingLayout(inst);
    uint32_t hash = (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u
                  ^ (uint32_t)layout * 83492791u;
    FrontCell *cell = &sFrontCells[(hash ^ (hash >> 16)) & (FRONT_SETS - 1u)];

    if (!cell->valid || cell->layout != layout || cell->originX != inst->originX
     || cell->originY != inst->originY || cell->x != x || cell->y != y || cell->height != height)
    {
        cell->valid = true;
        cell->layout = layout;
        cell->originX = inst->originX;
        cell->originY = inst->originY;
        cell->x = x;
        cell->y = y;
        cell->height = height;
        cell->count = CollectFronts(inst, x, y, height, cell->face, FRONT_FACES);
    }
    return cell;
}

bool VoxelBuildings_FrontAt(const VoxelMapInstance *inst, int x, int y,
                            float x0, float x1, float height, float *z)
{
    FrontFace many[64];
    const FrontCell *cell = FrontCellAt(inst, x, y, height);
    const FrontFace *face = cell->face;
    unsigned count = cell->count;
    bool found = false;

    if (count > FRONT_FACES)
    {
        static bool logged;

        if (!logged)
            PORT_LOG("[VIDEO] VOXEL front faces: %u in cell %d,%d, read each time\n", count, x, y);
        logged = true;
        count = CollectFronts(inst, x, y, height, many, 64);
        if (count > 64)
            count = 64;
        face = many;
    }
    for (unsigned i = 0; i < count; ++i)
    {
        /* across the span */
        if (face[i].lo >= x1 || face[i].hi <= x0)
            continue;
        if (!found || face[i].z > *z)
            *z = face[i].z;
        found = true;
    }
    return found;
}

bool VoxelBuildings_StaticModel(unsigned pageModel, const VoxelVertex **vertices,
                                unsigned *count, float *ox, float *oy, float *su, float *sv)
{
    const BuildingPageModel *pm;
    const BuildingModel *m;
    const BuildingPage *page;

    if (pageModel >= sPageModelCount)
        return false;
    pm = &sPageModels[pageModel];
    m = &sModels[pm->model];
    page = &sPages[pm->page];
    *vertices = &sVertices[m->firstVertex];
    *count = m->vertexCount;
    *ox = (float)pm->ox;
    *oy = (float)pm->oy;
    *su = 1.0f / page->w;
    *sv = 1.0f / page->texHeight;
    return true;
}

bool VoxelBuildings_DoorWall(const VoxelMapInstance *inst, int x, int y, float *z)
{
    /* across the middle of the cell, half a tile up */
    return VoxelBuildings_FrontAt(inst, x, y, (float)x + 0.49f, (float)x + 0.51f, 0.5f, z);
}

bool VoxelBuildings_EmitSome(VoxelBuilder *builder, const VoxelMapInstance *inst,
                             int x0, int y0, int x1, int y1, VoxelBuildingCursor *cursor,
                             unsigned triangles)
{
    unsigned count;
    const BuildingPlacement *p = LayoutPlacements(inst, &count);

    for (; p != NULL && cursor->placement < count; ++cursor->placement, cursor->part = 0,
                                                     cursor->vertex = 0)
    {
        unsigned i = cursor->placement;
        const BuildingPageModel *pm = &sPageModels[p[i].pageModel];
        const BuildingModel *m = &sModels[pm->model];
        const BuildingPage *page = &sPages[pm->page];
        float wx = (float)(inst->originX + p[i].x);
        float wz = (float)(inst->originY + p[i].y);
        float su = 1.0f / page->w, sv = 1.0f / page->texHeight;
        /* The model, then this placement's own ground patches: the model's
         * uv are pixels of its drawing, placed on the page at (ox, oy). */
        const uint32_t first[2] = { m->firstVertex, p[i].extraFirst };
        const uint32_t total[2] = { m->vertexCount, p[i].extraCount };
        const float ox[2] = { (float)pm->ox, 0.0f }, oy[2] = { (float)pm->oy, 0.0f };

        if (inst->originX + p[i].x < x0 || inst->originX + p[i].x >= x1
         || inst->originY + p[i].y < y0 || inst->originY + p[i].y >= y1
         || !PlacementLive(inst, &p[i]))
            continue;
        /* On a lifted plateau the building stands on it: the lift of the
         * cell under its door, the bottom-left of its rectangle. */
        builder->lift = VoxelRelief_CellLift(inst, inst->originX + p[i].x,
                                             inst->originY + p[i].y + m->h - 1);
        builder->shift = VoxelRelief_CellShift(inst, inst->originX + p[i].x,
                                             inst->originY + p[i].y + m->h - 1);
        /* A static model is drawn from its own mesh: the chunk only notes
         * where (in its own frame, as VoxelBuilder_Tri would have put it). */
        /* (not one on a map's eased rows: only its vertices are drawn
         * longer with the ground, VoxelRelief_Stretch) */
        if (m->isStatic && builder->statics != NULL && cursor->part == 0
         && !(builder->stretchAdd > 0.0f && wz + builder->shift + (float)m->h > builder->stretchFrom))
        {
            if (cursor->vertex == 0 && builder->staticCount < builder->staticCapacity)
            {
                VoxelStaticDraw *s = &builder->statics[builder->staticCount++];

                s->pageModel = p[i].pageModel;
                s->lit = builder->lighting;
                s->x = wx - builder->originX;
                s->y = builder->lift + builder->base;
                s->z = VoxelBuilder_StretchZ(builder, wz + builder->shift) - builder->originZ;
                s->w = (float)m->w;
                s->d = (float)m->h;
                s->top = m->top;
            }
            else if (cursor->vertex == 0 && !sStaticsFullLogged)
            {
                sStaticsFullLogged = true;
                PORT_LOG("[VIDEO] VOXEL static models: a chunk holds more than %u\n",
                         builder->staticCapacity);
            }
            cursor->part = 1;
            cursor->vertex = 0;
        }
        for (; cursor->part < 2; ++cursor->part, cursor->vertex = 0)
        {
            unsigned r = cursor->part;
            const VoxelVertex *v = &sVertices[first[r]];

            /* The ground patches are ground: a quad a cell, lit like the
             * terrain they lie on, so a shadow falls across them too. The
             * model keeps the shading it was drawn with. */
            unsigned step = r == 1 ? 6u : 3u;

            for (; cursor->vertex + step - 1 < total[r]; cursor->vertex += step)
            {
                uint32_t k = cursor->vertex;
                VoxelVertex t[6];

                if (triangles < step / 3u)
                {
                    builder->lift = 0.0f;
                    builder->shift = 0.0f;
                    return false;
                }
                triangles -= step / 3u;

                for (unsigned j = 0; j < step; ++j)
                {
                    t[j] = v[k + j];
                    t[j].x += wx;
                    t[j].z += wz;
                    t[j].u = (ox[r] + t[j].u) * su;
                    t[j].v = 1.0f - (oy[r] + t[j].v) * sv;
                }
                /* a patch is written a, b, c, a, c, d */
                if (step == 6)
                    VoxelBuilder_Quad(builder, &t[0], &t[1], &t[2], &t[5]);
#if CTR_VOXEL_LIGHTING
                else if (builder->lighting)
                {
                    /* Lit by the same sun as the terrain, not by the side
                     * the drawing was made for. */
                    VoxelLighting_ModelTri(builder, &t[0], &t[1], &t[2], t[0].shade);
                }
#endif
                else
                    VoxelBuilder_Tri(builder, &t[0], &t[1], &t[2]);
            }
        }
        builder->lift = 0.0f;
        builder->shift = 0.0f;
    }
    return true;
}

void VoxelBuildings_EmitInstance(VoxelBuilder *builder, const VoxelMapInstance *inst,
                                 int x0, int y0, int x1, int y1)
{
    VoxelBuildingCursor cursor = { 0, 0, 0 };

    VoxelBuildings_EmitSome(builder, inst, x0, y0, x1, y1, &cursor, UINT32_MAX);
}
