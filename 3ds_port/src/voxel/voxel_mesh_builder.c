/*
 * CPU geometry for the voxel overworld. See voxel_mesh_builder.h.
 *
 * The shapes, heights, face culling rules and the camera-side cutaway are
 * ported from the MIT-licensed src/platform/voxel/voxel_mesh.c of
 * pokeemerald-multiplatform (see NOTICE.md). The immediate-mode emission is
 * replaced by the builder, and the reference's 2048x2048 "wall consumed" array
 * is not ported at all: nothing here remembers what it has already emitted, so
 * a cell can be built without building its neighbours first.
 */

#include <math.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "voxel_mesh_builder.h"
#include "voxel_atlas.h"
#include "voxel_tree.h"
#include "voxel_lighting.h"
#include "voxel_sign.h"
#include "voxel_building.h"
#include "voxel_relief.h"

/*
 * Face shades, from the greys the reference gives its untextured side faces.
 * Here every face is textured and the shade multiplies the sample, so the
 * ordering is what matters: a lit top, a bright south face towards the camera,
 * a dark north face away from it.
 */
#define SHADE_TOP   1.00f
#define SHADE_SOUTH 0.60f
#define SHADE_NORTH 0.50f
#define SHADE_WEST  0.65f
#define SHADE_EAST  0.55f

/*
 * Solid furniture parts barely vary with facing. The shades above are meant
 * for metatile art, which is bright; a flat colour that is already dark goes
 * to black when multiplied by 0.55, which is how the first build turned every
 * television into a hole in the floor. Enough variation to read as a box, no
 * more.
 */
#define SOLID_LIT   1.00f
#define SOLID_FRONT 0.94f
#define SOLID_BACK  0.88f
#define SOLID_SIDE  0.91f

static int sWindowX0, sWindowY0, sWindowW, sWindowH;

/*
 * Classification of each tile in the window, computed at most once per
 * rebuild. 0 means "not yet", otherwise shape + 1.
 *
 * VoxelWorld_ClassifyTile is not cheap - it reads the block, the collision of
 * up to five neighbours and the metatile behaviour, which reaches into the
 * asset layer - and the rebuild asks for the same tile about five times: once
 * for itself, and once as the neighbour of each of its four neighbours when
 * they decide which faces to cull.
 */
static uint8_t sShapes[VOXEL_WINDOW_MAX * VOXEL_WINDOW_MAX];

VoxelVisualShape VoxelMesh_Classify(int x, int y)
{
    int cx = x - sWindowX0, cy = y - sWindowY0;
    unsigned slot;
    VoxelVisualShape shape;

    if (cx < 0 || cx >= sWindowW || cy < 0 || cy >= sWindowH)
        return VoxelWorld_ClassifyTile(x, y);
    slot = (unsigned)(cy * VOXEL_WINDOW_MAX + cx);
    if (sShapes[slot] != 0)
        return (VoxelVisualShape)(sShapes[slot] - 1);
    shape = VoxelWorld_ClassifyTile(x, y);
    sShapes[slot] = (uint8_t)(shape + 1);
    return shape;
}

void VoxelBuilder_Init(VoxelBuilder *builder, VoxelVertex *storage, unsigned capacity)
{
    builder->vertices = storage;
    builder->capacity = capacity;
    builder->count = 0;
    builder->dropped = 0;
    builder->atlas = NULL;
    builder->uncovered = 0;
    builder->originX = 0.0f;
    builder->originZ = 0.0f;
    builder->lift = 0.0f;
    builder->shift = 0.0f;
    builder->base = 0.0f;
    builder->artShaded = false;
    builder->lighting = false;
    builder->lightingRefine = true;
    builder->lightingConstant = -1.0f;
    builder->rounded = false;
    builder->vertexFace = false;
    builder->statics = NULL;
    builder->staticCapacity = 0;
    builder->staticCount = 0;
}

void VoxelBuilder_SetOrigin(VoxelBuilder *builder, int originX, int originZ)
{
    builder->originX = (float)originX;
    builder->originZ = (float)originZ;
}

/* Does not clear `uncovered`: a rebuild walks several instances and several
 * atlases, and the caller wants the total. VoxelBuilder_Init clears it. */
void VoxelBuilder_SetAtlas(VoxelBuilder *builder, const struct VoxelAtlasMap *atlas)
{
    builder->atlas = atlas;
}

void VoxelBuilder_Tri(VoxelBuilder *builder, const VoxelVertex *a,
                      const VoxelVertex *b, const VoxelVertex *c)
{
    if (builder->count + 3 > builder->capacity)
    {
        ++builder->dropped;
        return;
    }
    const VoxelVertex *v[3] = { a, b, c };

    for (int i = 0; i < 3; ++i)
    {
        VoxelVertex *out = &builder->vertices[builder->count++];

        *out = *v[i];
        out->x -= builder->originX;
        out->y += builder->lift + builder->base;
        out->z += builder->shift - builder->originZ;
    }
}

void VoxelBuilder_Quad(VoxelBuilder *builder, const VoxelVertex *a, const VoxelVertex *b,
                       const VoxelVertex *c, const VoxelVertex *d)
{
#if CTR_VOXEL_LIGHTING
    if (builder->lighting)
    {
        VoxelLighting_Quad(builder, a, b, c, d);
        return;
    }
#endif
    /* Whole or not at all: half a quad is a sliver along its diagonal. */
    if (builder->count + 6 > builder->capacity)
    {
        builder->dropped += 2;
        return;
    }
    VoxelBuilder_Tri(builder, a, b, c);
    VoxelBuilder_Tri(builder, a, c, d);
}

/* ── Window bookkeeping ─────────────────────────────────────────────────── */

static void WindowReset(int x0, int y0, int x1, int y1)
{
    sWindowX0 = x0;
    sWindowY0 = y0;
    sWindowW = x1 - x0;
    sWindowH = y1 - y0;
    if (sWindowW > VOXEL_WINDOW_MAX) sWindowW = VOXEL_WINDOW_MAX;
    if (sWindowH > VOXEL_WINDOW_MAX) sWindowH = VOXEL_WINDOW_MAX;
    /* Row by row rather than the whole 6.4 KiB: a chunk build opens a
     * rectangle of about 10x18, and clearing the other 6 KiB of a cache it
     * will never touch costs more than the classifications it saves. */
    for (int row = 0; row < sWindowH; ++row)
        memset(&sShapes[row * VOXEL_WINDOW_MAX], 0, (size_t)sWindowW);
}

/* ── UV lookup ──────────────────────────────────────────────────────────── */

/* Returns false when the atlas has no pixels for this tile's metatile; the
 * caller emits nothing, and `uncovered` has already asked for a rebuild. */
/* UVs of an explicit metatile id in the bound atlas. */
static bool MetatileUV(VoxelBuilder *builder, int metatileId,
                       float *u0, float *v0, float *u1, float *v1)
{
    unsigned slot;

    if (metatileId < 0 || metatileId >= (int)VOXEL_METATILE_IDS)
        return false;
    slot = builder->atlas->slotOf[metatileId];
    if (slot == 0 || slot == VOXEL_SLOT_PENDING)
    {
        ++builder->uncovered;
        return false;
    }
    if (slot == VOXEL_SLOT_ABSENT)
        return false;
    VoxelAtlas_SlotUV(slot - 1, u0, v0, u1, v1);
    return true;
}

bool VoxelMesh_MetatileUV(VoxelBuilder *builder, int metatileId,
                          float *u0, float *v0, float *u1, float *v1)
{
    return MetatileUV(builder, metatileId, u0, v0, u1, v1);
}

bool VoxelMesh_TileUV(VoxelBuilder *builder, int x, int y,
                   float *u0, float *v0, float *u1, float *v1)
{
    unsigned slot;
    int metatile;

    /*
     * Nothing stands on a tile that belongs to no map on screen, and indoors
     * VOID is the black out-of-bounds filler. Asking either for its metatile
     * gives an id the atlas was never built with, which used to be counted as
     * a gap - so a wall whose top reached past the building had the atlas
     * rebuilt, every frame, for geometry that must not be drawn at all.
     */
    if (VoxelMesh_Classify(x, y) == VOXEL_SHAPE_VOID)
        return false;
    metatile = VoxelWorld_GetMetatileId(x, y);
    {
        const VoxelMapInstance *inst = VoxelWorld_GetInstanceAt(x, y);
        if (VoxelWorld_UsesTreeSprites(inst))
            metatile = VoxelTree_GroundMetatile(inst->secondaryTileset, metatile);
    }
    slot = builder->atlas->slotOf[metatile];

    if (slot == 0 || slot == VOXEL_SLOT_PENDING)
    {
        ++builder->uncovered;
        return false;
    }
    if (slot == VOXEL_SLOT_ABSENT)
        return false;
    VoxelAtlas_SlotUV(slot - 1, u0, v0, u1, v1);
    return true;
}

/* ── Face emission ──────────────────────────────────────────────────────── */

/* Horizontal face at height `h`, spanning the tile inset from its edges. */
void VoxelMesh_Top(VoxelBuilder *b, float wx, float wz, float h, float inset,
                    float u0, float v0, float u1, float v1, float shade)
{
    float x0 = wx + inset, x1 = wx + 1.0f - inset;
    float z0 = wz + inset, z1 = wz + 1.0f - inset;

    VoxelBuilder_Quad(b,
        &(VoxelVertex){x0, h, z0, u0, v0, shade},
        &(VoxelVertex){x1, h, z0, u1, v0, shade},
        &(VoxelVertex){x1, h, z1, u1, v1, shade},
        &(VoxelVertex){x0, h, z1, u0, v1, shade});
}

void VoxelMesh_South(VoxelBuilder *b, float wx, float wz, float yBottom, float yTop,
                      float inset, float u0, float v0, float u1, float v1)
{
    float x0 = wx + inset, x1 = wx + 1.0f - inset, z = wz + 1.0f - inset;

    VoxelBuilder_Quad(b,
        &(VoxelVertex){x0, yBottom, z, u0, v1, SHADE_SOUTH},
        &(VoxelVertex){x1, yBottom, z, u1, v1, SHADE_SOUTH},
        &(VoxelVertex){x1, yTop,    z, u1, v0, SHADE_SOUTH},
        &(VoxelVertex){x0, yTop,    z, u0, v0, SHADE_SOUTH});
}

void VoxelMesh_North(VoxelBuilder *b, float wx, float wz, float yBottom, float yTop,
                      float inset, float u0, float v0, float u1, float v1)
{
    float x0 = wx + inset, x1 = wx + 1.0f - inset, z = wz + inset;

    VoxelBuilder_Quad(b,
        &(VoxelVertex){x1, yBottom, z, u0, v1, SHADE_NORTH},
        &(VoxelVertex){x0, yBottom, z, u1, v1, SHADE_NORTH},
        &(VoxelVertex){x0, yTop,    z, u1, v0, SHADE_NORTH},
        &(VoxelVertex){x1, yTop,    z, u0, v0, SHADE_NORTH});
}

void VoxelMesh_West(VoxelBuilder *b, float wx, float wz, float yBottom, float yTop,
                     float inset, float u0, float v0, float u1, float v1)
{
    float x = wx + inset, z0 = wz + inset, z1 = wz + 1.0f - inset;

    VoxelBuilder_Quad(b,
        &(VoxelVertex){x, yBottom, z0, u0, v1, SHADE_WEST},
        &(VoxelVertex){x, yBottom, z1, u1, v1, SHADE_WEST},
        &(VoxelVertex){x, yTop,    z1, u1, v0, SHADE_WEST},
        &(VoxelVertex){x, yTop,    z0, u0, v0, SHADE_WEST});
}

void VoxelMesh_East(VoxelBuilder *b, float wx, float wz, float yBottom, float yTop,
                     float inset, float u0, float v0, float u1, float v1)
{
    float x = wx + 1.0f - inset, z0 = wz + inset, z1 = wz + 1.0f - inset;

    VoxelBuilder_Quad(b,
        &(VoxelVertex){x, yBottom, z1, u0, v1, SHADE_EAST},
        &(VoxelVertex){x, yBottom, z0, u1, v1, SHADE_EAST},
        &(VoxelVertex){x, yTop,    z0, u1, v0, SHADE_EAST},
        &(VoxelVertex){x, yTop,    z1, u0, v0, SHADE_EAST});
}

/* ── Single tiles ───────────────────────────────────────────────────────── */

/* Height of the block a shape is drawn as, or 0 for a flat tile.
 * The values are the reference's. Terrain has none: what stands
 * up outdoors is modelled or read off its drawing, never raised from here.
 * Water lies flush with the ground: recessed, its rim drew a line along every
 * shore that the art, which already draws its own bank, does not have. */
static float ShapeHeight(VoxelVisualShape shape)
{
    switch (shape)
    {
    case VOXEL_SHAPE_DECAL:   return 0.02f; /* rugs and mats, just off the floor */
    case VOXEL_SHAPE_COUNTER: return 0.70f;
    default:                  return 0.0f;
    }
}

/* ── Furniture ──────────────────────────────────────────────────────────── */

/* A box of one flat colour, inset from the tile edges. Used for the parts the
 * reference draws untextured: table sides and legs, television bodies. */
static void EmitSolidBox(VoxelBuilder *b, float wx, float wz, VoxelSolidColor color,
                         float x0, float z0, float x1, float z1, float yBottom, float yTop)
{
    float u0, v0, u1, v1;

    /* Written out rather than routed through the face helpers, which span a
     * whole tile; a leg is a tenth of one. */
    VoxelAtlas_SolidUV(color, &u0, &v0, &u1, &v1);
    VoxelBuilder_Quad(b,
        &(VoxelVertex){wx + x0, yTop, wz + z0, u0, v0, SOLID_LIT},
        &(VoxelVertex){wx + x1, yTop, wz + z0, u1, v0, SOLID_LIT},
        &(VoxelVertex){wx + x1, yTop, wz + z1, u1, v1, SOLID_LIT},
        &(VoxelVertex){wx + x0, yTop, wz + z1, u0, v1, SOLID_LIT});
    /* South */
    VoxelBuilder_Quad(b,
        &(VoxelVertex){wx + x0, yBottom, wz + z1, u0, v1, SOLID_FRONT},
        &(VoxelVertex){wx + x1, yBottom, wz + z1, u1, v1, SOLID_FRONT},
        &(VoxelVertex){wx + x1, yTop,    wz + z1, u1, v0, SOLID_FRONT},
        &(VoxelVertex){wx + x0, yTop,    wz + z1, u0, v0, SOLID_FRONT});
    /* North */
    VoxelBuilder_Quad(b,
        &(VoxelVertex){wx + x1, yBottom, wz + z0, u0, v1, SOLID_BACK},
        &(VoxelVertex){wx + x0, yBottom, wz + z0, u1, v1, SOLID_BACK},
        &(VoxelVertex){wx + x0, yTop,    wz + z0, u1, v0, SOLID_BACK},
        &(VoxelVertex){wx + x1, yTop,    wz + z0, u0, v0, SOLID_BACK});
    /* West */
    VoxelBuilder_Quad(b,
        &(VoxelVertex){wx + x0, yBottom, wz + z0, u0, v1, SOLID_SIDE},
        &(VoxelVertex){wx + x0, yBottom, wz + z1, u1, v1, SOLID_SIDE},
        &(VoxelVertex){wx + x0, yTop,    wz + z1, u1, v0, SOLID_SIDE},
        &(VoxelVertex){wx + x0, yTop,    wz + z0, u0, v0, SOLID_SIDE});
    /* East */
    VoxelBuilder_Quad(b,
        &(VoxelVertex){wx + x1, yBottom, wz + z1, u0, v1, SOLID_SIDE},
        &(VoxelVertex){wx + x1, yBottom, wz + z0, u1, v1, SOLID_SIDE},
        &(VoxelVertex){wx + x1, yTop,    wz + z0, u1, v0, SOLID_SIDE},
        &(VoxelVertex){wx + x1, yTop,    wz + z1, u0, v0, SOLID_SIDE});
}

/*
 * Furniture: VoxelWorld_ClassifyTile announces televisions only (TABLE, BED
 * and DECAL are kept in the enum for the scene dumps but never produced).
 */
static void EmitFurniture(VoxelBuilder *builder, int x, int y,
                          float u0, float v0, float u1, float v1)
{
    float wx = (float)x, wz = (float)y;
    /* A television: a stand, a screen facing south, and a dark body. */
    const float stand = 0.3f, top = 1.0f;

    EmitSolidBox(builder, wx, wz, VOXEL_SOLID_GREY,
                 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, stand);
    EmitSolidBox(builder, wx, wz, VOXEL_SOLID_BLACK,
                 0.0f, 0.3f, 1.0f, 0.8f, stand, top);
    /* The metatile's own art is the screen. */
    VoxelBuilder_Quad(builder,
        &(VoxelVertex){wx,        stand, wz + 0.79f, u0, v1, SHADE_SOUTH},
        &(VoxelVertex){wx + 1.0f, stand, wz + 0.79f, u1, v1, SHADE_SOUTH},
        &(VoxelVertex){wx + 1.0f, top,   wz + 0.79f, u1, v0, SHADE_SOUTH},
        &(VoxelVertex){wx,        top,   wz + 0.79f, u0, v0, SHADE_SOUTH});
}

/* A sign or a potted plant: the metatile stood upright across the tile. */
static void EmitUpright(VoxelBuilder *builder, int x, int y,
                        float u0, float v0, float u1, float v1)
{
    float wx = (float)x, wz = (float)y + 0.5f;
    const float height = 1.0f;

    VoxelBuilder_Quad(builder,
        &(VoxelVertex){wx,        0.0f,   wz, u0, v1, SHADE_TOP},
        &(VoxelVertex){wx + 1.0f, 0.0f,   wz, u1, v1, SHADE_TOP},
        &(VoxelVertex){wx + 1.0f, height, wz, u1, v0, SHADE_TOP},
        &(VoxelVertex){wx,        height, wz, u0, v0, SHADE_TOP});
}

static void EmitTile(VoxelBuilder *builder, int x, int y, VoxelVisualShape shape)
{
    float wx = (float)x, wz = (float)y;
    float u0, v0, u1, v1;
    float h = ShapeHeight(shape);

    if (!VoxelMesh_TileUV(builder, x, y, &u0, &v0, &u1, &v1))
        return;

    switch (shape)
    {
    case VOXEL_SHAPE_FURNITURE:
        EmitFurniture(builder, x, y, u0, v0, u1, v1);
        return;
    case VOXEL_SHAPE_SIGN:
        EmitUpright(builder, x, y, u0, v0, u1, v1);
        return;
    default:
        break;
    }

    VoxelMesh_Top(builder, wx, wz, h, 0.0f, u0, v0, u1, v1, SHADE_TOP);
    if (h == 0.0f)
        return;

    {
        VoxelVisualShape south = VoxelMesh_Classify(x, y + 1);
        VoxelVisualShape north = VoxelMesh_Classify(x, y - 1);
        VoxelVisualShape west = VoxelMesh_Classify(x - 1, y);
        VoxelVisualShape east = VoxelMesh_Classify(x + 1, y);

        /* A face is hidden by the void and by a neighbour of the same height. */
        if (south != VOXEL_SHAPE_VOID && south != shape)
            VoxelMesh_South(builder, wx, wz, 0.0f, h, 0.0f, u0, v0, u1, v1);
        if (north != VOXEL_SHAPE_VOID && north != shape)
            VoxelMesh_North(builder, wx, wz, 0.0f, h, 0.0f, u0, v0, u1, v1);
        if (west != VOXEL_SHAPE_VOID && west != shape)
            VoxelMesh_West(builder, wx, wz, 0.0f, h, 0.0f, u0, v0, u1, v1);
        if (east != VOXEL_SHAPE_VOID && east != shape)
            VoxelMesh_East(builder, wx, wz, 0.0f, h, 0.0f, u0, v0, u1, v1);
    }
}

/*
 * A lifted cell: its own metatile laid on its relief lattice, every point of
 * the drawing at (u, h, v + h). Level cells need one quad; slopes take the
 * 4-pixel lattice. Shade 1: the drawing's own light is already in it.
 */
#if CTR_VOXEL_LIGHTING
/*
 * The sun's face term at lattice point (a, c) of cell (x, y), from the slope
 * a tile across: two points either side, reaching into the neighbouring
 * cells. The lattice follows the drawing pixel by pixel - a ledge's berm
 * rises 3 px on one row and 2 on the next - and a quad's own normal turned
 * that into stripes a quarter of a tile apart wherever a slope faces away
 * from the sun. The shape of a mountain or a berm is a tile or more across;
 * over an even span the alternation cancels out and the shape stays.
 *
 * Points (u, h, v + h): the surface's tangents are (1, hu, hu) and
 * (0, hv, 1 + hv), so its upward normal is (-hu, 1 + hv, -hv).
 */
static float ReliefHeight(const int16_t *const cells[3][3], int a, int c)
{
    const int n = VOXEL_RELIEF_SIDE - 1;
    int cx = a < 0 ? 0 : a > n ? 2 : 1, cz = c < 0 ? 0 : c > n ? 2 : 1;
    const int16_t *g = cells[cz][cx];

    if (g == NULL)
        return 0.0f;
    a -= (cx - 1) * n;
    c -= (cz - 1) * n;
    return (float)g[c * VOXEL_RELIEF_SIDE + a];
}

static float ReliefFace(const int16_t *const cells[3][3], int a, int c)
{
    /* pixels over four lattice steps (a tile): tiles per tile */
    float hu = (ReliefHeight(cells, a + 2, c) - ReliefHeight(cells, a - 2, c)) / 16.0f;
    float hv = (ReliefHeight(cells, a, c + 2) - ReliefHeight(cells, a, c - 2)) / 16.0f;

    return VoxelLighting_Face(-hu, 1.0f + hv, -hv);
}
#endif

/* A cut tile's flat ground, on the relief's own lattice: every edge it
 * shares with a relief cell beside it has that cell's points on it, so no
 * T-junction opens a crack between them. */
static bool OnFloor(const int16_t *g, int i, int j, int floor)
{
    return g[j * VOXEL_RELIEF_SIDE + i] <= floor && g[j * VOXEL_RELIEF_SIDE + i + 1] <= floor
        && g[(j + 1) * VOXEL_RELIEF_SIDE + i] <= floor && g[(j + 1) * VOXEL_RELIEF_SIDE + i + 1] <= floor;
}

/* Where a cut tile's ground runs on under the rock beside it. */
static const int8_t kCutFill[][2] = { { 0, 1 }, { -1, 1 }, { 1, 1 }, { -1, 0 }, { 1, 0 } };

/* A cut tile's drawing, flat at its foot (the builder's lift), in the
 * relief's own grid: its quads meet the relief's at every lattice point and
 * leave no crack between them. */
/* Does the 4x4 pixel block (i, j) of a cut tile have rock in it? `rows` are
 * the tile's background bits (VoxelRelief_CutVariant): a clear bit is rock. */
static int CutRockPixels(const uint8_t *rows, int i, int j)
{
    int rock = 0;

    for (int py = 4 * j; py < 4 * j + 4; ++py)
    {
        unsigned bits = (unsigned)rows[2 * py] | ((unsigned)rows[2 * py + 1] << 8);

        for (int px = 4 * i; px < 4 * i + 4; ++px)
            if (!((bits >> px) & 1u))
                ++rock;
    }
    return rock;
}

/*
 * A cut tile's flat layer, at its foot. Where a block of the drawing has rock
 * in it the layer is `gu0..` - the plain ground behind it - and where it is all
 * background, the tile's own drawing (a shore's foam, a coast's edge): the
 * rock the drawing carries, flat at the foot, was a second copy of the lifted
 * rock, which the camera's perspective moves off it - a wedge stuck to the
 * ground beside the mountain. With no rows or no ground, the whole tile.
 */
static void EmitCutGround(VoxelBuilder *b, int x, int y, float u0, float v0, float u1, float v1,
                          const uint8_t *rows, const float *gnd, const int16_t *g, int floor)
{
    const int n = VOXEL_RELIEF_SIDE - 1;
    int lo = INT16_MAX, hi = INT16_MIN;
    /* a mountain's rock, a level or more of it; a ledge's lip keeps its
     * whole drawing flat under it, as it always had */
    bool mountain;

    for (int k = 0; g != NULL && k < VOXEL_RELIEF_SIDE * VOXEL_RELIEF_SIDE; ++k)
    {
        if (g[k] < lo) lo = g[k];
        if (g[k] > hi) hi = g[k];
    }
    mountain = g != NULL && hi - lo >= 16;
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i)
        {
            float xa = x + i / (float)n, xb = x + (i + 1) / (float)n;
            float za = y + j / (float)n, zb = y + (j + 1) / (float)n;
            float ua, ub, va, vb;
            int rock = rows != NULL ? CutRockPixels(rows, i, j) : 0;

            /* a block that is mostly rock lying on the foot stays its own
             * drawing: it is the flank itself there (EmitRelief leaves it to
             * this layer). Anywhere the rock is lifted, its copy flat at the
             * foot is a second outline beside the lifted one - a row of grey
             * teeth along a flank's foot -, and the ground is drawn */
            if (rows != NULL && gnd != NULL && rock > 0
             && (rock < 12 || (mountain && !OnFloor(g, i, j, floor))))
            {
                ua = gnd[0] + (gnd[2] - gnd[0]) * i / n; ub = gnd[0] + (gnd[2] - gnd[0]) * (i + 1) / n;
                va = gnd[1] + (gnd[3] - gnd[1]) * j / n; vb = gnd[1] + (gnd[3] - gnd[1]) * (j + 1) / n;
            }
            else
            {
                ua = u0 + (u1 - u0) * i / n; ub = u0 + (u1 - u0) * (i + 1) / n;
                va = v0 + (v1 - v0) * j / n; vb = v0 + (v1 - v0) * (j + 1) / n;
            }

            VoxelBuilder_Quad(b, &(VoxelVertex){xa, 0.0f, za, ua, va, SHADE_TOP},
                              &(VoxelVertex){xb, 0.0f, za, ub, va, SHADE_TOP},
                              &(VoxelVertex){xb, 0.0f, zb, ub, vb, SHADE_TOP},
                              &(VoxelVertex){xa, 0.0f, zb, ua, vb, SHADE_TOP});
        }
}

/* A cut tile cut along its diagonals (VoxelRelief_CutDiagonals). */
typedef VoxelCutDiagonals CutDiagonals;

/* A triangle of the cell, its vertices at pixels (X, Y) of the tile, cut by
 * the diagonals: what is left drawn as quads (a fan of the convex piece),
 * an odd one left over as a triangle. */
static void EmitCutTri(VoxelBuilder *b, const VoxelVertex *v0, const VoxelVertex *v1,
                       const VoxelVertex *v2, const float px[3][2], const CutDiagonals *cut)
{
    VoxelVertex poly[2][3 + 4];
    float pp[2][3 + 4][2];
    int n = 3, cur = 0;

    poly[0][0] = *v0; poly[0][1] = *v1; poly[0][2] = *v2;
    for (int i = 0; i < 3; ++i)
    {
        pp[0][i][0] = px[i][0];
        pp[0][i][1] = px[i][1];
    }
    for (int k = 0; k < cut->count && n > 0; ++k)
    {
        int m = 0, nxt = cur ^ 1;

        for (int i = 0; i < n; ++i)
        {
            int j = (i + 1) % n;
            float di = cut->a[k] * pp[cur][i][0] + cut->b[k] * pp[cur][i][1] - cut->c[k];
            float dj = cut->a[k] * pp[cur][j][0] + cut->b[k] * pp[cur][j][1] - cut->c[k];

            if (di <= 0.0f)
            {
                poly[nxt][m] = poly[cur][i];
                pp[nxt][m][0] = pp[cur][i][0];
                pp[nxt][m][1] = pp[cur][i][1];
                ++m;
            }
            if ((di < 0.0f && dj > 0.0f) || (di > 0.0f && dj < 0.0f))
            {
                float t = di / (di - dj);
                const VoxelVertex *p = &poly[cur][i], *q = &poly[cur][j];

                poly[nxt][m] = (VoxelVertex){ p->x + (q->x - p->x) * t, p->y + (q->y - p->y) * t,
                                              p->z + (q->z - p->z) * t, p->u + (q->u - p->u) * t,
                                              p->v + (q->v - p->v) * t,
                                              p->shade + (q->shade - p->shade) * t };
                pp[nxt][m][0] = pp[cur][i][0] + (pp[cur][j][0] - pp[cur][i][0]) * t;
                pp[nxt][m][1] = pp[cur][i][1] + (pp[cur][j][1] - pp[cur][i][1]) * t;
                ++m;
            }
        }
        n = m;
        cur = nxt;
    }
    for (int i = 1; i + 1 < n; i += 2)
    {
        if (i + 2 < n)
            VoxelBuilder_Quad(b, &poly[cur][0], &poly[cur][i], &poly[cur][i + 1], &poly[cur][i + 2]);
        else
        {
#if CTR_VOXEL_LIGHTING
            if (b->lighting)
                VoxelLighting_Tri(b, &poly[cur][0], &poly[cur][i], &poly[cur][i + 1]);
            else
#endif
                VoxelBuilder_Tri(b, &poly[cur][0], &poly[cur][i], &poly[cur][i + 1]);
        }
    }
}

/* A quad of the cell (p0..p3 at pixels px), split into two triangles as
 * VoxelBuilder_Quad splits it, each cut by the diagonals; whole when none
 * of them reaches it. */
static void EmitCutQuad(VoxelBuilder *b, const VoxelVertex *p0, const VoxelVertex *p1,
                        const VoxelVertex *p2, const VoxelVertex *p3, const float px[4][2],
                        const CutDiagonals *cut)
{
    bool inside = true, outside = false;

    for (int k = 0; k < cut->count; ++k)
    {
        int in = 0;

        for (int i = 0; i < 4; ++i)
            in += cut->a[k] * px[i][0] + cut->b[k] * px[i][1] <= cut->c[k];
        inside = inside && in == 4;
        outside = outside || in == 0;
    }
    if (outside)
        return;
    if (inside)
    {
        VoxelBuilder_Quad(b, p0, p1, p2, p3);
        return;
    }
    {
        const float t0[3][2] = { { px[0][0], px[0][1] }, { px[1][0], px[1][1] }, { px[2][0], px[2][1] } };
        const float t1[3][2] = { { px[0][0], px[0][1] }, { px[2][0], px[2][1] }, { px[3][0], px[3][1] } };

        EmitCutTri(b, p0, p1, p2, t0, cut);
        EmitCutTri(b, p0, p2, p3, t1, cut);
    }
}

/* `metatile` >= 0 draws the cell with that atlas id (a cut tile's). With
 * `onFloor`, every quad lying on `floor` (pixels) is left out: the ground
 * drawn flat under it is that quad already, and the two would fight for
 * its depth. `rows`: the cut tile's background bits, cut along its
 * diagonals where it is a polygon (VoxelRelief_CutDiagonals). */
static void EmitRelief(VoxelBuilder *b, int x, int y, const int16_t *g, const int16_t *s,
                       int artY, int metatile, bool onFloor, int floor, const uint8_t *rows)
{
    const int n = VOXEL_RELIEF_SIDE - 1;
    float u0, v0, u1, v1;
    CutDiagonals cut;

    if (metatile >= 0 ? !MetatileUV(b, metatile, &u0, &v0, &u1, &v1)
                      : !VoxelMesh_TileUV(b, x, artY, &u0, &v0, &u1, &v1))
        return;
    VoxelRelief_CutDiagonals(rows, &cut);
    /* The drawing's own light is in it, a face's included: shadows only. */
    b->artShaded = true;
    if (!VoxelRelief_IsSlope(g) && !VoxelRelief_IsSlope(s))
    {
        float h = g[0] / 16.0f, d = s[0] / 16.0f;
        static const float kWhole[4][2] = { { 0, 0 }, { 16, 0 }, { 16, 16 }, { 0, 16 } };

        /* Through the lit quad, as all ground is: what stands in the sun
         * (a house beside the mountain) shades it. */
        EmitCutQuad(b, &(VoxelVertex){x, h, y + d, u0, v0, SHADE_TOP},
                    &(VoxelVertex){x + 1, h, y + d, u1, v0, SHADE_TOP},
                    &(VoxelVertex){x + 1, h, y + 1 + d, u1, v1, SHADE_TOP},
                    &(VoxelVertex){x, h, y + 1 + d, u0, v1, SHADE_TOP}, kWhole, &cut);
        b->artShaded = false;
        return;
    }
#if CTR_VOXEL_LIGHTING
    const VoxelMapInstance *inst = VoxelWorld_GetInstanceAt(x, y);
    const int16_t *cells[3][3];
    float face[VOXEL_RELIEF_SIDE * VOXEL_RELIEF_SIDE];

    /* The face table is read only by a lit build: an unlit one (indoors)
     * skips its 8 neighbour lookups and 25 normals. */
    if (b->lighting)
    {
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx)
                cells[dz + 1][dx + 1] = dx == 0 && dz == 0 ? g
                                      : VoxelWorld_GetInstanceAt(x + dx, y + dz) == inst
                                      ? VoxelRelief_Cell(inst, x + dx, y + dz) : NULL;
        for (int c = 0; c <= n; ++c)
            for (int a = 0; a <= n; ++a)
                face[c * VOXEL_RELIEF_SIDE + a] = ReliefFace(cells, a, c);
    }
    b->vertexFace = b->lighting;
#endif
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i)
        {
            VoxelVertex p[4];
            static const int di[4] = { 0, 1, 1, 0 }, dj[4] = { 0, 0, 1, 1 };

            float raise = 0.0f;

            if (onFloor && OnFloor(g, i, j, floor))
            {
                /* the flat layer under rock is plain ground (EmitCutGround):
                 * rock on the foot is drawn, half a pixel over it */
                int rock = rows != NULL ? CutRockPixels(rows, i, j) : 0;

                if (rock == 0 || rock >= 12)
                    continue;       /* all ground, or the flat layer is the rock itself */
                raise = 0.5f / 16.0f;
            }

            for (int k = 0; k < 4; ++k)
            {
                int a = i + di[k], c = j + dj[k];
                float h = g[c * VOXEL_RELIEF_SIDE + a] / 16.0f;
                float d = s[c * VOXEL_RELIEF_SIDE + a] / 16.0f;
                float shade = SHADE_TOP;

#if CTR_VOXEL_LIGHTING
                if (b->vertexFace)
                    shade = face[c * VOXEL_RELIEF_SIDE + a];
#endif
                p[k] = (VoxelVertex){ x + a / (float)n, h + raise, y + c / (float)n + d,
                                      u0 + (u1 - u0) * a / n, v0 + (v1 - v0) * c / n, shade };
            }
            /* Split along the crease: a mountain's corner is two planes
             * meeting on a diagonal of the cell (gen_voxel_relief.py), and a
             * quad on it has its two corners off the crease level with each
             * other - cut the other way it would fold across the crease. */
            float px[4][2];

            for (int k = 0; k < 4; ++k)
            {
                px[k][0] = 16.0f * (i + di[k]) / n;
                px[k][1] = 16.0f * (j + dj[k]) / n;
            }
            if (g[j * VOXEL_RELIEF_SIDE + i] == g[(j + 1) * VOXEL_RELIEF_SIDE + i + 1]
             && g[j * VOXEL_RELIEF_SIDE + i + 1] != g[(j + 1) * VOXEL_RELIEF_SIDE + i])
            {
                const float pr[4][2] = { { px[1][0], px[1][1] }, { px[2][0], px[2][1] },
                                         { px[3][0], px[3][1] }, { px[0][0], px[0][1] } };

                EmitCutQuad(b, &p[1], &p[2], &p[3], &p[0], pr, &cut);
            }
            else
                EmitCutQuad(b, &p[0], &p[1], &p[2], &p[3], px, &cut);
        }
    b->vertexFace = false;
    b->artShaded = false;
}

/* ── Entry point ────────────────────────────────────────────────────────── */

void VoxelMesh_BeginWindow(int x0, int y0, int x1, int y1)
{
    WindowReset(x0, y0, x1, y1);
}

/*
 * Where the world does not close up, two maps meet at different levels
 * (voxel_relief.h: a map stands at its base) and the higher one's edge is a
 * step down to the other. It is closed by a skirt of the edge cell's own
 * drawing, hanging from the edge to the ground beyond it, so the seam is a
 * low wall and never a gap onto the clear colour.
 */
static bool BorderWaterLevel(int x, int y, float *level, float *shift);

static void EmitSeamSkirt(VoxelBuilder *b, const VoxelMapInstance *inst, int x, int y)
{
    static const int kDir[4][2] = { {0, -1}, {0, 1}, {-1, 0}, {1, 0} };
    float top = VoxelRelief_CellLift(inst, x, y), shift = VoxelRelief_CellShift(inst, x, y);
    float wx = (float)x, wz = (float)y + shift, southZ = wz;
    float u0, v0, u1, v1;
    bool uv = false;

    for (int d = 0; d < 4; ++d)
    {
        int nx = x + kDir[d][0], ny = y + kDir[d][1];
        const VoxelMapInstance *other;
        float low, h, du, dv;

        if (nx >= inst->originX && ny >= inst->originY
         && nx < inst->originX + inst->width && ny < inst->originY + inst->height)
            continue;
        other = VoxelWorld_GetInstanceAt(nx, ny);
        if (other == NULL && !inst->indoor)
        {
            /*
             * The belt past the last map (VoxelMesh_EmitBorder) stands at the
             * base of instance 0. An edge lowered under it - Rustboro's sea,
             * Route 117's wood - left a step up to it with nothing in it, a
             * band of the clear colour along the map's edge. It is closed by
             * a wall of the edge cell's own drawing, from the edge up to the
             * belt. Not along the south edge of a stood belt: there the belt
             * stands in front (a belt of water stands nothing, see below).
             * Its foot is the edge as the cell draws it, point by point of
             * the lattice: a rock or a slope at the edge stands each point at
             * its own height, and that far south (voxel_relief.h), and a foot
             * level at the centre's height left slivers of the clear under
             * it. Its top is where the belt stands, unmoved. Not round a
             * belt laid as the drawing (VoxelRelief_Belt): that meets every
             * edge at its height, and the map's faces close the steps.
             */
            /*
             * A belt of water is flat and stands nothing in front, so it is
             * closed on every side, the south one too, and both ways: an edge
             * over its plane (BorderWaterLevel) hangs a skirt down to it, an
             * edge under it stands one up. Each skirt runs from the edge's
             * lattice points to the plane's own edge - its level and its
             * shift south - so a drawn map's skirt lies along the 45-degree
             * view, edge on, and the drawing reads as before.
             */
            const VoxelMapInstance *ring = VoxelWorld_Instance(0);
            float belt = ring != NULL ? VoxelRelief_Base(ring) - VoxelRelief_Base(inst) : 0.0f;
            const int16_t *grid = VoxelRelief_Cell(inst, x, y);
            const int16_t *depth = VoxelRelief_Depth(inst, x, y);
            const unsigned last = VOXEL_RELIEF_SIDE - 1;
            float footH[VOXEL_RELIEF_SIDE], footZ[VOXEL_RELIEF_SIDE], along[VOXEL_RELIEF_SIDE];
            float beltZ[VOXEL_RELIEF_SIDE];
            float lowest, highest, level = 0.0f, shift = 0.0f, skew = 0.0f;
            bool water;

            if (ring == NULL || VoxelRelief_Belt(ring) != 0)
                continue;
            water = BorderWaterLevel(nx, ny, &level, &shift);
            /* a stood belt hides the south edge behind its own face */
            if (d == 1 && !water)
                continue;
            belt += level;
            lowest = highest = belt;
            for (unsigned k = 0; k < VOXEL_RELIEF_SIDE; ++k)
            {
                /* the north row, the south row, or the west or east column */
                unsigned i = d == 0 ? k
                           : d == 1 ? last * VOXEL_RELIEF_SIDE + k
                           : k * VOXEL_RELIEF_SIDE + (d == 2 ? 0 : last);
                float line = (float)y + (d == 0 ? 0.0f : d == 1 ? 1.0f : 0.0f)
                           + (d >= 2 ? k / (float)last : 0.0f);

                along[k] = k / (float)last;
                footH[k] = (grid ? grid[i] : 0) / 16.0f;
                footZ[k] = line + (depth ? depth[i] : 0) / 16.0f;
                /* where the belt's cell ends: unmoved for a stood belt */
                beltZ[k] = d == 0 && !water ? (float)y : line + shift;
                if (footH[k] < lowest)
                    lowest = footH[k];
                if (footH[k] > highest)
                    highest = footH[k];
                if (fabsf(footZ[k] - beltZ[k]) > skew)
                    skew = fabsf(footZ[k] - beltZ[k]);
            }
            if (water ? (lowest >= belt - 0.05f && highest <= belt + 0.05f && skew <= 0.02f)
                      : lowest >= belt - 0.05f)
                continue;
            if (!uv && !(uv = VoxelMesh_TileUV(b, x, y, &u0, &v0, &u1, &v1)))
                return;
            for (unsigned k = 0; k < last; ++k)
            {
                float hA = belt - footH[k], hB = belt - footH[k + 1];
                /* a skirt hanging down reads its drawing as far as it falls */
                float mA = water ? fabsf(hA) : hA, mB = water ? fabsf(hB) : hB;
                float fA = mA < 1.0f ? mA : 1.0f, fB = mB < 1.0f ? mB : 1.0f;

                if (water ? (fabsf(hA) <= 0.05f && fabsf(hB) <= 0.05f
                             && fabsf(footZ[k] - beltZ[k]) <= 0.02f
                             && fabsf(footZ[k + 1] - beltZ[k + 1]) <= 0.02f)
                          : (hA <= 0.05f && hB <= 0.05f))
                    continue;
                if (d == 0) /* faces south, on the map's north edge, where the belt's face ends */
                {
                    float xa = wx + along[k], xb = wx + along[k + 1];
                    float ua = u0 + (u1 - u0) * along[k], ub = u0 + (u1 - u0) * along[k + 1];

                    VoxelBuilder_Quad(b,
                        &(VoxelVertex){xa, footH[k],     footZ[k],     ua, v0 + (v1 - v0) * fA, SHADE_SOUTH},
                        &(VoxelVertex){xb, footH[k + 1], footZ[k + 1], ub, v0 + (v1 - v0) * fB, SHADE_SOUTH},
                        &(VoxelVertex){xb, belt,         beltZ[k + 1], ub, v0,                  SHADE_SOUTH},
                        &(VoxelVertex){xa, belt,         beltZ[k],     ua, v0,                  SHADE_SOUTH});
                }
                else if (d == 1) /* the south edge over a belt of water */
                {
                    float xa = wx + along[k], xb = wx + along[k + 1];
                    float ua = u0 + (u1 - u0) * along[k], ub = u0 + (u1 - u0) * along[k + 1];

                    VoxelBuilder_Quad(b,
                        &(VoxelVertex){xb, footH[k + 1], footZ[k + 1], ub, v1,                  SHADE_SOUTH},
                        &(VoxelVertex){xa, footH[k],     footZ[k],     ua, v1,                  SHADE_SOUTH},
                        &(VoxelVertex){xa, belt,         beltZ[k],     ua, v1 - (v1 - v0) * fA, SHADE_SOUTH},
                        &(VoxelVertex){xb, belt,         beltZ[k + 1], ub, v1 - (v1 - v0) * fB, SHADE_SOUTH});
                }
                else
                {
                    float va = v0 + (v1 - v0) * along[k], vb = v0 + (v1 - v0) * along[k + 1];

                    if (d == 2) /* faces east, on the west edge */
                        VoxelBuilder_Quad(b,
                            &(VoxelVertex){wx, belt,         beltZ[k + 1], u0,                  vb, SHADE_EAST},
                            &(VoxelVertex){wx, belt,         beltZ[k],     u0,                  va, SHADE_EAST},
                            &(VoxelVertex){wx, footH[k],     footZ[k],     u0 + (u1 - u0) * fA, va, SHADE_EAST},
                            &(VoxelVertex){wx, footH[k + 1], footZ[k + 1], u0 + (u1 - u0) * fB, vb, SHADE_EAST});
                    else /* faces west, on the east edge */
                        VoxelBuilder_Quad(b,
                            &(VoxelVertex){wx + 1.0f, belt,         beltZ[k],     u1,                  va, SHADE_WEST},
                            &(VoxelVertex){wx + 1.0f, belt,         beltZ[k + 1], u1,                  vb, SHADE_WEST},
                            &(VoxelVertex){wx + 1.0f, footH[k + 1], footZ[k + 1], u1 - (u1 - u0) * fB, vb, SHADE_WEST},
                            &(VoxelVertex){wx + 1.0f, footH[k],     footZ[k],     u1 - (u1 - u0) * fA, va, SHADE_WEST});
                }
            }
            continue;
        }
        if (other == NULL || other == inst || other->indoor)
            continue;
        low = VoxelRelief_LiftAt((float)nx + 0.5f, (float)ny + 0.5f) - VoxelRelief_Base(inst);
        h = top - low;
        if (d == 1)
        {
            /*
             * A point stands as far south as it is high over its map's base
             * (voxel_relief.h), and a map's base lifts it straight up. So two
             * maps can meet at one level with their edges a row apart: Route
             * 104's sea, a level under its land, ends a cell north of where
             * Route 105's, the same sea on a base a level down, begins; so do
             * the forest and the ridge's band on the seam beside it. The edge
             * cell's drawing goes on south, 1:1, from our edge to theirs at
             * every point of the lattice - the water's own animated tile, a
             * band's slope as it falls -, never a gap onto the clear colour.
             */
            const int16_t *ours = VoxelRelief_Depth(inst, x, y);
            const int16_t *theirs = VoxelRelief_Depth(other, nx, ny);
            const int16_t *oursH = VoxelRelief_Cell(inst, x, y);
            const int16_t *theirsH = VoxelRelief_Cell(other, nx, ny);
            /* their heights over our base, in pixels */
            float rebase = (VoxelRelief_Base(other) - VoxelRelief_Base(inst)) * 16.0f;
            const unsigned last = (VOXEL_RELIEF_SIDE - 1) * VOXEL_RELIEF_SIDE;
            float hA[VOXEL_RELIEF_SIDE], zA[VOXEL_RELIEF_SIDE];
            float hB[VOXEL_RELIEF_SIDE], zB[VOXEL_RELIEF_SIDE];
            float gap = 0.0f;
            bool meets = true;

            /* our south edge and their north edge, point by point of the
             * lattice: the same ground (a band's slope too) a row apart */
            for (unsigned k = 0; k < VOXEL_RELIEF_SIDE; ++k)
            {
                hA[k] = oursH ? oursH[last + k] : 0.0f;
                zA[k] = (float)y + 1.0f + (ours ? ours[last + k] : 0) / 16.0f;
                hB[k] = (theirsH ? theirsH[k] : 0.0f) + rebase;
                zB[k] = (float)y + 1.0f + (theirs ? theirs[k] : 0) / 16.0f;
                if (zB[k] - zA[k] > gap)
                    gap = zB[k] - zA[k];
                if (fabsf(hB[k] - hA[k]) > 1.0f)
                    meets = false;
            }
            if (gap > 0.02f && meets)
            {
                float len = gap < 1.0f ? gap : 1.0f;
                float vb;

                if (!uv && !(uv = VoxelMesh_TileUV(b, x, y, &u0, &v0, &u1, &v1)))
                    return;
                vb = v0 + (v1 - v0) * len;
                for (unsigned k = 0; k + 1 < VOXEL_RELIEF_SIDE; ++k)
                {
                    float xa = wx + k / (float)(VOXEL_RELIEF_SIDE - 1);
                    float xb = wx + (k + 1) / (float)(VOXEL_RELIEF_SIDE - 1);
                    float ua = u0 + (u1 - u0) * k / (float)(VOXEL_RELIEF_SIDE - 1);
                    float ub = u0 + (u1 - u0) * (k + 1) / (float)(VOXEL_RELIEF_SIDE - 1);

                    VoxelBuilder_Quad(b,
                        &(VoxelVertex){xa, hA[k] / 16.0f,     zA[k],     ua, v0, SHADE_TOP},
                        &(VoxelVertex){xb, hA[k + 1] / 16.0f, zA[k + 1], ub, v0, SHADE_TOP},
                        &(VoxelVertex){xb, hB[k + 1] / 16.0f, zB[k + 1], ub, vb, SHADE_TOP},
                        &(VoxelVertex){xa, hB[k] / 16.0f,     zB[k],     ua, vb, SHADE_TOP});
                }
                /* a step down is hung from where the ground now ends */
                southZ += gap;
            }
        }
        if (h <= 0.05f)
            continue;
        if (!uv && !(uv = VoxelMesh_TileUV(b, x, y, &u0, &v0, &u1, &v1)))
            return;
        du = (u1 - u0) * (h < 1.0f ? h : 1.0f);
        dv = (v1 - v0) * (h < 1.0f ? h : 1.0f);
        if (d == 0) /* faces north, at the north edge */
            VoxelBuilder_Quad(b,
                &(VoxelVertex){wx + 1.0f, top, wz, u1, v0,      SHADE_NORTH},
                &(VoxelVertex){wx,        top, wz, u0, v0,      SHADE_NORTH},
                &(VoxelVertex){wx,        low, wz, u0, v0 + dv, SHADE_NORTH},
                &(VoxelVertex){wx + 1.0f, low, wz, u1, v0 + dv, SHADE_NORTH});
        else if (d == 1) /* faces south, at the south edge */
            VoxelBuilder_Quad(b,
                &(VoxelVertex){wx,        top, southZ + 1.0f, u0, v1,      SHADE_SOUTH},
                &(VoxelVertex){wx + 1.0f, top, southZ + 1.0f, u1, v1,      SHADE_SOUTH},
                &(VoxelVertex){wx + 1.0f, low, southZ + 1.0f, u1, v1 - dv, SHADE_SOUTH},
                &(VoxelVertex){wx,        low, southZ + 1.0f, u0, v1 - dv, SHADE_SOUTH});
        else if (d == 2) /* faces west, at the west edge */
            VoxelBuilder_Quad(b,
                &(VoxelVertex){wx, top, wz,        u0,      v0, SHADE_WEST},
                &(VoxelVertex){wx, top, wz + 1.0f, u0,      v1, SHADE_WEST},
                &(VoxelVertex){wx, low, wz + 1.0f, u0 + du, v1, SHADE_WEST},
                &(VoxelVertex){wx, low, wz,        u0 + du, v0, SHADE_WEST});
        else /* faces east, at the east edge */
            VoxelBuilder_Quad(b,
                &(VoxelVertex){wx + 1.0f, top, wz + 1.0f, u1,      v1, SHADE_EAST},
                &(VoxelVertex){wx + 1.0f, top, wz,        u1,      v0, SHADE_EAST},
                &(VoxelVertex){wx + 1.0f, low, wz,        u1 - du, v0, SHADE_EAST},
                &(VoxelVertex){wx + 1.0f, low, wz + 1.0f, u1 - du, v1, SHADE_EAST});
    }
}

/*
 * A cell's cliffs (voxel_relief.h), wherever its west, east or south edge
 * stands over its neighbour's. Down the west and east the rock's own edge
 * goes on to the ground, each pixel of the drawing's edge column drawn down
 * as far as the step falls: the camera sees such a wall edge on, and what
 * it shows is the rock's outline going down, never a sliver of anything
 * else (a cut tile's clear background stays clear down it too). Down the
 * south, the mountain's face (`wall`, or none for VOXEL_RELIEF_NO_FACE), a
 * level of it a level of the wall. Never the north edge, the back.
 */
static void EmitCliffWalls(VoxelBuilder *b, const VoxelMapInstance *inst, int x, int y,
                           const int16_t *g, const int16_t *s, int wall, int art, int artY,
                           unsigned sides)
{
    static const int16_t kLevel[VOXEL_RELIEF_SIDE * VOXEL_RELIEF_SIDE];
    const int n = VOXEL_RELIEF_SIDE - 1, R = VOXEL_RELIEF_SIDE;
    float f0, g0, f1, g1;      /* the face */
    float o0, p0, o1, p1;      /* the cell's own drawing */
    bool face = wall != VOXEL_RELIEF_NO_FACE && MetatileUV(b, wall, &f0, &g0, &f1, &g1);
    bool own = art >= 0 ? MetatileUV(b, art, &o0, &p0, &o1, &p1)
                        : VoxelMesh_TileUV(b, x, artY, &o0, &p0, &o1, &p1);
    /* a hand-built block: its west and east walls are the face as well */
    bool blockWalls = face && (sides & VOXEL_RELIEF_BLOCK_WALLS) != 0;

    for (int d = 0; d < 3; ++d)
    {
        int nx = x + (d == 0 ? -1 : d == 1 ? 1 : 0), ny = y + (d == 2 ? 1 : 0);
        const int16_t *o = NULL, *os = NULL;

        if (d == 2 ? !face : !(own || blockWalls))
            continue;
        if (VoxelWorld_GetInstanceAt(nx, ny) == inst)
        {
            o = VoxelRelief_Cell(inst, nx, ny);
            os = o != NULL ? VoxelRelief_Depth(inst, nx, ny) : NULL;
        }
        if (o == NULL)
            o = os = kLevel;
        if (d < 2 && !(sides & VOXEL_RELIEF_THIN_DECK) && o != kLevel)
        {
            /* beside a bridge's thin deck: the water under it runs up to
             * this cell's side, walled down to the water (seen past the
             * deck's edge, the gap under it showed the void) */
            static int16_t under[VOXEL_RELIEF_SIDE * VOXEL_RELIEF_SIDE];
            float nfoot;
            int ng, nw;
            unsigned ns = 0;

            if (VoxelRelief_Cut(inst, nx, ny, &nfoot, &ng, &nw, &ns) != -1 && (ns & VOXEL_RELIEF_THIN_DECK))
            {
                for (int i = 0; i < R * R; ++i)
                    under[i] = (int16_t)lroundf(nfoot * 16.0f);
                o = under;
            }
        }
        for (int k = 0; k < n; ++k)
        {
            /* ours at a and b, theirs at c and d, along the edge */
            int ia, ib, ic, id;
            float ta, tb, ba, bb, za, zb, zc, zd, xa, xb;

            if (d == 0)
            {
                ia = k * R; ib = (k + 1) * R; ic = k * R + n; id = (k + 1) * R + n;
            }
            else if (d == 1)
            {
                ia = k * R + n; ib = (k + 1) * R + n; ic = k * R; id = (k + 1) * R;
            }
            else
            {
                ia = n * R + k; ib = n * R + k + 1; ic = k; id = k + 1;
            }
            if (g[ia] - o[ic] <= 0 && g[ib] - o[id] <= 0)
                continue;
            /* The edge column's pixels of this stretch that the tile clears
             * are the ground beside the rock, not rock: no wall stands on
             * them (a flank's sand corner hung from the rock as a flat
             * yellow triangle) */
            if (d < 2 && !blockWalls && !((sides >> ((d == 0 ? 0u : 4u) + (unsigned)k)) & 1u))
                continue;
            ta = g[ia] / 16.0f; tb = g[ib] / 16.0f;
            ba = (g[ia] > o[ic] ? o[ic] : g[ia]) / 16.0f;
            bb = (g[ib] > o[id] ? o[id] : g[ib]) / 16.0f;
            if (sides & VOXEL_RELIEF_THIN_DECK)
            {
                ba = fmaxf(ba, ta - VOXEL_RELIEF_DECK_EDGE);
                bb = fmaxf(bb, tb - VOXEL_RELIEF_DECK_EDGE);
            }
            if (d == 2)
            {
                float ua = f0 + (f1 - f0) * k / n, ub = f0 + (f1 - f0) * (k + 1) / n;
                float dva = (g1 - g0) * fminf(1.0f, ta - ba), dvb = (g1 - g0) * fminf(1.0f, tb - bb);

                xa = x + k / (float)n; xb = x + (k + 1) / (float)n;
                za = y + 1 + s[ia] / 16.0f; zb = y + 1 + s[ib] / 16.0f;
                zc = y + 1 + os[ic] / 16.0f; zd = y + 1 + os[id] / 16.0f;
                VoxelBuilder_Quad(b, &(VoxelVertex){xa, ta, za, ua, g0, SHADE_SOUTH},
                                  &(VoxelVertex){xb, tb, zb, ub, g0, SHADE_SOUTH},
                                  &(VoxelVertex){xb, bb, zd, ub, g0 + dvb, SHADE_SOUTH},
                                  &(VoxelVertex){xa, ba, zc, ua, g0 + dva, SHADE_SOUTH});
            }
            else if (blockWalls)
            {
                /* the face down the side, as down the south, laid on the
                 * wall where it stands in the world: across it by its z,
                 * down it from the level its top is at (a taller wall
                 * stretches it). A block's side beside a face is two
                 * pieces (the rows of two cells): mapped by the world they
                 * read as the one wall they are. Seen from the east the
                 * south is on the left, so that one runs the other way. */
                float xe = (float)(d == 0 ? x : x + 1);
                float shade = d == 0 ? SHADE_WEST : SHADE_EAST;
                float zm, yt;

                za = y + k / (float)n + s[ia] / 16.0f;
                zb = y + (k + 1) / (float)n + s[ib] / 16.0f;
                zc = y + k / (float)n + os[ic] / 16.0f;
                zd = y + (k + 1) / (float)n + os[id] / 16.0f;
                zm = floorf((za + zb + zc + zd) * 0.25f);
                yt = ceilf(fmaxf(ta, tb) - 0.01f);
#define WALL_U(z) (d == 0 ? f0 + (f1 - f0) * fminf(1.0f, fmaxf(0.0f, (z) - zm)) \
                          : f1 - (f1 - f0) * fminf(1.0f, fmaxf(0.0f, (z) - zm)))
#define WALL_V(h) (g0 + (g1 - g0) * fminf(1.0f, fmaxf(0.0f, yt - (h))))
                if (d == 0) /* faces west */
                    VoxelBuilder_Quad(b, &(VoxelVertex){xe, ta, za, WALL_U(za), WALL_V(ta), shade},
                                      &(VoxelVertex){xe, tb, zb, WALL_U(zb), WALL_V(tb), shade},
                                      &(VoxelVertex){xe, bb, zd, WALL_U(zd), WALL_V(bb), shade},
                                      &(VoxelVertex){xe, ba, zc, WALL_U(zc), WALL_V(ba), shade});
                else        /* faces east */
                    VoxelBuilder_Quad(b, &(VoxelVertex){xe, tb, zb, WALL_U(zb), WALL_V(tb), shade},
                                      &(VoxelVertex){xe, ta, za, WALL_U(za), WALL_V(ta), shade},
                                      &(VoxelVertex){xe, ba, zc, WALL_U(zc), WALL_V(ba), shade},
                                      &(VoxelVertex){xe, bb, zd, WALL_U(zd), WALL_V(bb), shade});
#undef WALL_U
#undef WALL_V
            }
            else
            {
                /* the edge column of the drawing, half a pixel in */
                float ue = d == 0 ? o0 + (o1 - o0) / 32.0f : o1 - (o1 - o0) / 32.0f;
                float va = p0 + (p1 - p0) * k / n, vb = p0 + (p1 - p0) * (k + 1) / n;
                float xe = (float)(d == 0 ? x : x + 1);

                za = y + k / (float)n + s[ia] / 16.0f;
                zb = y + (k + 1) / (float)n + s[ib] / 16.0f;
                zc = y + k / (float)n + os[ic] / 16.0f;
                zd = y + (k + 1) / (float)n + os[id] / 16.0f;
                /* lit as the drawing it goes on from: its own light is in
                 * it (EmitRelief), so the outline reads as one with it */
                b->artShaded = true;
                if (d == 0) /* faces west */
                    VoxelBuilder_Quad(b, &(VoxelVertex){xe, ta, za, ue, va, SHADE_TOP},
                                      &(VoxelVertex){xe, tb, zb, ue, vb, SHADE_TOP},
                                      &(VoxelVertex){xe, bb, zd, ue, vb, SHADE_TOP},
                                      &(VoxelVertex){xe, ba, zc, ue, va, SHADE_TOP});
                else        /* faces east */
                    VoxelBuilder_Quad(b, &(VoxelVertex){xe, tb, zb, ue, vb, SHADE_TOP},
                                      &(VoxelVertex){xe, ta, za, ue, va, SHADE_TOP},
                                      &(VoxelVertex){xe, ba, zc, ue, va, SHADE_TOP},
                                      &(VoxelVertex){xe, bb, zd, ue, vb, SHADE_TOP});
                b->artShaded = false;
            }
        }
    }
}

int VoxelMesh_DraftSlot(const VoxelMapInstance *inst, const struct VoxelAtlasMap *atlas,
                        int x, int y, unsigned *uncovered)
{
    int metatile;
    unsigned slot;

    /* Straight to the world, not through the window's cache: the build that
     * may be running keeps its own. Outdoors nothing is void. */
    if (inst->indoor && VoxelWorld_ClassifyTile(x, y) == VOXEL_SHAPE_VOID)
        return -1;
    metatile = VoxelWorld_GetMetatileId(x, y);
    /* The trunk's ground, not the card of the crown drawn over it. */
    if (VoxelWorld_UsesTreeSprites(inst))
        metatile = VoxelTree_GroundMetatile(inst->secondaryTileset, metatile);
    if (metatile < 0 || metatile >= (int)VOXEL_METATILE_REAL)
        return -1;
    slot = atlas->slotOf[metatile];
    if (slot == 0 || slot == VOXEL_SLOT_PENDING)
    {
        ++*uncovered;
        return -1;
    }
    if (slot == VOXEL_SLOT_ABSENT)
        return -1;
    return (int)slot - 1;
}

void VoxelMesh_DraftCell(VoxelBuilder *b, const VoxelMapInstance *inst, int x, int y, int slot)
{
    float u0, v0, u1, v1;

    VoxelAtlas_SlotUV((unsigned)slot, &u0, &v0, &u1, &v1);
    /* At the level its relief puts it, as the ground of a modelled building
     * is: the draft is for seeing something, not for matching the slopes. */
    b->lift = VoxelRelief_CellLift(inst, x, y);
    b->shift = VoxelRelief_CellShift(inst, x, y);
    VoxelMesh_Top(b, (float)x, (float)y, 0.0f, 0.0f, u0, v0, u1, v1, SHADE_TOP);
    b->lift = 0.0f;
    b->shift = 0.0f;
}

/* One row of the ground pass over [x0,x1), already clipped to the instance. */
void VoxelMesh_EmitGroundRow(VoxelBuilder *builder, const VoxelMapInstance *inst,
                             int x0, int x1, int y)
{
    for (int x = x0; x < x1; ++x)
        if (x == inst->originX || x == inst->originX + inst->width - 1
         || y == inst->originY || y == inst->originY + inst->height - 1)
            EmitSeamSkirt(builder, inst, x, y);
    for (int x = x0; x < x1; ++x)
    {
        VoxelVisualShape shape = VoxelMesh_Classify(x, y);

        /* The tree pass supplies both the trunk's ground and its crown. */
        if (VoxelWorld_UsesTreeSprites(inst)
         && VoxelTree_Part(inst->secondaryTileset, VoxelWorld_GetMetatileId(x, y)) >= 0)
            continue;
        if (shape == VOXEL_SHAPE_VOID)
            continue;
        /* A lifted cell lays its own drawing on its relief. A slope - rock,
         * stairs - is nothing but that; a level lifted cell still carries
         * whatever stands on it, drawn below with the same lift. */
        {
            const int16_t *relief = VoxelRelief_Cell(inst, x, y);

            /* A building's cells are its model's: a mountain's foot that
             * runs on under a roof is not drawn over it. */
            if (relief != NULL && VoxelBuildings_CellAt(inst, x, y, NULL, NULL))
                relief = NULL;
            if (relief != NULL)
            {
                /* A signpost's drawing is the sign: its relief is laid with
                 * the ground round it, and the sign stands without its own. */
                bool sign = VoxelSign_IsCell(inst, x, y);
                float foot = 0.0f, u0, v0, u1, v1;
                int ground = -1, art = -1, wall = -1;
                unsigned sides = 0xFF;
                const uint8_t *cutRows = NULL;
                int cut = sign ? -1 : VoxelRelief_Cut(inst, x, y, &foot, &ground, &wall, &sides);

                /* that ground: the one matched to the drawing round the sign
                 * (the cell south of it without one) */
                if (sign && VoxelSign_Ground(inst, x, y, &art)
                 && !MetatileUV(builder, art, &u0, &v0, &u1, &v1))
                    art = -1;

                /*
                 * A cut tile (voxel_relief.h): the cell's own drawing lies
                 * flat at its foot and its relief shows the rock alone, cut
                 * where its pixels end, leaving the quads on the foot to
                 * the flat drawing. Without a slot for the variant, the
                 * tile as it is.
                 */
                /*
                 * The ground behind a terrace's rim, run on under the cell
                 * two pixels under its own level (voxel_relief.h): only ever
                 * seen from above, past the rim's edge. The cell's relief
                 * is drawn as it is.
                 */
                if (cut == VOXEL_RELIEF_NO_VARIANT)
                {
                    const int16_t *north = VoxelWorld_GetInstanceAt(x, y - 1) == inst
                                         ? VoxelRelief_Cell(inst, x, y - 1) : NULL;

                    if ((sides & VOXEL_RELIEF_GOES_ON) && north != NULL)
                    {
                        /* the flank north of this top goes on under it, a row
                         * on and a pixel down, its own drawing (its cut
                         * variant if it has one) */
                        float nf, a0, b0, a1, b1;
                        int ng, nw, nart = -1;
                        unsigned ns;
                        int nc = VoxelRelief_Cut(inst, x, y - 1, &nf, &ng, &nw, &ns);

                        const uint8_t *nrows = NULL;
                        unsigned nl, nm;

                        if (nc >= 0 && nc < (int)VOXEL_CUTS
                         && MetatileUV(builder, (int)(VOXEL_CUT_FIRST + (unsigned)nc), &a0, &b0, &a1, &b1))
                        {
                            nart = (int)(VOXEL_CUT_FIRST + (unsigned)nc);
                            if (!VoxelRelief_CutVariant((unsigned)nc, &nl, &nm, &nrows))
                                nrows = NULL;
                        }
                        builder->lift = -1.0f / 16.0f;
                        builder->shift = builder->lift;
                        EmitRelief(builder, x, y, north, VoxelRelief_Depth(inst, x, y - 1), y - 1, nart,
                                   false, 0, nrows);
                        builder->lift = 0.0f;
                        builder->shift = 0.0f;
                    }
                    if (ground >= 0 && MetatileUV(builder, ground, &u0, &v0, &u1, &v1))
                    {
                        /* under a cut tile's own run on (kCutFill), and two
                         * pixels past the cell all round: the floors beside
                         * it stand a pixel or two higher, and a ray between
                         * them would find the clear colour. Those of cells
                         * side by side overlap: a hair apart, by parity, so
                         * two drawings never share a plane */
                        builder->lift = foot - (2.0f + 0.125f * (float)((x + y) & 1)) / 16.0f;
                        builder->shift = builder->lift;
                        /* sunlit as the open ground it stands for: under
                         * the terrain's own heights every ray is stopped,
                         * and the fill came out as a shadowed olive patch */
                        builder->lightingConstant = 1.0f;
                        VoxelMesh_Top(builder, (float)x, (float)y, 0.0f, -2.0f / 16.0f,
                                      u0, v0, u1, v1, SHADE_TOP);
                        builder->lightingConstant = -1.0f;
                        builder->lift = 0.0f;
                        builder->shift = 0.0f;
                    }
                    cut = -1;
                }
                else if (cut >= 0 && cut < (int)VOXEL_CUTS
                      && MetatileUV(builder, (int)(VOXEL_CUT_FIRST + (unsigned)cut), &u0, &v0, &u1, &v1)
                      && VoxelMesh_TileUV(builder, x, y, &u0, &v0, &u1, &v1))
                    art = (int)(VOXEL_CUT_FIRST + (unsigned)cut);
                else
                    cut = -1;
                if (cut >= 0)
                {
                    builder->lift = foot;
                    builder->shift = foot;
                    /*
                     * Its flat layer: with a ground behind it, that ground
                     * where the drawing has rock and the drawing where it is
                     * background (EmitCutGround); with none, the whole
                     * drawing. The rock the drawing carries, flat at the
                     * foot, showed as a dark wedge on the sand wherever the
                     * lifted rock did not cover it.
                     */
                    {
                        unsigned vLayout = 0, vMetatile = 0;
                        float gnd[4];

                        if (ground >= 0 && VoxelRelief_CutVariant((unsigned)cut, &vLayout, &vMetatile, &cutRows)
                         && MetatileUV(builder, ground, &gnd[0], &gnd[1], &gnd[2], &gnd[3]))
                            EmitCutGround(builder, x, y, u0, v0, u1, v1, cutRows, gnd,
                                          relief, (int)lroundf(foot * 16.0f));
                        else
                        {
                            cutRows = NULL;
                            EmitCutGround(builder, x, y, u0, v0, u1, v1, NULL, NULL, relief, 0);
                        }
                    }
                    /* and the ground behind it, run on under the rock beside
                     * it, a pixel under its own level: only ever seen through
                     * the clear background, never beside anything drawn */
                    if (ground >= 0 && MetatileUV(builder, ground, &u0, &v0, &u1, &v1))
                        for (unsigned k = 0; k < sizeof(kCutFill) / sizeof(kCutFill[0]); ++k)
                        {
                            int nx = x + kCutFill[k][0], ny = y + kCutFill[k][1];
                            const int16_t *n = VoxelWorld_GetInstanceAt(nx, ny) == inst
                                             ? VoxelRelief_Cell(inst, nx, ny) : NULL;
                            int top = INT16_MIN;

                            for (unsigned g = 0; n != NULL && g < VOXEL_RELIEF_SIDE * VOXEL_RELIEF_SIDE; ++g)
                                if (n[g] > top)
                                    top = n[g];
                            float nu0 = u0, nv0 = v0, nu1 = u1, nv1 = v1, nfoot;
                            int nground, nwall;
                            unsigned nsides;

                            if (n == NULL || top <= (int)lroundf(foot * 16.0f))
                                continue;
                            /* what runs on under the neighbour is the ground
                             * the neighbour itself stands on, if it has one of
                             * its own: this tile's sea under the next tile's
                             * sand was a bar of water across the beach */
                            if (VoxelRelief_Cut(inst, nx, ny, &nfoot, &nground, &nwall, &nsides) != -1 && nground >= 0
                             && !MetatileUV(builder, nground, &nu0, &nv0, &nu1, &nv1))
                            {
                                nu0 = u0; nv0 = v0; nu1 = u1; nv1 = v1;
                            }
                            /* a hair apart by side: two cut tiles may run
                             * their grounds on under the same cell */
                            builder->lift = foot - (1.0f + 0.125f * (float)k) / 16.0f;
                            builder->shift = builder->lift;
                            /* lit as the ground it is seen beside, this
                             * tile's own at its foot: in full sun it was a
                             * bright wedge in the shade the rock casts */
                            builder->lightingConstant = 1.0f;
#if CTR_VOXEL_LIGHTING
                            if (builder->lighting)
                                builder->lightingConstant = VoxelLighting_Sample(x + 0.5f, foot + 0.05f,
                                                                                 y + 0.5f + foot);
#endif
                            VoxelMesh_Top(builder, (float)nx, (float)ny, 0.0f, -2.0f / 16.0f,
                                          nu0, nv0, nu1, nv1, SHADE_TOP);
                            builder->lightingConstant = -1.0f;
                        }
                    builder->lift = 0.0f;
                    builder->shift = 0.0f;
                }
                EmitRelief(builder, x, y, relief, VoxelRelief_Depth(inst, x, y),
                           sign ? y + 1 : y, art, cut >= 0, (int)lroundf(foot * 16.0f), cutRows);
                if (wall >= 0)
                    EmitCliffWalls(builder, inst, x, y, relief, VoxelRelief_Depth(inst, x, y), wall,
                                   art, sign ? y + 1 : y, sides);
                if (VoxelRelief_IsSlope(relief))
                {
                    /* A signpost at the foot of a face stands on the ground
                     * under its board, three quarters of the way down the
                     * cell (voxel_sign.c's EmitSign). */
                    const int16_t *depth = VoxelRelief_Depth(inst, x, y);
                    const int under = 3 * VOXEL_RELIEF_SIDE + 2;

                    builder->lift = relief[under] / 16.0f;
                    builder->shift = depth[under] / 16.0f;
                    VoxelSign_EmitStanding(builder, inst, x, y);
                    builder->lift = 0.0f;
                    builder->shift = 0.0f;
                    continue;
                }
                builder->lift = VoxelRelief_CellLift(inst, x, y);
                builder->shift = VoxelRelief_CellShift(inst, x, y);
                VoxelSign_EmitStanding(builder, inst, x, y);
                builder->lift = 0.0f;
                builder->shift = 0.0f;
                continue;
            }
        }
        /* A modelled building brings its own walls and roof; what is
         * left of its cells is the ground its drawing was painted over. */
        {
            int ground;
            float u0, v0, u1, v1;

            if (VoxelBuildings_CellAt(inst, x, y, &ground, NULL))
            {
                bool drawn = MetatileUV(builder, ground, &u0, &v0, &u1, &v1);

                /* A ground variant the atlas has no slot for (a full atlas)
                 * gives way to the cell's whole drawing: never a hole. */
                if (!drawn && ground >= (int)VOXEL_METATILE_REAL && ground < (int)VOXEL_METATILE_IDS
                 && builder->atlas->slotOf[ground] == VOXEL_SLOT_ABSENT)
                    drawn = MetatileUV(builder, VoxelWorld_GetMetatileId(x, y), &u0, &v0, &u1, &v1);
                if (drawn)
                {
                    /* at the level the cell's relief puts it, as the model
                     * standing on it is: Route 104's sea, a level under its
                     * land, under a rock in it */
                    builder->lift = VoxelRelief_CellLift(inst, x, y);
                    builder->shift = VoxelRelief_CellShift(inst, x, y);
                    VoxelMesh_Top(builder, (float)x, (float)y, 0.0f, 0.0f,
                                  u0, v0, u1, v1, SHADE_TOP);
                    builder->lift = 0.0f;
                    builder->shift = 0.0f;
                }
                /* a signpost on a bridge stands on its deck, lifted and
                 * moved south by the deck's height as its drawing is */
                {
                    float deck;

                    if (VoxelSign_IsCell(inst, x, y)
                     && VoxelBuildings_DeckAt(inst, x + 0.5f, y + 0.5f, 0, &deck))
                    {
                        builder->lift = VoxelRelief_CellLift(inst, x, y) + deck;
                        builder->shift = VoxelRelief_CellShift(inst, x, y) + deck;
                        VoxelSign_EmitStanding(builder, inst, x, y);
                        builder->lift = 0.0f;
                        builder->shift = 0.0f;
                    }
                }
                continue;
            }
        }
        if (VoxelSign_EmitCell(builder, inst, x, y))
            continue;
        /* A lamp's lantern stands on its post; under it, the ground. */
        {
            int ground;
            float u0, v0, u1, v1;

            if (VoxelSign_HeadGround(inst, x, y, &ground))
            {
                if (MetatileUV(builder, ground, &u0, &v0, &u1, &v1))
                    VoxelMesh_Top(builder, (float)x, (float)y, 0.0f, 0.0f,
                                  u0, v0, u1, v1, SHADE_TOP);
                continue;
            }
        }
        EmitTile(builder, x, y, shape);
    }
}

void VoxelMesh_EmitInstance(VoxelBuilder *builder, const VoxelMapInstance *inst,
                            int x0, int y0, int x1, int y1)
{
    if (inst == NULL || builder->atlas == NULL)
        return;
    if (x0 < inst->originX) x0 = inst->originX;
    if (y0 < inst->originY) y0 = inst->originY;
    if (x1 > inst->originX + inst->width) x1 = inst->originX + inst->width;
    if (y1 > inst->originY + inst->height) y1 = inst->originY + inst->height;
    if (x1 <= x0 || y1 <= y0)
        return;

    /*
     * The ground, with the relief it is lifted on and the small things that
     * stand on it cell by cell (signs, furniture). The staging buffer is
     * finite, and this goes first so that it is never what gets refused:
     * losing the floor leaves a hole straight through the world to the clear
     * colour. Trees and modelled buildings follow in passes of their own.
     */
    for (int y = y0; y < y1; ++y)
        VoxelMesh_EmitGroundRow(builder, inst, x0, x1, y);
}

/*
 * The world past the last map.
 *
 * The window reaches beyond every instance on purpose, and until now those
 * tiles were VOID and drew nothing, so a town ended on a plate over the clear
 * colour and the horizon was a black wedge. The 2D game tiles the layout's 2x2
 * border block outwards for ever; this stands the same block up.
 *
 * Two courses, because that is what the block's own drawing measures: it is a
 * 2x2 pattern tiled, so a column through it reads one row, the other, the
 * first again - two distinct drawings, by the same rule every other column
 * here is heighted with. Laid flat instead, a border of trees reads as a leaf
 * carpet.
 *
 * Instance 0 only: the border belongs to the map the player is standing on,
 * and running it for every connection would stack four copies of it on the
 * same tiles.
 *
 * A map can say how tall its belt stands (VoxelRelief_Belt): a cave carved
 * out of the rock, its edges standing that high all round. Then the block is
 * laid as the drawing lays everything else, each of its cells at (u, h,
 * v + h): stood the map's courses and as many rows south of where the 2D game
 * tiles it, it meets the rock at the map's edges without a step or a gap -
 * stood where it is tiled, it left a trench along the north edge as wide as
 * it is tall, and doubled the rock along the south one.
 */
#define VOXEL_BORDER_COURSES 2

static bool BorderAt(int x, int y)
{
    const VoxelMapInstance *inst = VoxelWorld_GetInstanceAt(x, y);
    if (inst == NULL)
        return true;
    /* Outdoor classifiers never produce VOID inside an instance. Do not read
     * metatiles, attributes and neighbours just to discover there is no belt. */
    if (!inst->indoor)
        return false;
    return VoxelMesh_Classify(x, y) == VOXEL_SHAPE_VOID;
}

/*
 * A belt of water (Slateport's sea, Lilycove's, every route's out at sea)
 * is no wall: stood two courses up like a wood it was a step of sea standing
 * over the map's own, a raised rim round the whole of the harbour. Water lies
 * flush with the ground (ShapeHeight), and the belt's lies flush with the
 * map's.
 *
 * One plane for the whole belt: the level (and shift south) of the lowest of
 * the map's edge cells of water that face the void - a sea lowered under the
 * land, Rustboro's, goes on at its own level -, the map's ground when none
 * does. Taken cell by cell from the nearest edge, two belt cells side by side
 * (at a corner, or where the edge's water changes level) stood at different
 * levels and shifts, flat with no faces between them: slits onto the clear
 * colour. With one plane the belt is whole, and every edge that is not at
 * the plane is closed by EmitSeamSkirt. Over instance 0's base, as the belt
 * is built.
 *
 * Worked out once per map (the perimeter's classification is not cheap) and
 * kept while instance 0 is the same map at the same place.
 */
static struct
{
    const VoxelMapInstance *ring;
    int layoutId, mapGroup, mapNum, originX, originY, width, height;
    float level, shift;
} sBeltPlane;

static bool EdgeFacesVoid(const VoxelMapInstance *ring, int x, int y)
{
    static const int kDir[4][2] = { {0, -1}, {0, 1}, {-1, 0}, {1, 0} };

    for (int d = 0; d < 4; ++d)
    {
        int nx = x + kDir[d][0], ny = y + kDir[d][1];

        if (nx >= ring->originX && ny >= ring->originY
         && nx < ring->originX + ring->width && ny < ring->originY + ring->height)
            continue;
        if (VoxelWorld_GetInstanceAt(nx, ny) == NULL)
            return true;
    }
    return false;
}

static void BeltWaterPlane(const VoxelMapInstance *ring, float *level, float *shift)
{
    if (sBeltPlane.ring != ring || sBeltPlane.layoutId != ring->layoutId
     || sBeltPlane.mapGroup != ring->mapGroup || sBeltPlane.mapNum != ring->mapNum
     || sBeltPlane.originX != ring->originX || sBeltPlane.originY != ring->originY
     || sBeltPlane.width != ring->width || sBeltPlane.height != ring->height)
    {
        int x0 = ring->originX, y0 = ring->originY;
        int x1 = x0 + ring->width - 1, y1 = y0 + ring->height - 1;
        bool found = false;
        float lv = 0.0f, sh = 0.0f;

        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; x += (y == y0 || y == y1 || x1 <= x0) ? 1 : x1 - x0)
            {
                float lift;

                if (VoxelMesh_Classify(x, y) != VOXEL_SHAPE_WATER || !EdgeFacesVoid(ring, x, y))
                    continue;
                lift = VoxelRelief_CellLift(ring, x, y);
                if (!found || lift < lv - 0.001f)
                {
                    lv = lift;
                    sh = VoxelRelief_CellShift(ring, x, y);
                    found = true;
                }
            }
        sBeltPlane.ring = ring;
        sBeltPlane.layoutId = ring->layoutId;
        sBeltPlane.mapGroup = ring->mapGroup;
        sBeltPlane.mapNum = ring->mapNum;
        sBeltPlane.originX = ring->originX;
        sBeltPlane.originY = ring->originY;
        sBeltPlane.width = ring->width;
        sBeltPlane.height = ring->height;
        sBeltPlane.level = lv;
        sBeltPlane.shift = sh;
    }
    *level = sBeltPlane.level;
    *shift = sBeltPlane.shift;
}

static bool BorderWaterLevel(int x, int y, float *level, float *shift)
{
    const VoxelMapInstance *ring = VoxelWorld_Instance(0);

    if (ring == NULL || ring->indoor || VoxelRelief_Belt(ring) != 0
     || ring->width <= 0 || ring->height <= 0
     || !VoxelWorld_BorderIsWater(x, y))
        return false;
    BeltWaterPlane(ring, level, shift);
    return true;
}

void VoxelMesh_EmitBorder(VoxelBuilder *builder, int x0, int y0, int x1, int y1)
{
    float u0, v0, u1, v1;
    unsigned drawn, courses;

    if (builder->atlas == NULL)
        return;
    /* Indoors the border is the black filler outside the rooms: stood up it
     * was a black wall two tiles tall round the room, and the south one hid
     * the doorway. The 2D game shows black there, and so does the clear. */
    if (VoxelWorld_Instance(0) != NULL && VoxelWorld_Instance(0)->indoor)
        return;
    /* A belt laid as the drawing (above): each cell stands `drawn` courses,
     * that many rows south. Its cells are the window's; their blocks may
     * stand past it. */
    drawn = VoxelWorld_Instance(0) != NULL ? VoxelRelief_Belt(VoxelWorld_Instance(0)) : 0u;
    courses = drawn ? drawn : VOXEL_BORDER_COURSES;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
        {
            float wx = (float)x, wz = (float)(y + (int)drawn);
            /* Only where the border meets something else: the inside of a
             * belt that reaches the whole way to the horizon is never seen,
             * and drawing it is most of the tiles in the window. */
            bool south, east, west, southW, eastW, westW;
            float level = 0.0f, shift = 0.0f;

            if (!BorderAt(x, y))
                continue;
            if (BorderWaterLevel(x, y, &level, &shift))
            {
                /* flat on the belt's one plane, and no faces: its cells meet
                 * edge to edge, and EmitSeamSkirt closes the map's edge down
                 * (or up) to it */
                if (MetatileUV(builder, VoxelWorld_BorderMetatile(x, y), &u0, &v0, &u1, &v1))
                    VoxelMesh_Top(builder, wx, wz + shift, level, 0.0f,
                                  u0, v0, u1, v1, SHADE_TOP);
                continue;
            }
            if (VoxelWorld_UsesTreeSprites(VoxelWorld_Instance(0))
             && VoxelTree_Part(VoxelWorld_Instance(0)->secondaryTileset,
                               VoxelWorld_BorderMetatile(x, y)) >= 0)
                continue; /* flat trunks and tilted crowns are appended later */
            /* a standing cell beside a flat one of water shows its face too */
            southW = BorderAt(x, y + 1) && BorderWaterLevel(x, y + 1, &level, &shift);
            eastW = BorderAt(x + 1, y) && BorderWaterLevel(x + 1, y, &level, &shift);
            westW = BorderAt(x - 1, y) && BorderWaterLevel(x - 1, y, &level, &shift);
            south = !BorderAt(x, y + 1) || southW;
            east = !BorderAt(x + 1, y) || eastW;
            west = !BorderAt(x - 1, y) || westW;
            /* Laid as drawn, a cell's top is its own drawing; stood where it
             * is tiled, the column reads the rows above it. */
            if (MetatileUV(builder, VoxelWorld_BorderMetatile(x, drawn ? y : y - (int)courses + 1),
                           &u0, &v0, &u1, &v1))
                VoxelMesh_Top(builder, wx, wz, (float)courses, 0.0f,
                              u0, v0, u1, v1, SHADE_TOP);
            /* Laid as drawn it meets the map at its own height: the map's
             * faces close every step there, and a wall of the belt's would
             * stand in the same place as the cave's north wall. */
            if (drawn || (!south && !east && !west))
                continue;
            for (int h = 0; h < (int)courses; ++h)
            {
                float yBottom = (float)h, yTop = (float)(h + 1);

                if (!MetatileUV(builder, VoxelWorld_BorderMetatile(x, drawn ? y : y - h),
                                &u0, &v0, &u1, &v1))
                    continue;
                if (south) VoxelMesh_South(builder, wx, wz, yBottom, yTop, 0.0f, u0, v0, u1, v1);
                if (east) VoxelMesh_East(builder, wx, wz, yBottom, yTop, 0.0f, u0, v0, u1, v1);
                if (west) VoxelMesh_West(builder, wx, wz, yBottom, yTop, 0.0f, u0, v0, u1, v1);
            }
            /* Beside a belt of water lowered under the map's ground (its one
             * plane, BorderWaterLevel), the stood cell's faces go on down to
             * it: they stopped at the ground and left a slit over the sea. */
            if (level < -0.05f && (southW || eastW || westW)
             && MetatileUV(builder, VoxelWorld_BorderMetatile(x, y), &u0, &v0, &u1, &v1))
            {
                float vb = v1 - (v1 - v0) * (-level < 1.0f ? -level : 1.0f);

                if (southW) VoxelMesh_South(builder, wx, wz, level, 0.0f, 0.0f, u0, vb, u1, v1);
                if (eastW) VoxelMesh_East(builder, wx, wz, level, 0.0f, 0.0f, u0, vb, u1, v1);
                if (westW) VoxelMesh_West(builder, wx, wz, level, 0.0f, 0.0f, u0, vb, u1, v1);
            }
        }
}
