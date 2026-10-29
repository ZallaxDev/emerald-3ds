/*
 * Reader for voxel/relief.bin (game data); see voxel_relief.h and the generator,
 * scripts/gen_voxel_relief.py.
 *
 * Layout (little endian):
 *   "VXL4", u16 layouts, u16 side (5)
 *   layouts x 14: u16 layout id, u16 cells, u16 width (bit 15: once a
 *                 level map's, no longer read; bit 14: wide, its
 *                 cells' heights are s16 pixels; bits 10-13: the south
 *                 seam's ease, 8 px a unit, see EaseAt), u16 height (bit 15:
 *                 drawn; bit 14: heights in units of 2 pixels; bits 10-13:
 *                 the belt's courses, see VoxelRelief_Belt), u32 offset,
 *                 s16 base (pixels: the level the whole map stands at)
 *   cells x (2 + 25): u8 x, u8 y, int8 heights[25] (row major, over the base);
 *   on a wide map x (2 + 50): u8 x, u8 y, s16 heights[25] - a mountain more
 *   than a signed byte of 2-pixel units tall (16 levels) over its base
 *
 * then the cut tiles (see VoxelRelief_Cut): u16 variants, u16 cells;
 *   variants x 36: u16 layout drawing the tileset, u16 metatile, u16 rows[16]
 *                  (bit set: background)
 *   cells x 14:    u16 layout, u8 x, u8 y, u16 variant, s16 foot (pixels),
 *                  u16 metatile of the ground behind it (0xFFFF: none),
 *                  u16 metatile its cliff walls are drawn with (0xFFFF: none),
 *                  u8 sides (bits 0-3: stretches of its west edge column, 4-7:
 *                  of its east one, that have rock for a wall to hang from),
 *                  u8 flags, `sides` bits 8-15 to VoxelRelief_Cut (1: all its
 *                  background is the plain ground behind it - written, no
 *                  longer read; 2: VOXEL_RELIEF_GOES_ON; 4:
 *                  VOXEL_RELIEF_BLOCK_WALLS; 8: VOXEL_RELIEF_THIN_DECK;
 *                  16: VOXEL_RELIEF_FACE_TOP; 32: VOXEL_RELIEF_GOES_ON_FACE; 64: VOXEL_RELIEF_GOES_ON_SOUTH)
 *   then, if the map's fixes fill its edges (VoxelRelief_EdgeFill), "EDGE",
 *   u16 stretches, u16 0; stretches x 18: u16 layout, u8 edge (1 west, 2
 *   east, 4 north, 8 south), u8 flags (2: `top` given; bits 4-7: metatile
 *   0-3 is water), s16 first, s16 last (the cells along the edge, -1 -1:
 *   all of it), s16 top (pixels over the map's base: where the fill's top
 *   stands), u16 metatiles[4] (the map's own ids: a 2x2 block tiled out as
 *   the game's border block is, VoxelRelief_EdgeFillAt)
 *   and last u32 the table's offset, "CUTS".
 *
 * A point's depth is its height in the world, its map's base and its own
 * ((u, h, v + h)), so the file does not carry it. The heights are decoded to
 * pixels at load: a drawn mountain can stand taller than a signed byte of
 * pixels.
 */
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* Host tests define PORT_LOG away rather than link the console's logger. */
#ifndef PORT_LOG
#include "port_log.h"
#endif

#include "voxel_relief.h"
#include "voxel_file.h"

#ifndef VOXEL_RELIEF_PATH
#define VOXEL_RELIEF_PATH "voxel/relief.bin"
#endif
#define GRID (VOXEL_RELIEF_SIDE * VOXEL_RELIEF_SIDE)
#define CELL_BYTES (2 + GRID)
#define WIDE_CELL_BYTES (2 + 2 * GRID)
#define ROW_BYTES 14

/*
 * Cells are searched, not indexed: every map with a ledge has a few dozen
 * lifted cells, and a dense index of each map's every cell cost more heap
 * than all of them. The generator writes them row by row, x within y.
 */
typedef struct
{
    uint16_t layoutId, width, height, count;
    int16_t base;          /* pixels */
    bool drawn;            /* the relief is all of this map's terrain */
    uint8_t belt;          /* courses the border stands, as drawn (0: as ever) */
    uint8_t ease;          /* the south seam's ease, 8 px a unit (EaseAt) */
    uint8_t easeRows;      /* the last rows it is spread over */
    int faceTile;          /* the cliff wall its cut cells hang most often, -1 */
    const uint8_t *cells;  /* into sBlob: x, y and the stored heights */
    uint8_t stride;        /* bytes a cell: CELL_BYTES, or WIDE_CELL_BYTES */
    int16_t *heights;      /* GRID per cell, pixels */
    /* A drawn map's surface where it stands, for the sun to be stopped by:
     * its height (pixels) every lattice step of world X and Z, not of the
     * drawing's rows - a point of the drawing lies as far south as it is
     * high, so the table runs past the map's south edge as many cells as its
     * highest point is tall (SURFACE_SPAN at least). */
    int8_t *surface;       /* in units of surfaceUnit pixels */
    uint8_t surfaceUnit;   /* 2, or 4 where 2 do not reach its top */
    uint16_t surfaceW, surfaceH;
    float top;             /* its highest point, tiles */
} ReliefLayout;

#define SURFACE_SPAN 6
/* A rise between two lattice points of a drawing's column this high (pixels)
 * is a wall, not a slope (BuildSurface): half a level. */
#define SURFACE_WALL 8

/*
 * The cells last found, by their place in an 8x8 tiling of the layout: a
 * ground cell asks for itself, its eight neighbours (the face terms), its depth,
 * lift and shift and its walls' neighbours, a dozen and more searches of the
 * same few cells, and a ray over a mountain asks again cell by cell. Exact: a
 * cell is known by its layout and place, and a miss only searches. Cleared
 * with the layouts it points at (VoxelRelief_Shutdown).
 */
#define FIND_MEMO_SIDE 8u
typedef struct
{
    const ReliefLayout *layout;
    int16_t x, y;
    int k;
} FindMemo;
static FindMemo sFindMemo[FIND_MEMO_SIDE * FIND_MEMO_SIDE];

static int Find(const ReliefLayout *l, const VoxelMapInstance *inst, int x, int y);

/* The rows a south seam's ease is spread over (VoxelRelief_Stretch): six a
 * level, at most half the map; voxel_relief_fixes.py ease_rows. */
static unsigned EaseRows(const ReliefLayout *l)
{
    unsigned rows = (6u * 8u * l->ease + 15u) / 16u;

    return rows < l->height / 2u ? rows : l->height / 2u;
}
static bool BuildSurface(ReliefLayout *l);

static uint8_t *sBlob;
static ReliefLayout *sLayouts;
static unsigned sLayoutCount;
/* The layouts by id (their entry + 1, 0 for none): every height, lift and
 * shadow-ray step asks which layout a map is, and a scan of them all was the
 * bulk of what a step on a mountain cost. */
static uint16_t *sLayoutIndex;
static unsigned sLayoutIndexSize;
static const uint8_t *sCutVariants, *sCutCells;
static unsigned sCutVariantCount, sCutCellCount;
static const uint8_t *sEdges;
static unsigned sEdgeCount;
#define EDGE_BYTES 18
#define CUT_VARIANT_BYTES 36
#define CUT_CELL_BYTES 14

/* The cut cells last looked up, tiled as sFindMemo: a relief cell asks about
 * itself and its neighbours several times a build (its walls, its ground,
 * the cells beside it), each a search of every map's cut cells. Exact, and
 * cleared with the table (VoxelRelief_Shutdown). */
typedef struct
{
    bool used;
    int layoutId;
    int16_t x, y;
    int record;            /* index into sCutCells, -1: not cut */
} CutMemo;
static CutMemo sCutMemo[FIND_MEMO_SIDE * FIND_MEMO_SIDE];

/* floorf as a conversion and one correction: the library call it replaces was
 * made at every step of every shadow ray over a mountain. */
static inline int FloorI(float v)
{
    int i = (int)v;

    return (float)i > v ? i - 1 : i;
}

static unsigned U16(const uint8_t *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static uint32_t U32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static void ForgetSeams(void);

bool VoxelRelief_Init(void)
{
    FILE *file;
    long size;

    VoxelRelief_Shutdown();
    file = VoxelFile_Open(VOXEL_RELIEF_PATH);
    if (file == NULL)
    {
        PORT_LOG("[VIDEO] VOXEL relief: %s absent, terrain stays level\n", VOXEL_RELIEF_PATH);
        return false;
    }
    fseek(file, 0, SEEK_END);
    size = ftell(file);
    fseek(file, 0, SEEK_SET);
    sBlob = size > 8 ? malloc((size_t)size) : NULL;
    if (sBlob == NULL || fread(sBlob, 1, (size_t)size, file) != (size_t)size
     || memcmp(sBlob, "VXL4", 4) != 0 || U16(sBlob + 6) != VOXEL_RELIEF_SIDE
     || memcmp(sBlob + size - 4, "CUTS", 4) != 0)
        goto fail;
    {
        uint32_t at = U32(sBlob + size - 8);

        if ((long)at + 4 > size - 8)
            goto fail;
        sCutVariantCount = U16(sBlob + at);
        sCutCellCount = U16(sBlob + at + 2);
        sCutVariants = sBlob + at + 4;
        sCutCells = sCutVariants + CUT_VARIANT_BYTES * sCutVariantCount;
        if ((long)(sCutCells - sBlob) + (long)(CUT_CELL_BYTES * sCutCellCount) > size - 8)
            goto fail;
        {
            const uint8_t *e = sCutCells + CUT_CELL_BYTES * sCutCellCount;

            if ((long)(e - sBlob) + 8 <= size - 8 && memcmp(e, "EDGE", 4) == 0)
            {
                sEdgeCount = U16(e + 4);
                sEdges = e + 8;
                if ((long)(sEdges - sBlob) + (long)(EDGE_BYTES * sEdgeCount) > size - 8)
                    goto fail;
            }
        }
    }
    sLayoutCount = U16(sBlob + 4);
    if (8 + (unsigned long)ROW_BYTES * sLayoutCount > (unsigned long)size)
        goto fail;
    sLayouts = calloc(sLayoutCount ? sLayoutCount : 1, sizeof(*sLayouts));
    if (sLayouts == NULL)
        goto fail;
    for (unsigned i = 0; i < sLayoutCount; ++i)
    {
        const uint8_t *row = sBlob + 8 + ROW_BYTES * i;
        ReliefLayout *l = &sLayouts[i];
        unsigned cells = U16(row + 2);
        uint32_t offset = U32(row + 8);

        l->layoutId = (uint16_t)U16(row);
        l->base = (int16_t)U16(row + 12);
        l->width = (uint16_t)(U16(row + 4) & 0x03FF);
        unsigned unit = (U16(row + 6) & 0x4000) ? 2 : 1;
        bool wide = (U16(row + 4) & 0x4000) != 0;

        l->height = (uint16_t)(U16(row + 6) & 0x03FF);
        l->drawn = (U16(row + 6) & 0x8000) != 0;
        l->belt = (uint8_t)((U16(row + 6) >> 10) & 0x0F);
        l->ease = (uint8_t)((U16(row + 4) >> 10) & 0x0F);
        l->easeRows = EaseRows(l);
        l->cells = sBlob + offset;
        l->count = (uint16_t)cells;
        l->stride = wide ? WIDE_CELL_BYTES : CELL_BYTES;
        if (offset > (uint32_t)size || (uint32_t)cells * l->stride > (uint32_t)size - offset)
            goto fail;
        for (unsigned k = 1; k < cells; ++k)
        {
            const uint8_t *a = l->cells + (k - 1) * l->stride, *c = a + l->stride;
            if (a[1] > c[1] || (a[1] == c[1] && a[0] >= c[0]))
                goto fail;  /* not in row order: the search would miss cells */
        }
        l->heights = malloc((size_t)(cells ? cells : 1) * GRID * sizeof(int16_t));
        if (l->heights == NULL)
            goto fail;
        for (unsigned k = 0; k < cells; ++k)
            for (unsigned g = 0; g < GRID; ++g)
                l->heights[k * GRID + g] = wide
                    ? (int16_t)U16(l->cells + k * WIDE_CELL_BYTES + 2 + 2 * g)
                    : (int16_t)((int8_t)l->cells[k * CELL_BYTES + 2 + g] * (int)unit);
    }
    fclose(file);
    /* each map's commonest cliff wall (VoxelRelief_FaceTile): its cut cells
     * are sorted by layout, so a run of them is one map's */
    for (unsigned i = 0; i < sLayoutCount; ++i)
        sLayouts[i].faceTile = -1;
    for (unsigned k = 0; k < sCutCellCount;)
    {
        unsigned layout = U16(sCutCells + CUT_CELL_BYTES * k), end = k;
        unsigned walls[8] = {0}, counts[8] = {0}, kinds = 0, best = 0;

        while (end < sCutCellCount && U16(sCutCells + CUT_CELL_BYTES * end) == layout)
        {
            unsigned wall = U16(sCutCells + CUT_CELL_BYTES * end + 10), j = 0;

            ++end;
            if (wall >= 0xFFFEu)
                continue;
            while (j < kinds && walls[j] != wall)
                ++j;
            if (j == kinds && kinds < 8)
                walls[kinds++] = wall;
            if (j < kinds)
                ++counts[j];
        }
        for (unsigned j = 1; j < kinds; ++j)
            if (counts[j] > counts[best])
                best = j;
        for (unsigned i = 0; kinds > 0 && i < sLayoutCount; ++i)
            if (sLayouts[i].layoutId == layout)
                sLayouts[i].faceTile = (int)walls[best];
        k = end;
    }
    for (unsigned i = 0; i < sLayoutCount; ++i)
        if (sLayouts[i].drawn && !BuildSurface(&sLayouts[i]))
        {
    PORT_LOG("[ERROR] VOXEL relief: no memory for a drawn map's surface\n");
            VoxelRelief_Shutdown();
            return false;
        }
    {
        unsigned most = 0;

        for (unsigned i = 0; i < sLayoutCount; ++i)
            if (sLayouts[i].layoutId > most)
                most = sLayouts[i].layoutId;
        sLayoutIndex = calloc((size_t)most + 1u, sizeof(*sLayoutIndex));
        if (sLayoutIndex != NULL)
        {
            sLayoutIndexSize = most + 1u;
            /* backwards: the first of two layouts of one id answers, as the scan did */
            for (unsigned i = sLayoutCount; i-- > 0;)
                sLayoutIndex[sLayouts[i].layoutId] = (uint16_t)(i + 1u);
        }
    }
    PORT_LOG("[VIDEO] VOXEL relief: %u layouts\n", sLayoutCount);
    return true;

fail:
    fclose(file);
    PORT_LOG("[ERROR] VOXEL relief: %s is truncated or malformed\n", VOXEL_RELIEF_PATH);
    VoxelRelief_Shutdown();
    return false;
}

void VoxelRelief_Shutdown(void)
{
    for (unsigned i = 0; sLayouts != NULL && i < sLayoutCount; ++i)
    {
        free(sLayouts[i].surface);
        free(sLayouts[i].heights);
    }
    memset(sFindMemo, 0, sizeof(sFindMemo));
    memset(sCutMemo, 0, sizeof(sCutMemo));
    ForgetSeams();
    free(sLayouts);
    free(sLayoutIndex);
    sLayoutIndex = NULL;
    sLayoutIndexSize = 0;
    free(sBlob);
    sLayouts = NULL;
    sBlob = NULL;
    sLayoutCount = 0;
    sCutVariants = sCutCells = NULL;
    sCutVariantCount = sCutCellCount = 0;
    sEdges = NULL;
    sEdgeCount = 0;
}

bool VoxelRelief_EdgeFill(unsigned layoutId, unsigned edge, int along, unsigned index, VoxelEdgeFill *fill)
{
    for (unsigned i = 0; i < sEdgeCount; ++i)
    {
        const uint8_t *e = sEdges + EDGE_BYTES * i;
        int first = (int16_t)U16(e + 4), last = (int16_t)U16(e + 6);

        if (U16(e) != layoutId || e[2] != edge)
            continue;
        if (first >= 0 && (along < first || along > last))
            continue;
        index &= 3u;
        fill->metatile = (int)U16(e + 10 + 2 * index);
        fill->water = (e[3] >> (4u + index) & 1u) != 0;
        fill->hasTop = (e[3] & 2) != 0;
        fill->top = (int16_t)U16(e + 8);
        return true;
    }
    return false;
}

bool VoxelRelief_EdgeFillAt(const VoxelMapInstance *ring, int worldX, int worldY, VoxelEdgeFill *fill)
{
    int lx, ly, cx, cy;
    unsigned index;

    if (sEdgeCount == 0 || ring == NULL || ring->indoor || ring->width <= 0 || ring->height <= 0)
        return false;
    lx = worldX - ring->originX;
    ly = worldY - ring->originY;
    cx = lx < 0 ? 0 : lx >= ring->width ? ring->width - 1 : lx;
    cy = ly < 0 ? 0 : ly >= ring->height ? ring->height - 1 : ly;
    /* which of the block's four: phased from the map's corner as the game's
     * border block is (fieldmap.c, MAP_OFFSET 7 past it: odd) */
    index = (unsigned)(lx & 1) | ((unsigned)(ly & 1) << 1);
    /* a corner past two edges: the west or east edge's first, then the
     * north or south one's */
    if (lx < 0 && VoxelRelief_EdgeFill((unsigned)ring->layoutId, 1, cy, index, fill))
        return true;
    if (lx >= ring->width && VoxelRelief_EdgeFill((unsigned)ring->layoutId, 2, cy, index, fill))
        return true;
    if (ly < 0 && VoxelRelief_EdgeFill((unsigned)ring->layoutId, 4, cx, index, fill))
        return true;
    if (ly >= ring->height && VoxelRelief_EdgeFill((unsigned)ring->layoutId, 8, cx, index, fill))
        return true;
    return false;
}

bool VoxelRelief_EdgeFills(unsigned layoutId, unsigned i, int *metatile)
{
    for (unsigned k = 0; k < sEdgeCount; ++k)
    {
        const uint8_t *e = sEdges + EDGE_BYTES * k;

        if (U16(e) != layoutId)
            continue;
        if (i < 4u)
        {
            *metatile = (int)U16(e + 10 + 2 * i);
            return true;
        }
        i -= 4u;
    }
    return false;
}

unsigned VoxelRelief_CutCount(void)
{
    return sCutVariantCount;
}

bool VoxelRelief_CutVariant(unsigned i, unsigned *layout, unsigned *metatile,
                            const uint8_t **rows)
{
    const uint8_t *v;

    if (i >= sCutVariantCount)
        return false;
    v = sCutVariants + CUT_VARIANT_BYTES * i;
    *layout = U16(v);
    *metatile = U16(v + 2);
    *rows = v + 4;
    return true;
}

static const int8_t kCutDir[8][2] = { { 0, -1 }, { 0, 1 }, { -1, 0 }, { 1, 0 },
                                      { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } };

/* The rock's eight sides (its hull: a*i + b*j <= c over its pixels), and
 * whether it is that hull: a polygon. */
static bool CutHull(const uint8_t *rows, int c[8])
{
    int rock = 0;

    for (int k = 0; k < 8; ++k)
        c[k] = INT16_MIN;
    for (int j = 0; j < 16; ++j)
    {
        unsigned bits = (unsigned)rows[2 * j] | ((unsigned)rows[2 * j + 1] << 8);

        for (int i = 0; i < 16; ++i)
            if (!((bits >> i) & 1u))
            {
                ++rock;
                for (int k = 0; k < 8; ++k)
                    if (kCutDir[k][0] * i + kCutDir[k][1] * j > c[k])
                        c[k] = kCutDir[k][0] * i + kCutDir[k][1] * j;
            }
    }
    if (rock == 0 || rock == 256)
        return false;
    for (int j = 0; j < 16; ++j)
    {
        unsigned bits = (unsigned)rows[2 * j] | ((unsigned)rows[2 * j + 1] << 8);

        for (int i = 0; i < 16; ++i)
        {
            bool in = true;

            for (int k = 0; k < 8 && in; ++k)
                in = kCutDir[k][0] * i + kCutDir[k][1] * j <= c[k];
            if (in == (((bits >> i) & 1u) != 0))
                return false;
        }
    }
    return true;
}

void VoxelRelief_CutDiagonals(const uint8_t *rows, VoxelCutDiagonals *out)
{
    int c[8];

    out->count = 0;
    if (rows == NULL || !CutHull(rows, c))
        return;
    for (int k = 4; k < 8; ++k)
    {
        /* its value at the corner of the rock's rectangle (its horizontal
         * and vertical sides): a side that clips nothing more is none */
        int a = kCutDir[k][0], b = kCutDir[k][1];
        int corner = a * (a > 0 ? c[3] : -c[2]) + b * (b > 0 ? c[1] : -c[0]);

        if (c[k] >= corner)
            continue;
        out->a[out->count] = (int8_t)a;
        out->b[out->count] = (int8_t)b;
        /* through the outer corners of the rock's pixels along it */
        out->c[out->count] = (int16_t)(c[k] + (a > 0) + (b > 0));
        ++out->count;
    }
}

/* Does the polygon of sides `c` (through the outer corners of the rock's
 * pixels) take in part of pixel (i, j)? */
static bool CutTakes(const int c[8], int i, int j)
{
    for (int k = 0; k < 8; ++k)
    {
        int a = kCutDir[k][0], b = kCutDir[k][1];
        int least = a * (a > 0 ? i : i + 1) + b * (b > 0 ? j : j + 1);

        if (least >= c[k] + (a > 0) + (b > 0))
            return false;
    }
    return true;
}

void VoxelRelief_CutClear(const uint8_t *rows, uint16_t clear[16])
{
    VoxelCutDiagonals d;
    int c[8];

    for (int j = 0; j < 16; ++j)
        clear[j] = (uint16_t)((unsigned)rows[2 * j] | ((unsigned)rows[2 * j + 1] << 8));
    VoxelRelief_CutDiagonals(rows, &d);
    if (d.count == 0)
        return;
    CutHull(rows, c);
    /* a horizontal or vertical side a pixel long, where a 45-degree side
     * ends at the tile's edge, is no side: the staircase's last step (a
     * half of the tile cut corner to corner has its rock a pixel short of
     * the far corner). Cut there, the tile's corner pixel was left clear,
     * a notch at the end of the diagonal the relief itself (cut along its
     * 45-degree sides only) does not have */
    for (int k = 0; k < 4; ++k)
    {
        int gained = 0;

        if (c[k] >= (kCutDir[k][0] + kCutDir[k][1] > 0 ? 15 : 0))
            continue;
        ++c[k];
        for (int j = 0; j < 16 && gained <= 1; ++j)
            for (int i = 0; i < 16; ++i)
                if (((clear[j] >> i) & 1u) && kCutDir[k][0] * i + kCutDir[k][1] * j == c[k]
                 && CutTakes(c, i, j))
                    ++gained;
        if (gained != 1)
            --c[k];
    }
    for (int j = 0; j < 16; ++j)
        for (int i = 0; i < 16; ++i)
        {
            /* a background pixel the polygon (its sides through the outer
             * corners of the rock's pixels) takes in part of: drawn */
            if (((clear[j] >> i) & 1u) && CutTakes(c, i, j))
                clear[j] &= (uint16_t)~(1u << i);
        }
}

int VoxelRelief_CutFringe(const uint8_t *rows, const uint16_t clear[16], int x, int y)
{
    static const int8_t kAround[8][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1},
                                         {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
    unsigned row = (unsigned)rows[2 * y] | ((unsigned)rows[2 * y + 1] << 8);

    if (!((row >> x) & 1u) || ((clear[y] >> x) & 1u))
        return -1;
    for (int k = 0; k < 8; ++k)
    {
        int nx = x + kAround[k][0], ny = y + kAround[k][1];
        unsigned nrow;

        if (nx < 0 || nx > 15 || ny < 0 || ny > 15)
            continue;
        nrow = (unsigned)rows[2 * ny] | ((unsigned)rows[2 * ny + 1] << 8);
        if (!((nrow >> nx) & 1u))
            return ny * 16 + nx;
    }
    return -1;
}

int VoxelRelief_Cut(const VoxelMapInstance *inst, int x, int y, float *foot, int *ground,
                    int *wall, unsigned *sides)
{
    unsigned lo = 0, hi = sCutCellCount, key;
    const uint8_t *c;
    CutMemo *memo;

    if (inst == NULL || sCutCells == NULL)
        return -1;
    x -= inst->originX;
    y -= inst->originY;
    if (x < 0 || y < 0 || x > 255 || y > 255)
        return -1;
    memo = &sCutMemo[((unsigned)y & (FIND_MEMO_SIDE - 1u)) * FIND_MEMO_SIDE
                     + ((unsigned)x & (FIND_MEMO_SIDE - 1u))];
    if (!memo->used || memo->layoutId != inst->layoutId || memo->x != x || memo->y != y)
    {
        key = ((unsigned)inst->layoutId << 16) | ((unsigned)x << 8) | (unsigned)y;
        memo->used = true;
        memo->layoutId = inst->layoutId;
        memo->x = (int16_t)x;
        memo->y = (int16_t)y;
        memo->record = -1;
        while (lo < hi)
        {
            unsigned mid = (lo + hi) / 2;
            unsigned at;

            c = sCutCells + CUT_CELL_BYTES * mid;
            at = (U16(c) << 16) | ((unsigned)c[2] << 8) | c[3];
            if (at == key)
            {
                memo->record = (int)mid;
                break;
            }
            if (at < key)
                lo = mid + 1;
            else
                hi = mid;
        }
    }
    if (memo->record < 0)
        return -1;
    c = sCutCells + CUT_CELL_BYTES * (unsigned)memo->record;
    *foot = (int16_t)U16(c + 6) / 16.0f;
    *ground = U16(c + 8) == 0xFFFFu ? -1 : (int)U16(c + 8);
    *wall = U16(c + 10) == 0xFFFFu ? -1 : (int)U16(c + 10);
    *sides = (unsigned)c[12] | ((unsigned)c[13] << 8);
    return (int)U16(c + 4);
}

void VoxelRelief_ForEachCut(const VoxelMapInstance *inst, VoxelReliefCutVisit visit, void *ctx)
{
    unsigned lo = 0, hi = sCutCellCount, first;

    if (inst == NULL || sCutCells == NULL)
        return;
    first = (unsigned)inst->layoutId << 16;
    while (lo < hi)
    {
        unsigned mid = (lo + hi) / 2;
        const uint8_t *c = sCutCells + CUT_CELL_BYTES * mid;

        if (((unsigned)U16(c) << 16) < first)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (; lo < sCutCellCount; ++lo)
    {
        const uint8_t *c = sCutCells + CUT_CELL_BYTES * lo;

        if (U16(c) != (unsigned)inst->layoutId)
            break;
        visit((int)c[2], (int)c[3],
              U16(c + 8) == 0xFFFFu ? -1 : (int)U16(c + 8),
              U16(c + 10) == 0xFFFFu ? -1 : (int)U16(c + 10), ctx);
    }
}

static const ReliefLayout *LayoutOf(const VoxelMapInstance *inst)
{
    if (inst == NULL)
        return NULL;
    if (sLayoutIndex != NULL)
    {
        unsigned k = (unsigned)inst->layoutId < sLayoutIndexSize ? sLayoutIndex[inst->layoutId] : 0u;

        return k != 0 ? &sLayouts[k - 1u] : NULL;
    }
    for (unsigned i = 0; i < sLayoutCount; ++i)
        if (sLayouts[i].layoutId == (unsigned)inst->layoutId)
            return &sLayouts[i];
    return NULL;
}

float VoxelRelief_Base(const VoxelMapInstance *inst)
{
    const ReliefLayout *l = LayoutOf(inst);

    if (inst == NULL)
        return 0.0f;
    return ((l != NULL ? l->base : 0) + inst->lift) / 16.0f;
}

/*
 * Hoenn does not close. Route 119 climbs six levels to Fortree's plateau, and
 * nothing on Routes 120 and 121 comes down again: Lilycove's sea stands where
 * Fortree does, the open sea round it (Routes 122 and 124) where Slateport's
 * does. Whatever level the world gives each map, some seam round that loop
 * asks for a step, and a step moved off one seam turns up at the next.
 *
 * No view sees the whole loop, though: only the maps round the player. So a
 * map is not placed at one level for good - each is placed from the map it
 * was reached by, as high as their seam asks, starting from the current map
 * (voxel_world.c AddConnections). The two sides of a seam always meet, and
 * what the loop adds up to is only ever where the view does not reach. A map
 * keeps the level it was placed at across a crossing (the new current map
 * keeps its lift), so nothing moves when the player walks over a seam.
 *
 * The step a seam asks: every cell along it compares the edge both maps
 * share, base and relief, the mean of its lattice points; the commonest
 * difference, where three cells in four agree on it (a seam whose maps only
 * differ in places - an island, a ledge - has none), in 4-pixel steps (a
 * lattice step of the sun's surface table). The pixels to add to `b`'s lift
 * for it to meet `a` as `a` stands; 0 for maps that do not touch.
 */
#define SEAM_CELLS_MAX 256

/*
 * Kept, as the relief it is read from never changes: the world is placed
 * again every frame (voxel_world.c BuildInstances), and its seams measured
 * with it took 2-3 ms of every frame round Route 110's eleven maps on an Old
 * 3DS, before the GPU could start (port.log, 2026-10-26: "VOXEL world parts
 * ... instances 2.92"). A seam is the two layouts, their sizes and where one
 * stands from the other.
 */
#define SEAM_MEMO 256u

static struct
{
    bool used;
    uint16_t layoutA, layoutB;
    int16_t dx, dy, widthA, heightA, widthB, heightB;
    int step;
} sSeamMemo[SEAM_MEMO];

static int SeamStepOf(const VoxelMapInstance *a, const VoxelMapInstance *b);

static void ForgetSeams(void)
{
    memset(sSeamMemo, 0, sizeof(sSeamMemo));
}

int VoxelRelief_SeamStep(const VoxelMapInstance *a, const VoxelMapInstance *b)
{
    int dx, dy;
    unsigned key;

    if (a == NULL || b == NULL)
        return 0;
    dx = b->originX - a->originX;
    dy = b->originY - a->originY;
    key = ((unsigned)a->layoutId * 73856093u ^ (unsigned)b->layoutId * 19349663u
         ^ (unsigned)dx * 83492791u ^ (unsigned)dy * 2654435761u) % SEAM_MEMO;
    if (!sSeamMemo[key].used || sSeamMemo[key].layoutA != a->layoutId
     || sSeamMemo[key].layoutB != b->layoutId || sSeamMemo[key].dx != dx || sSeamMemo[key].dy != dy
     || sSeamMemo[key].widthA != a->width || sSeamMemo[key].heightA != a->height
     || sSeamMemo[key].widthB != b->width || sSeamMemo[key].heightB != b->height)
    {
        sSeamMemo[key].used = true;
        sSeamMemo[key].layoutA = (uint16_t)a->layoutId;
        sSeamMemo[key].layoutB = (uint16_t)b->layoutId;
        sSeamMemo[key].dx = (int16_t)dx;
        sSeamMemo[key].dy = (int16_t)dy;
        sSeamMemo[key].widthA = (int16_t)a->width;
        sSeamMemo[key].heightA = (int16_t)a->height;
        sSeamMemo[key].widthB = (int16_t)b->width;
        sSeamMemo[key].heightB = (int16_t)b->height;
        sSeamMemo[key].step = SeamStepOf(a, b);
    }
    return sSeamMemo[key].step;
}

static int SeamStepOf(const VoxelMapInstance *a, const VoxelMapInstance *b)
{
    const ReliefLayout *la = LayoutOf(a), *lb = LayoutOf(b);
    const unsigned last = VOXEL_RELIEF_SIDE - 1;
    int diff[SEAM_CELLS_MAX];
    int from, to, ax, ay, bx, by, dx, dy;
    unsigned ai, aj, bi, bj, di, dj, count = 0, best = 0, n = 0;
    int step = 0;

    if (a == NULL || b == NULL || (la == NULL && lb == NULL))
        return 0;
    if (a->originX + a->width == b->originX || b->originX + b->width == a->originX)
    {
        bool east = a->originX + a->width == b->originX;

        from = a->originY > b->originY ? a->originY : b->originY;
        to = a->originY + a->height < b->originY + b->height ? a->originY + a->height
                                                             : b->originY + b->height;
        ax = east ? a->originX + a->width - 1 : a->originX;
        bx = east ? b->originX : b->originX + b->width - 1;
        ay = by = from;
        dx = 0, dy = 1;
        ai = east ? last : 0, bi = east ? 0 : last, aj = bj = 0, di = 0, dj = 1;
    }
    else if (a->originY + a->height == b->originY || b->originY + b->height == a->originY)
    {
        bool south = a->originY + a->height == b->originY;

        from = a->originX > b->originX ? a->originX : b->originX;
        to = a->originX + a->width < b->originX + b->width ? a->originX + a->width
                                                           : b->originX + b->width;
        ay = south ? a->originY + a->height - 1 : a->originY;
        by = south ? b->originY : b->originY + b->height - 1;
        ax = bx = from;
        dx = 1, dy = 0;
        aj = south ? last : 0, bj = south ? 0 : last, ai = bi = 0, di = 1, dj = 0;
    }
    else
        return 0;
    for (int c = from; c < to && n < SEAM_CELLS_MAX; ++c, ++n)
    {
        const int16_t *ga = VoxelRelief_Cell(a, ax + dx * (c - from), ay + dy * (c - from));
        const int16_t *gb = VoxelRelief_Cell(b, bx + dx * (c - from), by + dy * (c - from));
        int sum = 0;

        for (unsigned p = 0; p < VOXEL_RELIEF_SIDE; ++p)
        {
            unsigned ka = (aj + dj * p) * VOXEL_RELIEF_SIDE + ai + di * p;
            unsigned kb = (bj + dj * p) * VOXEL_RELIEF_SIDE + bi + di * p;

            sum += (la != NULL ? la->base : 0) + (ga != NULL ? ga[ka] : 0)
                 - (lb != NULL ? lb->base : 0) - (gb != NULL ? gb[kb] : 0);
        }
        /* to the nearest 4 pixels */
        diff[n] = (int)floorf(sum / (4.0f * VOXEL_RELIEF_SIDE) + 0.5f) * 4;
    }
    for (unsigned i = 0; i < n; ++i)
    {
        count = 0;
        for (unsigned j = 0; j < n; ++j)
            count += diff[j] == diff[i];
        if (count > best)
        {
            best = count;
            step = diff[i];
        }
    }
    return n != 0 && 4u * best >= 3u * n ? step : 0;
}

const int16_t *VoxelRelief_Cell(const VoxelMapInstance *inst, int x, int y)
{
    const ReliefLayout *l = LayoutOf(inst);
    int k = Find(l, inst, x, y);
    return k >= 0 ? l->heights + (unsigned)k * GRID : NULL;
}

const int16_t *VoxelRelief_Depth(const VoxelMapInstance *inst, int x, int y)
{
    return VoxelRelief_Cell(inst, x, y);
}

/*
 * A point once stood as far south as it was high over its own map's base, so
 * a map whose south edge was ground under its base met the map south of it at
 * one height but rows short of it: Route 111's, two levels under the plateau
 * it stands on, two rows north of Mauville's. A point now stands as far south
 * as it is high in the world (the base counts), and maps meeting at one height
 * meet in the same row: the generator writes no ease. It wrote those rows,
 * the ease (voxel_relief_fixes.py seam_eases), and the map's last rows are
 * drawn that much longer in the world - everything on them, ground, rock,
 * trees and walkers alike - so its edge meets the next map's with nothing
 * between them and no step to walk over.
 */
bool VoxelRelief_Stretch(const VoxelMapInstance *inst, float *from, float *to, float *add)
{
    const ReliefLayout *l = LayoutOf(inst);
    float a;

    if (l == NULL || l->ease == 0 || l->easeRows == 0)
        return false;
    a = 8.0f * l->ease / 16.0f;
    if (to != NULL)
        *to = (float)(inst->originY + l->height) - a;
    if (from != NULL)
        *from = (float)(inst->originY + l->height) - a - (float)l->easeRows;
    if (add != NULL)
        *add = a;
    return true;
}

float VoxelRelief_StretchZ(const VoxelMapInstance *inst, float z)
{
    float from, to, add;

    if (!VoxelRelief_Stretch(inst, &from, &to, &add) || z <= from)
        return z;
    if (z >= to)
        return z + add;
    return from + (z - from) * (to - from + add) / (to - from);
}

static int Find(const ReliefLayout *l, const VoxelMapInstance *inst, int x, int y)
{
    unsigned lo = 0, hi, key;
    FindMemo *memo;

    if (l == NULL)
        return -1;
    x -= inst->originX;
    y -= inst->originY;
    if (x < 0 || y < 0 || x >= l->width || y >= l->height)
        return -1;
    memo = &sFindMemo[((unsigned)y & (FIND_MEMO_SIDE - 1u)) * FIND_MEMO_SIDE
                      + ((unsigned)x & (FIND_MEMO_SIDE - 1u))];
    if (memo->layout == l && memo->x == x && memo->y == y)
        return memo->k;
    key = ((unsigned)y << 8) | (unsigned)x;
    hi = l->count;
    memo->layout = l;
    memo->x = (int16_t)x;
    memo->y = (int16_t)y;
    memo->k = -1;
    while (lo < hi)
    {
        unsigned mid = (lo + hi) / 2;
        const uint8_t *c = l->cells + mid * l->stride;
        unsigned at = ((unsigned)c[1] << 8) | c[0];

        if (at == key)
        {
            memo->k = (int)mid;
            return (int)mid;
        }
        if (at < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return -1;
}

bool VoxelRelief_IsDrawn(const VoxelMapInstance *inst)
{
    const ReliefLayout *l = LayoutOf(inst);
    return l != NULL && l->drawn;
}

int VoxelRelief_FaceTile(const VoxelMapInstance *inst)
{
    const ReliefLayout *l = LayoutOf(inst);
    return l != NULL ? l->faceTile : -1;
}

unsigned VoxelRelief_Belt(const VoxelMapInstance *inst)
{
    const ReliefLayout *l = LayoutOf(inst);
    return l != NULL ? l->belt : 0u;
}

bool VoxelRelief_IsSlope(const int16_t *grid)
{
    for (unsigned i = 1; grid != NULL && i < VOXEL_RELIEF_SIDE * VOXEL_RELIEF_SIDE; ++i)
        if (grid[i] != grid[0])
            return true;
    return false;
}

float VoxelRelief_CellLift(const VoxelMapInstance *inst, int x, int y)
{
    const int16_t *g = VoxelRelief_Cell(inst, x, y);
    return g ? g[2 * VOXEL_RELIEF_SIDE + 2] / 16.0f : 0.0f;
}

float VoxelRelief_CellShift(const VoxelMapInstance *inst, int x, int y)
{
    const int16_t *g = VoxelRelief_Depth(inst, x, y);
    return g ? g[2 * VOXEL_RELIEF_SIDE + 2] / 16.0f : 0.0f;
}

static float Sample(const int16_t *g, float worldX, float worldZ);

float VoxelRelief_LiftAt(float worldX, float worldZ)
{
    int x = FloorI(worldX), y = FloorI(worldZ);
    const VoxelMapInstance *inst = VoxelWorld_GetInstanceAt(x, y);

    return VoxelRelief_Base(inst) + Sample(VoxelRelief_Cell(inst, x, y), worldX, worldZ);
}

float VoxelRelief_ShiftAt(float worldX, float worldZ)
{
    int x = FloorI(worldX), y = FloorI(worldZ);
    const VoxelMapInstance *inst = VoxelWorld_GetInstanceAt(x, y);
    float shift = Sample(VoxelRelief_Depth(inst, x, y), worldX, worldZ);

    /* where it is drawn: on a map's eased rows, as its ground is, and as far
     * south again as its map's base is high (VoxelBuilder_StretchZ) */
    return VoxelRelief_StretchZ(inst, worldZ + shift) + VoxelRelief_Base(inst) - worldZ;
}

static float Sample(const int16_t *g, float worldX, float worldZ)
{
    int x = FloorI(worldX), y = FloorI(worldZ);
    float fx, fy, a, b;
    int i, j;

    if (g == NULL)
        return 0.0f;
    fx = (worldX - x) * (VOXEL_RELIEF_SIDE - 1);
    fy = (worldZ - y) * (VOXEL_RELIEF_SIDE - 1);
    i = (int)fx;
    j = (int)fy;
    if (i > VOXEL_RELIEF_SIDE - 2) i = VOXEL_RELIEF_SIDE - 2;
    if (j > VOXEL_RELIEF_SIDE - 2) j = VOXEL_RELIEF_SIDE - 2;
    fx -= i;
    fy -= j;
    a = g[j * VOXEL_RELIEF_SIDE + i] * (1 - fx) + g[j * VOXEL_RELIEF_SIDE + i + 1] * fx;
    b = g[(j + 1) * VOXEL_RELIEF_SIDE + i] * (1 - fx) + g[(j + 1) * VOXEL_RELIEF_SIDE + i + 1] * fx;
    return (a * (1 - fy) + b * fy) / 16.0f;
}

/* A lattice point of a layout's drawing: its height over the base and its
 * depth, pixels - as far south as it stands high in the world. */
static void Point(const ReliefLayout *l, unsigned i, unsigned j, int *h, int *d)
{
    const unsigned n = VOXEL_RELIEF_SIDE - 1;
    unsigned x = i / n, y = j / n, a, b;
    int k;
    VoxelMapInstance inst;

    if (x >= l->width) x = l->width - 1u;
    if (y >= l->height) y = l->height - 1u;
    a = i - x * n;
    b = j - y * n;
    memset(&inst, 0, sizeof(inst));
    inst.layoutId = l->layoutId;
    k = Find(l, &inst, (int)x, (int)y);
    *h = k >= 0 ? l->heights[(unsigned)k * GRID + b * VOXEL_RELIEF_SIDE + a] : 0;
    *d = l->base + *h;
}

/* Each column of the drawing, walked down its rows, runs south through the
 * world as v + d: the height at every world lattice row it passes is read off
 * between the two points either side of it (the drawing never folds over). */
static bool BuildSurface(ReliefLayout *l)
{
    const unsigned n = VOXEL_RELIEF_SIDE - 1, step = 16 / n;
    unsigned w = l->width * n + 1u, rows, span = SURFACE_SPAN;
    int top = 0, most = 0, deepest = l->base;

    /* The highest point stands as many cells south of its row as it is tall
     * in the world, and a byte of 2-pixel units holds 16 levels of it. */
    for (unsigned g = 0; g < (unsigned)l->count * GRID; ++g)
    {
        if (abs(l->heights[g]) > most)
            most = abs(l->heights[g]);
        if (l->base + l->heights[g] > deepest)
            deepest = l->base + l->heights[g];
    }
    if (deepest > 0 && (unsigned)(deepest + 15) / 16u + 1u > span)
        span = (unsigned)(deepest + 15) / 16u + 1u;
    l->surfaceUnit = most > 254 ? 4 : 2;
    rows = (l->height + span) * n + 1u;
    l->surface = calloc((size_t)w * rows, 1);
    if (l->surface == NULL)
        return false;
    l->surfaceW = (uint16_t)w;
    l->surfaceH = (uint16_t)rows;
    for (unsigned i = 0; i < w; ++i)
    {
        int h0, d0;
        unsigned k = 0;

        Point(l, i, 0, &h0, &d0);
        for (unsigned j = 1; j <= l->height * n; ++j)
        {
            int h1, d1;
            float z0 = (float)((j - 1) * step) + d0, z1;

            Point(l, i, j, &h1, &d1);
            z1 = (float)(j * step) + d1;
            for (; k < rows && (float)(k * step) <= z1; ++k)
            {
                float t = z1 > z0 ? ((float)(k * step) - z0) / (z1 - z0) : 1.0f;
                int v;

                if (t < 0.0f) t = 0.0f;
                /* A wall rising to the next point: the ground below runs on
                 * to its foot, hidden behind it, and the wall stands
                 * straight up there, as the geometry has it. Read between
                 * the two points it was a slope up through that hidden
                 * ground, which shaded the strip north of every block - a
                 * square of shadow cut straight along the block's north
                 * edge, seen from a camera steeper than the drawing. The
                 * row before its foot already has its top: the surface
                 * rises across that row's step, never under the block's
                 * edge, which would let the sun through its top's lip. */
                if (h1 - h0 >= SURFACE_WALL)
                    t = (float)((k + 1) * step) > z1 ? 1.0f : 0.0f;
                v = (int)(h0 + (h1 - h0) * t + 0.5f);
                {
                    int u = l->surfaceUnit;
                    l->surface[k * w + i] = (int8_t)(v >= 0 ? (v + u / 2) / u : -((u / 2 - v) / u));
                }
                if (v > top) top = v;
            }
            h0 = h1;
            d0 = d1;
        }
    }
    l->top = top / 16.0f;
    return true;
}

static float SurfacePoint(const ReliefLayout *l, int i, int k)
{
    if (i < 0 || k < 0 || i >= l->surfaceW || k >= l->surfaceH)
        return 0.0f;
    return (float)l->surfaceUnit * l->surface[k * l->surfaceW + i];
}

float VoxelRelief_SurfaceAt(float worldX, float worldZ)
{
    const int n = VOXEL_RELIEF_SIDE - 1;
    int x = FloorI(worldX), z = FloorI(worldZ);
    const VoxelMapInstance *inst = VoxelWorld_GetInstanceAt(x, z);
    const ReliefLayout *l = LayoutOf(inst);
    float fx, fz;
    int i, k;

    if (l == NULL || l->surface == NULL)
        return 0.0f;
    fx = (worldX - inst->originX) * n;
    /* the table is of the drawing on its base: a lift moves it south too */
    fz = (worldZ - inst->originY) * n - inst->lift / 4.0f;
    i = FloorI(fx);
    k = FloorI(fz);
    fx -= i;
    fz -= k;
    return ((SurfacePoint(l, i, k) * (1 - fx) + SurfacePoint(l, i + 1, k) * fx) * (1 - fz)
          + (SurfacePoint(l, i, k + 1) * (1 - fx) + SurfacePoint(l, i + 1, k + 1) * fx) * fz)
           / 16.0f;
}

float VoxelRelief_SurfaceTop(const VoxelMapInstance *inst, int x, int z, float *bottom)
{
    const ReliefLayout *l = LayoutOf(inst);
    const int n = VOXEL_RELIEF_SIDE - 1;
    float top = 0.0f, low = 1000.0f;

    *bottom = 0.0f;
    if (l == NULL || l->surface == NULL)
        return 0.0f;
    x -= inst->originX;
    z -= inst->originY;
    /* (a lift is in 4-pixel steps, one of the table's rows each) */
    for (int k = z * n - inst->lift / 4; k <= z * n + n - inst->lift / 4; ++k)
        for (int i = x * n; i <= x * n + n; ++i)
        {
            float v = SurfacePoint(l, i, k);

            if (v > top)
                top = v;
            if (v < low)
                low = v;
        }
    *bottom = low / 16.0f;
    return top / 16.0f;
}

float VoxelRelief_DrawnTop(const VoxelMapInstance *inst)
{
    const ReliefLayout *l = LayoutOf(inst);
    return l != NULL && l->surface != NULL ? l->top : 0.0f;
}
