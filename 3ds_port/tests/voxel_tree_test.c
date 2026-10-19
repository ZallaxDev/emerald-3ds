/* Exercise the real mesh emitters with a tiny world crossing four chunks. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "voxel_tileset_fixture.h"

#include "voxel_tree.h"
#include "voxel_atlas.h"
#include "voxel_regions.h"

/* voxel_tree.c compares the map's secondary tileset with this object to
 * recognise Dewford's tree art; this test never uses that art, so an empty
 * tileset is enough for the link. */
const struct Tileset gTileset_Dewford = {0};

static VoxelMapInstance sMap = {.originX = 7, .originY = 7, .width = 2, .height = 2};
static bool sGeneral = true;
static int sTiles[] = {0x1D6, 0x1D7, 0x1E6, 0x1E7};
static VoxelAtlasMap sAtlas;
static VoxelVertex sWhole[1024], sParts[1024];
static VoxelCrown sCrowns[64];
static VoxelCrownList sList = {sCrowns, 64, 0, false};

bool VoxelWorld_UsesTreeSprites(const VoxelMapInstance *inst)
{
    return inst == &sMap && sGeneral;
}

const VoxelMapInstance *VoxelWorld_Instance(unsigned index)
{
    return index == 0 ? &sMap : NULL;
}

const VoxelMapInstance *VoxelWorld_GetInstanceAt(int x, int y)
{
    return x >= sMap.originX && x < sMap.originX + 2
        && y >= sMap.originY && y < sMap.originY + 2 ? &sMap : NULL;
}

int VoxelWorld_GetMetatileId(int x, int y)
{
    assert(VoxelWorld_GetInstanceAt(x, y) != NULL);
    return sTiles[(y - sMap.originY) * 2 + x - sMap.originX];
}

VoxelVisualShape VoxelWorld_ClassifyTile(int x, int y)
{
    return VoxelWorld_GetInstanceAt(x, y) ? VOXEL_SHAPE_FLAT : VOXEL_SHAPE_VOID;
}

int VoxelWorld_BorderMetatile(int x, int y)
{
    const int pattern[] = {0x1D4, 0x1D5, 0x1DC, 0x1DD};
    return pattern[((x - sMap.originX) & 1) + 2 * ((y - sMap.originY) & 1)];
}

bool VoxelWorld_BorderIsWater(int x, int y) { (void)x; (void)y; return false; }

void VoxelWorld_GetPlayerWorldCoords(float *x, float *z) { *x = *z = 0; }

unsigned VoxelRegions_RoleAt(unsigned layoutId, int x, int y)
{
    (void)layoutId; (void)x; (void)y;
    return VOXEL_ROLE_TREE;
}

void VoxelAtlas_SlotUV(unsigned slot, float *u0, float *v0, float *u1, float *v1)
{
    *u0 = (slot % 32) / 32.0f;
    *v0 = 1.0f - (slot / 32) / 16.0f;
    *u1 = *u0 + 1.0f / 32;
    *v1 = *v0 - 1.0f / 16;
}

void VoxelAtlas_SolidUV(VoxelSolidColor color, float *u0, float *v0, float *u1, float *v1)
{
    VoxelAtlas_SlotUV((unsigned)color, u0, v0, u1, v1);
}

static void Init(VoxelBuilder *b, VoxelVertex *v)
{
    VoxelBuilder_Init(b, v, 1024);
    VoxelBuilder_SetAtlas(b, &sAtlas);
    VoxelBuilder_SetOrigin(b, sMap.originX, sMap.originY);
    VoxelMesh_BeginWindow(sMap.originX - 2, sMap.originY - 2,
                          sMap.originX + 4, sMap.originY + 4);
}

/*
 * Each recorded card is the card built: turned by nothing, it comes out as
 * the builder's own triangles, and turned by a quarter it is the same card
 * about the same trunk, now leaning along the other axis.
 */
static void CheckCrowns(const VoxelBuilder *b)
{
    const unsigned first = 24; /* after the tops */

    assert(!sList.overflow && sList.count == 4 && b->count == first + 6 * sList.count);
    for (unsigned i = 0; i < sList.count; ++i)
    {
        const VoxelCrown *crown = &sList.items[i];
        VoxelVertex out[6], quarter[6];

        VoxelTree_CrownVertices(crown, 0.0f, 1.0f, 0.0f, 0.0f, out);
        for (unsigned k = 0; k < 6; ++k)
        {
            const VoxelVertex *built = &b->vertices[first + 6 * i + k];

            assert(fabsf(out[k].x - built->x) < 0.003f);
            assert(fabsf(out[k].y - built->y) < 0.003f);
            assert(fabsf(out[k].z - built->z) < 0.003f);
            assert(fabsf(out[k].u - built->u) < 0.0001f && fabsf(out[k].v - built->v) < 0.0001f);
            assert(fabsf(out[k].shade - built->shade) < 0.001f);
        }
        /* The trunk's axis: the two halves of a tree share it. */
        assert(sList.items[0].px == sList.items[1].px || sList.items[0].px == sList.items[2].px);

        /* A camera to the east (yaw 90 degrees): the card stands at the same
         * trunk, as far from it, as high, and leans west, away from the eye. */
        VoxelTree_CrownVertices(crown, 1.0f, 0.0f, 0.0f, 0.0f, quarter);
        for (unsigned k = 0; k < 6; ++k)
        {
            float rb = hypotf(out[k].x - crown->px, out[k].z - crown->pz);
            float rq = hypotf(quarter[k].x - crown->px, quarter[k].z - crown->pz);

            assert(fabsf(rb - rq) < 0.0001f && quarter[k].y == out[k].y);
        }
        assert(quarter[0].x < quarter[5].x);               /* top is west of the foot */
        assert(fabsf(quarter[0].z - quarter[5].z) < 0.0001f);
        assert(quarter[1].z < quarter[0].z);               /* its right edge is north now */
        /* Moved by the frame's offset, nothing else. */
        VoxelTree_CrownVertices(crown, 0.0f, 1.0f, 3.0f, -2.0f, quarter);
        assert(fabsf(quarter[0].x - out[0].x - 3.0f) < 0.0001f
            && fabsf(quarter[0].z - out[0].z + 2.0f) < 0.0001f);
    }
}

/* The two passes over a rectangle, as the build runs them (ctr_voxel.c). */
static void EmitTrees(VoxelBuilder *b, int x0, int y0, int x1, int y1)
{
    VoxelTree_EmitInstance(b, &sMap, x0, y0, x1, y1, VOXEL_TREE_TOPS);
    VoxelTree_EmitInstance(b, &sMap, x0, y0, x1, y1, VOXEL_TREE_CROWNS);
}

/* ... and cell by cell, tops of every cell before the crowns of any. */
static void EmitTreeCells(VoxelBuilder *b, int x0, int y0, int x1, int y1, int w, int h)
{
    for (int pass = 0; pass < 2; ++pass)
        for (int y = y0; y < y1; y += h)
            for (int x = x0; x < x1; x += w)
                VoxelTree_EmitInstance(b, &sMap, x, y, x + w, y + h,
                                       pass == 0 ? VOXEL_TREE_TOPS : VOXEL_TREE_CROWNS);
}

int main(void)
{
    VoxelBuilder whole, parts;
    unsigned ground = 0, crown = 0;

    Init(&whole, sWhole);
    VoxelMesh_EmitInstance(&whole, &sMap, 7, 7, 9, 9);
    assert(whole.count == 0); /* no original ground artwork or extruded region */
    whole.crowns = &sList;
    EmitTrees(&whole, 7, 7, 9, 9);
    assert(whole.count == 48 && whole.dropped == 0);
    for (unsigned i = 0; i < whole.count; ++i)
    {
        const VoxelVertex *v = &whole.vertices[i];
        assert(isfinite(v->x) && isfinite(v->y) && isfinite(v->z));
        assert(v->x >= 0 && v->x <= 2 && v->v >= 0 && v->v <= 1);
        if (v->y == 0)
        {
            ++ground;
            assert(v->u >= 32.0f / VOXEL_TREE_TEXTURE_DIM
                && v->u <= 64.0f / VOXEL_TREE_TEXTURE_DIM && v->z >= 0 && v->z <= 2);
        }
        else
        {
            ++crown;
            assert(v->u >= 0 && v->u <= 32.0f / VOXEL_TREE_TEXTURE_DIM);
        }
    }
    assert(ground == 24 && crown == 24);
    /* The tops first, then the crowns: the first card leans north. */
    assert(sWhole[24].z < sWhole[26].z && sWhole[24].y > sWhole[26].y);
    CheckCrowns(&whole);

    Init(&parts, sParts);
    EmitTreeCells(&parts, 0, 0, 16, 16, 8, 8);
    assert(parts.count == whole.count);
    assert(memcmp(sWhole, sParts, whole.count * sizeof(VoxelVertex)) == 0);

    /* Streaming emits one cell per slice; preserve vertices and order. */
    Init(&parts, sParts);
    EmitTreeCells(&parts, 7, 7, 9, 9, 1, 1);
    assert(parts.count == whole.count);
    assert(memcmp(sWhole, sParts, whole.count * sizeof(VoxelVertex)) == 0);

    /* A map crossing changes the origin, not the cached local mesh. */
    sMap.originX = -3;
    sMap.originY = -5;
    Init(&parts, sParts);
    EmitTrees(&parts, -3, -5, -1, -3);
    assert(parts.count == whole.count);
    for (unsigned i = 0; i < whole.count; ++i)
    {
        assert(fabsf(sWhole[i].x - sParts[i].x) < 0.00001f);
        assert(fabsf(sWhole[i].y - sParts[i].y) < 0.00001f);
        assert(fabsf(sWhole[i].z - sParts[i].z) < 0.00001f);
    }

    /* Border excludes every mapped cell, including at negative coordinates. */
    Init(&parts, sParts);
    VoxelMesh_BeginWindow(-5, -7, 1, 1);
    VoxelMesh_EmitBorder(&parts, -5, -7, 1, 1);
    assert(parts.count == 0); /* no raised belt under the replacement */
    VoxelTree_EmitBorder(&parts, -5, -7, 1, 1, VOXEL_TREE_TOPS);
    VoxelTree_EmitBorder(&parts, -5, -7, 1, 1, VOXEL_TREE_CROWNS);
    assert(parts.count == (6 * 8 - 4) * 12 && parts.dropped == 0);

    unsigned borderCount = parts.count;
    memcpy(sWhole, sParts, borderCount * sizeof(VoxelVertex));
    Init(&parts, sParts);
    for (int pass = 0; pass < 2; ++pass)
        for (int y = -7; y < 1; ++y)
            for (int x = -5; x < 1; ++x)
                VoxelTree_EmitBorder(&parts, x, y, x + 1, y + 1,
                                     pass == 0 ? VOXEL_TREE_TOPS : VOXEL_TREE_CROWNS);
    assert(parts.count == borderCount);
    assert(memcmp(sWhole, sParts, borderCount * sizeof(VoxelVertex)) == 0);

    sGeneral = false;
    Init(&parts, sParts);
    EmitTrees(&parts, -3, -5, -1, -3);
    VoxelTree_EmitBorder(&parts, -5, -7, 1, 1, VOXEL_TREE_TOPS);
    VoxelTree_EmitBorder(&parts, -5, -7, 1, 1, VOXEL_TREE_CROWNS);
    assert(parts.count == 0);
    assert(VoxelTree_Part(NULL, 0x026) == -1); /* secret base tree */
    assert(VoxelTree_Part(NULL, 0x1D9) == -1); /* rock */

    /* Grass fringes lose the old canopy and never acquire region walls. */
    sGeneral = true;
    sTiles[0] = 0x1CE; sTiles[1] = 0x1CF;
    sTiles[2] = 0x1C6; sTiles[3] = 0x1C7;
    sAtlas.slotOf[0x001] = 1;
    sAtlas.slotOf[0x00D] = 2;
    Init(&parts, sParts);
    VoxelMesh_EmitInstance(&parts, &sMap, -3, -5, -1, -3);
    EmitTrees(&parts, -3, -5, -1, -3);
    assert(parts.count == 24 && parts.uncovered == 0);
    for (unsigned i = 0; i < parts.count; ++i)
        assert(parts.vertices[i].y == 0);

    /* Small trees: the canopy top keeps its ground; each trunk cell - edge of
     * a wood or inside it - lays the trunk and stands the whole crown on it. */
    sTiles[0] = 0x00E; sTiles[1] = 0x00F;
    sTiles[2] = 0x016; sTiles[3] = 0x0C7;
    assert(VoxelTree_GroundMetatile(NULL, 0x00E) == 0x001);
    assert(VoxelTree_Part(NULL, 0x016) == VOXEL_TREE_SMALL);
    assert(VoxelTree_Part(NULL, 0x1EC) == 2 && VoxelTree_Part(NULL, 0x1ED) == 3);
    assert(VoxelTree_Part(&gTileset_Dewford, 0x23A) == VOXEL_TREE_SMALL);
    assert(VoxelTree_Part(&gTileset_Dewford, 0x243) == VOXEL_TREE_SMALL);
    assert(VoxelTree_Part(NULL, 0x23A) == -1 && VoxelTree_Part(NULL, 0x243) == -1);
    assert(VoxelTree_GroundMetatile(&gTileset_Dewford, 0x239) == 0x124);
    assert(VoxelTree_GroundMetatile(NULL, 0x239) == 0x239);
    Init(&parts, sParts);
    VoxelMesh_EmitInstance(&parts, &sMap, -3, -5, -1, -3);
    assert(parts.count == 12 && parts.uncovered == 0); /* two grass tops */
    for (unsigned i = 0; i < parts.count; ++i)
        assert(parts.vertices[i].y == 0 && parts.vertices[i].z <= 1);
    parts.count = 0;
    EmitTrees(&parts, -3, -5, -1, -3);
    assert(parts.count == 24 && parts.dropped == 0);
    for (unsigned i = 0; i < parts.count; ++i)
    {
        const VoxelVertex *v = &parts.vertices[i];
        assert(v->u >= 32.0f / VOXEL_TREE_TEXTURE_DIM
            && v->u <= 64.0f / VOXEL_TREE_TEXTURE_DIM
            && v->v >= 1.0f - 64.0f / VOXEL_TREE_TEXTURE_DIM
            && v->v <= 1.0f - 32.0f / VOXEL_TREE_TEXTURE_DIM);
        if (v->y == 0)
            assert(v->u >= 48.0f / VOXEL_TREE_TEXTURE_DIM
                && v->v >= 1.0f - 48.0f / VOXEL_TREE_TEXTURE_DIM && v->z >= 1 && v->z <= 2);
        else
            assert(v->u <= 48.0f / VOXEL_TREE_TEXTURE_DIM);
    }
    /* Leaning north from its trunk into the cell above, as tall as it is long. */
    assert(parts.vertices[12].z < parts.vertices[14].z && parts.vertices[12].y > parts.vertices[14].y);
    assert(parts.vertices[12].z < 1 && fabsf(parts.vertices[12].y - 1.432089f) < 0.0001f);

    /* A staggered wood's lower row is a whole tree: trunks and both rows. */
    assert(VoxelTree_Part(NULL, 0x1F3) == 2 && VoxelTree_Part(NULL, 0x1F2) == 3);
    assert(VoxelTree_Part(NULL, 0x1FC) == 0 && VoxelTree_Part(NULL, 0x1FD) == 1);
    sTiles[0] = 0x1D4; sTiles[1] = 0x1D5;
    sTiles[2] = 0x1F3; sTiles[3] = 0x1F2;
    Init(&parts, sParts);
    EmitTrees(&parts, -3, -5, -1, -3);
    assert(parts.count == 4 * 2 * 6 && parts.dropped == 0);

    /* A roof over the lower row: the crown keeps that row, its ground does
     * not become a trunk. */
    sTiles[2] = 0x008; sTiles[3] = 0x009;
    Init(&parts, sParts);
    EmitTrees(&parts, -3, -5, -1, -3);
    assert(parts.count == (2 * 2 + 2) * 6 && parts.dropped == 0);
    for (unsigned i = 0; i < parts.count; ++i)
        if (parts.vertices[i].y == 0 && parts.vertices[i].u >= 32.0f / VOXEL_TREE_TEXTURE_DIM)
            assert(parts.vertices[i].z <= 8); /* trunk ground only in the tree's row */

    puts("PASS voxel trees: ground/crown, chunk seams, map origins, border, tileset scope, grass fringes, small trees, staggered and covered rows");
    return 0;
}
