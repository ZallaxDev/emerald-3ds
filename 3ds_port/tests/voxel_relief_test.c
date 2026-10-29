/* The shipped relief.bin, read by the runtime: a ledge's lip on Route 101.
 *
 * argv[1] is Route 101's layout id. Cell (3, 7) is one of the south ledge's
 * middle pieces (metatile 0x87): its lip's top edge, lattice row 2 (pixel row
 * 8), stands LIP (6) pixels up, its foot on the cell's south edge and the
 * ground behind it at its north edge stay on the ground; the walkable cell
 * north of it is not lifted at all.
 *
 * argv[2] is Route 116's, a map read off its drawing: cell (60, 2) is on the
 * mountain's top terrace, level, more than three levels up, and as far south
 * as it is high, like every point of the drawing.
 *
 * argv[3] is Rusturf Tunnel's, a cave carved out of the rock by hand
 * (relief_fixes.json): its belt stands three courses, as high as the rock
 * at its edges (cell (0, 0)); the corridor (5, 4) is a level up from the
 * west room's floor (4, 8), which is the base and not lifted.
 *
 * argv[4] is Route 108's, the open sea that takes the level the world gives
 * up between Route 107 and Slateport (gen_voxel_relief.SEA_RAMPS): a level
 * map, its sea a level down at its west edge and on its base at its east one,
 * lifted straight up - no point of it moved south.
 *
 * argv[5], argv[6] and argv[7] are Lilycove's, Route 124's and Route 126's.
 * Lilycove's sea stands where Fortree does, six levels over the sea of Route
 * 124 east of it: the seam asks for Route 124 lifted 96 pixels to meet it
 * (VoxelRelief_SeamStep), and Lilycove 96 down to meet Route 124; Route 124
 * and Route 126 meet as they are. A lifted map stands that much higher.
 *
 * Then every cut cell of the file, asked for through VoxelRelief_Cut's memo
 * in an order that hits and evicts it, against the table read here. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "voxel_relief.h"

static const VoxelMapInstance *sOnly;

static unsigned Le16(const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }

static int CompareKey(const void *a, const void *b)
{
    unsigned x = *(const unsigned *)a, y = *(const unsigned *)b;
    return x < y ? -1 : x > y;
}

/* Every cut cell of the file, and cells beside them sharing their memo
 * slot, as VoxelRelief_Cut gives them, against the table itself. */
static void CheckCuts(void)
{
    FILE *f = fopen(VOXEL_RELIEF_PATH, "rb");
    long size;
    unsigned char *blob;
    unsigned at, nv, nc, *keys, checked = 0;
    const unsigned char *cells;

    assert(f != NULL);
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    rewind(f);
    assert(size > 8);
    blob = malloc((size_t)size);
    assert(blob != NULL);
    if (fread(blob, 1, (size_t)size, f) != (size_t)size)
        assert(!"relief.bin truncated");
    fclose(f);
    assert(memcmp(blob + size - 4, "CUTS", 4) == 0);
    at = Le16(blob + size - 8) | (Le16(blob + size - 6) << 16);
    nv = Le16(blob + at);
    nc = Le16(blob + at + 2);
    cells = blob + at + 4 + 36u * nv;
    assert(nc > 0);
    keys = malloc(nc * sizeof(*keys));
    assert(keys != NULL);
    for (unsigned k = 0; k < nc; ++k)
        keys[k] = (Le16(cells + 14u * k) << 16) | ((unsigned)cells[14u * k + 2] << 8) | cells[14u * k + 3];
    for (int pass = 0; pass < 2; ++pass)
        for (unsigned k = 0; k < nc; ++k)
        {
            const unsigned char *c = cells + 14u * k;
            VoxelMapInstance inst = {0};
            /* the cell, again (a memo hit), and those 8 cells east and
             * south of it (its memo slot, another cell) */
            static const int kAsk[][2] = { { 0, 0 }, { 0, 0 }, { 8, 0 }, { 0, 8 }, { 0, 0 } };

            inst.layoutId = (int)Le16(c);
            inst.originX = pass ? -7 : 0;
            inst.originY = pass ? 5 : 0;
            for (unsigned q = 0; q < sizeof(kAsk) / sizeof(kAsk[0]); ++q)
            {
                int x = c[2] + kAsk[q][0], y = c[3] + kAsk[q][1], ground = 7, wall = 7, variant;
                unsigned sides = 7, key = ((unsigned)inst.layoutId << 16) | ((unsigned)x << 8) | (unsigned)y;
                float foot = 7.0f;
                const unsigned *hit = x <= 255 && y <= 255
                                    ? bsearch(&key, keys, nc, sizeof(*keys), CompareKey) : NULL;

                variant = VoxelRelief_Cut(&inst, x + inst.originX, y + inst.originY, &foot, &ground, &wall, &sides);
                if (hit == NULL)
                {
                    assert(variant == -1 && foot == 7.0f && ground == 7 && wall == 7 && sides == 7);
                    continue;
                }
                c = cells + 14u * (unsigned)(hit - keys);
                assert(variant == (int)Le16(c + 4));
                assert(foot == (int16_t)Le16(c + 6) / 16.0f);
                assert(ground == (Le16(c + 8) == 0xFFFFu ? -1 : (int)Le16(c + 8)));
                assert(wall == (Le16(c + 10) == 0xFFFFu ? -1 : (int)Le16(c + 10)));
                assert(sides == ((unsigned)c[12] | ((unsigned)c[13] << 8)));
                c = cells + 14u * k;
                ++checked;
            }
        }
    assert(checked >= 2 * 3 * nc);
    free(keys);
    free(blob);
}

const VoxelMapInstance *VoxelWorld_GetInstanceAt(int x, int y)
{
    (void)x; (void)y;
    return sOnly;
}

int main(int argc, char **argv)
{
    VoxelMapInstance inst = {0};
    const int16_t *g;

    assert(argc == 8);
    inst.layoutId = atoi(argv[1]);
    assert(VoxelRelief_Init());
    g = VoxelRelief_Cell(&inst, 3, 7);
    assert(g != NULL && VoxelRelief_IsSlope(g));
    assert(g[2 * VOXEL_RELIEF_SIDE + 2] == 6);
    assert(g[4 * VOXEL_RELIEF_SIDE + 2] == 0);
    assert(g[0 * VOXEL_RELIEF_SIDE + 2] == 0);
    assert(g[3 * VOXEL_RELIEF_SIDE + 2] > 0 && g[3 * VOXEL_RELIEF_SIDE + 2] < 6);
    assert(VoxelRelief_Cell(&inst, 3, 6) == NULL);
    /* the same cell of a map with no relief is not lifted */
    inst.layoutId = 0;
    assert(VoxelRelief_Cell(&inst, 3, 7) == NULL);
    /* the drawn mountain */
    inst.layoutId = atoi(argv[2]);
    assert(VoxelRelief_IsDrawn(&inst));
    g = VoxelRelief_Cell(&inst, 60, 2);
    assert(g != NULL && !VoxelRelief_IsSlope(g) && g[0] > 48);
    assert(VoxelRelief_Depth(&inst, 60, 2) != NULL);
    assert(!VoxelRelief_IsSlope(VoxelRelief_Depth(&inst, 60, 2)));
    assert(VoxelRelief_Depth(&inst, 60, 2)[0] == g[0]);
    assert(VoxelRelief_CellShift(&inst, 60, 2) == VoxelRelief_CellLift(&inst, 60, 2));
    /* and the sun meets it where it stands, as far south as it is high */
    sOnly = &inst;
    assert(VoxelRelief_DrawnTop(&inst) >= g[0] / 16.0f);
    assert(fabsf(VoxelRelief_SurfaceAt(60.5f, 2.5f + g[0] / 16.0f) - g[0] / 16.0f) < 0.1f);
    sOnly = NULL;
    assert(VoxelRelief_Belt(&inst) == 0);
    /* the carved cave and its belt */
    inst.layoutId = atoi(argv[3]);
    assert(VoxelRelief_IsDrawn(&inst));
    assert(VoxelRelief_Belt(&inst) == 3);
    g = VoxelRelief_Cell(&inst, 0, 0);
    assert(g != NULL && !VoxelRelief_IsSlope(g) && g[0] == 16 * 3);
    g = VoxelRelief_Cell(&inst, 5, 4);
    assert(g != NULL && !VoxelRelief_IsSlope(g) && g[0] == 16);
    assert(VoxelRelief_Cell(&inst, 4, 8) == NULL);
    /* the sea that leans a level, as far south as it is high in the world,
     * as every point stands: its west edge in Route 107's row, a level down */
    inst.layoutId = atoi(argv[4]);
    assert(!VoxelRelief_IsDrawn(&inst));
    g = VoxelRelief_Cell(&inst, 0, 10);
    assert(g != NULL && g[2 * VOXEL_RELIEF_SIDE] == -16);
    assert(VoxelRelief_Depth(&inst, 0, 10) != NULL);
    for (unsigned i = 0; i < VOXEL_RELIEF_SIDE * VOXEL_RELIEF_SIDE; ++i)
        assert(VoxelRelief_Depth(&inst, 0, 10)[i] == g[i]);
    sOnly = &inst;
    {
        float last = VoxelRelief_LiftAt(0.0f, 10.5f);

        assert(fabsf(last + 1.0f) < 0.01f);
        for (float x = 0.25f; x < 60.0f; x += 0.25f)
        {
            float lift = VoxelRelief_LiftAt(x, 10.5f);

            assert(lift >= last && lift - last <= 1.0f / 16.0f + 0.001f);
            assert(fabsf(VoxelRelief_ShiftAt(x, 10.5f) - lift) < 0.001f);
            last = lift;
        }
        assert(fabsf(last) < 1.0f / 16.0f + 0.001f);
    }
    sOnly = NULL;
    /* seams: Lilycove at (0, 0), Route 124 east of it ten rows up (map.json
     * offset -10), Route 126 south of Route 124 */
    {
        VoxelMapInstance lilycove = {0}, r124 = {0}, r126 = {0};
        float base;

        lilycove.layoutId = atoi(argv[5]);
        lilycove.width = 80, lilycove.height = 40;
        r124.layoutId = atoi(argv[6]);
        r124.originX = 80, r124.originY = -10, r124.width = 80, r124.height = 80;
        r126.layoutId = atoi(argv[7]);
        r126.originX = 80, r126.originY = 70, r126.width = 80, r126.height = 80;
        assert(VoxelRelief_SeamStep(&lilycove, &r124) == 96);
        assert(VoxelRelief_SeamStep(&r124, &lilycove) == -96);
        assert(VoxelRelief_SeamStep(&r124, &r126) == 0);
        assert(VoxelRelief_SeamStep(&lilycove, &r126) == 0);  /* not touching */
        base = VoxelRelief_Base(&r124);
        r124.lift = VoxelRelief_SeamStep(&lilycove, &r124);
        assert(VoxelRelief_Base(&r124) == base + 6.0f);
    }
    CheckCuts();
    VoxelRelief_Shutdown();
    puts("PASS relief: ledge lip, a drawn terrace, a carved cave's belt and Route 108's leaning sea "
         "from the ROMFS file, searched in row order; Lilycove's seam with Route 124 asks a lift; "
         "every cut cell through the memo");
    return 0;
}
