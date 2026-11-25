"""The grass rustling under the follower and the visible wild Pokemon, on the
host, with the game's own code: FldEff_TallGrass, FldEff_LongGrass,
UpdateTallGrassFieldEffect, UpdateLongGrassFieldEffect, GrassWalkerAt and
RetireOlderGrass taken from src/field_effect_helpers.c, and
VisibleWild_GrassOwner, VisibleGrass, VisibleElevation, VisibleSpriteValid
and struct VisibleWild from src/wild_encounter.c, around a small field of
sprites, object events and a metatile grid.

Checked: the follower stepping onto the tile the player has just left gets a
rustle of its own, from the start of its picture, and the player's settled
grass there goes (the two were drawn one over the other: the report); the
player's own steps still rustle; a visible wild Pokemon's step into tall or
long grass rustles with its slot's grass id as owner, the grass stays while
it stands there and goes once it has walked off and the rustle is over, at
once when it has met the player or vanished; where it appears the grass is
already settled; with the field short of sprites it leaves its grass out;
the follower's grass outlives the map crossing that changes its map."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent


def function(source, signature):
    """The definition (not a prototype) that starts with `signature`."""
    start = 0
    while True:
        start = source.index(signature, start)
        rest = source[start + len(signature):]
        if re.match(r'[^;{]*\{', rest):
            break
        start += len(signature)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


def define(source, name):
    return re.search(r'^#define %s\b.*$' % name, source, re.M).group(0) + '\n'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--tree', type=Path, default=REPO)
    args = parser.parse_args()
    helpers = (args.tree / 'src/field_effect_helpers.c').read_text(encoding='utf-8')
    wild = (args.tree / 'src/wild_encounter.c').read_text(encoding='utf-8')
    follower = (args.tree / 'include/follower_mon.h').read_text(encoding='utf-8')

    data = re.search(r'// Sprite data for FLDEFF_TALL_GRASS and FLDEFF_LONG_GRASS\n((?:#define .*\n)+)',
                     helpers).group(1)
    visible = re.search(r'struct VisibleWild\n\{.*?\n\};\n', wild, re.S).group(0)
    source = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef uint8_t bool8;
#define TRUE 1
#define FALSE 0
#define PLATFORM_3DS 1
#define MAX_SPRITES 64
#define OBJECT_EVENTS_COUNT 16
#define OBJ_EVENT_GFX_FOLLOWER 239
#define FLDEFFOBJ_TALL_GRASS 0
#define FLDEFFOBJ_LONG_GRASS 1
enum { FLDEFF_TALL_GRASS = 4, FLDEFF_LONG_GRASS = 5 };
enum { MB_NORMAL, MB_TALL_GRASS, MB_LONG_GRASS };
''' + define(follower, 'LOCALID_FOLLOWER') + define(follower, 'LOCALID_VISIBLE_WILD') \
        + define(follower, 'VISIBLE_WILD_GRASS_IDS') + r'''
struct Sprite
{
    bool8 inUse, animEnded, coordOffsetEnabled, invisible;
    u8 animCmdIndex, animNum, subpriority;
    s16 x, y, data[8];
    struct { u8 priority; } oam;
    void (*callback)(struct Sprite *);
    int seek;                  /* SeekSpriteAnim's frame, -1: none */
};
struct SpriteTemplate { void (*callback)(struct Sprite *); };
struct Coords16 { s16 x, y; };
struct ObjectEvent
{
    bool8 active;
    u8 localId, mapNum, mapGroup, graphicsId, previousElevation;
    struct Coords16 currentCoords, previousCoords;
};
static struct Sprite gSprites[MAX_SPRITES];
static struct ObjectEvent gObjectEvents[OBJECT_EVENTS_COUNT];
static struct { u8 objectEventId; } gPlayerAvatar;
static struct { struct { u8 mapNum, mapGroup; } location; } sSave, *gSaveBlock1Ptr = &sSave;
static struct { bool8 active; s16 x, y; } gCamera;
static u32 gFieldEffectArguments[8];
static u8 sGrid[64][64];
static int sStarted[8], sStopped[8];

void UpdateTallGrassFieldEffect(struct Sprite *sprite);
void UpdateLongGrassFieldEffect(struct Sprite *sprite);
static const struct SpriteTemplate sTall = {UpdateTallGrassFieldEffect}, sLong = {UpdateLongGrassFieldEffect};
static const struct SpriteTemplate *const gFieldEffectObjectTemplatePointers[] = {&sTall, &sLong};

static u8 CreateSpriteAtEnd(const struct SpriteTemplate *template, s16 x, s16 y, u8 subpriority)
{
    for (int i = MAX_SPRITES - 1; i >= 0; i--)
        if (!gSprites[i].inUse)
        {
            memset(&gSprites[i], 0, sizeof(gSprites[i]));
            gSprites[i].inUse = TRUE;
            gSprites[i].callback = template->callback;
            gSprites[i].x = x;
            gSprites[i].y = y;
            gSprites[i].subpriority = subpriority;
            gSprites[i].seek = -1;
            return (u8)i;
        }
    return MAX_SPRITES;
}
static void SeekSpriteAnim(struct Sprite *sprite, u8 frame) { sprite->seek = frame; }
static void SetSpritePosToOffsetMapCoords(s16 *x, s16 *y, s16 dx, s16 dy) { *x = *x * 16 + dx; *y = *y * 16 + dy; }
static u8 MapGridGetMetatileBehaviorAt(int x, int y) { return sGrid[y & 63][x & 63]; }
static u8 MapGridGetElevationAt(int x, int y) { (void)x; (void)y; return 3; }
static bool8 MetatileBehavior_IsTallGrass(u8 b) { return b == MB_TALL_GRASS; }
static bool8 MetatileBehavior_IsLongGrass(u8 b) { return b == MB_LONG_GRASS; }
static u8 ElevationToPriority(u8 elevation) { return elevation == 4 ? 1 : 2; }
static void UpdateObjectEventSpriteInvisibility(struct Sprite *sprite, bool8 invisible) { sprite->invisible = invisible; }
static void UpdateGrassFieldEffectSubpriority(struct Sprite *sprite, u8 elevation, u8 subpriority)
{
    (void)elevation;
    sprite->subpriority = subpriority;
}
static void FieldEffectStop(struct Sprite *sprite, u8 id)
{
    sprite->inUse = FALSE;
    sStopped[id]++;
}
static bool8 FollowerMon_IsFollower(const struct ObjectEvent *o)
{
    return o->active && o->localId == LOCALID_FOLLOWER && o->graphicsId == OBJ_EVENT_GFX_FOLLOWER;
}
static bool8 TryGetObjectEventIdByLocalIdAndMap(u8 localId, u8 mapNum, u8 mapGroup, u8 *id)
{
    for (*id = 0; *id < OBJECT_EVENTS_COUNT; (*id)++)
        if (gObjectEvents[*id].active && gObjectEvents[*id].localId == localId
         && gObjectEvents[*id].mapNum == mapNum && gObjectEvents[*id].mapGroup == mapGroup)
            return FALSE;
    return TRUE;
}
bool8 VisibleWild_GrassOwner(u8 localId, s16 *x, s16 *y, s16 *fromX, s16 *fromY);
u32 FldEff_TallGrass(void);
u32 FldEff_LongGrass(void);
static void FieldEffectStart(u8 id)
{
    sStarted[id]++;
    if (id == FLDEFF_TALL_GRASS)
        FldEff_TallGrass();
    else
        FldEff_LongGrass();
}

/* field_effect_helpers.c */
''' + data + function(helpers, 'static bool8 GrassWalkerAt(') \
        + function(helpers, 'static void RetireOlderGrass(') \
        + function(helpers, 'u32 FldEff_TallGrass(void)') \
        + function(helpers, 'void UpdateTallGrassFieldEffect(struct Sprite *sprite)') \
        + function(helpers, 'u32 FldEff_LongGrass(void)') \
        + function(helpers, 'void UpdateLongGrassFieldEffect(struct Sprite *sprite)') + r'''
/* wild_encounter.c */
''' + define(wild, 'VW_MAX') + define(wild, 'VW_MAGIC') + define(wild, 'VW_GRASS_SPARE') \
        + visible + 'static struct VisibleWild sVisible[VW_MAX];\n' \
        + function(wild, 'static bool8 VisibleSpriteValid(const struct VisibleWild *v)') \
        + function(wild, 'static u8 VisibleElevation(const struct VisibleWild *v)') \
        + function(wild, 'static void VisibleGrass(') \
        + function(wild, 'bool8 VisibleWild_GrassOwner(') + r'''
static int sFailures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
    printf("\n"); sFailures++; } } while (0)

/* The sprite callbacks, as AnimateSprites runs them; the rustle's picture
 * ends after `frames` frames of it (50 for tall grass). */
static void RunSprites(void)
{
    for (int i = 0; i < MAX_SPRITES; i++)
        if (gSprites[i].inUse && gSprites[i].callback != NULL)
            gSprites[i].callback(&gSprites[i]);
}

static int GrassOn(void (*callback)(struct Sprite *), s16 x, s16 y, u8 localId, int *seek)
{
    int count = 0;
    for (int i = 0; i < MAX_SPRITES; i++)
    {
        struct Sprite *s = &gSprites[i];
        if (s->inUse && s->callback == callback && s->data[1] == x && s->data[2] == y
         && (localId == 0 || (u8)(s->data[3] >> 8) == localId))
        {
            count++;
            if (seek)
                *seek = s->seek;
        }
    }
    return count;
}

static int GrassCount(void)
{
    int count = 0;
    for (int i = 0; i < MAX_SPRITES; i++)
        count += gSprites[i].inUse && (gSprites[i].callback == UpdateTallGrassFieldEffect
                                    || gSprites[i].callback == UpdateLongGrassFieldEffect);
    return count;
}

/* GroundEffect_StepOnTallGrass / SpawnOnTallGrass of event_object_movement.c. */
static void ObjectGrass(struct ObjectEvent *o, bool8 spawn)
{
    gFieldEffectArguments[0] = o->currentCoords.x;
    gFieldEffectArguments[1] = o->currentCoords.y;
    gFieldEffectArguments[2] = o->previousElevation;
    gFieldEffectArguments[3] = 2;
    gFieldEffectArguments[4] = o->localId << 8 | o->mapNum;
    gFieldEffectArguments[5] = o->mapGroup;
    gFieldEffectArguments[6] = (u8)gSaveBlock1Ptr->location.mapNum << 8 | (u8)gSaveBlock1Ptr->location.mapGroup;
    gFieldEffectArguments[7] = spawn;
    FieldEffectStart(MetatileBehavior_IsTallGrass(MapGridGetMetatileBehaviorAt(o->currentCoords.x, o->currentCoords.y))
                     ? FLDEFF_TALL_GRASS : FLDEFF_LONG_GRASS);
}

static void Step(struct ObjectEvent *o, s16 x, s16 y)
{
    o->previousCoords = o->currentCoords;
    o->currentCoords.x = x;
    o->currentCoords.y = y;
    if (MapGridGetMetatileBehaviorAt(x, y) != MB_NORMAL)
        ObjectGrass(o, FALSE);
}

static void EndAnims(void)
{
    for (int i = 0; i < MAX_SPRITES; i++)
        gSprites[i].animEnded = gSprites[i].inUse;
}

static void Reset(void)
{
    memset(gSprites, 0, sizeof(gSprites));
    memset(gObjectEvents, 0, sizeof(gObjectEvents));
    memset(sVisible, 0, sizeof(sVisible));
    memset(sGrid, MB_TALL_GRASS, sizeof(sGrid));
    sSave.location.mapNum = 16;
    sSave.location.mapGroup = 0;
    gCamera.active = FALSE;
    gPlayerAvatar.objectEventId = 0;
    gObjectEvents[0] = (struct ObjectEvent){TRUE, 255, 16, 0, 0, 3, {10, 10}, {10, 10}};
    gObjectEvents[1] = (struct ObjectEvent){TRUE, LOCALID_FOLLOWER, 16, 0, OBJ_EVENT_GFX_FOLLOWER, 3, {10, 11}, {10, 11}};
}

static void TestFollowerBehindPlayer(void)
{
    struct ObjectEvent *player = &gObjectEvents[0], *follower = &gObjectEvents[1];
    int seek = 0;

    Reset();
    ObjectGrass(player, TRUE);                    /* standing in it */
    Step(player, 10, 9);                          /* north: the player's rustle */
    CHECK(GrassOn(UpdateTallGrassFieldEffect, 10, 9, 255, &seek) == 1 && seek == -1,
          "the player's step does not rustle from the start");
    RunSprites();
    CHECK(GrassOn(UpdateTallGrassFieldEffect, 10, 10, 255, NULL) == 1,
          "the player's grass on the tile it is leaving went at once");
    for (int i = 0; i < 4; i++)
    {
        /* The player walks on through the grass, the follower a tile behind. */
        s16 x = 10, y = player->currentCoords.y;

        Step(player, x, y - 1);
        RunSprites();
        Step(follower, x, y);
        CHECK(GrassOn(UpdateTallGrassFieldEffect, x, y, 0, NULL) == 1,
              "step %d: %d grass sprites on the follower's tile, one wanted", i,
              GrassOn(UpdateTallGrassFieldEffect, x, y, 0, NULL));
        CHECK(GrassOn(UpdateTallGrassFieldEffect, x, y, LOCALID_FOLLOWER, &seek) == 1 && seek == -1,
              "step %d: the follower's own rustle is not there, from its start", i);
        RunSprites();
        CHECK(GrassOn(UpdateTallGrassFieldEffect, x, y, LOCALID_FOLLOWER, NULL) == 1,
              "step %d: the follower's rustle went while it stands there", i);
        EndAnims();
        RunSprites();
    }
    CHECK(GrassCount() <= 4, "%d grass sprites left behind the walk", GrassCount());
}

static void TestFollowerCrossesMap(void)
{
    struct ObjectEvent *follower = &gObjectEvents[1];

    Reset();
    Step(follower, 10, 10);
    /* The camera crosses: the follower takes the player's new map. */
    sSave.location.mapNum = 17;
    follower->mapNum = 17;
    RunSprites();
    CHECK(GrassOn(UpdateTallGrassFieldEffect, 10, 10, LOCALID_FOLLOWER, NULL) == 1,
          "the follower's grass went with its map");
}

static struct VisibleWild *Wild(u32 slot, s16 x, s16 y)
{
    struct VisibleWild *v = &sVisible[slot];

    memset(v, 0, sizeof(*v));
    v->active = TRUE;
    v->x = x;
    v->y = y;
    v->spriteId = 5 + slot;
    gSprites[v->spriteId].inUse = TRUE;
    gSprites[v->spriteId].data[7] = VW_MAGIC;
    return v;
}

static void TestWild(void)
{
    struct VisibleWild *v;
    u8 id;
    int seek = 0;

    Reset();
    v = Wild(2, 20, 20);
    id = LOCALID_VISIBLE_WILD + 2;
    VisibleGrass(v, v->x, v->y, TRUE);            /* it appears in it */
    CHECK(GrassOn(UpdateTallGrassFieldEffect, 20, 20, id, &seek) == 1 && seek >= 0,
          "where it appears the grass is not settled round it");
    RunSprites();
    CHECK(GrassOn(UpdateTallGrassFieldEffect, 20, 20, id, NULL) == 1, "its grass went while it stands there");

    /* A step east (VisibleMove): the rustle at the step's start. */
    v->dx = 1;
    v->progress = 1;
    VisibleGrass(v, 21, 20, FALSE);
    CHECK(GrassOn(UpdateTallGrassFieldEffect, 21, 20, id, &seek) == 1 && seek == -1,
          "its step into tall grass does not rustle");
    RunSprites();
    CHECK(GrassOn(UpdateTallGrassFieldEffect, 20, 20, id, NULL) == 1,
          "the grass it is leaving went before the step was over");
    /* The step over: on the new tile, the old grass's picture over. */
    v->x = 21;
    v->dx = 0;
    v->progress = 0;
    EndAnims();
    RunSprites();                                 /* marked as left, */
    RunSprites();                                 /* then gone, as an object's */
    CHECK(GrassOn(UpdateTallGrassFieldEffect, 20, 20, 0, NULL) == 0, "the grass it left stayed");
    CHECK(GrassOn(UpdateTallGrassFieldEffect, 21, 20, id, NULL) == 1, "the grass it stands in went");

    /* Long grass. */
    sGrid[20][22] = MB_LONG_GRASS;
    v->dx = 1;
    v->progress = 1;
    VisibleGrass(v, 22, 20, FALSE);
    CHECK(GrassOn(UpdateLongGrassFieldEffect, 22, 20, id, NULL) == 1, "its step into long grass does not rustle");
    RunSprites();
    CHECK(GrassOn(UpdateLongGrassFieldEffect, 22, 20, id, NULL) == 1, "its long grass went mid-step");

    /* Met the player (VisibleRemove): its grass goes at once. */
    v->active = FALSE;
    RunSprites();
    CHECK(GrassOn(UpdateTallGrassFieldEffect, 21, 20, id, NULL) == 0
       && GrassOn(UpdateLongGrassFieldEffect, 22, 20, id, NULL) == 0, "its grass outlived it");

    /* Its sprite taken by the field (a battle): the grass goes with it. */
    v = Wild(0, 30, 30);
    VisibleGrass(v, v->x, v->y, TRUE);
    gSprites[v->spriteId].data[7] = 0;
    RunSprites();
    CHECK(GrassOn(UpdateTallGrassFieldEffect, 30, 30, 0, NULL) == 0, "grass left without its sprite");

    /* On water, or not on grass: nothing. */
    v = Wild(1, 40, 40);
    sGrid[40][40] = MB_NORMAL;
    VisibleGrass(v, 40, 40, FALSE);
    v->water = TRUE;
    sGrid[40][41] = MB_TALL_GRASS;
    VisibleGrass(v, 41, 40, FALSE);
    CHECK(GrassOn(UpdateTallGrassFieldEffect, 40, 40, 0, NULL) + GrassOn(UpdateTallGrassFieldEffect, 41, 40, 0, NULL)
          == 0, "grass where it should not rustle");

    /* Another slot's ids and an object's own are left alone. */
    CHECK(!VisibleWild_GrassOwner(LOCALID_VISIBLE_WILD + VW_MAX, &v->x, &v->y, &v->x, &v->y)
       && !VisibleWild_GrassOwner(LOCALID_FOLLOWER, &v->x, &v->y, &v->x, &v->y), "a grass id out of range answered");
}

static void TestSpriteBudget(void)
{
    struct VisibleWild *v;
    int i, free = 0;

    Reset();
    v = Wild(3, 20, 20);
    for (i = 0; i < MAX_SPRITES && free < MAX_SPRITES - VW_GRASS_SPARE; i++)
        if (!gSprites[i].inUse)
        {
            gSprites[i].inUse = TRUE;
            free++;
        }
    VisibleGrass(v, 20, 20, FALSE);
    CHECK(GrassCount() == 0, "the field's last %d sprites taken for a wild one's grass", VW_GRASS_SPARE);
}

int main(void)
{
    TestFollowerBehindPlayer();
    TestFollowerCrossesMap();
    TestWild();
    TestSpriteBudget();
    if (sFailures)
        return 1;
    printf("overworld grass: ok\n");
    return 0;
}
'''
    with tempfile.TemporaryDirectory() as tmp:
        c = Path(tmp) / 'grass.c'
        exe = Path(tmp) / 'grass'
        c.write_text(source, encoding='utf-8')
        subprocess.run([args.cc, '-std=gnu99', '-Wall', '-Wno-unused-function', '-o', str(exe), str(c)], check=True)
        raise SystemExit(subprocess.run([str(exe)]).returncode)


if __name__ == '__main__':
    main()
