#include <stddef.h>
#include "voxel_tree.h"
#include "voxel_relief.h"
#include "voxel_regions.h"

struct Tileset;
extern const struct Tileset gTileset_Dewford;
extern const struct Tileset gTileset_Fortree;
extern const struct Tileset gTileset_Mauville;
extern const struct Tileset gTileset_Lavaridge;
extern const struct Tileset gTileset_Rustboro;
extern const struct Tileset gTileset_Lilycove;
extern const struct Tileset gTileset_EverGrande;
extern const struct Tileset gTileset_BattleFrontierOutsideWest;
extern const struct Tileset gTileset_Mossdeep;
extern const struct Tileset gTileset_Fallarbor;

/* Where gen_voxel_trees.py packs each tree, in texels. */
#define TEX_U(px) ((float)(px) / VOXEL_TREE_TEXTURE_DIM)
#define TEX_V(py) (1.0f - (float)(py) / VOXEL_TREE_TEXTURE_DIM)

/* Ids from 0x200 are the secondary tileset's, the same number meaning a
 * different drawing on every map. */
#define SECONDARY_ID 0x200

/*
 * Fortree's houses, decks and bridges stand in the treetops (voxel_fortree.py
 * lifts them over the crowns): the cells they are drawn on are cells of the
 * wood, every one a small tree under them, as 22A is round them. The roof's
 * fronds (224-226), the hut and the deck (22B-22F, 233-237, 23B-23F), the
 * deck's front (243, 245-247) and a bridge over the wood (256).
 */
static bool FortreeWood(int metatileId)
{
    return (metatileId >= 0x224 && metatileId <= 0x226)
        || (metatileId >= 0x22B && metatileId <= 0x22F)
        || (metatileId >= 0x233 && metatileId <= 0x237)
        || (metatileId >= 0x23B && metatileId <= 0x23F)
        || metatileId == 0x243
        || (metatileId >= 0x245 && metatileId <= 0x247)
        || metatileId == 0x256;
}

/* Where a small trunk's ground is packed: the General one's at 48,32,
 * Dewford's at 80,0, and the trunks a secondary tileset draws on its own
 * ground (3: the General's own grass). -1 for any other metatile. */
enum { TRUNK_JUNGLE, TRUNK_GRASS_ROAD, TRUNK_VOLCAN, TRUNK_GENERAL, TRUNK_ACCESS };

static int SmallTrunkGround(const void *secondaryTileset, int metatileId)
{
    if (secondaryTileset == &gTileset_Fortree && metatileId == 0x253)
        return 0; /* tree_small_trunk_jungle.png at 48,48 */
    /* 22A: the General's 0C6 with the crown drawn on both layers, in the
     * woods round Fortree's tree houses */
    if (secondaryTileset == &gTileset_Fortree && (metatileId == 0x22A || FortreeWood(metatileId)))
        return 3;
    if (secondaryTileset == &gTileset_Mauville && metatileId == 0x226)
        return 1; /* tree_small_trunk_grass_road.png at 80,16 */
    /* At a wood's way in, on the darker grass of its path: Rustboro's 307
     * (Route 104, Southern Island, Faraway Island) and Lavaridge's 201
     * (Route 112) */
    if ((secondaryTileset == &gTileset_Rustboro && metatileId == 0x307)
     || (secondaryTileset == &gTileset_Lavaridge && metatileId == 0x201))
        return TRUNK_ACCESS; /* tree_small_trunk_access.png at 112,32 */
    /* 21A, 3B8 and 203 inside a wood, where the next crown overlaps the
     * trunk (the General's 0C6), on the soil and on the dark rock */
    if (secondaryTileset == &gTileset_Lavaridge
     && (metatileId == 0x218 || metatileId == 0x21A || metatileId == 0x3B8 || metatileId == 0x203))
        return 2; /* tree_small_trunk_volcan.png at 64,32 */
    return -1;
}

/*
 * The 2x2 trees a secondary tileset draws with its own art: Mossdeep's round
 * tree (on grass and in the shallows) and the leaning tree of Route 113,
 * Fallarbor and Route 114 (on its brown soil and on the ash). Only the trunk
 * row is a tree part: its cell lays the trunk's ground and carries the crown,
 * the row above added over whatever stands there (EmitMissingRow), as the
 * crown row's own cells are their plain ground (VoxelTree_GroundMetatile).
 * In a wood the trees are staggered and a trunk's cell also holds the next
 * tree's crown top: it is that trunk. 2 for the left half, 3 the right; the
 * kind of ground in `ground`.
 */
enum { OWN_GRASS, OWN_SHALLOWS, OWN_SOIL, OWN_ASH };

static int OwnLargePart(const void *secondaryTileset, int metatileId, int *ground)
{
    int part = -1, kind = -1;

    if (secondaryTileset == &gTileset_Mossdeep)
        switch (metatileId)
        {
        /* on grass: alone (322-323, 326-327 unused) and in a wood (355-356) */
        case 0x322: case 0x326: case 0x355: part = 2; kind = OWN_GRASS; break;
        case 0x323: case 0x327: case 0x356: part = 3; kind = OWN_GRASS; break;
        /* in the shallows: alone (2F8-2F9) and in a wood (324-325) */
        case 0x2F8: case 0x324: part = 2; kind = OWN_SHALLOWS; break;
        case 0x2F9: case 0x325: part = 3; kind = OWN_SHALLOWS; break;
        }
    else if (secondaryTileset == &gTileset_Fallarbor)
        switch (metatileId)
        {
        /* on the brown soil: alone (20E-20F), in a wood (26B-26C,
         * 273-274) and on the soil's orange rim (2CA-2CB, 2CE-2CF) */
        case 0x20E: case 0x26C: case 0x273: case 0x2CA: case 0x2CF: part = 2; kind = OWN_SOIL; break;
        case 0x20F: case 0x26B: case 0x274: case 0x2CB: case 0x2CE: part = 3; kind = OWN_SOIL; break;
        /* on the ash: alone (20C-20D) and in a wood (26D-26E, 275-276) */
        case 0x20C: case 0x26E: case 0x275: part = 2; kind = OWN_ASH; break;
        case 0x20D: case 0x26D: case 0x276: part = 3; kind = OWN_ASH; break;
        }
    if (ground != NULL)
        *ground = kind;
    return part;
}

/* The plain ground under such a tree's cell: the tile its lower layer is.
 * -1 for any other. */
static int OwnLargeGround(const void *secondaryTileset, int metatileId)
{
    if (secondaryTileset == &gTileset_Mossdeep)
        switch (metatileId)
        {
        case 0x2F0: case 0x2F1: case 0x2F8: case 0x2F9: case 0x324: case 0x325:
            return 0x19E; /* the shallows, animated */
        case 0x31A: case 0x31B: return 0x001; /* grass */
        case 0x31C: case 0x31D: return 0x07C; /* the cliff's face */
        case 0x31E: case 0x31F: return 0x002; /* grass at a ledge */
        }
    else if (secondaryTileset == &gTileset_Fallarbor)
        switch (metatileId)
        {
        case 0x204: case 0x205: return 0x218; /* ash */
        case 0x206: case 0x207: return 0x279; /* brown soil */
        case 0x214: case 0x215: return 0x271; /* the soil's orange rim */
        case 0x2C2: case 0x2C3: return 0x269; /* the brick rock */
        case 0x2CC: case 0x2CD: return 0x170; /* water */
        }
    return -1;
}

bool VoxelTree_LaysGround(const void *secondaryTileset, int metatileId)
{
    int ground;

    /* in the shallows the ground pass lays the animated water itself */
    return !(metatileId >= SECONDARY_ID && OwnLargePart(secondaryTileset, metatileId, &ground) >= 0
             && ground == OWN_SHALLOWS);
}

int VoxelTree_Part(const void *secondaryTileset, int metatileId)
{
    if (metatileId >= SECONDARY_ID)
    {
        int part = OwnLargePart(secondaryTileset, metatileId, NULL);

        if (part >= 0)
            return part;
        /* Dewford's small tree: the trunk on the sand (23A), and inside the
         * wood (243), where the next crown overlaps it. */
        if (secondaryTileset == &gTileset_Dewford
         && (metatileId == 0x23A || metatileId == 0x243))
            return VOXEL_TREE_SMALL;
        /* The General's small tree drawn over another ground: Fortree's
         * jungle floor, Route 111's pale grass, Route 112's volcanic soil. */
        if (SmallTrunkGround(secondaryTileset, metatileId) >= 0)
            return VOXEL_TREE_SMALL;
        /* Fortree's long grass at a patch's south edge, over another ground */
        if (secondaryTileset == &gTileset_Fortree
         && (metatileId == 0x208 || (metatileId >= 0x281 && metatileId <= 0x283)))
            return VOXEL_TREE_GRASS;
        return -1;
    }
    switch (metatileId)
    {
    /* The secret base's tree: its upper row, and the lower closed (026-027)
     * or opened (036-037, set by secret_base.c). */
    case 0x01E: return 0;
    case 0x01F: return 1;
    case 0x026: case 0x036: return 2;
    case 0x027: case 0x037: return 3;
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
    case 0x015: return VOXEL_TREE_GRASS; /* long grass */
    default: return -1;
    }
}

int VoxelTree_GroundMetatile(const void *secondaryTileset, int metatileId)
{
    if (metatileId >= SECONDARY_ID)
    {
        int own = OwnLargeGround(secondaryTileset, metatileId);

        if (own >= 0)
            return own;
        /* Dewford's crown top over the sand. */
        if (secondaryTileset == &gTileset_Dewford && metatileId == 0x239)
            return 0x124;
        /* A secondary tileset's own small crown top over its ground, north
         * of a trunk, on the maps the relief does not clear it on (its
         * CANOPY record): the ground the crown is drawn over. */
        if ((secondaryTileset == &gTileset_Rustboro && metatileId == 0x2C0)
         || (secondaryTileset == &gTileset_Lilycove && metatileId == 0x277))
            return 0x079; /* rock (Southern Island, Route 123) */
        /* Route 112's crowns over its dark rock, where no slope of the
         * relief draws the rock instead */
        if (secondaryTileset == &gTileset_Lavaridge && (metatileId == 0x211 || metatileId == 0x20B))
            return metatileId == 0x211 ? 0x079 : 0x07D;
        if (secondaryTileset == &gTileset_BattleFrontierOutsideWest && metatileId == 0x3F8)
            return 0x204;
        if (secondaryTileset == &gTileset_Mauville && metatileId == 0x2C1)
            return 0x205; /* Verdanturf's grass */
        if (secondaryTileset == &gTileset_EverGrande && metatileId == 0x262)
            return 0x222;
        if (secondaryTileset == &gTileset_Fortree && (metatileId == 0x316 || metatileId == 0x317))
            return 0x001; /* grass (Faraway Island) */
        /* Fortree's canopy over the next row's trunks (its crowns' tops,
         * as 00E-00F are the General's), and the cells under a palm's
         * crown and trunk (the palm stands as a model of its own) */
        if (secondaryTileset == &gTileset_Fortree
         && (metatileId == 0x244
             || (metatileId >= 0x248 && metatileId <= 0x24A)
             || (metatileId >= 0x250 && metatileId <= 0x252)
             || metatileId == 0x241 || metatileId == 0x25C || metatileId == 0x273))
            return 0x001;
        /* the crown's top over the path (the General's 00E-00F on it) */
        if (secondaryTileset == &gTileset_Fortree && (metatileId == 0x24B || metatileId == 0x24C))
            return 0x261;
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

/*
 * The trunk's ground. Where the cell's relief is level, one plate at its
 * height. On a piece of slope one level tall (a band, a face, a corner: a
 * wood on a mountain's rock, Route 112's trees on the crags' bands) the
 * relief itself, the trunk's drawing laid on it: a plate at the centre's
 * height stood off the slope round it, half a level over its foot and
 * under its top, the void seen either side. The tree stands at the
 * centre's height either way. Anything taller is still a plate (Route
 * 120's wood, its cells rising two levels along their south edge: drawn,
 * more of the void showed past them). Past the last map, no relief.
 */
static bool OneLevelSlope(const int16_t *g)
{
    int lo, hi;

    if (g == NULL)
        return false;
    lo = hi = g[0];
    for (unsigned i = 1; i < VOXEL_RELIEF_SIDE * VOXEL_RELIEF_SIDE; ++i)
    {
        lo = g[i] < lo ? g[i] : lo;
        hi = g[i] > hi ? g[i] : hi;
    }
    return hi - lo == 16;
}

static void EmitGround(VoxelBuilder *builder, const VoxelMapInstance *inst, int x, int y,
                       float u0, float v0, float u1, float v1)
{
    float lift = builder->lift, shift = builder->shift;

    /* on a mountain's rock the ground pass lays the rock (EmitTreeStep) */
    if (inst != NULL && VoxelMesh_TreeOnRock(inst, x, y))
        return;
    if (inst == NULL || !OneLevelSlope(VoxelRelief_Cell(inst, x, y)))
    {
        VoxelMesh_Top(builder, (float)x, (float)y, 0.0f, 0.0f, u0, v0, u1, v1, 1.0f);
        return;
    }
    builder->lift = 0.0f;
    builder->shift = 0.0f;
    VoxelMesh_ReliefGround(builder, inst, x, y, u0, v0, u1, v1);
    builder->lift = lift;
    builder->shift = shift;
}

/* The small crown is 16:32: width 1, length 2 tiles, at the same 50 degrees
 * and sunk the same way as the large one, standing on its one-cell trunk.
 * Dewford's own small tree has its art at 64,0, the General one at 32,32;
 * a trunk on a secondary tileset's ground keeps the General's crown.
 * Long grass is drawn the same: its tuft (16:32 at 80,32) leant back over
 * its floor (96,32), every cell of a patch alike. The tuft of the cell south
 * of a walker stands in front of it up to its shoulders, as the GBA's grass
 * covers all but its head. */
static void EmitSmallCell(VoxelBuilder *builder, const VoxelMapInstance *inst, int x, int y,
                          const void *secondaryTileset, int metatileId, int part, bool rock)
{
    static const uint8_t kTrunks[][2] = {
        [TRUNK_JUNGLE] = { 48, 48 }, [TRUNK_GRASS_ROAD] = { 80, 16 }, [TRUNK_VOLCAN] = { 64, 32 },
        [TRUNK_GENERAL] = { 48, 32 }, [TRUNK_ACCESS] = { 112, 32 },
    };
    bool grass = part == VOXEL_TREE_GRASS;
    int ground = grass ? -1 : SmallTrunkGround(secondaryTileset, metatileId);
    bool dewford = !grass && metatileId >= SECONDARY_ID && ground < 0;
    int cx = grass ? 80 : dewford ? 64 : 32, cy = dewford ? 0 : 32;
    int tx = ground >= 0 ? kTrunks[ground][0] : cx + 16, ty = ground >= 0 ? kTrunks[ground][1] : cy;
    float wx = (float)x, wz = (float)y;
    /* Against a mountain's face (its rock right behind the crown's cell,
     * Route 110's cave) or under higher ground right behind the trunk (a
     * terrace's edge) the crown stands up steeper, its top at the trunk
     * cell's back edge: leant back at 50 degrees it ran into the rock and
     * came out of it wherever the rock falls away, a corner's slope, and
     * the higher ground's top cut it across. */
    const float rise = 1.532089f, run = rock ? 0.8f : 1.285575f;
    const float baseHeight = -0.10f;
    /* 0.15 further forward than half the large tree's: any less and the
     * leaves stand through the ground behind the trunk. Dewford's another
     * 0.15, so its crown stands over the shadow drawn on its sand. */
    float baseZ = wz + (dewford ? 0.975f : 0.825f);

    EmitGround(builder, inst, x, y, TEX_U(tx), TEX_V(ty), TEX_U(tx + 16), TEX_V(ty + 16));
    builder->rounded = true;
    VoxelBuilder_Quad(builder,
        &(VoxelVertex){wx,        baseHeight + rise, baseZ - run, TEX_U(cx),      TEX_V(cy),      1.0f},
        &(VoxelVertex){wx + 1.0f, baseHeight + rise, baseZ - run, TEX_U(cx + 16), TEX_V(cy),      1.0f},
        &(VoxelVertex){wx + 1.0f, baseHeight,        baseZ,       TEX_U(cx + 16), TEX_V(cy + 32), 1.0f},
        &(VoxelVertex){wx,        baseHeight,        baseZ,       TEX_U(cx),      TEX_V(cy + 32), 1.0f});
    builder->rounded = false;
}

/* A 2x2 tree's art: its crown (rows of `rowPx` texels), its trunk's ground,
 * and how far into the trunk row the crown's foot stands (`foot`, cells
 * from its north edge): on its trunk's shadow. The General's large tree,
 * and the secret base's, closed or open: their upper rows are the same
 * drawing. The secret base's shadow is drawn further south and its crown
 * has no margin under the trunk: at the large tree's foot it stood on the
 * shadow's north rim, the tree floating over the ground. */
typedef struct
{
    uint8_t crownX, crownY, rowPx, groundX, groundY;
    float foot;
    /* the ground is a transparent decal over the water the ground pass
     * lays (VoxelTree_LaysGround), not an opaque plate */
    bool decal;
} LargeArt;

static const LargeArt kLargeTree = { 0, 0, 18, 32, 0, 0.35f, false };
static const LargeArt kBaseTree = { 0, 64, 16, 96, 0, 0.66f, false };
static const LargeArt kBaseTreeOpen = { 32, 64, 16, 96, 0, 0.66f, false };
/* A secondary tileset's own 2x2 trees: a 32x32 crown, trunk included, and
 * only the trunk row's ground (32x16); groundY is 16 texels above it, as the
 * trunk row is row 1. The foot stands the trunk's drawn foot on the middle
 * of its shadow. */
static const LargeArt kOwnTrees[] = {
    [OWN_GRASS] = { 64, 64, 16, 64, 80, 1.15f, false },    /* tree_base_mossdeep.png at 64,96 */
    [OWN_SHALLOWS] = { 64, 64, 16, 96, 80, 0.95f, true },  /* tree_base_water_mossdeep.png at 96,96 */
    [OWN_SOIL] = { 96, 64, 16, 0, 80, 1.00f, false },      /* tree_north_base_rock.png at 0,96 */
    [OWN_ASH] = { 96, 64, 16, 32, 80, 1.00f, false },      /* tree_north_base_ash.png at 32,96 */
};

static const LargeArt *LargeArtOf(const void *secondaryTileset, int metatileId)
{
    int ground;

    if (metatileId >= SECONDARY_ID && OwnLargePart(secondaryTileset, metatileId, &ground) >= 0)
        return &kOwnTrees[ground];
    switch (metatileId)
    {
    case 0x01E: case 0x01F: case 0x026: case 0x027: return &kBaseTree;
    case 0x036: case 0x037: return &kBaseTreeOpen;
    default: return &kLargeTree;
    }
}

/* One row of a large crown: the card that stands over the cell (x, y) as
 * the half of row `row`, column `col`, of a tree whose trunk row is
 * y - row + 1. */
static void EmitCrownCard(VoxelBuilder *builder, const LargeArt *art, int x, int y, int col, int row,
                          bool steep)
{
    float wx = (float)x, wz = (float)y;
    float u0 = (art->crownX + col * 16.0f) / VOXEL_TREE_TEXTURE_DIM;
    float u1 = u0 + 16.0f / VOXEL_TREE_TEXTURE_DIM;
    float v0 = 1.0f - (art->crownY + row * (float)art->rowPx) / VOXEL_TREE_TEXTURE_DIM;
    float v1 = v0 - (float)art->rowPx / VOXEL_TREE_TEXTURE_DIM;
    /* The crown keeps its aspect ratio: width 2, length 2.25 tiles for the
     * large tree's 32:36, 2 for the secret base's 32:32. sin/cos(50
     * degrees), fixed in the world rather than camera billboarding.
     * Lower the transparent bottom margin into the ground so the visible
     * leaves overlap the trunk instead of exposing a horizontal cut. */
    const float length = art->rowPx / 8.0f;
    /* Against rock or higher ground behind the trunk, a tileset's own
     * tree stands up steeper as the small one does (EmitSmallCell): leant
     * back, its upper row ran into the rock. */
    const float rise = 0.766044f * length, run = (steep ? 0.4f : 0.642788f) * length;
    const float baseHeight = -0.10f;
    float baseZ = wz - row + 1.0f + art->foot;
    float top = 1.0f - row * 0.5f, bottom = top - 0.5f;

    /* A crown card: lit as the rounded crown it stands for. */
    builder->rounded = true;
    VoxelBuilder_Quad(builder,
        &(VoxelVertex){wx,        baseHeight + top * rise,    baseZ - top * run,    u0, v0, 1.0f},
        &(VoxelVertex){wx + 1.0f, baseHeight + top * rise,    baseZ - top * run,    u1, v0, 1.0f},
        &(VoxelVertex){wx + 1.0f, baseHeight + bottom * rise, baseZ - bottom * run, u1, v1, 1.0f},
        &(VoxelVertex){wx,        baseHeight + bottom * rise, baseZ - bottom * run, u0, v1, 1.0f});
    builder->rounded = false;
}

static void EmitCell(VoxelBuilder *builder, const VoxelMapInstance *inst, const void *secondaryTileset,
                     int x, int y, int part, int metatileId, bool rock)
{
    const LargeArt *art = LargeArtOf(secondaryTileset, metatileId);
    int col = part & 1, row = part >> 1;
    float u0 = (art->groundX + col * 16.0f) / VOXEL_TREE_TEXTURE_DIM;
    float u1 = u0 + 16.0f / VOXEL_TREE_TEXTURE_DIM;
    float v0 = 1.0f - (art->groundY + row * 16.0f) / VOXEL_TREE_TEXTURE_DIM;
    float v1 = v0 - 16.0f / VOXEL_TREE_TEXTURE_DIM;

    if (part == VOXEL_TREE_SMALL || part == VOXEL_TREE_GRASS)
    {
        EmitSmallCell(builder, inst, x, y, secondaryTileset, metatileId, part, rock);
        return;
    }
    /* over the water: its roots and shadow just off the surface, where the
     * water shows through and keeps its animation */
    if (art->decal)
        VoxelMesh_Top(builder, (float)x, (float)y, 0.02f, 0.0f, u0, v0, u1, v1, 1.0f);
    else
        EmitGround(builder, inst, x, y, u0, v0, u1, v1);
    EmitCrownCard(builder, art, x, y, col, row,
                  rock && metatileId >= SECONDARY_ID && OwnLargePart(secondaryTileset, metatileId, NULL) >= 0);
}

/* A tree part at a cell anywhere: this map's, the next map's (its own
 * tileset), or the repeated border's past the last map. */
int VoxelTree_PartAt(int x, int y)
{
    const VoxelMapInstance *at = VoxelWorld_GetInstanceAt(x, y);

    if (at == NULL)
    {
        at = VoxelWorld_Instance(0);
        return VoxelWorld_UsesTreeSprites(at)
             ? VoxelTree_Part(at->secondaryTileset, VoxelWorld_BorderMetatile(x, y)) : -1;
    }
    return VoxelWorld_UsesTreeSprites(at) ? VoxelTree_Part(at->secondaryTileset, VoxelWorld_GetMetatileId(x, y))
                                          : -1;
}

/*
 * The other row of a large tree whose half is drawn over by something else:
 * a hedge, a roof, a fence or a ledge of the secondary tileset, whose
 * metatile is not a tree part. The GBA draws the rest of the leaves in that
 * metatile's art; here its row of the crown is added over it, so the crown
 * is not cut in two. Its ground is left as it is. Only inside the map: across
 * its edge the neighbour's own cells answer.
 */
/*
 * A secondary tileset's own tree (OwnLargePart) never has its crown row's
 * cells as parts: its trunk always adds that row, across the map's edge too
 * (the next map's cell, or the border's, is another tree's or its ground),
 * and so does a trunk of the repeated border (inst NULL).
 */
static void EmitMissingRow(VoxelBuilder *builder, const VoxelMapInstance *inst, const void *secondaryTileset,
                           int x, int y, int part, int metatileId, bool rock)
{
    int row = part >> 1;
    int other = row == 0 ? y + 1 : y - 1;
    bool own = metatileId >= SECONDARY_ID && OwnLargePart(secondaryTileset, metatileId, NULL) >= 0;

    if (part == VOXEL_TREE_SMALL || part == VOXEL_TREE_GRASS)
        return;
    if (own)
    {
        if (VoxelTree_PartAt(x, other) == (part ^ 2))
            return;
    }
    else
    {
        if (inst == NULL || other < inst->originY || other >= inst->originY + inst->height)
            return;
        if (VoxelTree_Part(inst->secondaryTileset, VoxelWorld_GetMetatileId(x, other)) == (part ^ 2))
            return;
    }
    EmitCrownCard(builder, LargeArtOf(secondaryTileset, metatileId), x, other, part & 1, row ^ 1, rock && own);
}

/*
 * Does a tree whose trunk is at (x, y) stand against rock: a mountain's rock
 * behind the crown's cell, or ground higher than the trunk's right behind it
 * (the next terrace up, the next row of a wood climbing one: Route 112, 119),
 * or a 2x2 crown row standing on the rock itself (Fallarbor's brick by its
 * Pokemon Center)? Leant back, its crown ran into it.
 */
static bool AgainstRock(const VoxelMapInstance *inst, int x, int y, float lift)
{
    unsigned behind = VoxelRegions_RoleAt((unsigned)inst->layoutId, x - inst->originX, y - 2 - inst->originY);
    unsigned over = VoxelRegions_RoleAt((unsigned)inst->layoutId, x - inst->originX, y - 1 - inst->originY);
    int part = VoxelTree_Part(inst->secondaryTileset, VoxelWorld_GetMetatileId(x, y));
    bool large = part != VOXEL_TREE_SMALL && part != VOXEL_TREE_GRASS;

    if (behind == VOXEL_ROLE_CLIFF || behind == VOXEL_ROLE_SHELF)
        return true;
    if (large && (over == VOXEL_ROLE_CLIFF || over == VOXEL_ROLE_SHELF))
        return true;
    if (y - 1 >= inst->originY && VoxelRelief_CellLift(inst, x, y - 1) > lift + 0.25f)
        return true;
    /* a 2x2 crown leans back over two rows: the terrace over a cliff's
     * face north of its crown row (Mossdeep's woods under the cliffs) */
    return large && y - 2 >= inst->originY && VoxelRelief_CellLift(inst, x, y - 2) > lift + 0.25f;
}

void VoxelTree_EmitInstance(VoxelBuilder *builder, const VoxelMapInstance *inst,
                            int x0, int y0, int x1, int y1)
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
                float lift = VoxelRelief_CellLift(inst, x, y);
                bool rock = AgainstRock(inst, x, y, lift);

                /* a 2x2 tree is one: both its columns stand alike, as
                 * either one's back is rock */
                if (part == 2 || part == 3)
                {
                    int other = part == 2 ? x + 1 : x - 1;

                    if (other >= inst->originX && other < inst->originX + inst->width)
                        rock = rock || AgainstRock(inst, other, y, lift);
                }
                builder->lift = lift;
                builder->shift = VoxelRelief_CellShift(inst, x, y);
                EmitCell(builder, inst, inst->secondaryTileset, x, y, part, metatileId, rock);
                /* At the tree's own lift, so both rows meet. */
                EmitMissingRow(builder, inst, inst->secondaryTileset, x, y, part, metatileId, rock);
                builder->lift = 0.0f;
                builder->shift = 0.0f;
            }
        }
}

void VoxelTree_EmitBorder(VoxelBuilder *builder, int x0, int y0, int x1, int y1)
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
            {
                /* at its map edge's level (VoxelMesh_BorderLevel) */
                float level, shift;

                VoxelMesh_BorderLevel(x, y, &level, &shift);
                builder->lift = level;
                builder->shift = shift;
                EmitCell(builder, NULL, current->secondaryTileset, x, y, part, metatileId, false);
                EmitMissingRow(builder, NULL, current->secondaryTileset, x, y, part, metatileId, false);
                builder->lift = 0.0f;
                builder->shift = 0.0f;
            }
        }
}
