#include <stddef.h>
#include "voxel_tree.h"
#include "voxel_relief.h"
#include "voxel_regions.h"

struct Tileset;
extern const struct Tileset gTileset_Dewford;

/* Where gen_voxel_trees.py packs each tree, in texels. */
#define TEX_U(px) ((float)(px) / VOXEL_TREE_TEXTURE_DIM)
#define TEX_V(py) (1.0f - (float)(py) / VOXEL_TREE_TEXTURE_DIM)

/* Ids from 0x200 are the secondary tileset's, the same number meaning a
 * different drawing on every map. */
#define SECONDARY_ID 0x200

int VoxelTree_Part(const void *secondaryTileset, int metatileId)
{
    if (metatileId >= SECONDARY_ID)
    {
        /* Dewford's small tree: the trunk on the sand (23A), and inside the
         * wood (243), where the next crown overlaps it. */
        if (secondaryTileset == &gTileset_Dewford
         && (metatileId == 0x23A || metatileId == 0x243))
            return VOXEL_TREE_SMALL;
        return -1;
    }
    switch (metatileId)
    {
    case 0x1D4: case 0x1D6: return 0; /* upper left, including forest edge */
    case 0x1D5: case 0x1D7: return 1;
    case 0x1DC: case 0x1DE: case 0x1E4: case 0x1E6: return 2;
    case 0x1DD: case 0x1DF: case 0x1E5: case 0x1E7: return 3;
    /* A large tree's lower row under a small tree's canopy top. */
    case 0x1EC: return 2;
    case 0x1ED: return 3;
    /* A lower row under the canopy top of a tree one column to the side, as
     * in staggered woods: 1F3 is the left half, 1F2 the right. */
    case 0x1F3: return 2;
    case 0x1F2: return 3;
    /* An upper row in front of a fence or rock (the Safari Zone's). */
    case 0x1FC: return 0;
    case 0x1FD: return 1;
    /* Small trees: the edge of a wood, and inside it where the next crown
     * overlaps the trunk. 1F4-1F5 also carry a neighbour's leaves. */
    case 0x016: case 0x017: case 0x0C6: case 0x0C7:
    case 0x1F4: case 0x1F5: return VOXEL_TREE_SMALL;
    default: return -1;
    }
}

int VoxelTree_GroundMetatile(const void *secondaryTileset, int metatileId)
{
    if (metatileId >= SECONDARY_ID)
    {
        /* Dewford's crown top over the sand. */
        if (secondaryTileset == &gTileset_Dewford && metatileId == 0x239)
            return 0x124;
        return metatileId;
    }
    switch (metatileId)
    {
    case 0x1C6: case 0x1C7: return 0x00D; /* tall grass without canopy */
    case 0x1CE: case 0x1CF: return 0x001; /* ordinary grass */
    /* A small tree's canopy top, over whatever it stood in front of. The
     * fence feet under 040 have no metatile of their own: grass. */
    case 0x00E: case 0x00F: case 0x040: return 0x001;
    case 0x01D: return 0x002; /* ledge edge */
    case 0x025: return 0x00D; /* tall grass */
    case 0x02D: return 0x0A1; /* reflective water */
    case 0x035: case 0x193: return 0x170; /* calm water */
    case 0x0CE: return 0x091; /* rock wall, sand base */
    default: return metatileId;
    }
}

static int16_t Fix(float v)
{
    float scaled = v * 512.0f;

    return (int16_t)(scaled < 0.0f ? scaled - 0.5f : scaled + 0.5f);
}

/*
 * Notes a crown card just emitted (its triangles are builder->vertices from
 * `first`) for drawing again turned. Everything is given in world tiles as
 * the emitter had it, before the builder's origin, lift, base and shift; the
 * pivot is the vertical axis the card turns about. The shade of each corner
 * is the lit vertex the build put there.
 */
static void RecordCrown(VoxelBuilder *b, unsigned first, float pivotX, float pivotZ,
                        float xl, float xr, float zt, float zb, float yt, float yb,
                        int u0, int v0, int u1, int v1)
{
    VoxelCrownList *list = b->crowns;
    VoxelCrown *crown;
    float lift = b->lift + b->base;
    const float cx[4] = {xl, xr, xr, xl}, cy[4] = {yt, yt, yb, yb}, cz[4] = {zt, zt, zb, zb};

    if (list == NULL)
        return;
    if (b->count <= first)
    {
        list->overflow = true; /* refused for room: nothing to turn */
        return;
    }
    if (list->count >= list->capacity)
    {
        list->overflow = true;
        return;
    }
    crown = &list->items[list->count++];
    crown->px = pivotX - b->originX;
    crown->pz = pivotZ + b->shift - b->originZ;
    crown->xl = Fix(xl - pivotX);
    crown->xr = Fix(xr - pivotX);
    crown->zt = Fix(zt - pivotZ);
    crown->zb = Fix(zb - pivotZ);
    crown->yt = Fix(yt + lift);
    crown->yb = Fix(yb + lift);
    crown->u0 = (uint8_t)u0;
    crown->v0 = (uint8_t)v0;
    crown->u1 = (uint8_t)u1;
    crown->v1 = (uint8_t)v1;
    for (int k = 0; k < 4; ++k)
    {
        float x = cx[k] - b->originX, y = cy[k] + lift, z = cz[k] + b->shift - b->originZ;
        float best = 1.0e30f, shade = 1.0f;

        for (unsigned i = first; i < b->count; ++i)
        {
            const VoxelVertex *v = &b->vertices[i];
            float dx = v->x - x, dy = v->y - y, dz = v->z - z;
            float d = dx * dx + dy * dy + dz * dz;

            if (d < best)
            {
                best = d;
                shade = v->shade;
            }
        }
        {
            float scaled = shade * 16384.0f;

            if (scaled > 32767.0f) scaled = 32767.0f;
            if (scaled < 0.0f) scaled = 0.0f;
            crown->shade[k] = (int16_t)(scaled + 0.5f);
        }
    }
}

void VoxelTree_CrownVertices(const VoxelCrown *crown, float sinYaw, float cosYaw,
                             float ox, float oz, VoxelVertex out[6])
{
    const float scale = 1.0f / 512.0f;
    float xs[4] = {crown->xl * scale, crown->xr * scale, crown->xr * scale, crown->xl * scale};
    float zs[4] = {crown->zt * scale, crown->zt * scale, crown->zb * scale, crown->zb * scale};
    float ys[4] = {crown->yt * scale, crown->yt * scale, crown->yb * scale, crown->yb * scale};
    float us[4] = {TEX_U(crown->u0), TEX_U(crown->u1), TEX_U(crown->u1), TEX_U(crown->u0)};
    float vs[4] = {TEX_V(crown->v0), TEX_V(crown->v0), TEX_V(crown->v1), TEX_V(crown->v1)};
    VoxelVertex corner[4];

    for (int k = 0; k < 4; ++k)
    {
        corner[k].x = ox + crown->px + xs[k] * cosYaw + zs[k] * sinYaw;
        corner[k].y = ys[k];
        corner[k].z = oz + crown->pz - xs[k] * sinYaw + zs[k] * cosYaw;
        corner[k].u = us[k];
        corner[k].v = vs[k];
        corner[k].shade = crown->shade[k] * (1.0f / 16384.0f);
    }
    /* A B C, A C D: the quad's winding. */
    out[0] = corner[0]; out[1] = corner[1]; out[2] = corner[2];
    out[3] = corner[0]; out[4] = corner[2]; out[5] = corner[3];
}

/* The small crown is 16:32: width 1, length 2 tiles, at the same 50 degrees
 * and sunk the same way as the large one, standing on its one-cell trunk.
 * Dewford's own small tree has its art at 64,0, the General one at 32,32. */
static void EmitSmallCell(VoxelBuilder *builder, int x, int y, bool dewford, bool rock,
                          VoxelTreePass pass)
{
    int cx = dewford ? 64 : 32, cy = dewford ? 0 : 32;
    int tx = cx + 16, ty = cy;
    float wx = (float)x, wz = (float)y;
    /* Against a mountain's face (its rock right behind the crown's cell,
     * Route 110's cave) the crown stands up steeper, its top at the trunk
     * cell's back edge: leant back at 50 degrees it ran into the rock and
     * came out of it wherever the rock falls away, a corner's slope. */
    const float rise = 1.532089f, run = rock ? 0.8f : 1.285575f;
    const float baseHeight = -0.10f;
    /* 0.15 further forward than half the large tree's: any less and the
     * leaves stand through the ground behind the trunk. Dewford's another
     * 0.15, so its crown stands over the shadow drawn on its sand. */
    float baseZ = wz + (dewford ? 0.975f : 0.825f);

    unsigned first;

    if (pass == VOXEL_TREE_TOPS)
    {
        VoxelMesh_Top(builder, wx, wz, 0.0f, 0.0f,
                      TEX_U(tx), TEX_V(ty), TEX_U(tx + 16), TEX_V(ty + 16), 1.0f);
        return;
    }
    first = builder->count;
    builder->rounded = true;
    VoxelBuilder_Quad(builder,
        &(VoxelVertex){wx,        baseHeight + rise, baseZ - run, TEX_U(cx),      TEX_V(cy),      1.0f},
        &(VoxelVertex){wx + 1.0f, baseHeight + rise, baseZ - run, TEX_U(cx + 16), TEX_V(cy),      1.0f},
        &(VoxelVertex){wx + 1.0f, baseHeight,        baseZ,       TEX_U(cx + 16), TEX_V(cy + 32), 1.0f},
        &(VoxelVertex){wx,        baseHeight,        baseZ,       TEX_U(cx),      TEX_V(cy + 32), 1.0f});
    builder->rounded = false;
    /* It turns about its own cell's middle, at its foot. */
    RecordCrown(builder, first, wx + 0.5f, baseZ, wx, wx + 1.0f, baseZ - run, baseZ,
                baseHeight + rise, baseHeight, cx, cy, cx + 16, cy + 32);
}

/* One row of a large crown: the card that stands over the cell (x, y) as
 * the half of row `row`, column `col`, of a tree whose trunk row is
 * y - row + 1. */
static void EmitCrownCard(VoxelBuilder *builder, int x, int y, int col, int row)
{
    float wx = (float)x, wz = (float)y;
    float u0 = col * 16.0f / VOXEL_TREE_TEXTURE_DIM;
    float u1 = u0 + 16.0f / VOXEL_TREE_TEXTURE_DIM;
    float v0 = 1.0f - row * 18.0f / VOXEL_TREE_TEXTURE_DIM;
    float v1 = v0 - 18.0f / VOXEL_TREE_TEXTURE_DIM;
    /* The crown keeps its 32:36 aspect ratio: width 2, length 2.25 tiles.
     * sin/cos(50 degrees), fixed in the world rather than camera billboarding.
     * Lower the transparent bottom margin into the ground so the visible
     * leaves overlap the trunk instead of exposing a horizontal cut. */
    const float rise = 1.723600f, run = 1.446272f;
    const float baseHeight = -0.10f;
    float baseZ = wz - row + 1.35f;
    float top = 1.0f - row * 0.5f, bottom = top - 0.5f;
    unsigned first = builder->count;

    /* A crown card: lit as the rounded crown it stands for. */
    builder->rounded = true;
    VoxelBuilder_Quad(builder,
        &(VoxelVertex){wx,        baseHeight + top * rise,    baseZ - top * run,    u0, v0, 1.0f},
        &(VoxelVertex){wx + 1.0f, baseHeight + top * rise,    baseZ - top * run,    u1, v0, 1.0f},
        &(VoxelVertex){wx + 1.0f, baseHeight + bottom * rise, baseZ - bottom * run, u1, v1, 1.0f},
        &(VoxelVertex){wx,        baseHeight + bottom * rise, baseZ - bottom * run, u0, v1, 1.0f});
    builder->rounded = false;
    /* The tree turns about the middle of its two columns, at its foot: the
     * edge this half shares with the other. Both rows have the same foot. */
    RecordCrown(builder, first, col == 0 ? wx + 1.0f : wx, baseZ, wx, wx + 1.0f,
                baseZ - top * run, baseZ - bottom * run,
                baseHeight + top * rise, baseHeight + bottom * rise,
                col * 16, row * 18, col * 16 + 16, row * 18 + 18);
}

static void EmitCell(VoxelBuilder *builder, int x, int y, int part, int metatileId, bool rock,
                     VoxelTreePass pass)
{
    int col = part & 1, row = part >> 1;
    float u0 = (32.0f + col * 16.0f) / VOXEL_TREE_TEXTURE_DIM;
    float u1 = u0 + 16.0f / VOXEL_TREE_TEXTURE_DIM;
    float v0 = 1.0f - row * 16.0f / VOXEL_TREE_TEXTURE_DIM;
    float v1 = v0 - 16.0f / VOXEL_TREE_TEXTURE_DIM;

    if (part == VOXEL_TREE_SMALL)
    {
        EmitSmallCell(builder, x, y, metatileId >= SECONDARY_ID, rock, pass);
        return;
    }
    if (pass == VOXEL_TREE_TOPS)
        VoxelMesh_Top(builder, (float)x, (float)y, 0.0f, 0.0f, u0, v0, u1, v1, 1.0f);
    else
        EmitCrownCard(builder, x, y, col, row);
}

/*
 * The other row of a large tree whose half is drawn over by something else:
 * a hedge, a roof, a fence or a ledge of the secondary tileset, whose
 * metatile is not a tree part. The GBA draws the rest of the leaves in that
 * metatile's art; here its row of the crown is added over it, so the crown
 * is not cut in two. Its ground is left as it is. Only inside the map: across
 * its edge the neighbour's own cells answer.
 */
static void EmitMissingRow(VoxelBuilder *builder, const VoxelMapInstance *inst,
                           int x, int y, int part)
{
    int row = part >> 1;
    int other = row == 0 ? y + 1 : y - 1;

    if (part == VOXEL_TREE_SMALL)
        return;
    if (other < inst->originY || other >= inst->originY + inst->height)
        return;
    if (VoxelTree_Part(inst->secondaryTileset, VoxelWorld_GetMetatileId(x, other)) == (part ^ 2))
        return;
    EmitCrownCard(builder, x, other, part & 1, row ^ 1);
}

void VoxelTree_EmitInstance(VoxelBuilder *builder, const VoxelMapInstance *inst,
                            int x0, int y0, int x1, int y1, VoxelTreePass pass)
{
    if (!VoxelWorld_UsesTreeSprites(inst))
        return;
    if (x0 < inst->originX) x0 = inst->originX;
    if (y0 < inst->originY) y0 = inst->originY;
    if (x1 > inst->originX + inst->width) x1 = inst->originX + inst->width;
    if (y1 > inst->originY + inst->height) y1 = inst->originY + inst->height;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
        {
            int metatileId = VoxelWorld_GetMetatileId(x, y);
            int part = VoxelTree_Part(inst->secondaryTileset, metatileId);
            if (part >= 0)
            {
                /* what stands behind the crown's cell: a mountain's rock? */
                unsigned behind = VoxelRegions_RoleAt((unsigned)inst->layoutId, x - inst->originX,
                                                      y - 2 - inst->originY);

                builder->lift = VoxelRelief_CellLift(inst, x, y);
                builder->shift = VoxelRelief_CellShift(inst, x, y);
                EmitCell(builder, x, y, part, metatileId,
                         behind == VOXEL_ROLE_CLIFF || behind == VOXEL_ROLE_SHELF, pass);
                /* At the tree's own lift, so both rows meet. */
                if (pass == VOXEL_TREE_CROWNS)
                    EmitMissingRow(builder, inst, x, y, part);
                builder->lift = 0.0f;
                builder->shift = 0.0f;
            }
        }
}

void VoxelTree_EmitBorder(VoxelBuilder *builder, int x0, int y0, int x1, int y1,
                          VoxelTreePass pass)
{
    const VoxelMapInstance *current = VoxelWorld_Instance(0);

    if (!VoxelWorld_UsesTreeSprites(current))
        return;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
        {
            int metatileId, part;
            if (VoxelWorld_GetInstanceAt(x, y) != NULL)
                continue;
            metatileId = VoxelWorld_BorderMetatile(x, y);
            part = VoxelTree_Part(current->secondaryTileset, metatileId);
            if (part >= 0)
                EmitCell(builder, x, y, part, metatileId, false, pass);
        }
}
