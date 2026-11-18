#include <math.h>
#include <stddef.h>
#include <string.h>

#include "voxel_lighting.h"
#include "voxel_tree.h"
#include "voxel_building.h"
#include "voxel_relief.h"
#include "voxel_sign.h"

#if CTR_VOXEL_LIGHTING
/*
 * Casters and finished samples, kept for as long as the world they were read
 * from: VoxelLighting_Reset() is called whenever a live tile changes or the
 * maps on screen move (ctr_voxel.c's world epoch), and never per build. Reset
 * per chunk, as they used to be, every build re-classified the ~300 cells its
 * rays cross, most of which its neighbour had just classified.
 *
 * Direct mapped and exact: a bucket holds one coordinate and a collision only
 * costs a recomputation, never a wrong answer. Ordinary RAM, not linear.
 */
#define CELL_CACHE_SIZE 4096u
/* At most this many cells drawn across one (Drawn): a cell's own row and
 * its neighbours' where the relief steps, a fold where it steps down. */
#define DRAWN_FROM 4
typedef struct
{
    int16_t x, z;
    float top;        /* over the base */
    float base;       /* the base of the map the cell is on (VoxelRelief_Base) */
    /* On a drawn map, how high over the base what stands on the cell is
     * drawn (VoxelRelief_CellShift): a tree on ground two levels under the
     * base stands two levels down. */
    float lift;
    /* And how far south: as far as it stands high in the world, its lift and
     * its map's base (VoxelBuilder_StretchZ) - that tree two rows north of
     * its cell on a map at the world's level. */
    float shift;
    /* A drawn map's relief: the lowest of its surface over the cell. A point
     * under it is under the surface wherever in the cell it is. */
    float bottom;
    /* A shifted cell (shift not 0): the height past which nothing
     * ObjectAt tests in it can stop a ray (ShiftedClear), once known. */
    float clear;
    /* A drawn map's relief over the cell's own ground, whatever else stands
     * on the cell: a wood on a terrace is its crowns, drawn `shift` rows
     * south, and the terrace under them, which casts as the bare rock beside
     * it does. 0 where there is none. */
    float relief, reliefBottom;
    float reliefTop;  /* its highest, kept where `relief` is too low to cast */
    /* For the rays that start over the cell (RayBounds), once known: the
     * height past which they meet nothing, and whether they may cross a
     * map's eased rows. */
    float rayCeiling;
    uint8_t rayKnown; /* 0: not yet; 1: known; 2: known, and stretched */
    const VoxelMapInstance *reliefOn; /* the map whose table it is read from */
    uint32_t generation;
    int8_t crownPart; /* VoxelTree_Part when the cell is a crown, else -1 */
    bool surface;     /* a drawn map's relief: tested as a height field */
    bool sign;        /* a sign or a lamp: tested against its drawing */
    bool clearKnown;  /* `clear` and `from` worked out (Drawn) */
    /* On a drawn map (or one off the world's level): what is drawn over the
     * cell comes from other cells, ObjectAt. */
    bool drawn;
    uint8_t fromCount;
    /* The cells whose casters are drawn across this one's row, `from[i]`
     * rows north of it (negative: south) - each drawn as far south as its
     * own lift and base, which on relief is not this cell's. */
    int8_t from[DRAWN_FROM];
    const uint16_t *mask; /* a model over part of the cell: its footprint */
} LightCell;
static LightCell sCells[CELL_CACHE_SIZE];

/* Adjacent ground quads share their half-tile samples. Cache the final ray/AO
 * result as well as casters; exact coordinates keep walls and sloped crowns
 * distinct even when their hash buckets collide. */
#define SAMPLE_CACHE_SIZE 4096u
typedef struct
{
    float x, y, z;
    float contact; /* the contact AO factor, 1 where nothing stands close */
    bool lit;      /* the ray reached the sun */
    /* The sun `lit` was cast under (sSunGeneration); any other, and only the
     * contact is known - a point only its contact was asked of, or one lit
     * by an earlier sun. The contact reads the casters alone, never the sun
     * (Contact), so it outlives a sun that moves. */
    uint16_t sun;
    uint32_t generation;
} LightSample;
static LightSample sSamples[SAMPLE_CACHE_SIZE];
#define SAMPLE_WAYS 4u
#define SAMPLE_SETS (SAMPLE_CACHE_SIZE / SAMPLE_WAYS)
static uint8_t sSampleNext[SAMPLE_SETS];
static uint32_t sGeneration;
static uint32_t sSampleGeneration;
/* Bumped by every new sun (VoxelLighting_SetSun); never 0, which no sun is. */
static uint16_t sSunGeneration = 1;
VoxelSun gVoxelSun = {0.85f, 0.55f, 1.0f, 0};
static float sAmbient = VOXEL_AMBIENT;
/*
 * SHADOW QUALITY HIGH (VoxelLighting_SetQuality): the GPU's shadow map
 * (ctr_voxel.c "Shadow map") casts every shadow, the trees' too (their
 * crowns turned to the sun, TreeShadowCards): no ray is cast here, every
 * sample is in the sun and keeps its contact shade, and a face turned from
 * the sun takes HIGH's ambient. LOW is the lighting as it always was.
 */
static bool sHigh;

/* LOW's ambient, or HIGH's under the shadow map, for a sun this strong. */
static float AmbientOf(float strength)
{
    float ambient = sHigh ? VOXEL_SHADOW_MAP_DARK : VOXEL_AMBIENT;

    return 1.0f - (1.0f - ambient) * strength;
}
static float sStepInverseX = 4.0f / 0.85f, sStepInverseZ = 4.0f / 0.55f;
static unsigned sRays;
/* Lit without the blocks' bound (VoxelLighting_SampleAlone). */
static bool sAlone;

/*
 * No caster anywhere on screen is taller than this, so a ray that has climbed
 * past it can stop: Occludes() needs a caster whose top is above the ray. It
 * is what most rays reach long before VOXEL_LIGHT_REACH - a street of two-
 * storey houses tops out at a quarter of the reach - and it changes no answer.
 */
static float sCeiling;
#define CROWN_TOP 1.62f
/* The least the ceiling is ever set to: the tallest sign or lamp drawing. */
#define PROP_TOP 2.0f
/*
 * The same bound map by map: a caster stands on one map, over its base, and
 * is never taller than that map's own tallest model, relief, prop or crown.
 * A ray can only meet casters of the maps its reach overlaps (RayCeiling), so
 * on a route of trees it stops at their height rather than at that of the
 * tallest building in the game. Every answer is the same.
 */
static float sMapCeiling[MAX_VOXEL_MAP_INSTANCES];
static unsigned sMapCeilings;

/*
 * And finer still, block by block: the tallest caster of each 4x4 cells. A
 * map's ceiling is that of its tallest model, so on Rustboro's streets every
 * ray climbed to the height of the Devon tower however far from it it was.
 * A ray only meets the cells its remaining points fall in, so past the
 * tallest of the blocks those cover nothing can stop it (Lit). Worked out
 * from Cell() itself, block by block as rays first need them, and kept for
 * the generation like the cells: the same casters, so the same answers.
 */
#define BLOCK_SHIFT 2
#define BLOCK_CACHE_SIZE 1024u
/* Over each caster's own top + base: Lit compares the ray with the sum and
 * CellOccludes with the difference, which may part by a rounding. */
#define BLOCK_MARGIN (1.0f / 256.0f)
typedef struct
{
    int16_t bx, bz;
    float ceiling;    /* world height; -1000 where nothing casts */
    uint32_t generation;
} LightBlock;
static LightBlock sBlocks[BLOCK_CACHE_SIZE];
/*
 * The tallest of a block and the two blocks west and north of it (3x3): all
 * a ray from inside the block can reach (VOXEL_LIGHT_REACH runs 6.8 cells
 * west and 4.4 north, less than two blocks). One lookup per ray rather than
 * one per block its reach covers: with those, rays that were short already
 * cost more than they saved. Its own table, as working it out reads blocks
 * whose entries it would otherwise evict.
 */
#define REACH_BLOCKS 2
/* How far a block's ceiling looks north and south for what is drawn into it
 * from another cell (ObjectAt): a tree four levels up a drawn mountain. */
#define SHIFT_ROWS 4
/* How far north of a row past every map's cells Cell looks for the map whose
 * drawing reaches it: a map stands as far south as it is high in the world. */
#define DRAWN_PAST_ROWS 16

/* How far a map's drawing reaches past its cells, north or south, rows: its
 * base and, on a drawn map, SHIFT_ROWS of relief over it. */
static int DrawnReach(const VoxelMapInstance *inst)
{
    float reach = fabsf(VoxelRelief_Base(inst));

    return (int)ceilf(reach) + (VoxelRelief_IsDrawn(inst) ? SHIFT_ROWS : 0);
}
static LightBlock sReach[BLOCK_CACHE_SIZE];
/* Read for a ray with more than this left to climb to the map's bound. */
#define BLOCK_CLIMB 1.0f

/* The work rays do, for the slow chunk log: points marched, and of those (and
 * of the contact tests) how many were tested against a relief's surface. */
static unsigned sSteps, sSurfaceTests;

static int Tile(float n)
{
    int i = (int)n;
    return n < (float)i ? i - 1 : i;
}

/*
 * A map's eased rows (VoxelRelief_Stretch) are drawn longer than its cells
 * stand: the rays run where things are drawn - a point on the eased ground,
 * on the map south of it - and what may stop them is looked up where it
 * stands, each point taken back through the stretch of the map drawn there.
 * Else the casters of Route 111's last rows shaded Mauville from two rows
 * north of where they are drawn: a cut along the seam, shadows of nothing.
 */
typedef struct
{
    float x0, x1, from, to, add;
} LightStretch;
static LightStretch sStretches[MAX_VOXEL_MAP_INSTANCES];
static unsigned sStretchCount, sStretchMaps = ~0u;

static void ReadStretches(void)
{
    sStretchCount = 0;
    sStretchMaps = VoxelWorld_InstanceCount();
    for (unsigned i = 0; i < sStretchMaps && i < MAX_VOXEL_MAP_INSTANCES; ++i)
    {
        const VoxelMapInstance *inst = VoxelWorld_Instance(i);
        LightStretch *s = &sStretches[sStretchCount];

        if (inst != NULL && VoxelRelief_Stretch(inst, &s->from, &s->to, &s->add))
        {
            s->x0 = (float)inst->originX;
            s->x1 = (float)(inst->originX + inst->width);
            ++sStretchCount;
        }
    }
}

/* Where what is drawn at (x, z) stands. */
static float Standing(float x, float z)
{
    if (sStretchMaps != VoxelWorld_InstanceCount())
        ReadStretches();
    for (unsigned i = 0; i < sStretchCount; ++i)
    {
        const LightStretch *s = &sStretches[i];

        if (x >= s->x0 && x < s->x1 && z > s->from && z < s->to + s->add)
            return s->from + (z - s->from) * (s->to - s->from) / (s->to - s->from + s->add);
    }
    return z;
}

/* Does a ray from (x, z) cross a map's eased rows? */
static bool RayStretched(float x, float z)
{
    float endX = x - VOXEL_SUN_DX * VOXEL_LIGHT_REACH;
    float endZ = z - VOXEL_SUN_DZ * VOXEL_LIGHT_REACH;

    if (sStretchMaps != VoxelWorld_InstanceCount())
        ReadStretches();
    for (unsigned i = 0; i < sStretchCount; ++i)
    {
        const LightStretch *s = &sStretches[i];

        if (fmaxf(x, endX) + 1.0f > s->x0 && fminf(x, endX) - 1.0f < s->x1
         && fmaxf(z, endZ) + 1.0f > s->from && fminf(z, endZ) - 1.0f < s->to + s->add)
            return true;
    }
    return false;
}

static float CasterCeiling(void)
{
    float ceiling = PROP_TOP > CROWN_TOP ? PROP_TOP : CROWN_TOP;
    /* (what stands on a drawn map's terrace stands that much higher) */
    float buildings = VoxelBuildings_MaxTop(), highest = 0.0f, relief = 0.0f;

    if (buildings > ceiling)
        ceiling = buildings;
    /* over the base of the highest map on screen */
    for (unsigned i = 0; i < VoxelWorld_InstanceCount(); ++i)
    {
        const VoxelMapInstance *inst = VoxelWorld_Instance(i);

        if (inst == NULL || inst->indoor)
            continue;
        if (VoxelRelief_DrawnTop(inst) > relief)
            relief = VoxelRelief_DrawnTop(inst);
        if (VoxelRelief_Base(inst) > highest)
            highest = VoxelRelief_Base(inst);
    }
    return ceiling + relief + highest;
}

static void MapCeilings(void)
{
    ReadStretches();
    sMapCeilings = 0;
    for (unsigned i = 0; i < VoxelWorld_InstanceCount() && i < MAX_VOXEL_MAP_INSTANCES; ++i)
    {
        const VoxelMapInstance *inst = VoxelWorld_Instance(i);
        float top = PROP_TOP > CROWN_TOP ? PROP_TOP : CROWN_TOP;

        if (inst == NULL)
            break;
        /* Indoors nothing casts (Cell). */
        if (inst->indoor)
            top = -1000.0f;
        else
        {
            if (VoxelBuildings_LayoutTop(inst) > top)
                top = VoxelBuildings_LayoutTop(inst);
            /* a crown, a model or a sign on its highest terrace */
            top += VoxelRelief_DrawnTop(inst) + VoxelRelief_Base(inst);
        }
        sMapCeiling[sMapCeilings++] = top;
    }
}

static void ResetSamples(void)
{
    if (++sSampleGeneration == 0)
    {
        memset(sSamples, 0, sizeof(sSamples));
        sSampleGeneration = 1;
    }
}

bool VoxelLighting_SetSun(float dx, float dz, float strength, uint32_t key)
{
    if (!isfinite(dx) || !isfinite(dz) || !isfinite(strength)
     || fabsf(dx) > 1.5f || fabsf(dz) > 1.5f || strength < 0.0f || strength > 1.0f)
        return false;
    if (gVoxelSun.dx == dx && gVoxelSun.dz == dz
     && gVoxelSun.strength == strength && gVoxelSun.key == key)
        return false;
    gVoxelSun = (VoxelSun){dx, dz, strength, key};
    sAmbient = AmbientOf(strength);
    sStepInverseX = fabsf(dx) > 0.0001f ? 4.0f / dx : 0.0f;
    sStepInverseZ = fabsf(dz) > 0.0001f ? 4.0f / dz : 0.0f;
    /* Every ray is cast again, but not the contacts: they test the casters
     * round a point, which no sun moves, and with the world unchanged (a
     * change resets the samples) each is the number it would be again. A
     * door with DAY / NIGHT on is a new sun both ways. Wrapped round, a
     * sample cast 65536 suns ago would pass for this one's: all go then. */
    if (++sSunGeneration == 0)
    {
        ResetSamples();
        sSunGeneration = 1;
    }
    return true;
}

bool VoxelLighting_SetQuality(int quality)
{
    bool high = quality == VOXEL_SHADOWS_HIGH;

    if (high == sHigh)
        return false;
    sHigh = high;
    sAmbient = AmbientOf(gVoxelSun.strength);
    /* every sample's ray was cast, or not */
    ResetSamples();
    return true;
}

int VoxelLighting_Quality(void)
{
    return sHigh ? VOXEL_SHADOWS_HIGH : VOXEL_SHADOWS_LOW;
}

float VoxelLighting_Ambient(void)
{
    return sAmbient;
}

void VoxelLighting_Reset(void)
{
    if (++sGeneration == 0)
    {
        memset(sCells, 0, sizeof(sCells));
        memset(sBlocks, 0, sizeof(sBlocks));
        memset(sReach, 0, sizeof(sReach));
        sGeneration = 1;
    }
    ResetSamples();
    sCeiling = CasterCeiling();
    MapCeilings();
}

/*
 * The height past which nothing a ray from (x, z) can meet stands: over the
 * maps its reach overlaps (it runs towards the sun, and a cell's
 * point may lie a little off the ray's own), never above the global bound.
 */
static float RayCeiling(float x, float z)
{
    float endX = x - VOXEL_SUN_DX * VOXEL_LIGHT_REACH;
    float endZ = z - VOXEL_SUN_DZ * VOXEL_LIGHT_REACH;
    float x0 = fminf(x, endX) - 1.0f, x1 = fmaxf(x, endX) + 1.0f;
    float z0 = fminf(z, endZ) - 1.0f, z1 = fmaxf(z, endZ) + 1.0f;
    float ceiling = -1000.0f;
    unsigned count = VoxelWorld_InstanceCount();

    /* Maps put on screen since the last reset: the global bound. */
    if (count != sMapCeilings)
        return sCeiling;
    for (unsigned i = 0; i < count; ++i)
    {
        const VoxelMapInstance *inst = VoxelWorld_Instance(i);

        if ((float)inst->originX < x1 && (float)(inst->originX + inst->width) > x0
         && (float)inst->originY < z1 && (float)(inst->originY + inst->height) > z0
         && sMapCeiling[i] > ceiling)
            ceiling = sMapCeiling[i];
    }
    return ceiling < sCeiling ? ceiling : sCeiling;
}

static const LightCell *Cell(int x, int z);

/*
 * RayCeiling and RayStretched for every ray that starts over cell (tx, tz),
 * kept with the cell: both walk the maps on screen, a hundred comparisons a
 * ray, and the rays of a cell's ground all start over it. The cell's whole
 * square is asked, so the bound is a little wider than a point's own: no
 * lower, and a ray it calls stretched that is not marches point by point to
 * the same answer.
 */
static void RayBounds(float x, float z, float *ceiling, bool *stretched)
{
    int tx = Tile(x), tz = Tile(z);
    LightCell *cell = (LightCell *)Cell(tx, tz);
    float x0, x1, z0, z1, top = -1000.0f;
    unsigned count = VoxelWorld_InstanceCount();
    bool eased = false;

    /* Maps put on screen since the last reset: the global bound. */
    if (count != sMapCeilings)
    {
        *ceiling = sCeiling;
        *stretched = RayStretched(x, z);
        return;
    }
    if (cell->rayKnown != 0)
    {
        *ceiling = cell->rayCeiling;
        *stretched = cell->rayKnown == 2;
        return;
    }
    x0 = fminf((float)tx, (float)tx - VOXEL_SUN_DX * VOXEL_LIGHT_REACH) - 1.0f;
    x1 = fmaxf((float)tx, (float)tx - VOXEL_SUN_DX * VOXEL_LIGHT_REACH) + 2.0f;
    z0 = fminf((float)tz, (float)tz - VOXEL_SUN_DZ * VOXEL_LIGHT_REACH) - 1.0f;
    z1 = fmaxf((float)tz, (float)tz - VOXEL_SUN_DZ * VOXEL_LIGHT_REACH) + 2.0f;
    for (unsigned i = 0; i < count; ++i)
    {
        const VoxelMapInstance *inst = VoxelWorld_Instance(i);

        if ((float)inst->originX < x1 && (float)(inst->originX + inst->width) > x0
         && (float)inst->originY < z1 && (float)(inst->originY + inst->height) > z0
         && sMapCeiling[i] > top)
            top = sMapCeiling[i];
    }
    if (top > sCeiling)
        top = sCeiling;
    if (sStretchMaps != count)
        ReadStretches();
    for (unsigned i = 0; i < sStretchCount; ++i)
    {
        const LightStretch *s = &sStretches[i];

        if (x1 > s->x0 && x0 < s->x1 && z1 > s->from && z0 < s->to + s->add)
            eased = true;
    }
    cell->rayCeiling = top;
    cell->rayKnown = eased ? 2 : 1;
    *ceiling = top;
    *stretched = eased;
}

static const LightCell *Cell(int x, int z)
{
    unsigned key = ((uint32_t)x * 73856093u ^ (uint32_t)z * 19349663u)
                 & (CELL_CACHE_SIZE - 1);
    LightCell *cell = &sCells[key];
    const VoxelMapInstance *inst;
    int metatile;

    if (cell->generation == sGeneration && cell->x == x && cell->z == z)
        return cell;
    cell->generation = sGeneration;
    cell->x = (int16_t)x;
    cell->z = (int16_t)z;
    cell->top = 0.0f;
    cell->crownPart = -1;
    cell->surface = false;
    cell->sign = false;
    cell->clearKnown = false;
    cell->rayKnown = 0;
    cell->drawn = false;
    cell->fromCount = 0;
    cell->relief = 0.0f;
    cell->reliefBottom = 0.0f;
    cell->reliefTop = 0.0f;
    cell->mask = NULL;
    inst = VoxelWorld_GetInstanceAt(x, z);
    cell->base = VoxelRelief_Base(inst);
    cell->lift = 0.0f;
    cell->shift = 0.0f;
    if (inst == NULL)
    {
        /* Past every map's cells: what is drawn here is the map north of it,
         * as far south as it stands high (Fortree's last rows, on its base,
         * past its south edge). Looked for there, as ObjectAt looks for what
         * is drawn across a shifted cell, from the depth of that map's edge. */
        for (int k = 1; k <= DRAWN_PAST_ROWS; ++k)
        {
            const VoxelMapInstance *north = VoxelWorld_GetInstanceAt(x, z - k);
            float reach;

            if (north == NULL)
                continue;
            reach = VoxelRelief_Base(north)
                  + (VoxelRelief_IsDrawn(north) ? VoxelRelief_CellShift(north, x, z - k) : 0.0f);
            if (!north->indoor && reach > (float)k - 1.0f)
            {
                cell->base = VoxelRelief_Base(north);
                cell->shift = reach;
                cell->drawn = true;
                /* and its relief, drawn as far: its table runs on here */
                if (VoxelRelief_IsDrawn(north))
                {
                    cell->relief = VoxelRelief_SurfaceTop(north, x, z, &cell->reliefBottom);
                    cell->reliefOn = north;
                    cell->reliefTop = cell->relief;
                    if (cell->relief <= 0.05f)
                        cell->relief = 0.0f;
                }
            }
            break;
        }
        return cell;
    }
    if (inst->indoor)
        return cell;
    if (VoxelRelief_IsDrawn(inst))
        cell->lift = VoxelRelief_CellShift(inst, x, z);
    cell->shift = cell->lift + cell->base;
    cell->drawn = cell->shift != 0.0f || VoxelRelief_IsDrawn(inst);
    if (VoxelRelief_IsDrawn(inst))
    {
        cell->relief = VoxelRelief_SurfaceTop(inst, x, z, &cell->reliefBottom);
        cell->reliefOn = inst;
        cell->reliefTop = cell->relief;
        if (cell->relief <= 0.05f)
            cell->relief = 0.0f;
    }
    metatile = VoxelWorld_GetMetatileId(x, z);
    if (VoxelWorld_UsesTreeSprites(inst))
    {
        int part = VoxelTree_Part(inst->secondaryTileset, metatile);
        float over;

        /* Long grass is low: no crown to cast, the ground it is */
        if (part == VOXEL_TREE_GRASS)
            part = -1;

        /* A wood under a deck (Fortree's tree houses): the deck casts,
         * standing over the crowns. */
        if (part >= 0 && VoxelBuildings_CellAt(inst, x, z, NULL, &over) && over > CROWN_TOP)
        {
            cell->top = over;
            cell->mask = VoxelBuildings_Footprint(inst, x, z);
            return cell;
        }
        if (part >= 0)
        {
            /* A rounded crown proxy, rather than the opaque rectangular card.
             * Match the replacement tree's two-tile footprint and 1.62 height. */
            cell->crownPart = (int8_t)part;
            cell->top = CROWN_TOP;
            return cell;
        }
        if (VoxelTree_GroundMetatile(inst->secondaryTileset, metatile) != metatile)
            return cell; /* canopy fringe removed by the tree renderer */
    }
    /* A modelled building casts from its own solid, cell by cell - and a
     * railing from its line, not from its whole cell. */
    if (VoxelBuildings_CellAt(inst, x, z, NULL, &cell->top))
    {
        cell->mask = VoxelBuildings_Footprint(inst, x, z);
        return cell;
    }
    /* A sign or a lamp casts its own drawing, not a box. */
    cell->top = VoxelSign_CasterTop(inst, x, z);
    if (cell->top > 0.0f)
    {
        cell->sign = true;
        return cell;
    }
    /* A map read off its drawing casts from its relief as it stands - the
     * mountain on the grass, a terrace on the one below - tested as a height
     * field, so a slope facing the sun is not shadowed by its own cell. */
    if (VoxelRelief_IsDrawn(inst))
    {
        cell->top = cell->relief;
        cell->bottom = cell->reliefBottom;
        cell->surface = cell->top > 0.0f;
        return cell;
    }
    /* Anything else is ground: relief elsewhere (a rock band, a ledge's lip)
     * is a sheet, not a box, and nothing is raised from a guess, so nothing
     * casts from one. The only casters are what was modelled, above. */
    return cell;
}

/* Floor division by the block side, negative tiles included. */
static int BlockOf(int tile)
{
    return tile >= 0 ? tile >> BLOCK_SHIFT : -((-tile + (1 << BLOCK_SHIFT) - 1) >> BLOCK_SHIFT);
}

/* The height a ray must pass to clear every caster of block (bx, bz). */
static float BlockCeiling(int bx, int bz)
{
    unsigned key = ((uint32_t)bx * 73856093u ^ (uint32_t)bz * 19349663u)
                 & (BLOCK_CACHE_SIZE - 1);
    LightBlock *block = &sBlocks[key];
    float ceiling = -1000.0f;
    int x0 = bx * (1 << BLOCK_SHIFT), z0 = bz * (1 << BLOCK_SHIFT);
    int x1 = x0 + (1 << BLOCK_SHIFT), z1 = z0 + (1 << BLOCK_SHIFT);
    bool onMap = false;
    int margin = 0;

    if (block->generation == sGeneration && block->bx == bx && block->bz == bz)
        return block->ceiling;
    /* Off every map's drawing no cell casts (Cell): not worth classifying
     * sixteen cells, each evicting one rays do test. A map's drawing is its
     * cells and the rows round them it reaches (DrawnReach): what stands on
     * its cells is drawn as far south as it stands high, ObjectAt. */
    for (unsigned i = 0; i < VoxelWorld_InstanceCount(); ++i)
    {
        const VoxelMapInstance *inst = VoxelWorld_Instance(i);
        int reach;

        if (inst == NULL || inst->originX >= x1 || inst->originX + inst->width <= x0)
            continue;
        reach = DrawnReach(inst);
        if (inst->originY < z1 + reach && inst->originY + inst->height > z0 - reach)
        {
            onMap = true;
            if (reach > margin)
                margin = reach;
        }
    }
    for (int z = z0 - margin; onMap && z < z1 + margin; ++z)
        for (int x = x0; x < x1; ++x)
        {
            const LightCell *cell = Cell(x, z);
            float lift = cell->surface ? 0.0f : cell->lift;
            /* drawn from z + shift to a row further south (ObjectAt) */
            int at = cell->surface ? z : Tile((float)z + cell->shift);
            int last = cell->surface ? z : Tile((float)z + 0.999f + cell->shift);

            /* and the relief, on the cell's own ground */
            if (z >= z0 && z < z1 && cell->relief > 0.0f
             && cell->relief + cell->base + BLOCK_MARGIN > ceiling)
                ceiling = cell->relief + cell->base + BLOCK_MARGIN;
            /* CellOccludes: nothing under its top, and nothing at all from a
             * cell whose top is not above its base (a surface's always is). */
            if (last < z0 || at >= z1)
                continue;
            if (cell->top > 0.0f && cell->top + cell->base + lift + BLOCK_MARGIN > ceiling)
                ceiling = cell->top + cell->base + lift + BLOCK_MARGIN;
        }
    block->generation = sGeneration;
    block->bx = (int16_t)bx;
    block->bz = (int16_t)bz;
    block->ceiling = ceiling;
    return ceiling;
}

/* The tallest caster of the blocks from (bx0, bz0) to (bx1, bz1). */
static float BlocksCeiling(int bx0, int bz0, int bx1, int bz1)
{
    float ceiling = -1000.0f;

    for (int bz = bz0; bz <= bz1; ++bz)
        for (int bx = bx0; bx <= bx1; ++bx)
        {
            float block = BlockCeiling(bx, bz);

            if (block > ceiling)
                ceiling = block;
        }
    return ceiling;
}

/* The same over (bx0, bz0) to (bx1, bz1), from sReach when it covers them. */
static float ReachCeiling(int bx0, int bz0, int bx1, int bz1)
{
    if (bx0 > bx1)
    {
        int first = bx1;

        bx1 = bx0;
        bx0 = first;
    }
    if (bz0 > bz1)
    {
        int first = bz1;

        bz1 = bz0;
        bz0 = first;
    }
    unsigned key = ((uint32_t)bx1 * 73856093u ^ (uint32_t)bz1 * 19349663u)
                 & (BLOCK_CACHE_SIZE - 1);
    LightBlock *reach = &sReach[key];

    if (bx1 - bx0 > REACH_BLOCKS || bz1 - bz0 > REACH_BLOCKS)
        return BlocksCeiling(bx0, bz0, bx1, bz1);
    if (reach->generation != sGeneration || reach->bx != bx1 || reach->bz != bz1)
    {
        reach->ceiling = BlocksCeiling(bx1 - REACH_BLOCKS, bz1 - REACH_BLOCKS, bx1, bz1);
        reach->generation = sGeneration;
        reach->bx = (int16_t)bx1;
        reach->bz = (int16_t)bz1;
    }
    return reach->ceiling;
}

/* A ray this far under a drawn map's surface is under it, not grazing it. */
#define SURFACE_BIAS 0.03f
/* How far over its point a ray starts. */
#define RAY_BIAS 0.04f

static bool CellOccludes(const LightCell *cell, float x, float y, float z)
{
    /* Everything a cell casts from stands on its map's base. */
    y -= cell->base;
    /* Level ground stops no sun, even for a point below it: a beach under
     * its cliff (Route 104) is lit where nothing stands over it. */
    if (cell->top <= y || (cell->top <= 0.0f && !cell->surface))
        return false;
    if (cell->surface)
    {
        /* Under the lowest of it: inside the rock, with no need to read
         * where (a ray into a mountain, a corner's contact against a wall). */
        if (y < cell->bottom - SURFACE_BIAS)
            return true;
        if (cell->top - cell->bottom < 1.0f / 64.0f)
            return false;
        ++sSurfaceTests;
        return y < VoxelRelief_SurfaceOn(cell->reliefOn, x, z) - SURFACE_BIAS;
    }
    if (cell->mask != NULL)
    {
        int px = (int)((x - (float)cell->x) * 16.0f), pz = (int)((z - (float)cell->z) * 16.0f);

        px = px < 0 ? 0 : px > 15 ? 15 : px;
        pz = pz < 0 ? 0 : pz > 15 ? 15 : pz;
        return (cell->mask[pz] >> px) & 1u;
    }
    if (cell->sign)
        return VoxelSign_Occludes(VoxelWorld_GetInstanceAt(cell->x, cell->z), cell->x, cell->z,
                                  x, y, z);
    if (cell->crownPart == VOXEL_TREE_SMALL)
    {
        /* Half the large proxy across, kept on its own cell: in a wood the
         * cell to the north is the next small tree and carries its own. */
        /* Reciprocals: a division is some twenty cycles on the VFP and
         * three of them were taken at every point of a ray over a wood. */
        float dx = (x - ((float)cell->x + 0.5f)) * (1.0f / 0.48f);
        float dz = (z - ((float)cell->z + 0.35f)) * (1.0f / 0.55f);
        float dy = (y - 0.80f) * (1.0f / 0.62f);
        return dx * dx + dz * dz + dy * dy < 1.0f;
    }
    if (cell->crownPart >= 0)
    {
        float crownX = (float)(cell->x - (cell->crownPart & 1)) + 1.0f;
        float crownZ = (float)(cell->z - (cell->crownPart >> 1)) + 0.65f;
        float dx = (x - crownX) * (1.0f / 0.95f);
        float dz = (z - crownZ) * (1.0f / 0.65f);
        float dy = (y - 0.95f) * (1.0f / 0.67f);
        return dx * dx + dz * dz + dy * dy < 1.0f;
    }
    return true;
}

/* Under the relief drawn over the cell's ground (`relief`)? */
static bool ReliefOccludes(const LightCell *cell, float x, float y, float z)
{
    y -= cell->base;
    if (cell->relief <= y)
        return false;
    /* Under the lowest of it: inside the rock, with no need to read where. */
    if (y < cell->reliefBottom - SURFACE_BIAS)
        return true;
    /* Level all over (most of a terrace): not under its lowest is over it. */
    if (cell->relief - cell->reliefBottom < 1.0f / 64.0f)
        return false;
    ++sSurfaceTests;
    return y < VoxelRelief_SurfaceOn(cell->reliefOn, x, z) - SURFACE_BIAS;
}

/*
 * A drawn cell's `from` and `clear`: the cells whose drawing lies across its
 * row, and the height past which nothing ObjectAt tests in it can stop a ray -
 * its own relief and the tallest of theirs, and the ray only climbs, so past
 * it none can. A cell of a drawn map is drawn from its row + shift to the row
 * after, its shift its own (its lift and its map's base): where the relief
 * steps, the cells drawn over a row are not those the row's own shift points
 * to - a wood on a terrace was looked for with the shift of the ground under
 * the ray, three levels off, and cast nothing on the ground below it. Read
 * once a cell, as rays first need it, and kept with it.
 */
static float Drawn(int tx, int tz)
{
    LightCell *cell = (LightCell *)Cell(tx, tz);
    float clear, low, high;
    const VoxelMapInstance *own;
    int8_t from[DRAWN_FROM];
    unsigned count = 0;
    int lo, hi;

    if (cell->clearKnown)
        return cell->clear;
    clear = cell->relief > 0.0f ? cell->relief + cell->base : -1000.0f;
    /* What is drawn here stands on the ground drawn here: as high as the
     * relief over this cell, between its lowest and its highest, and so as
     * many rows south of its own cell, its map's base more. A row either
     * side for a slope's cell, lifted as its middle is. Every row a cell
     * could be drawn from was a score of lookups a cell, most of them a
     * search of the relief's cells: a fifth of a cold chunk's build. */
    low = cell->base + fminf(cell->reliefBottom, 0.0f);
    high = cell->base + fmaxf(cell->reliefTop, 0.0f);
    /* (and, by a seam, what the map past it draws on from its own base) */
    for (unsigned i = 0; i < VoxelWorld_InstanceCount(); ++i)
    {
        const VoxelMapInstance *on = VoxelWorld_Instance(i);
        float base;

        if (on == NULL || on->indoor || on->originX > tx || on->originX + on->width <= tx)
            continue;
        base = VoxelRelief_Base(on);
        if (base < low) low = base;
        if (base > high) high = base;
    }
    lo = (int)floorf(low) - 1;
    hi = (int)ceilf(high) + 1;
    own = VoxelWorld_GetInstanceAt(tx, tz);
    memset(from, 0, sizeof(from));
    for (int k = lo; k <= hi; ++k)
    {
        /* (its own map's rows without the search for the map) */
        const VoxelMapInstance *on = own != NULL && tz - k >= own->originY
                                  && tz - k < own->originY + own->height
                                   ? own : VoxelWorld_GetInstanceAt(tx, tz - k);
        const LightCell *o;
        float at;

        /* (where it is drawn first, from its map alone: a cell is read
         * whole only if its drawing lies across this row) */
        if (on == NULL || on->indoor)
            continue;
        at = (float)(tz - k) + VoxelRelief_Base(on)
           + (VoxelRelief_IsDrawn(on) ? VoxelRelief_CellShift(on, tx, tz - k) : 0.0f);
        if (at >= (float)(tz + 1) || at + 1.0f <= (float)tz)
            continue;
        o = Cell(tx, tz - k);
        if (o->surface || o->top <= 0.0f)
            continue;
        if (count < DRAWN_FROM)
            from[count++] = (int8_t)k;
        if (o->top + o->base + o->lift > clear)
            clear = o->top + o->base + o->lift;
    }
    /* (the lookups may have reused its bucket: found again) */
    cell = (LightCell *)Cell(tx, tz);
    cell->clear = clear;
    memcpy(cell->from, from, sizeof(from));
    cell->fromCount = (uint8_t)count;
    cell->clearKnown = true;
    return clear;
}

/*
 * Is the world point (x, y, z) inside a caster? A drawn map's relief stands in
 * the world as it is; a tree, a model or a sign on one of its cells is drawn
 * as far south and as high as the cell is lifted (VoxelRelief_CellShift), and
 * is looked for there - else a wood on Route 111's low ground, two levels
 * under its base, cast its shadows two rows south of its trees, onto open
 * paths. `seen` gets the cell the point is in, for the march.
 */
static bool ObjectAt(int tx, float x, float y, float z, const LightCell **seen)
{
    const LightCell *cell = Cell(tx, Tile(z));
    int tz;

    *seen = cell;
    if (!cell->drawn)
        return CellOccludes(cell, x, y - cell->lift, z);
    if (cell->relief > 0.0f && ReliefOccludes(cell, x, y, z))
        return true;
    tz = cell->z;
    if (!cell->clearKnown)
    {
        Drawn(tx, tz);
        cell = Cell(tx, tz);
        *seen = cell;
    }
    if (cell->fromCount != 0)
    {
        int8_t from[DRAWN_FROM];
        unsigned count = cell->fromCount;

        memcpy(from, cell->from, sizeof(from));
        for (unsigned i = 0; i < count; ++i)
        {
            const LightCell *o = Cell(tx, tz - from[i]);
            float oz = z - o->shift;

            /* only where it is drawn: a point past its row is another's */
            if (oz >= (float)o->z && oz < (float)(o->z + 1) && CellOccludes(o, x, y - o->lift, oz))
                return true;
        }
    }
    return false;
}

/* A drawn cell's `clear` (Drawn). */
static inline float ShiftedClear(int tx, int tz)
{
    const LightCell *cell = Cell(tx, tz);

    return cell->clearKnown ? cell->clear : Drawn(tx, tz);
}

static bool Occludes(float x, float y, float z)
{
    const LightCell *cell;

    z = Standing(x, z);
    return ObjectAt(Tile(x), x, y, z, &cell);
}

/*
 * Where step `step` of a ray puts it. One expression, used by the march and by
 * the cell skip alike, so both see exactly the same points.
 */
static void RayPoint(float x, float y, float z, int step, float *rx, float *ry, float *rz)
{
    /* Half a step in: the first point stands an eighth over the ground, not
     * a third - under a low wall's lip, at whose foot the first point used
     * to be over its top already. */
    float rise = ((float)step - 0.5f) * 0.25f;

    *rx = x - VOXEL_SUN_DX * rise;
    *ry = y + rise + RAY_BIAS;
    *rz = z - VOXEL_SUN_DZ * rise;
}

#ifdef VOXEL_LIGHTING_TESTS
bool gVoxelLightingStepEveryPoint; /* the reference march, for the tests */
#endif

/*
 * Does the sun reach (x, y, z)? A world point: y counts the map's base.
 *
 * Bounded ray through the height proxies, a quarter tile of rise per step.
 * This runs during meshing, never per fragment. Bias avoids self-shadowing
 * of a flat roof.
 *
 * The ray only climbs, so a box cell that does not stop it at the first
 * point it has inside cannot stop it at any later one: the march jumps to
 * the first point in the next cell. That is five to seven cells for a ray
 * that used to take twenty-six points, with the same answer - only crowns,
 * which are round, are still tested point by point.
 */
static bool Lit(float x, float y, float z)
{
    float ceiling;
    /* The blocks' bound is only valid for the maps it was read from: with
     * maps put on screen since the last reset, the map bound alone. */
    bool blocks;
    bool stretched;

    if (gVoxelSun.strength == 0.0f)
        return true;
    if (sGeneration == 0)
        VoxelLighting_Reset();
    RayBounds(x, z, &ceiling, &stretched);
    blocks = !sAlone && VoxelWorld_InstanceCount() == sMapCeilings;
    for (int step = 1; step <= VOXEL_LIGHT_REACH * 4;)
    {
        float rx, ry, rz;
        int tx, tz;
        const LightCell *cell;

        RayPoint(x, y, z, step, &rx, &ry, &rz);
        if (ry >= ceiling)
            break;
        if (stretched)
            rz = Standing(rx, rz);
        tx = Tile(rx);
        tz = Tile(rz);
        /* The blocks' bound, read once, at the first point the map bound
         * has not stopped: its points from here to the last lie between this
         * block and the last point's (rx and rz only fall), and so do the
         * cells they test. Not for a ray with little left to climb anyway:
         * on a route of trees that was all of them, and the lookups cost
         * more than the points they saved. */
        if (blocks && ceiling - ry > BLOCK_CLIMB)
        {
            float fx, fy, fz, reach;

            blocks = false;
            RayPoint(x, y, z, VOXEL_LIGHT_REACH * 4, &fx, &fy, &fz);
            reach = ReachCeiling(BlockOf(Tile(fx)), BlockOf(Tile(fz)), BlockOf(tx), BlockOf(tz));
            if (reach < ceiling)
                ceiling = reach;
            if (ry >= ceiling)
                break;
        }
        ++sSteps;
        if (ObjectAt(tx, rx, ry, rz, &cell))
            return false;
        ++step;
#ifdef VOXEL_LIGHTING_TESTS
        if (gVoxelLightingStepEveryPoint)
            continue;
#endif
        /* (the cells' edges are where they stand, not along the ray) */
        if (stretched)
            continue;
        /* (ObjectAt's lookup across a shifted cell may have reused its
         * bucket: found again, or the skip reads the other cell) */
        if (cell->x != tx || cell->z != tz)
            cell = Cell(tx, tz);
        if (cell->drawn)
        {
            /* What is drawn across a shifted cell comes from the rows its
             * drawing lies over: point by point until the ray is over it
             * all, then past the cell at once. */
            if (ShiftedClear(tx, tz) > ry)
                continue;
        }
        else if ((cell->crownPart >= 0 || cell->surface || cell->sign || cell->mask != NULL)
              && cell->top + cell->base > ry)
            continue;  /* not a box: point by point while it is above the ray */
        /* Leave the cell along the current sun direction. The estimate starts a step short
         * of the crossing and the exact points settle it. */
        {
            /* Reciprocals, not divisions: the guess is a step short of the
             * crossing anyway, far more than their rounding. */
            float exitX = (float)tx + (VOXEL_SUN_DX < 0.0f ? 1.0f : 0.0f);
            float exitZ = (float)tz + (VOXEL_SUN_DZ < 0.0f ? 1.0f : 0.0f);
            float ex = sStepInverseX != 0.0f ? (x - exitX) * sStepInverseX : 33.0f;
            float ez = sStepInverseZ != 0.0f ? (z - exitZ) * sStepInverseZ : 33.0f;
            int guess = (int)fminf(33.0f, fminf(ex, ez)) - 1;
            const float left = (float)tx, top = (float)tz;

            if (guess > step)
                step = guess;
            /* The first point was inside the cell, so either boundary detects
             * Tile(r) != t, without computing Tile. */
            for (; step <= VOXEL_LIGHT_REACH * 4; ++step)
            {
                RayPoint(x, y, z, step, &rx, &ry, &rz);
                if (rx < left || rx >= left + 1.0f
                 || rz < top || rz >= top + 1.0f || ry >= ceiling)
                    break;
            }
        }
    }
    return true;
}

/*
 * The blocks' bounds the rays from the ground of cells x0..x1, z0..z1 read
 * (Lit's ReachCeiling), worked out ahead, `budget` new blocks a call: the
 * first ray into blocks not yet read classified some four hundred cells, and
 * the first cell of a chunk took 6-8 ms on an Old 3DS where its neighbours
 * took under one - a dropped frame from one slice the build had counted as a
 * millisecond (port.log, 2026-10-26: "slice ground ... 8.37ms remaining=1.43").
 * Read from each corner of the cells as Lit reads them; a ray from higher up a
 * drawn mountain may read blocks further south, which it reads itself. The
 * same bounds as the rays' own: only when they are read changes.
 */
bool VoxelLighting_Warm(int x0, int z0, int x1, int z1, unsigned *cursor, unsigned budget)
{
    enum { REGION = 64 };
    const int across = x1 - x0 + 1, points = across * (z1 - z0 + 1);
    unsigned read = 0;

    if (gVoxelSun.strength == 0.0f)
        return true;
    if (sGeneration == 0)
        VoxelLighting_Reset();
    if (sAlone || VoxelWorld_InstanceCount() != sMapCeilings)
        return true;
    for (unsigned point = *cursor / REGION, first = *cursor % REGION; point < (unsigned)points; ++point, first = 0)
    {
        float x = (float)(x0 + (int)point % across), z = (float)(z0 + (int)point / across);
        const LightCell *ground = Cell(Tile(x), Tile(z));
        /* where its ground is drawn, which is where its rays start */
        float y = ground->base + ground->lift, sx, sy, sz, fx, fy, fz;
        int bx0, bz0, bx1, bz1, w;

        z += ground->shift;
        RayPoint(x, y, z, 1, &sx, &sy, &sz);
        /* a ray with little to climb never reads them */
        if (RayCeiling(x, z) - sy <= BLOCK_CLIMB)
            continue;
        RayPoint(x, y, z, VOXEL_LIGHT_REACH * 4, &fx, &fy, &fz);
        bx0 = BlockOf(Tile(fx));
        bz0 = BlockOf(Tile(fz));
        bx1 = BlockOf(Tile(sx));
        bz1 = BlockOf(Tile(sz));
        if (bx0 > bx1) { int t = bx0; bx0 = bx1; bx1 = t; }
        if (bz0 > bz1) { int t = bz0; bz0 = bz1; bz1 = t; }
        /* as ReachCeiling reads them */
        if (bx1 - bx0 <= REACH_BLOCKS && bz1 - bz0 <= REACH_BLOCKS)
        {
            bx0 = bx1 - REACH_BLOCKS;
            bz0 = bz1 - REACH_BLOCKS;
        }
        w = bx1 - bx0 + 1;
        for (unsigned k = first; k < (unsigned)(w * (bz1 - bz0 + 1)) && k < REGION; ++k)
        {
            int bx = bx0 + (int)k % w, bz = bz0 + (int)k / w;
            const LightBlock *block = &sBlocks[((uint32_t)bx * 73856093u ^ (uint32_t)bz * 19349663u)
                                               & (BLOCK_CACHE_SIZE - 1)];

            if (block->generation == sGeneration && block->bx == bx && block->bz == bz)
                continue;
            if (read == budget)
            {
                *cursor = point * REGION + k;
                return false;
            }
            BlockCeiling(bx, bz);
            ++read;
        }
    }
    *cursor = (unsigned)points * REGION;
    return true;
}

/* Small contact AO at the bases of solid neighbours; leave roof/crown art
 * alone. Ambient light keeps the original pixel art legible in shadow. */
static float Contact(float x, float y, float z)
{
    unsigned covered = 0;

    if (y - Cell(Tile(x), Tile(Standing(x, z)))->base > 0.41f)
        return 1.0f;
    /* Nothing round the point stands as high as its tests: the blocks'
     * bound again, where the rays have read it already (never read for
     * this: a block is four hundred cells' worth), which on open ground
     * spares all four. */
    if (!sAlone && VoxelWorld_InstanceCount() == sMapCeilings)
    {
        float sz = Standing(x, z), ceiling = -1000.0f;
        int bx0 = BlockOf(Tile(x - 0.28f)), bx1 = BlockOf(Tile(x + 0.28f));
        int bz0 = BlockOf(Tile(sz - 0.28f)), bz1 = BlockOf(Tile(sz + 0.28f));
        bool known = true;

        for (int bz = bz0; bz <= bz1 && known; ++bz)
            for (int bx = bx0; bx <= bx1 && known; ++bx)
            {
                const LightBlock *block = &sBlocks[((uint32_t)bx * 73856093u ^ (uint32_t)bz * 19349663u)
                                                   & (BLOCK_CACHE_SIZE - 1)];

                known = block->generation == sGeneration && block->bx == bx && block->bz == bz;
                if (known && block->ceiling > ceiling)
                    ceiling = block->ceiling;
            }
        if (known && ceiling <= y + 0.3f)
            return 1.0f;
    }
    covered += Occludes(x - 0.28f, y + 0.3f, z);
    covered += Occludes(x + 0.28f, y + 0.3f, z);
    covered += Occludes(x, y + 0.3f, z - 0.28f);
    covered += Occludes(x, y + 0.3f, z + 0.28f);
    return 1.0f - 0.035f * (float)covered;
}

/*
 * Bucketed an eighth of a tile across: a relief lattice puts a point every
 * quarter tile, and on the half-tile buckets this used to have two of them
 * shared each bucket, one evicting the other, so a slope cell's shared
 * corners cast their rays again for each quad that met them. The bucket is
 * only where to look; the match is exact.
 */
static LightSample *FindSample(float x, float y, float z)
{
    uint32_t hash = (uint32_t)Tile(x * 8.0f) * 73856093u
                  ^ (uint32_t)Tile(z * 8.0f) * 19349663u
                  ^ (uint32_t)Tile(y * 16.0f) * 83492791u;
    /* Lattice coordinates have common low zero bits. Fold high bits before
     * selecting a set, then keep four exact samples per set at the same
     * total sample capacity. Collisions never approximate the lighting. */
    hash ^= hash >> 16;
    hash *= 0x7feb352du;
    hash ^= hash >> 15;
    unsigned set = hash & (SAMPLE_SETS - 1u);
    LightSample *ways = &sSamples[set * SAMPLE_WAYS], *sample = NULL;

    if (sGeneration == 0)
        VoxelLighting_Reset();
    for (unsigned i = 0; i < SAMPLE_WAYS; ++i)
    {
        if (ways[i].generation == sSampleGeneration)
        {
            if (ways[i].x == x && ways[i].y == y && ways[i].z == z)
                return &ways[i];
        }
        else if (sample == NULL)
            sample = &ways[i];
    }
    if (sample == NULL)
    {
        sample = &ways[sSampleNext[set]];
        sSampleNext[set] = (sSampleNext[set] + 1u) & (SAMPLE_WAYS - 1u);
    }
    sample->contact = Contact(x, y, z);
    sample->sun = 0;
    sample->x = x;
    sample->y = y;
    sample->z = z;
    sample->generation = sSampleGeneration;
    return sample;
}

static const LightSample *CachedSample(float x, float y, float z)
{
    LightSample *sample = FindSample(x, y, z);

    if (sample->sun != sSunGeneration)
    {
        if (gVoxelSun.strength > 0.0f && !sHigh)
            ++sRays;
        /* HIGH: the shadow map shades it, crowns and all */
        sample->lit = sHigh || Lit(x, y, z);
        sample->sun = sSunGeneration;
    }
    return sample;
}

/*
 * The contact AO alone, for a point turned from the sun: cached with the
 * samples, as it depends on the point alone. A relief corner is shared by
 * the four quads around it, and each tested its four neighbours again.
 */
static float CachedContact(float x, float y, float z)
{
    /* Contact's own first test: too cheap to cache, and caching it would
     * only evict samples whose rays were cast. */
    if (y - Cell(Tile(x), Tile(Standing(x, z)))->base > 0.41f)
        return 1.0f;
    return FindSample(x, y, z)->contact;
}

unsigned VoxelLighting_Rays(void)
{
    return sRays;
}

unsigned VoxelLighting_Steps(void)
{
    return sSteps;
}

unsigned VoxelLighting_SurfaceTests(void)
{
    return sSurfaceTests;
}

float VoxelLighting_Sample(float x, float y, float z)
{
    const LightSample *sample = CachedSample(x, y, z);

    return (sample->lit ? 1.0f : sAmbient) * sample->contact;
}

float VoxelLighting_SampleAlone(float x, float y, float z)
{
    float shade;

    sAlone = true;
    shade = VoxelLighting_Sample(x, y, z);
    sAlone = false;
    return shade;
}

uint32_t VoxelLighting_Hash(int x0, int z0, int x1, int z1)
{
    /* Include the moving ray's reach and neighbours for AO/face visibility. */
    int reachX = (int)ceilf(fabsf(VOXEL_SUN_DX) * VOXEL_LIGHT_REACH);
    int reachZ = (int)ceilf(fabsf(VOXEL_SUN_DZ) * VOXEL_LIGHT_REACH);
    int northReach = VOXEL_SUN_DZ > 0.0f ? reachZ : 0;
    int hx0 = x0 - (VOXEL_SUN_DX > 0.0f ? reachX : 0) - 1;
    int hz0 = z0 - (northReach > VOXEL_CHUNK_MARGIN_NORTH
                  ? northReach : VOXEL_CHUNK_MARGIN_NORTH) - 1;
    int hx1 = x1 + (VOXEL_SUN_DX < 0.0f ? reachX : 0) + 1;
    int hz1 = z1 + (VOXEL_SUN_DZ < 0.0f ? reachZ : 0) + 2;
    uint32_t hash = VoxelWorld_BlockHash(hx0, hz0, hx1, hz1);

    /* (HIGH bakes other shades: its chunks are other chunks) */
    hash = (hash ^ gVoxelSun.key ^ (sHigh ? 0x9E3779B9u : 0u)) * 16777619u;

    /* Equal tile IDs in a different tileset/layout do not mean equal casters.
     * Relative origins preserve the hash when the entire world is rebased. */
    for (unsigned i = 0; i < VoxelWorld_InstanceCount(); ++i)
    {
        const VoxelMapInstance *inst = VoxelWorld_Instance(i);
        uint32_t tag;
        if (inst->originX >= hx1 || inst->originY >= hz1
         || inst->originX + inst->width <= hx0 || inst->originY + inst->height <= hz0)
            continue;
        tag = (uint32_t)inst->layoutId * 16777619u;
        tag ^= (uint32_t)(inst->originX - x0) * 73856093u;
        tag ^= (uint32_t)(inst->originY - z0) * 19349663u;
        tag ^= (uint32_t)(uintptr_t)inst->primaryTileset;
        tag ^= (uint32_t)(uintptr_t)inst->secondaryTileset;
        tag ^= inst->indoor ? 0x80000000u : 0;
        hash ^= tag; /* instance ordering changes on a map crossing */
    }
    return hash;
}

static VoxelVertex Mix(VoxelVertex a, VoxelVertex b, float t)
{
    VoxelVertex v;
    v.x = a.x + (b.x - a.x) * t;
    v.y = a.y + (b.y - a.y) * t;
    v.z = a.z + (b.z - a.z) * t;
    v.u = a.u + (b.u - a.u) * t;
    v.v = a.v + (b.v - a.v) * t;
    v.shade = a.shade + (b.shade - a.shade) * t;
    return v;
}

static void RawQuad(VoxelBuilder *builder, const VoxelVertex *a,
                    const VoxelVertex *b, const VoxelVertex *c, const VoxelVertex *d)
{
    /* Whole or not at all: half a quad is a sliver along its diagonal. */
    if (builder->count + 6 > builder->capacity)
    {
        builder->dropped += 2;
        return;
    }
    /* Cut along the diagonal whose ends are lit most alike: a shadow's edge
     * crossing the quad the other way then runs straight along it, where the
     * fixed cut drew it as a zigzag of half triangles. The same corners and
     * the same samples, and the same winding either way. Only on a flat
     * parallelogram, its texture one too, where either cut is the same
     * surface and the same picture: on a relief's bent quad the diagonal is
     * part of the shape (a ledge's berm). */
    if (fabsf(a->shade - c->shade) > fabsf(b->shade - d->shade)
     && fabsf(a->x + c->x - b->x - d->x) < 0.001f
     && fabsf(a->y + c->y - b->y - d->y) < 0.001f
     && fabsf(a->z + c->z - b->z - d->z) < 0.001f
     && fabsf(a->u + c->u - b->u - d->u) < 0.0001f
     && fabsf(a->v + c->v - b->v - d->v) < 0.0001f)
    {
        VoxelBuilder_Tri(builder, a, b, d);
        VoxelBuilder_Tri(builder, b, c, d);
        return;
    }
    VoxelBuilder_Tri(builder, a, b, c);
    VoxelBuilder_Tri(builder, a, c, d);
}

float VoxelLighting_Face(float nx, float ny, float nz)
{
    /* n . sun over up . sun, the sun unnormalised at (-DX, 1, -DZ): level
     * ground is 1, a west wall 0.85, a north one 0.55, south and east 0. */
    float length = sqrtf(nx * nx + ny * ny + nz * nz);
    float facing;

    if (length <= 0.0f)
        return 1.0f;
    facing = (ny - VOXEL_SUN_DX * nx - VOXEL_SUN_DZ * nz) / length;
    if (facing <= 0.0f)
        return sAmbient;
    if (facing >= 1.0f)
        return 1.0f;
    return sAmbient + (1.0f - sAmbient) * facing;
}

/* Where the builder puts a point it is given: the world, base and lift in. */
static void WorldPoint(const VoxelBuilder *builder, const VoxelVertex *v,
                       float *x, float *y, float *z)
{
    *x = v->x;
    *y = v->y + builder->lift + builder->base;
    /* where it would be drawn with its faces upright (VoxelBuilder_SunZ):
     * the rays start there, and the casters stand there */
    *z = VoxelBuilder_SunZ(builder, v->z + builder->shift);
}

/* ── Faces ──────────────────────────────────────────────────────────────── */

static void EmitPolygon(VoxelBuilder *builder, const VoxelVertex *out, unsigned count)
{
    if (count == 4)
    {
        RawQuad(builder, &out[0], &out[1], &out[2], &out[3]);
        return;
    }
    VoxelBuilder_Tri(builder, &out[0], &out[1], &out[2]);
}

/*
 * A lit polygon of three or four points, its normal already turned to the
 * side it is seen from: the sun's face term, and each corner's cast shadow.
 * A face turned from the sun has no sun to lose, so it casts no ray.
 */
static void LightPolygon(VoxelBuilder *builder, const VoxelVertex *const *v, unsigned count,
                         float nx, float ny, float nz)
{
    VoxelVertex out[4];
    float face = VoxelLighting_Face(nx, ny, nz);
    bool sunward = face > sAmbient;
    float px, py, pz;

    for (unsigned i = 0; i < count; ++i)
        out[i] = *v[i];

    for (unsigned i = 0; i < count; ++i)
    {
        const LightSample *sample;

        WorldPoint(builder, v[i], &px, &py, &pz);
        if (!sunward)
        {
            out[i].shade *= sAmbient * CachedContact(px, py, pz);
            continue;
        }
        sample = CachedSample(px, py, pz);
        out[i].shade *= (sample->lit ? face : sAmbient) * sample->contact;
    }
    EmitPolygon(builder, out, count);
}

/*
 * The relief's lattice (builder->vertexFace): each corner brings its own
 * face term in its shade. A corner turned from the sun casts no ray.
 */
static void LightVertexFaces(VoxelBuilder *builder, const VoxelVertex *const *v, unsigned count)
{
    VoxelVertex out[4];

    for (unsigned i = 0; i < count; ++i)
    {
        float face = v[i]->shade, px, py, pz;

        out[i] = *v[i];
        WorldPoint(builder, v[i], &px, &py, &pz);
        if (face <= sAmbient + 0.001f)
        {
            out[i].shade = sAmbient * CachedContact(px, py, pz);
            continue;
        }
        {
            const LightSample *sample = CachedSample(px, py, pz);

            out[i].shade = (sample->lit ? face : sAmbient) * sample->contact;
        }
    }
    EmitPolygon(builder, out, count);
}

/*
 * What a lattice point (a, c) off the half-tile points gets from them: the
 * answer the two along its row or column (or the four round it, diagonally)
 * agree on, if it lies on their line (their plane) - else -1. A bend there
 * (a step's foot, a crease) can hide a shadow of its own.
 */
static int LatticeInfer(const float *wy, const float *wz, const int8_t *lit, int a, int c)
{
    enum { SIDE = VOXEL_RELIEF_SIDE };
    int k = c * SIDE + a, from[4], count = 0;
    float my = 0.0f, mz = 0.0f;

    if ((a & 1) && (c & 1))
    {
        from[count++] = k - SIDE - 1;
        from[count++] = k - SIDE + 1;
        from[count++] = k + SIDE - 1;
        from[count++] = k + SIDE + 1;
    }
    else if (a & 1)
    {
        from[count++] = k - 1;
        from[count++] = k + 1;
    }
    else
    {
        from[count++] = k - SIDE;
        from[count++] = k + SIDE;
    }
    for (int i = 0; i < count; ++i)
    {
        if (lit[from[i]] < 0 || lit[from[i]] != lit[from[0]])
            return -1;
        my += wy[from[i]];
        mz += wz[from[i]];
    }
    if (fabsf(my / count - wy[k]) > 0.02f || fabsf(mz / count - wz[k]) > 0.02f)
        return -1;
    return lit[from[0]];
}

/*
 * A relief cell's lattice (VOXEL_RELIEF_SIDE points a side, a quarter tile
 * apart), its shadows read ahead of its quads: rays at the half-tile points,
 * as level ground is sampled, and each point between them takes their answer
 * where LatticeInfer gives one - else the ray its quads cast when they are
 * lit. A drawn mountain's cell cast sixteen rays of its own: on an Old 3DS
 * its chunks took twice a town's (Route 112, 33 ms, port.log 2026-10-26).
 * The same shadows: one level coarser (rays at the corners alone) lost a
 * shadow's tip on one vertex in a thousand.
 */
void VoxelLighting_Lattice(VoxelBuilder *builder, const VoxelVertex *lattice)
{
    enum { SIDE = VOXEL_RELIEF_SIDE, LAST = VOXEL_RELIEF_SIDE - 1 };
    float wx[SIDE * SIDE], wy[SIDE * SIDE], wz[SIDE * SIDE];
    int8_t lit[SIDE * SIDE]; /* -1: unknown, or turned from the sun (no ray) */

    if (gVoxelSun.strength == 0.0f)
        return;
    for (int k = 0; k < SIDE * SIDE; ++k)
    {
        WorldPoint(builder, &lattice[k], &wx[k], &wy[k], &wz[k]);
        lit[k] = -1;
    }
    for (int c = 0; c <= LAST; c += 2)
        for (int a = 0; a <= LAST; a += 2)
        {
            int k = c * SIDE + a;

            if (lattice[k].shade > sAmbient + 0.001f)
                lit[k] = CachedSample(wx[k], wy[k], wz[k])->lit;
        }
    for (int c = 0; c <= LAST; ++c)
        for (int a = 0; a <= LAST; ++a)
        {
            int k = c * SIDE + a, v;
            LightSample *sample;

            if (((a | c) & 1) == 0 || lattice[k].shade <= sAmbient + 0.001f
             || (v = LatticeInfer(wy, wz, lit, a, c)) < 0)
                continue;
            sample = FindSample(wx[k], wy[k], wz[k]);
            if (sample->sun != sSunGeneration)
            {
                sample->lit = v != 0;
                sample->sun = sSunGeneration;
            }
        }
}

/* The light each lattice point gets, as LightVertexFaces gives it to the
 * corners of the lattice's quads: the same samples, from the same caches. */
void VoxelLighting_LatticeShades(VoxelBuilder *builder, const VoxelVertex *lattice, float *shade)
{
    for (int k = 0; k < VOXEL_RELIEF_SIDE * VOXEL_RELIEF_SIDE; ++k)
    {
        float face = lattice[k].shade, px, py, pz;

        WorldPoint(builder, &lattice[k], &px, &py, &pz);
        if (face <= sAmbient + 0.001f)
            shade[k] = sAmbient * CachedContact(px, py, pz);
        else
        {
            const LightSample *sample = CachedSample(px, py, pz);

            shade[k] = (sample->lit ? face : sAmbient) * sample->contact;
        }
    }
}

/* A quad, or with `count` 3 the triangle a b c (d is c): lit alike, the
 * triangle only spared the quad's empty second half. */
static void LightFace(VoxelBuilder *builder, const VoxelVertex *a,
                      const VoxelVertex *b, const VoxelVertex *c,
                      const VoxelVertex *d, unsigned count)
{
    VoxelVertex corners[4] = {*a, *b, *c, *d};
    const VoxelVertex *points[4] = {a, b, c, d};
    /* A B C winds a wall outwards and level ground downwards: ground is
     * turned up, and so is anything else found facing down. */
    float ux = b->x - a->x, uy = b->y - a->y, uz = b->z - a->z;
    float vx = c->x - a->x, vy = c->y - a->y, vz = c->z - a->z;
    float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
    float face;

    if (builder->vertexFace && builder->lightingConstant < 0.0f && !builder->rounded)
    {
        LightVertexFaces(builder, points, count);
        return;
    }
    if (ny < 0.0f)
    {
        nx = -nx;
        ny = -ny;
        nz = -nz;
    }
    /* A crown's card stands for a rounded crown: its light is half way
     * between the card's and the sky's. */
    if (builder->rounded)
    {
        float length = sqrtf(nx * nx + ny * ny + nz * nz);

        if (length > 0.0f)
        {
            nx /= length;
            ny = ny / length + 1.0f;
            nz /= length;
        }
    }
    face = VoxelLighting_Face(nx, ny, nz);

    /* An object lit as a whole (see VoxelSign_Emit): its one sample, and the
     * face term, for every corner - unless the sample is in shadow, where
     * there is no sun left to face. */
    if (builder->lightingConstant >= 0.0f)
    {
        float light = builder->lightingConstant > sAmbient + 0.01f
                    ? builder->lightingConstant * face : builder->lightingConstant;

        for (unsigned i = 0; i < 4; ++i)
            corners[i].shade *= light;
        EmitPolygon(builder, corners, count);
        return;
    }

    /* A crown's card: its corners, lit as the rounded crown. */
    if (builder->rounded)
    {
        for (unsigned i = 0; i < 4; ++i)
        {
            float px, py, pz;
            const LightSample *sample;

            WorldPoint(builder, points[i], &px, &py, &pz);
            sample = CachedSample(px, py, pz);
            corners[i].shade *= (sample->lit ? face : sAmbient) * sample->contact;
        }
        EmitPolygon(builder, corners, count);
        return;
    }

    /* Only ground-sized horizontal faces need extra samples. Four sub-quads
     * resolve diagonal shadow edges without multiplying all wall geometry.
     * A triangle spanning the cell goes this way as the quad it was. */
    if (builder->lightingRefine
     && a->y == b->y && a->y == c->y && a->y == d->y && (a->y <= 0.41f || builder->artShaded)
     && fabsf(a->x - c->x) >= 0.99f && fabsf(a->z - c->z) >= 0.99f
     && fabsf(a->x - c->x) <= 1.01f && fabsf(a->z - c->z) <= 1.01f)
    {
        VoxelVertex grid[9];
        float lo = 1.0f, hi = 0.0f;
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 3; ++col)
            {
                float t = col * 0.5f, s = row * 0.5f;
                VoxelVertex v = Mix(Mix(*a, *b, t), Mix(*d, *c, t), s);
                float px, py, pz, light;

                WorldPoint(builder, &v, &px, &py, &pz);
                light = VoxelLighting_Sample(px, py, pz);
                if (light < lo) lo = light;
                if (light > hi) hi = light;
                v.shade *= light;
                grid[row * 3 + col] = v;
            }
        if (hi - lo > 0.02f && builder->count + 24 <= builder->capacity)
        {
            for (int row = 0; row < 2; ++row)
                for (int col = 0; col < 2; ++col)
                {
                    int i = row * 3 + col;
                    RawQuad(builder, &grid[i], &grid[i + 1], &grid[i + 4], &grid[i + 3]);
                }
        }
        else
            RawQuad(builder, &grid[0], &grid[2], &grid[8], &grid[6]);
        return;
    }
    LightPolygon(builder, points, count, nx, ny, nz);
}

void VoxelLighting_Quad(VoxelBuilder *builder, const VoxelVertex *a,
                         const VoxelVertex *b, const VoxelVertex *c,
                         const VoxelVertex *d)
{
    LightFace(builder, a, b, c, d, 4);
}

void VoxelLighting_Tri(VoxelBuilder *builder, const VoxelVertex *a,
                       const VoxelVertex *b, const VoxelVertex *c)
{
    LightFace(builder, a, b, c, c, 3);
}

/*
 * The model's shades (voxel_building.py): 1 for the faces the GBA drew,
 * then the west, east and back sides. They say which side of the triangle
 * is its outside - the models are not wound consistently - and the sun then
 * says how lit that side is.
 */
#define MODEL_SHADE_WEST 0.80f
#define MODEL_SHADE_EAST 0.72f
#define MODEL_SHADE_BACK 0.66f
/* A face wound with its outside first (a rock's back, turned every way):
 * its normal is its winding's, no side's rule (voxel_building.py SHADE_WOUND). */
#define MODEL_SHADE_WOUND 0.90f

void VoxelLighting_ModelTri(VoxelBuilder *builder, const VoxelVertex *a,
                            const VoxelVertex *b, const VoxelVertex *c, float drawnShade)
{
    VoxelVertex plain[3] = {*a, *b, *c};
    float face = VoxelLighting_ModelShade(a, b, c, drawnShade);

    for (unsigned i = 0; i < 3; ++i)
        plain[i].shade = face;
    VoxelBuilder_Tri(builder, &plain[0], &plain[1], &plain[2]);
}

float VoxelLighting_ModelShade(const VoxelVertex *a, const VoxelVertex *b,
                               const VoxelVertex *c, float drawnShade)
{
    float ux = b->x - a->x, uy = b->y - a->y, uz = b->z - a->z;
    float vx = c->x - a->x, vy = c->y - a->y, vz = c->z - a->z;
    float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
    bool flip;

    if (fabsf(drawnShade - MODEL_SHADE_WOUND) < 0.02f)
        flip = false;
    else if (fabsf(drawnShade - MODEL_SHADE_WEST) < 0.02f)
        flip = nx > 0.0f;
    else if (fabsf(drawnShade - MODEL_SHADE_EAST) < 0.02f)
        flip = nx < 0.0f;
    else if (fabsf(drawnShade - MODEL_SHADE_BACK) < 0.02f)
        flip = ny + nz > 0.0f;
    else
        flip = ny + nz < 0.0f; /* drawn: it faces the GBA's view */
    if (flip)
    {
        nx = -nx;
        ny = -ny;
        nz = -nz;
    }
    /*
     * The drawn shade gives way to the sun's facing, and that is all: no
     * cast shadow on a model, as there never was. A town's models are
     * thousands of triangles, and a ray from each put a Rustboro chunk at
     * 40-80 ms on an Old 3DS. It depends on the triangle alone, so a
     * static model's is worked out once (ctr_voxel.c StaticMesh).
     */
    return VoxelLighting_Face(nx, ny, nz);
}
#endif
