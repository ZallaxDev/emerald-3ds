/*
 * The 3D battle's stage and the battlers' shadows. See voxel_battle.h.
 */

/* Before global.h, which redefines abs() as a macro over stdlib's prototype. */
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "global.h"
#include "main.h"
#include "battle.h"
#include "battle_anim.h"
#include "data.h"
#include "event_object_movement.h"
#include "field_player_avatar.h"
#include "metatile_behavior.h"
#include "sprite.h"

#include "3ds_platform.h"
#include "3ds_video.h"
#include "ctr_battle_layout.h"
#include "voxel_battle.h"
#include "voxel_building.h"
#include "voxel_mesh_builder.h"
#include "voxel_relief.h"
#include "voxel_tree.h"
#include "voxel_world.h"

#define DEG_TO_RAD (3.14159265358979323846f / 180.0f)

bool VoxelBattle_GameInBattle(void)
{
    return gMain.inBattle;
}

bool VoxelBattle_IsLink(void)
{
    return (gBattleTypeFlags & BATTLE_TYPE_LINK) != 0;
}

/* ── The battle camera, as the stage search sees it ─────────────────────── */

/*
 * The camera looks north from the south, the target at the origin and the
 * ground there at height 0: the eye at (0, D sin p, D cos p). Screen points are
 * the top screen's 400x240; the battle's GBA picture lies on it at
 * CTR_BATTLE_ZOOM, its point (120, 112) on (200, 192) (3ds_video.c).
 */
typedef struct
{
    float eyeY, eyeZ, sinP, cosP, tanX, tanY;
} BattleView;

static void ViewInit(BattleView *v)
{
    float p = VOXEL_BATTLE_PITCH * DEG_TO_RAD;

    v->sinP = sinf(p);
    v->cosP = cosf(p);
    v->eyeY = VOXEL_BATTLE_DISTANCE * v->sinP;
    v->eyeZ = VOXEL_BATTLE_DISTANCE * v->cosP;
    v->tanY = tanf(VOXEL_BATTLE_FOV * 0.5f * DEG_TO_RAD);
    v->tanX = v->tanY * (float)CTR_GAME_WIDTH / (float)CTR_GAME_HEIGHT;
}

static float GbaToScreenX(float x) { return CTR_GAME_WIDTH / 2 + (x - 120.0f) * CTR_BATTLE_ZOOM; }
static float GbaToScreenY(float y) { return CTR_GAME_HEIGHT - 48 + (y - 112.0f) * CTR_BATTLE_ZOOM; }

/* The ground under a screen point, from the target. */
static void ScreenToGround(const BattleView *v, float sx, float sy, float *x, float *z)
{
    float nx = (sx - CTR_GAME_WIDTH / 2) / (CTR_GAME_WIDTH / 2);
    float ny = (CTR_GAME_HEIGHT / 2 - sy) / (CTR_GAME_HEIGHT / 2);
    /* forward (0, -sin, -cos), right (1, 0, 0), up (0, cos, -sin) */
    float dx = nx * v->tanX;
    float dy = -v->sinP + ny * v->tanY * v->cosP;
    float dz = -v->cosP - ny * v->tanY * v->sinP;
    float t = dy < -0.01f ? v->eyeY / -dy : 100.0f;

    *x = dx * t;
    *z = v->eyeZ + dz * t;
}

/* A point (x, h, z) from the target on the screen; false behind the eye. */
static bool Project(const BattleView *v, float x, float h, float z, float *sx, float *sy)
{
    float ry = h - v->eyeY, rz = z - v->eyeZ;
    float depth = -ry * v->sinP - rz * v->cosP;
    float up = ry * v->cosP - rz * v->sinP;

    if (depth < 0.1f)
        return false;
    *sx = CTR_GAME_WIDTH / 2 + x / (depth * v->tanX) * (CTR_GAME_WIDTH / 2);
    *sy = CTR_GAME_HEIGHT / 2 - up / (depth * v->tanY) * (CTR_GAME_HEIGHT / 2);
    return true;
}

/* ── What the ground is, around the player ──────────────────────────────── */

enum
{
    GROUND_PLAIN,
    GROUND_GRASS,
    GROUND_SAND,
    GROUND_WATER,
    GROUND_VOID, /* nothing is drawn there: black indoors, past the map */
};

typedef struct
{
    uint8_t kind;
    bool blocked, slope;
    /* Height of what stands on the cell, tiles (0: nothing), and of its
     * ground in the world. */
    float top, lift;
} StageCell;

/*
 * The cells the search reads, read once: around the player's spot, as far as
 * the target may move from it plus what a camera there shows and stands in
 * front of. A cell outside it reads as plain ground.
 */
#define STAGE_REACH 6
#define GRID_X0 (-STAGE_REACH - 11)
#define GRID_X1 (STAGE_REACH + 11)
#define GRID_Z0 (-STAGE_REACH - 10)
#define GRID_Z1 (STAGE_REACH + 8)
#define GRID_W (GRID_X1 - GRID_X0 + 1)
#define GRID_H (GRID_Z1 - GRID_Z0 + 1)

static StageCell sGrid[GRID_H][GRID_W];
static int sGridX, sGridZ;

static uint8_t KindOf(unsigned behavior)
{
    if (MetatileBehavior_IsTallGrass(behavior) || MetatileBehavior_IsLongGrass(behavior))
        return GROUND_GRASS;
    if (MetatileBehavior_IsSandOrDeepSand(behavior))
        return GROUND_SAND;
    if (MetatileBehavior_IsSurfableWaterOrUnderwater(behavior)
     || MetatileBehavior_IsDeepOrOceanWater(behavior))
        return GROUND_WATER;
    return GROUND_PLAIN;
}

static void ReadCell(StageCell *cell, int x, int z, bool indoor)
{
    const VoxelMapInstance *inst = VoxelWorld_GetInstanceAt(x, z);
    VoxelVisualShape shape;
    int metatile;
    float top = 0.0f;
    const int16_t *relief;

    memset(cell, 0, sizeof(*cell));
    cell->lift = VoxelRelief_LiftAt(x + 0.5f, z + 0.5f);
    if (inst == NULL)
    {
        /* The sea belt is flat, at the same displaced plane the field
         * draws; it must not score as a two-tile obstruction. */
        if (!indoor && VoxelMesh_BorderWaterAt(x + 0.5f, z + 0.5f, &cell->lift))
        {
            cell->kind = GROUND_WATER;
            cell->blocked = true; /* border cells remain outside the walkable map */
            return;
        }
        /* Indoors the room ends in black; other outdoor borders are
         * mostly trees. */
        cell->kind = indoor ? GROUND_VOID : GROUND_PLAIN;
        cell->top = indoor ? 0.0f : 2.0f;
        cell->blocked = true;
        return;
    }
    shape = VoxelWorld_ClassifyTile(x, z);
    if (shape == VOXEL_SHAPE_VOID)
    {
        cell->kind = GROUND_VOID;
        cell->blocked = true;
        return;
    }
    cell->kind = KindOf(VoxelWorld_GetMetatileBehavior(x, z));
    cell->blocked = VoxelWorld_GetCollision(x, z) != 0;
    metatile = VoxelWorld_GetMetatileId(x, z);
    if (VoxelBuildings_CellAt(inst, x, z, NULL, &top) && top > cell->top)
        cell->top = top;
    if (VoxelWorld_UsesTreeSprites(inst))
    {
        int part = VoxelTree_Part(inst->secondaryTileset, metatile);

        /* A tree's trunk, or the cell its crown leans over. */
        if (part >= 0)
            top = part == VOXEL_TREE_SMALL ? 1.8f : 2.4f;
        else if (VoxelTree_GroundMetatile(inst->secondaryTileset, metatile) != metatile)
            top = 2.4f;
        if (top > cell->top)
            cell->top = top;
    }
    switch (shape)
    {
    case VOXEL_SHAPE_SIGN:
        top = 1.0f;
        break;
    case VOXEL_SHAPE_FURNITURE:
    case VOXEL_SHAPE_TABLE:
    case VOXEL_SHAPE_COUNTER:
    case VOXEL_SHAPE_BED:
        top = 0.8f;
        break;
    default:
        top = 0.0f;
        break;
    }
    if (top > cell->top)
        cell->top = top;
    relief = VoxelRelief_Cell(inst, x, z);
    cell->slope = relief != NULL && VoxelRelief_IsSlope(relief);
}

/* Retain continuous relief sampling inside maps. Outside them the camera
 * target must use the rendered water plane too, rather than NULL relief. */
static float StageGround(float x, float z)
{
    float height;

    return VoxelMesh_BorderWaterAt(x, z, &height) ? height : VoxelRelief_LiftAt(x, z);
}

static const StageCell *CellAt(float x, float z)
{
    static const StageCell sPlain = {GROUND_PLAIN, false, false, 0.0f, 0.0f};
    int gx = (int)floorf(x) - sGridX - GRID_X0, gz = (int)floorf(z) - sGridZ - GRID_Z0;

    if (gx < 0 || gx >= GRID_W || gz < 0 || gz >= GRID_H)
        return &sPlain;
    return &sGrid[gz][gx];
}

/* ── The search ─────────────────────────────────────────────────────────── */

/*
 * Where the battlers stand on the GBA picture (sBattlerCoords, the bottom of
 * their 64x64 pictures: battle_anim_mons.c), and the part of it each side's
 * pictures cover, singles and doubles together - each side moved as the
 * game moves it on the 3DS (ctr_battle_layout.h).
 */
#define FEET_PLAYER_X 72.0f
#define FEET_PLAYER_Y (110.0f + CTR_BATTLE_PLAYER_DROP)
#define FEET_ENEMY_X 176.0f
#define FEET_ENEMY_Y (70.0f - CTR_BATTLE_ENEMY_RISE)
static const float sSideRect[2][4] = {
    {16.0f, 40.0f + CTR_BATTLE_PLAYER_DROP, 128.0f, 112.0f + CTR_BATTLE_PLAYER_DROP}, /* the player's, x0 y0 x1 y1 */
    {128.0f, 8.0f - CTR_BATTLE_ENEMY_RISE, 224.0f, 76.0f - CTR_BATTLE_ENEMY_RISE},  /* the opponent's */
};

typedef struct
{
    BattleView view;
    /* Each side's feet on the ground, from the target, and its pictures'
     * rectangle on the screen. */
    float feetX[2], feetZ[2];
    float rect[2][4];
    uint8_t want[2];
} StageSearch;

static float Overlap(const float a[4], float x0, float y0, float x1, float y1)
{
    float w = fminf(a[2], x1) - fmaxf(a[0], x0), h = fminf(a[3], y1) - fmaxf(a[1], y0);

    return w > 0.0f && h > 0.0f ? w * h / ((a[2] - a[0]) * (a[3] - a[1])) : 0.0f;
}

static float FeetScore(const StageSearch *s, int side, float tx, float tz, float groundT)
{
    float fx = tx + s->feetX[side], fz = tz + s->feetZ[side];
    const StageCell *cell = CellAt(fx, fz);
    uint8_t want = s->want[side];
    float score = 0.0f;

    if (cell->kind == GROUND_VOID)
        return -60.0f;
    if (cell->top > 0.4f)
        score -= 16.0f;
    if (want == GROUND_WATER)
        score += cell->kind == GROUND_WATER ? 8.0f : -8.0f;
    else
    {
        if (cell->kind == GROUND_WATER)
            score -= 12.0f;
        else if (cell->blocked)
            score -= 6.0f;
        if (cell->kind == want)
            score += 4.0f;
    }
    if (cell->slope)
        score -= 4.0f;
    score -= 3.0f * fabsf(cell->lift - groundT);
    /* The ground around the feet: the same kind, open. */
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx)
        {
            const StageCell *near = CellAt(fx + dx, fz + dz);

            if (dx == 0 && dz == 0)
                continue;
            if (near->kind == GROUND_VOID)
                score -= 3.0f;
            else if (near->top > 0.4f)
                score -= 1.0f;
            if (near->kind == want)
                score += 0.5f;
            else if ((near->kind == GROUND_WATER) != (want == GROUND_WATER))
                score -= 0.8f;
            if (near->slope)
                score -= 0.4f;
        }
    return score;
}

static float StageScore(const StageSearch *s, float tx, float tz, float distance)
{
    const BattleView *v = &s->view;
    float groundT = CellAt(tx, tz)->lift;
    float score = -0.5f * distance;

    for (int side = 0; side < 2; ++side)
        score += FeetScore(s, side, tx, tz, groundT);

    /*
     * Anything standing tall where a side's pictures are, at or in front of
     * its feet: the Pokemon would be drawn over a tree or a wall it should
     * stand before. What rises behind them is scenery.
     */
    for (int dz = -3; dz <= 7; ++dz)
        for (int dx = -6; dx <= 6; ++dx)
        {
            float cx = floorf(tx) + dx, cz = floorf(tz) + dz;
            const StageCell *cell = CellAt(cx, cz);
            float x0, y0, x1, y1, h;

            if (cell->top <= 0.4f)
                continue;
            h = cell->lift - groundT;
            if (!Project(v, cx - tx, h + cell->top, cz + 0.5f - tz, &x0, &y0)
             || !Project(v, cx + 1.0f - tx, h, cz + 1.0f - tz, &x1, &y1))
                continue;
            for (int side = 0; side < 2; ++side)
                if (cz + 1.0f - tz > s->feetZ[side] - 0.5f)
                    score -= 24.0f * Overlap(s->rect[side], x0, y0, x1, y1);
        }

    /* Nothing drawn on screen: black where the room or the world ends. */
    for (int dz = -10; dz <= 4; ++dz)
        for (int dx = -10; dx <= 10; ++dx)
        {
            float cx = floorf(tx) + dx + 0.5f, cz = floorf(tz) + dz + 0.5f, sx, sy;

            if (CellAt(cx, cz)->kind != GROUND_VOID)
                continue;
            if (!Project(v, cx - tx, 0.0f, cz - tz, &sx, &sy) || sx < 0.0f || sx > CTR_GAME_WIDTH
             || sy < 0.0f || sy > CTR_GAME_HEIGHT - 48)
                continue;
            score -= sy > 90.0f ? 1.2f : 0.5f;
        }
    return score;
}

/* The kind of ground a cell of the world is, as the battle sees it. */
static uint8_t KindAt(int x, int z)
{
    return KindOf(VoxelWorld_GetMetatileBehavior(x, z));
}

/*
 * The search, a slice per frame: the cells first, then the candidates. A
 * battle's first frames are its loading, behind a closed curtain, and the
 * camera stays where the field left it until the stage is known.
 */
static struct
{
    StageSearch search;
    float baseX, baseZ, bestScore, bestX, bestZ;
    unsigned cells, candidates;
    bool indoor, done;
} sStage;

#define STAGE_SIDE (2 * STAGE_REACH + 1)

void VoxelBattle_BeginStage(void)
{
    const struct ObjectEvent *player = &gObjectEvents[gPlayerAvatar.objectEventId];
    const VoxelMapInstance *current = VoxelWorld_Instance(0);
    bool surfing = TestPlayerAvatarFlags(PLAYER_AVATAR_FLAG_SURFING) != 0;
    bool wild = !(gBattleTypeFlags & (BATTLE_TYPE_TRAINER | BATTLE_TYPE_LINK | BATTLE_TYPE_RECORDED_LINK
                                      | BATTLE_TYPE_FRONTIER | BATTLE_TYPE_EREADER_TRAINER));
    StageSearch *search = &sStage.search;
    int frontX = 0, frontZ = 0;
    float px, pz;
    uint8_t here, front;

    memset(&sStage, 0, sizeof(sStage));
    sStage.indoor = current != NULL && current->indoor;
    ViewInit(&search->view);
    for (int side = 0; side < 2; ++side)
    {
        float gx = side == 0 ? FEET_PLAYER_X : FEET_ENEMY_X, gy = side == 0 ? FEET_PLAYER_Y : FEET_ENEMY_Y;

        ScreenToGround(&search->view, GbaToScreenX(gx), GbaToScreenY(gy),
                       &search->feetX[side], &search->feetZ[side]);
        search->rect[side][0] = GbaToScreenX(sSideRect[side][0]);
        search->rect[side][1] = GbaToScreenY(sSideRect[side][1]);
        search->rect[side][2] = GbaToScreenX(sSideRect[side][2]);
        search->rect[side][3] = GbaToScreenY(sSideRect[side][3]);
    }

    VoxelWorld_GetPlayerWorldCoords(&px, &pz);
    switch (player->facingDirection)
    {
    case DIR_NORTH: frontZ = -1; break;
    case DIR_SOUTH: frontZ = 1; break;
    case DIR_WEST: frontX = -1; break;
    case DIR_EAST: frontX = 1; break;
    }
    here = KindAt((int)px, (int)pz);
    front = KindAt((int)px + frontX, (int)pz + frontZ);
    /*
     * What each side stands on. The player's Pokemon, on what the player
     * stands on (water when surfing). The other: a wild one where it came
     * from - the grass the player walked into, or the water the player is
     * fishing in, which is in front of them on land with no grass to come
     * out of - and a trainer's on the ground in front of the player, where
     * the trainer came to stand.
     */
    search->want[0] = surfing ? GROUND_WATER : here;
    if (surfing)
        search->want[1] = GROUND_WATER;
    else if (wild)
        search->want[1] = here != GROUND_GRASS && front == GROUND_WATER ? GROUND_WATER : here;
    else
        search->want[1] = front;
    for (int side = 0; side < 2; ++side)
        if (search->want[side] == GROUND_VOID)
            search->want[side] = GROUND_PLAIN;

    /* The spot with the player's Pokemon on the player's own tile. */
    sStage.baseX = px + 0.5f - search->feetX[0];
    sStage.baseZ = pz + 0.5f - search->feetZ[0];
    sStage.bestX = sStage.baseX;
    sStage.bestZ = sStage.baseZ;
    sStage.bestScore = -1.0e9f;
    sGridX = (int)floorf(sStage.baseX);
    sGridZ = (int)floorf(sStage.baseZ);
}

bool VoxelBattle_StepStage(unsigned cells, unsigned candidates, float *targetX, float *targetZ,
                           float *ground)
{
    if (!sStage.done)
    {
        for (; cells != 0 && sStage.cells < GRID_W * GRID_H; --cells, ++sStage.cells)
        {
            unsigned gx = sStage.cells % GRID_W, gz = sStage.cells / GRID_W;

            ReadCell(&sGrid[gz][gx], sGridX + GRID_X0 + (int)gx, sGridZ + GRID_Z0 + (int)gz, sStage.indoor);
        }
        if (sStage.cells < GRID_W * GRID_H)
            return false;
        for (; candidates != 0 && sStage.candidates < STAGE_SIDE * STAGE_SIDE; --candidates, ++sStage.candidates)
        {
            int dx = (int)(sStage.candidates % STAGE_SIDE) - STAGE_REACH;
            int dz = (int)(sStage.candidates / STAGE_SIDE) - STAGE_REACH;
            float tx = sStage.baseX + dx, tz = sStage.baseZ + dz;
            float score = StageScore(&sStage.search, tx, tz, sqrtf((float)(dx * dx + dz * dz)));

            if (score > sStage.bestScore)
            {
                sStage.bestScore = score;
                sStage.bestX = tx;
                sStage.bestZ = tz;
            }
        }
        if (sStage.candidates < STAGE_SIDE * STAGE_SIDE)
            return false;
        sStage.done = true;
        CtrLog_Write(CTR_LOG_VIDEO, "VOXEL battle stage: wants %u/%u, target %.1f,%.1f (%+.0f,%+.0f "
                     "from the player's spot) score %.1f%s",
                     sStage.search.want[0], sStage.search.want[1], sStage.bestX, sStage.bestZ,
                     sStage.bestX - sStage.baseX, sStage.bestZ - sStage.baseZ, sStage.bestScore,
                     sStage.indoor ? " indoor" : "");
    }
    *targetX = sStage.bestX;
    *targetZ = sStage.bestZ;
    *ground = StageGround(sStage.bestX, sStage.bestZ);
    return true;
}

/* ── Shadows ────────────────────────────────────────────────────────────── */

unsigned VoxelBattle_Shadows(VoxelBattleShadow *out, unsigned max)
{
    unsigned count = 0;

    if (!gMain.inBattle || gBattleSpritesDataPtr == NULL || gBattleSpritesDataPtr->healthBoxesData == NULL)
        return 0;
    for (unsigned b = 0; b < gBattlersCount && b < MAX_BATTLERS_COUNT && count < max; ++b)
    {
        unsigned id = gBattlerSpriteIds[b], shadow;
        const struct Sprite *sprite;
        bool enemy;

        if ((gAbsentBattlerFlags & (1u << b)) || id >= MAX_SPRITES)
            continue;
        sprite = &gSprites[id];
        if (!sprite->inUse || sprite->invisible)
            continue;
        enemy = GetBattlerSide(b) != B_SIDE_PLAYER;
        /* The player's trainer, before the Pokemon is sent out: no shadow -
         * the player side's Pokemon are drawn with gAnims_MonPic, the trainer
         * with its back picture's own. */
        if (!enemy && sprite->anims != gAnims_MonPic)
            continue;
        /* A floating Pokemon's shadow is the game's own sprite. */
        shadow = gBattleSpritesDataPtr->healthBoxesData[b].shadowSpriteId;
        if (enemy && shadow < MAX_SPRITES && gSprites[shadow].inUse && !gSprites[shadow].invisible)
            continue;
        out[count].x = sprite->x + sprite->x2;
        out[count].y = GetBattlerSpriteCoord(b, BATTLER_COORD_Y) + 30.0f;
        out[count].rx = enemy ? 24.0f : 30.0f;
        out[count].ry = enemy ? 5.0f : 6.0f;
        ++count;
    }
    return count;
}
