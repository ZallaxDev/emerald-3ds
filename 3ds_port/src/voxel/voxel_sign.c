#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "voxel_sign.h"
#include "voxel_file.h"
#include "voxel_lighting.h"
#include "voxel_atlas.h"
#include "voxel_regions.h"
#include "voxel_relief.h"
#include "voxel_building.h"

#ifndef SIGN_MASK_PATH
#define SIGN_MASK_PATH "voxel/signposts.bin"
#endif
#define SIGN_MAGIC "VXS3"

typedef struct
{
    uint16_t layout, x, y;
    uint16_t rows[16];
    uint16_t head[16];  /* the sign's part drawn in the cell north of it */
    uint16_t headGround;
    uint16_t ground;    /* the metatile the sign's own cell is laid with */
} SignRecord;

static SignRecord *sIndex;
static unsigned sCount;
static bool sTried;

void VoxelSign_Shutdown(void)
{
    free(sIndex);
    sIndex = NULL;
    sCount = 0;
    sTried = false;
}

static bool LoadMasks(void)
{
    uint8_t header[8];
    FILE *file;
    if (sTried) return sIndex != NULL;
    sTried = true;
    file = VoxelFile_Open(SIGN_MASK_PATH);
    if (!file) return false;
    if (fread(header, 1, sizeof(header), file) != sizeof(header)
        || memcmp(header, SIGN_MAGIC, 4) != 0) {
        fclose(file);
        return false;
    }
    sCount = (unsigned)header[4] | ((unsigned)header[5] << 8)
           | ((unsigned)header[6] << 16) | ((unsigned)header[7] << 24);
    if (sCount == 0 || sCount > 65535u) {
        fclose(file);
        return false;
    }
    sIndex = malloc((size_t)sCount * sizeof(*sIndex));
    if (!sIndex || fread(sIndex, sizeof(*sIndex), sCount, file) != sCount) {
        free(sIndex); sIndex = NULL; sCount = 0;
        fclose(file);
        return false;
    }
    fclose(file);
    return true;
}

void VoxelSign_Init(void)
{
    LoadMasks();
}

static const SignRecord *FindMask(unsigned layout, int x, int y)
{
    unsigned lo = 0, hi;
    if (!LoadMasks()) return NULL;
    hi = sCount;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        const SignRecord *r = &sIndex[mid];
        unsigned key = ((unsigned)r->layout << 20) | ((unsigned)r->y << 10) | r->x;
        unsigned want = (layout << 20) | ((unsigned)y << 10) | (unsigned)x;
        if (key < want) lo = mid + 1;
        else hi = mid;
    }
    if (lo == sCount || sIndex[lo].layout != layout
        || sIndex[lo].x != x || sIndex[lo].y != y) return NULL;
    return &sIndex[lo];
}

static bool HasHead(const SignRecord *r)
{
    for (unsigned i = 0; i < 16; ++i)
        if (r->head[i] != 0)
            return true;
    return false;
}

bool VoxelSign_IsLantern(const uint16_t head[16])
{
    unsigned top = head[0], middle = head[4], width = 0;
    for (unsigned column = 0; column < 16; ++column)
        width += (middle >> column) & 1u;
    return top != 0 && (top & (top - 1u)) == 0 && width >= 7;
}

/* The ground a lamp's light may fall on: what is walked or seen between
 * things - floor, water, the foot of a post, fence or prop. Not a building,
 * tree, cliff, ledge or stair, whose ground is not where the pool lies. */
static bool LampLit(const VoxelMapInstance *inst, int x, int z)
{
    int localX = x - inst->originX, localZ = z - inst->originY;
    unsigned role;

    if (localX < 0 || localZ < 0 || localX >= inst->width || localZ >= inst->height)
        return false;
    role = VoxelRegions_RoleAt(inst->layoutId, localX, localZ);
    return role == VOXEL_ROLE_FLOOR || role == VOXEL_ROLE_WATER || role == VOXEL_ROLE_SIGNPOST
        || role == VOXEL_ROLE_FENCE || role == VOXEL_ROLE_PROP;
}

/* A rectangle of the pool on one ground, its texture (the round falloff of
 * VOXEL_LAMP_POOL_RADIUS) mapped from the lamp's centre. */
static void LampPoolQuad(VoxelBuilder *b, float x0, float z0, float x1, float z1,
                         float centerX, float centerZ)
{
    float scale = 0.5f / VOXEL_LAMP_POOL_RADIUS;
    float u0 = 0.5f + (x0 - centerX) * scale, u1 = 0.5f + (x1 - centerX) * scale;
    float v0 = 0.5f + (z0 - centerZ) * scale, v1 = 0.5f + (z1 - centerZ) * scale;

    VoxelBuilder_Quad(b,
        &(VoxelVertex){x0, 0.0f, z0, u0, v0, 1.0f},
        &(VoxelVertex){x1, 0.0f, z0, u1, v0, 1.0f},
        &(VoxelVertex){x1, 0.0f, z1, u1, v1, 1.0f},
        &(VoxelVertex){x0, 0.0f, z1, u0, v1, 1.0f});
}

/*
 * The pool of light under a lamp: a textured disc on the ground, its falloff
 * per pixel. Where every cell it covers is lit ground at one height - a city
 * street - it is one quad; elsewhere a quad a cell, on the cells it may light.
 * It used to be four quads a cell with the falloff in the vertices, which drew
 * it in visible squares, and it left out the lamp's own cell (a signpost) and
 * any fence beside it: dark squares where the light is brightest.
 */
static void LampGround(VoxelBuilder *builder, const VoxelMapInstance *inst,
                       float centerX, float centerZ)
{
    const float r = VOXEL_LAMP_POOL_RADIUS;
    int firstX = (int)floorf(centerX - r), lastX = (int)floorf(centerX + r);
    int firstZ = (int)floorf(centerZ - r), lastZ = (int)floorf(centerZ + r);
    float lift = 0.0f, shift = 0.0f;
    bool flat = true;

    if (builder == NULL)
        return;
    builder->base = VoxelRelief_Base(inst);
    for (int zpos = firstZ; zpos <= lastZ && flat; ++zpos)
        for (int xpos = firstX; xpos <= lastX && flat; ++xpos)
        {
            float l = VoxelRelief_CellLift(inst, xpos, zpos), s = VoxelRelief_CellShift(inst, xpos, zpos);

            if (zpos == firstZ && xpos == firstX)
            {
                lift = l;
                shift = s;
            }
            flat = LampLit(inst, xpos, zpos) && l == lift && s == shift;
        }
    if (flat)
    {
        if (builder->capacity - builder->count < 6u)
            return;
        builder->lift = lift;
        builder->shift = shift;
        LampPoolQuad(builder, centerX - r, centerZ - r, centerX + r, centerZ + r, centerX, centerZ);
        return;
    }
    for (int zpos = firstZ; zpos <= lastZ; ++zpos)
        for (int xpos = firstX; xpos <= lastX; ++xpos)
        {
            /* the cell's nearest point to the centre: is any of it lit? */
            float nx = centerX < xpos ? xpos : centerX > xpos + 1 ? xpos + 1 : centerX;
            float nz = centerZ < zpos ? zpos : centerZ > zpos + 1 ? zpos + 1 : centerZ;

            if ((nx - centerX) * (nx - centerX) + (nz - centerZ) * (nz - centerZ) >= r * r
             || !LampLit(inst, xpos, zpos))
                continue;
            if (builder->capacity - builder->count < 6u)
                return;
            builder->lift = VoxelRelief_CellLift(inst, xpos, zpos);
            builder->shift = VoxelRelief_CellShift(inst, xpos, zpos);
            LampPoolQuad(builder, (float)xpos, (float)zpos, (float)(xpos + 1), (float)(zpos + 1),
                         centerX, centerZ);
        }
}

void VoxelSign_EmitNight(VoxelBuilder *glass, VoxelBuilder *ground,
                       const VoxelMapInstance *inst, int x0, int y0, int x1, int y1)
{
    if (glass == NULL || inst == NULL || inst->indoor || !LoadMasks())
        return;
    for (unsigned index = 0; index < sCount; ++index)
    {
        const SignRecord *record = &sIndex[index];
        int xpos = inst->originX + record->x, zpos = inst->originY + record->y;
        if (record->layout != inst->layoutId || !VoxelSign_IsLantern(record->head)
            || xpos < x0 || xpos >= x1 || zpos < y0 || zpos >= y1
            || VoxelRegions_RoleAt(inst->layoutId, record->x, record->y) != VOXEL_ROLE_SIGNPOST)
            continue;
        int last = 16;
        for (unsigned row = 0; row < 16; ++row)
            if (record->rows[row] != 0)
                last = (int)row + 16;
        glass->base = VoxelRelief_Base(inst);
        glass->lift = VoxelRelief_CellLift(inst, xpos, zpos);
        glass->shift = VoxelRelief_CellShift(inst, xpos, zpos);
        float back = zpos - 1 + (last / 8) * 0.5f + 0.25f - VOXEL_SIGN_PINNED_DEPTH / 32.0f;
        float front = back + VOXEL_SIGN_PINNED_DEPTH / 16.0f;
        for (unsigned row = 6; row < 11; ++row)
        {
            unsigned bits = record->head[row];
            int left = 0, right = 15;
            while (left < 16 && (bits & (1u << left)) == 0)
                ++left;
            while (right >= left && (bits & (1u << right)) == 0)
                --right;
            left += 2;
            right -= 2;
            if (left > right)
                continue;
            if (glass->capacity - glass->count < 12u)
                return;
            float leftX = xpos + left / 16.0f, rightX = xpos + (right + 1) / 16.0f;
            float bottom = (last - (int)row) / 16.0f, top = bottom + 1.0f / 16.0f;
            for (unsigned face = 0; face < 2; ++face)
            {
                float depth = face == 0 ? front : back;
                VoxelBuilder_Quad(glass,
                    &(VoxelVertex){leftX, bottom, depth, 0.0f, 0.0f, 1.0f},
                    &(VoxelVertex){rightX, bottom, depth, 0.0f, 0.0f, 1.0f},
                    &(VoxelVertex){rightX, top, depth, 0.0f, 0.0f, 1.0f},
                    &(VoxelVertex){leftX, top, depth, 0.0f, 0.0f, 1.0f});
            }
        }
        LampGround(ground, inst, xpos + 0.5f, (front + back) * 0.5f);
    }
}

static void PixelUV(const VoxelSignMask *m, unsigned x, unsigned y, float *u, float *v)
{
    if (y < m->headRows)
    {
        *u = m->headU0 + x * m->du;
        *v = m->headV0 + y * m->dv;
    }
    else
    {
        *u = m->u0 + x * m->du;
        *v = m->v0 + (y - m->headRows) * m->dv;
    }
}

static int Filled(const VoxelSignMask *m, int x, int y)
{
    return x >= 0 && y >= 0 && x < (int)m->width && y < (int)m->height
        && (m->opaque[y] & (uint16_t)(1u << x)) != 0;
}

static void Face(VoxelBuilder *b, float x0, float y0, float z0,
                 float x1, float y1, float z1,
                 float u0, float v0, float u1, float v1, float shade,
                 int side)
{
    VoxelVertex a, c, d, e;
    if (side == 1) shade *= .68f;
    else if (side == 2 || side == 3) shade *= .78f;
    else if (side == 4) shade *= .55f;
    if (side == 0) { /* front: SOUTH, +z */
        a = (VoxelVertex){x0, y0, z1, u0, v1, shade};
        c = (VoxelVertex){x1, y0, z1, u1, v1, shade};
        d = (VoxelVertex){x1, y1, z1, u1, v0, shade};
        e = (VoxelVertex){x0, y1, z1, u0, v0, shade};
    } else if (side == 1) { /* back: NORTH, -z */
        a = (VoxelVertex){x1, y0, z0, u1, v1, shade};
        c = (VoxelVertex){x0, y0, z0, u0, v1, shade};
        d = (VoxelVertex){x0, y1, z0, u0, v0, shade};
        e = (VoxelVertex){x1, y1, z0, u1, v0, shade};
    } else if (side == 2) { /* left/right */
        a = (VoxelVertex){x0, y0, z1, u0, v1, shade};
        c = (VoxelVertex){x0, y0, z0, u1, v1, shade};
        d = (VoxelVertex){x0, y1, z0, u1, v0, shade};
        e = (VoxelVertex){x0, y1, z1, u0, v0, shade};
    } else if (side == 3) {
        a = (VoxelVertex){x1, y0, z0, u0, v1, shade};
        c = (VoxelVertex){x1, y0, z1, u1, v1, shade};
        d = (VoxelVertex){x1, y1, z1, u1, v0, shade};
        e = (VoxelVertex){x1, y1, z0, u0, v0, shade};
    } else if (side == 4) { /* bottom/top */
        a = (VoxelVertex){x0, y0, z1, u0, v1, shade};
        c = (VoxelVertex){x1, y0, z1, u1, v1, shade};
        d = (VoxelVertex){x1, y0, z0, u1, v0, shade};
        e = (VoxelVertex){x0, y0, z0, u0, v0, shade};
    } else {
        a = (VoxelVertex){x0, y1, z0, u0, v1, shade};
        c = (VoxelVertex){x1, y1, z0, u1, v1, shade};
        d = (VoxelVertex){x1, y1, z1, u1, v0, shade};
        e = (VoxelVertex){x0, y1, z1, u0, v0, shade};
    }
    VoxelBuilder_Quad(b, &a, &c, &d, &e);
}

static void EmitSign(VoxelBuilder *b, float wx, float wz, float baseY,
                     const VoxelSignMask *m, unsigned depthPixels, float shade)
{
    unsigned y, x;
    float depth;
    uint16_t component[16 * VOXEL_SIGN_MAX_ROWS] = {0}, stack[16 * VOXEL_SIGN_MAX_ROWS];
    uint8_t lowY[16 * VOXEL_SIGN_MAX_ROWS + 1] = {0};
    unsigned components = 0;
    if (!b || !m || m->width == 0 || m->height == 0 || m->width > 16
        || m->height > VOXEL_SIGN_MAX_ROWS || depthPixels == 0)
        return;
    /* Structures.buildObject: 8-connected pieces each stand on their own
     * bottom, in the depth band of that bottom's 8px map row. */
    for (y = 0; y < m->height; ++y)
        for (x = 0; x < m->width; ++x)
        {
            unsigned n = 0, start = y * 16 + x;
            if (!Filled(m, x, y) || component[start]) continue;
            ++components;
            component[start] = (uint16_t)components;
            stack[n++] = (uint16_t)start;
            while (n)
            {
                unsigned p = stack[--n];
                int px = p % 16, py = p / 16;
                if (py > lowY[components]) lowY[components] = (uint8_t)py;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                        if (Filled(m, px + dx, py + dy))
                        {
                            unsigned q = (unsigned)((py + dy) * 16 + px + dx);
                            if (!component[q])
                            {
                                component[q] = (uint16_t)components;
                                stack[n++] = (uint16_t)q;
                            }
                        }
            }
        }
    if (!components) return;
    depth = (float)depthPixels / 16.0f;
    /*
     * Every face is merged along the pixels that share it: tops and bottoms
     * along a row, the sides down a column, front and back along a row. A
     * merged face carries the drawing across it as the faces of its pixels
     * did, each pixel on its own sixteenth: the same picture in a fifth
     * fewer quads. Pixels side by side or one over the other are one piece
     * and stand on the same bottom. A boulder is a signpost too (Sky
     * Pillar's top, thirteen of them to a chunk), and pixel by pixel they
     * were more than a chunk could hold.
     */
    for (x = 0; x < m->width; ++x) {
        for (int side = 2; side <= 3; ++side) {
            int dx = side == 2 ? -1 : 1;
            for (y = 0; y < m->height; ++y) {
                unsigned start = y;
                if (!Filled(m, (int)x, (int)y) || Filled(m, (int)x + dx, (int)y)) continue;
                int maxY = lowY[component[y * 16 + x]];
                /* never across the seam of a lamp's head and its post: the
                 * two are drawn in different places of the atlas */
                while (y + 1 < m->height && y + 1 != m->headRows
                       && Filled(m, (int)x, (int)y + 1) && !Filled(m, (int)x + dx, (int)y + 1))
                    ++y;
                {
                    float xa = wx + (float)x / 16.0f;
                    float xb = xa + 1.0f / 16.0f;
                    float za = wz + (maxY / 8) * 0.5f + 0.25f - depth * 0.5f;
                    float zb = za + depth;
                    /* buildObject's lowY is the real drawn bottom, not the
                     * cell bottom. The lowest opaque row therefore contacts
                     * baseY. */
                    float ya = baseY + (float)(maxY - (int)y) / 16.0f;
                    float yb = baseY + (float)(maxY - (int)start + 1) / 16.0f;
                    float ua, va;
                    PixelUV(m, x, start, &ua, &va);
                    Face(b, xa,ya,za,xb,yb,zb,ua,va,ua + m->du,va + (y + 1 - start) * m->dv,shade,side);
                }
            }
        }
    }
    for (y = 0; y < m->height; ++y) {
        for (int side = 4; side <= 5; ++side) {
            int dy = side == 5 ? -1 : 1;
            for (x = 0; x < m->width; ++x) {
                unsigned start = x;
                if (!Filled(m, (int)x, (int)y) || Filled(m, (int)x, (int)y + dy)) continue;
                int maxY = lowY[component[y * 16 + x]];
                while (x + 1 < m->width && Filled(m, (int)x + 1, (int)y)
                       && !Filled(m, (int)x + 1, (int)y + dy))
                    ++x;
                {
                    float xa = wx + (float)start / 16.0f;
                    float xb = wx + (float)(x + 1) / 16.0f;
                    float za = wz + (maxY / 8) * 0.5f + 0.25f - depth * 0.5f;
                    float zb = za + depth;
                    float ya = baseY + (float)(maxY - (int)y) / 16.0f;
                    float yb = ya + 1.0f / 16.0f;
                    float ua, va;
                    PixelUV(m, start, y, &ua, &va);
                    Face(b, xa,ya,za,xb,yb,zb,ua,va,ua + (x + 1 - start) * m->du,va + m->dv,shade,side);
                }
            }
        }
        /* Front and back. */
        for (x = 0; x < m->width; ++x) {
            unsigned start = x;
            if (!Filled(m, (int)x, (int)y)) continue;
            int maxY = lowY[component[y * 16 + x]];
            while (x + 1 < m->width && Filled(m, (int)x + 1, (int)y)) ++x;
            {
                float xa = wx + (float)start / 16.0f;
                float xb = wx + (float)(x + 1) / 16.0f;
                float za = wz + (maxY / 8) * 0.5f + 0.25f - depth * 0.5f;
                float zb = za + depth;
                float ua, ub, va;
                float ya = baseY + (float)(maxY - (int)y) / 16.0f;
                float yb = ya + 1.0f / 16.0f;
                PixelUV(m, start, y, &ua, &va);
                ub = ua + (x + 1 - start) * m->du;
                Face(b, xa,ya,za,xb,yb,zb,ua,va,ub,va + m->dv,shade,0);
                Face(b, xa,ya,za,xb,yb,zb,ua,va,ub,va + m->dv,shade,1);
            }
        }
    }
}

static bool EmitCell(VoxelBuilder *b, const VoxelMapInstance *inst, int x, int y, bool ground);

bool VoxelSign_EmitCell(VoxelBuilder *b, const VoxelMapInstance *inst, int x, int y)
{
    return EmitCell(b, inst, x, y, true);
}

bool VoxelSign_EmitStanding(VoxelBuilder *b, const VoxelMapInstance *inst, int x, int y)
{
    return EmitCell(b, inst, x, y, false);
}

bool VoxelSign_IsCell(const VoxelMapInstance *inst, int x, int y)
{
    return inst && FindMask((unsigned)inst->layoutId, x - inst->originX,
                           y - inst->originY) != NULL;
}

static bool EmitCell(VoxelBuilder *b, const VoxelMapInstance *inst, int x, int y, bool ground)
{
    const SignRecord *r;
    VoxelSignMask mask;
    float u0, v0, u1, v1;
    int lx, ly;
    if (!b || !inst) return false;
    lx = x - inst->originX;
    ly = y - inst->originY;
    r = FindMask((unsigned)inst->layoutId, lx, ly);
    if (!r || !VoxelMesh_TileUV(b, x, y, &u0, &v0, &u1, &v1)) return false;
    memset(&mask, 0, sizeof(mask));
    mask.width = mask.height = 16;
    mask.u0 = u0; mask.v0 = v0;
    mask.du = (u1 - u0) / 16.0f;
    mask.dv = (v1 - v0) / 16.0f;
    /* A lamp: its lantern, drawn in the cell north, stands on the post. */
    if (HasHead(r) && VoxelMesh_TileUV(b, x, y - 1, &mask.headU0, &mask.headV0, &u1, &v1))
        mask.headRows = 16;
    memcpy(mask.opaque, mask.headRows ? r->head : r->rows, sizeof(r->rows));
    if (mask.headRows)
    {
        memcpy(mask.opaque + 16, r->rows, sizeof(r->rows));
        mask.height = 32;
    }
    {
        float gu0, gv0, gu1, gv1;
        /* The ground the generator matched to the cell's drawing round the
         * sign (the lawn it stands on, not the path south of it); the cell
         * south of it when the atlas has none. Never paint the sign's
         * drawing a second time on its ground. */
        if (ground && (VoxelMesh_MetatileUV(b, r->ground, &gu0, &gv0, &gu1, &gv1)
                    || VoxelMesh_TileUV(b, x, y + 1, &gu0, &gv0, &gu1, &gv1)))
            VoxelMesh_Top(b, (float)x, (float)y, 0.0f, 0.0f,
                          gu0, gv0, gu1, gv1, 1.0f);
    }
    VoxelSign_Emit(b, (float)x, (float)(y - (mask.headRows ? 1 : 0)), 0.0f, &mask,
                   VOXEL_SIGN_PINNED_DEPTH, 1.0f);
    return true;
}

bool VoxelSign_Ground(const VoxelMapInstance *inst, int x, int y, int *metatile)
{
    const SignRecord *r;

    if (!inst) return false;
    r = FindMask((unsigned)inst->layoutId, x - inst->originX, y - inst->originY);
    if (!r) return false;
    if (metatile) *metatile = r->ground;
    return true;
}

bool VoxelSign_HeadGround(const VoxelMapInstance *inst, int x, int y, int *metatile)
{
    const SignRecord *r;
    int lx, ly;

    if (!inst) return false;
    lx = x - inst->originX;
    ly = y - inst->originY;
    r = FindMask((unsigned)inst->layoutId, lx, ly + 1);
    if (!r || !HasHead(r))
        return false;
    if (metatile) *metatile = r->headGround;
    return true;
}

/*
 * A sign or a lamp is lit as one object. Its geometry is drawn to the pixel -
 * dozens of faces a sixteenth of a tile across - and shading every corner of
 * them on its own was most of the lighting cost of a street of lamps, for a
 * shadow that does not visibly vary across something that small.
 */
void VoxelSign_Emit(VoxelBuilder *b, float wx, float wz, float baseY,
                    const VoxelSignMask *m, unsigned depthPixels, float shade)
{
#if CTR_VOXEL_LIGHTING
    float constant = b->lightingConstant;

    /* (where it stands in the world: on its cell's relief and its map's
     * base, as the builder places it - at the bare height a sign on a
     * raised map was sampled inside the rock and drawn in shadow) */
    if (b->lighting && constant < 0.0f)
        b->lightingConstant = VoxelLighting_Sample(
            wx + 0.5f, baseY + 0.5f + b->lift + b->base,
            VoxelBuilder_StretchZ(b, wz + 0.5f + b->shift));
    EmitSign(b, wx, wz, baseY, m, depthPixels, shade);
    b->lightingConstant = constant;
#else
    EmitSign(b, wx, wz, baseY, m, depthPixels, shade);
#endif
}

/*
 * The sun's view of a sign or a lamp: the pixels of its drawing, standing on
 * the thin board EmitSign builds them on. The board is widened to a quarter
 * tile here only so a ray marching a quarter tile a step cannot slip through
 * it; the shadow keeps the drawing's outline. A signpost at the foot of a
 * face stands on the ground under its board (voxel_mesh_builder.c).
 */
typedef struct
{
    uint16_t rows[VOXEL_SIGN_MAX_ROWS];
    unsigned count, low, high;
    float base, zMid;
} SignShadow;

static bool ShadowOf(const VoxelMapInstance *inst, int x, int y, SignShadow *s)
{
    const SignRecord *r;
    const int16_t *relief;
    bool head;

    /* one search: VoxelSign_IsCell would make the same one again */
    r = inst ? FindMask((unsigned)inst->layoutId, x - inst->originX, y - inst->originY) : NULL;
    if (r == NULL)
        return false;
    head = HasHead(r);
    memset(s, 0, sizeof(*s));
    if (head)
    {
        memcpy(s->rows, r->head, sizeof(r->head));
        memcpy(s->rows + 16, r->rows, sizeof(r->rows));
        s->count = 32;
    }
    else
    {
        memcpy(s->rows, r->rows, sizeof(r->rows));
        s->count = 16;
    }
    s->high = s->count;
    for (unsigned i = 0; i < s->count; ++i)
        if (s->rows[i])
        {
            if (s->high == s->count) s->high = i;
            s->low = i;
        }
    if (s->high == s->count)
        return false;
    relief = VoxelRelief_Cell(inst, x, y);
    if (relief != NULL)
        s->base = relief[3 * VOXEL_RELIEF_SIDE + 2] / 16.0f;
    s->zMid = (float)(y - (head ? 1 : 0)) + (float)(s->low / 8) * 0.5f + 0.25f;
    {
        /* on a bridge's deck (voxel_mesh_builder.c stands it there) */
        float deck;

        if (VoxelBuildings_DeckAt(inst, x + 0.5f, y + 0.5f, 0, &deck))
        {
            s->base += deck;
            s->zMid += deck;
        }
    }
    return true;
}

float VoxelSign_CasterTop(const VoxelMapInstance *inst, int x, int y)
{
    SignShadow s;
    if (!ShadowOf(inst, x, y, &s))
        return 0.0f;
    return s.base + (float)(s.low - s.high + 1) / 16.0f;
}

bool VoxelSign_Occludes(const VoxelMapInstance *inst, int x, int y,
                        float px, float py, float pz)
{
    SignShadow s;
    int column, row;

    if (!ShadowOf(inst, x, y, &s) || pz < s.zMid - 0.125f || pz > s.zMid + 0.125f)
        return false;
    column = (int)((px - (float)x) * 16.0f);
    row = (int)s.low - (int)((py - s.base) * 16.0f);
    if (column < 0 || column > 15 || row < (int)s.high || row > (int)s.low)
        return false;
    return (s.rows[row] & (uint16_t)(1u << column)) != 0;
}
