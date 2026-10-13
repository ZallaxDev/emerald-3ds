/* Exercise the game-facing follower and palette ownership against the real headers. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "global.h"
#include "main.h"
#include "pokemon.h"
#include "pokemon_icon.h"
#include "sprite.h"
#include "event_object_movement.h"
#include "3ds_field_icon.h"
#include "3ds_follower.h"
#include "constants/pokemon_icon.h"

void CtrFollower_Update(void);

/* Use the real OAM sorter, not a second model of draw order. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wmissing-braces"
#define IndexOfSpritePaletteTag CtrTest_UnusedIndexOfSpritePaletteTag
u8 CtrTest_UnusedIndexOfSpritePaletteTag(u16 tag);
#include "src/sprite.c"
#undef IndexOfSpritePaletteTag
#pragma GCC diagnostic pop
u8 VRAM_[0x18000];
struct ObjectEvent gObjectEvents[OBJECT_EVENTS_COUNT];
struct PlayerAvatar gPlayerAvatar;
struct Main gMain;
struct Pokemon gPlayerParty[PARTY_SIZE];
u8 gPlayerPartyCount;
static struct SaveBlock1 sSave;
struct SaveBlock1 *gSaveBlock1Ptr = &sSave;
const struct SpritePalette gMonIconPaletteTable[3] = {{0}};
const struct CompressedSpritePalette gMonPaletteTable[NUM_SPECIES] = {{0}};
const struct CompressedSpritePalette gMonShinyPaletteTable[NUM_SPECIES] = {{0}};
const u8 gMonIconPaletteIndices[NUM_SPECIES] = {[1] = 0, [2] = 0, [3] = 1};

static u8 sPalette[3];
static u8 sFlags = PLAYER_AVATAR_FLAG_ON_FOOT;
static bool8 sEnabled = TRUE, sLocked, sScript, sPaletteFull;
static unsigned sCreated, sDestroyed, sFreed;
static s16 sCamera;
static u8 sBlocked, sElevation;
static u8 sIconTiles[1024];
static struct { u16 species, hp; bool8 egg; u32 personality; } sParty[PARTY_SIZE];

/* This existing regression harness exercises normal followers only. */
bool8 IsMonShiny(struct Pokemon *mon) { (void)mon; return FALSE; }
void LoadPalette(const void *src, u16 offset, u16 size) { (void)src; (void)offset; (void)size; }
void LZ77UnCompWram(const u32 *src, void *dst) { (void)src; (void)dst; assert(!"unexpected shiny asset in normal follower test"); }
const void *Port_ResolveAssetPointerSized(const void *p, u32 size) { (void)size; return p; }

void CB2_Overworld(void) {}
static void Menu(void) {}
int CtrExtras_Get(const char *key) { assert(strcmp(key, "follower") == 0); return sEnabled; }
u8 GetPlayerAvatarFlags(void) { return sFlags; }
bool8 ArePlayerFieldControlsLocked(void) { return sLocked; }
bool8 ScriptContext_IsEnabled(void) { return sScript; }
int GetMapBorderIdAt(int x, int y) { (void)x; (void)y; return 0; }
bool32 CanCameraMoveInDirection(int direction) { (void)direction; return TRUE; }
u8 MapGridGetCollisionAt(int x, int y)
{
    int dx = x - gObjectEvents[0].currentCoords.x, dy = y - gObjectEvents[0].currentCoords.y;
    unsigned bit = dx < 0 ? 1 : dx > 0 ? 2 : dy < 0 ? 4 : 8;
    return (sBlocked & bit) != 0;
}
u8 MapGridGetElevationAt(int x, int y) { (void)x; (void)y; return sElevation; }
u32 MapGridGetMetatileBehaviorAt(int x, int y) { (void)x; (void)y; return 0; }
void SetSpritePosToMapCoords(s16 x, s16 y, s16 *sx, s16 *sy)
{
    *sx = x * 16 - sCamera;
    *sy = y * 16;
}
u32 GetMonData2(struct Pokemon *mon, s32 field)
{
    unsigned i = mon - gPlayerParty;
    assert(i < PARTY_SIZE);
    switch (field)
    {
    case MON_DATA_SPECIES: return sParty[i].species;
    case MON_DATA_HP: return sParty[i].hp;
    case MON_DATA_IS_EGG: return sParty[i].egg;
    case MON_DATA_PERSONALITY: return sParty[i].personality;
    default: assert(0); return 0;
    }
}
u8 IndexOfSpritePaletteTag(u16 tag)
{
    assert(tag >= POKE_ICON_BASE_PAL_TAG && tag < POKE_ICON_BASE_PAL_TAG + 3);
    return sPalette[tag - POKE_ICON_BASE_PAL_TAG];
}
void LoadMonIconPalette(u16 species)
{
    if (!sPaletteFull)
        sPalette[gMonIconPaletteIndices[species]] = 12 + gMonIconPaletteIndices[species];
}
void FreeMonIconPalette(u16 species)
{
    sPalette[gMonIconPaletteIndices[species]] = 0xFF;
    ++sFreed;
}
u8 CreateMonIcon(u16 species, void (*callback)(struct Sprite *), s16 x, s16 y,
                 u8 subpriority, u32 personality, bool32 deoxys)
{
    (void)personality; (void)deoxys;
    for (unsigned i = 0; i < MAX_SPRITES; ++i)
        if (!gSprites[i].inUse)
        {
            memset(&gSprites[i], 0, sizeof(gSprites[i]));
            gSprites[i].inUse = TRUE;
            gSprites[i].callback = callback;
            gSprites[i].images = (const struct SpriteFrameImage *)sIconTiles;
            gSprites[i].x = x;
            gSprites[i].y = y;
            gSprites[i].subpriority = subpriority;
            gSprites[i].centerToCornerVecX = -16;
            gSprites[i].centerToCornerVecY = -16;
            gSprites[i].oam.paletteNum = sPalette[gMonIconPaletteIndices[species]];
            ++sCreated;
            return i;
        }
    assert(0); /* The wrapper must detect exhaustion before calling the game API. */
    return MAX_SPRITES;
}
void FreeAndDestroyMonIconSprite(struct Sprite *sprite) { sprite->inUse = FALSE; ++sDestroyed; }
const void *Port_ResolveAssetPointer(const void *p) { return p; }
void CpuSet(const void *src, void *dest, u32 control) { (void)src; (void)dest; (void)control; }

static void PlacePlayer(s16 x, s16 y)
{
    gObjectEvents[0].currentCoords.x = x / 16;
    gObjectEvents[0].currentCoords.y = y / 16;
    gObjectEvents[0].previousCoords = gObjectEvents[0].currentCoords;
    gSprites[0].x = x + 8 - sCamera;
    gSprites[0].y = y;
    SetObjectSubpriorityByElevation(gObjectEvents[0].previousElevation, &gSprites[0], 1);
}
static struct Sprite *Follower(void)
{
    for (unsigned i = 1; i < MAX_SPRITES; ++i)
        if (gSprites[i].inUse && gSprites[i].data[7] == 0x464C)
            return &gSprites[i];
    return NULL;
}
static void Step(s16 x, s16 y) { PlacePlayer(x, y); CtrFollower_Update(); }

static unsigned DrawOrder(const struct Sprite *sprite)
{
    UpdateOamCoords();
    BuildSpritePriorities();
    for (unsigned i = 0; i < MAX_SPRITES; ++i) sSpriteOrder[i] = i;
    SortSprites();
    for (unsigned i = 0; i < MAX_SPRITES; ++i)
        if (&gSprites[sSpriteOrder[i]] == sprite) return i;
    assert(0); return MAX_SPRITES;
}

static void CheckNpcReservation(s16 x, s16 y)
{
    struct ObjectEvent *npc = &gObjectEvents[1];
    npc->active = TRUE;
    npc->currentCoords = (struct Coords16){x + 1, y};
    npc->previousCoords = npc->currentCoords;
    assert(CtrFollower_BlocksObjectAt(npc, x, y));
    assert(GetCollisionAtCoords(npc, x, y, DIR_WEST) == COLLISION_OBJECT_EVENT);
    assert(GetCollisionFlagsAtCoords(npc, x, y, DIR_WEST)
            & (1 << (COLLISION_OBJECT_EVENT - 1)));
    assert(!CtrFollower_BlocksObjectAt(&gObjectEvents[0], x, y));
    npc->active = FALSE;
}

int main(void)
{
    memset(sPalette, 0xFF, sizeof(sPalette));
    gMain.callback2 = CB2_Overworld;
    gObjectEvents[0].active = TRUE;
    gObjectEvents[0].isPlayer = TRUE;
    gSprites[0].inUse = TRUE;
    gSprites[0].centerToCornerVecY = -16;
    gSprites[0].oam.priority = 2;
    gPlayerPartyCount = 2;
    sParty[0].species = 1; sParty[0].hp = 10;
    sParty[1].species = 2; sParty[1].hp = 10;
    sIconTiles[((23 / 8) * 4) * 32 + (23 % 8) * 4] = 1;
    gObjectEvents[0].facingDirection = DIR_SOUTH;
    Step(0, 0);
    assert(Follower() && !Follower()->invisible && Follower()->coordOffsetEnabled);
    assert(Follower()->x == 8 && Follower()->y == -16);
    int32_t fx, fy; int pad;
    assert(CtrFollower_GetPosition(Follower(), &fx, &fy, &pad));
    assert(fx == 0 && fy == -16 && pad == 8);
    assert(CtrFollower_IsSprite(Follower()) && !CtrFollower_IsSprite(&gSprites[0]));
    CheckNpcReservation(0, -1);
    Step(16, 0);
    assert(!Follower()->invisible && Follower()->x == 8 && Follower()->y == 0);
    /* Camera scrolling cannot manufacture a step in world coordinates. */
    sCamera = 40; Step(16, 0);
    assert(Follower()->x == -32 && !Follower()->invisible);
    sCamera = 0; Step(16, 16);
    assert(Follower()->x == 24 && Follower()->y == 0);
    assert(Follower()->hFlip && (Follower()->oam.matrixNum & 0x08));
    CheckNpcReservation(1, 0);
    sFlags = PLAYER_AVATAR_FLAG_SURFING; CtrFollower_Update(); assert(!Follower());
    sFlags = PLAYER_AVATAR_FLAG_MACH_BIKE; CtrFollower_Update(); assert(!Follower());
    sFlags = PLAYER_AVATAR_FLAG_ON_FOOT; Step(16, 16); assert(!Follower()->invisible);
    Step(32, 16); assert(!Follower()->invisible);
    ++sSave.location.mapNum; CtrFollower_Update(); assert(!Follower()->invisible);
    Step(48, 16); assert(!Follower()->invisible);
    unsigned created = sCreated;
    sParty[0].hp = 0; CtrFollower_Update(); assert(sCreated == created + 1 && !Follower()->invisible);
    sParty[1].egg = TRUE; CtrFollower_Update(); assert(!Follower());
    sParty[1].egg = FALSE; Step(48, 16);
    struct Sprite *dialogueFollower = Follower();
    unsigned dialogueCreated = sCreated;
    int32_t dialogueX, dialogueY; int dialoguePad;
    assert(CtrFollower_GetPosition(dialogueFollower, &dialogueX, &dialogueY, &dialoguePad));
    sScript = TRUE; CtrFollower_Update(); assert(Follower() == dialogueFollower && !Follower()->invisible);
    assert(sCreated == dialogueCreated);
    assert(CtrFollower_GetPosition(Follower(), &fx, &fy, &pad) && fx == dialogueX && fy == dialogueY);
    gObjectEvents[1].active = TRUE;
    assert(!CtrFollower_BlocksObjectAt(&gObjectEvents[1], fx / 16, fy / 16));
    assert(GetCollisionAtCoords(&gObjectEvents[1], fx / 16, fy / 16, DIR_NORTH) == COLLISION_NONE);
    gObjectEvents[1].active = FALSE;
    sScript = FALSE; Step(48, 16);
    sLocked = TRUE; CtrFollower_Update(); assert(Follower() == dialogueFollower && !Follower()->invisible);
    assert(sCreated == dialogueCreated);
    sLocked = FALSE; Step(48, 16);
    gSprites[0].y2 = -8; CtrFollower_Update(); assert(!Follower());
    gSprites[0].y2 = 0; Step(48, 16);
    gMain.callback2 = Menu; CtrFollower_Update(); assert(!Follower());
    gMain.callback2 = CB2_Overworld; Step(48, 16);
    /* A recycled menu sprite at the old ID must never be destroyed. */
    Follower()->data[7] = 0;
    unsigned destroyed = sDestroyed;
    CtrFollower_Update(); assert(sDestroyed == destroyed && Follower());
    sEnabled = FALSE; CtrFollower_Update(); assert(!Follower());
    /* North places the follower lower on screen, in front of the player's feet.
     * Cover fractional running steps, camera offsets and OAM's wrapped Y range. */
    sEnabled = TRUE; gObjectEvents[0].facingDirection = DIR_NORTH;
    Step(64, 96);
    for (unsigned i = 0; i < 40; ++i)
    {
        Step(64, 96 - 2 * i);
        assert(!Follower()->invisible);
        for (int camera = -256; camera <= 256; camera += 8)
        {
            gSpriteCoordOffsetY = camera;
            SetObjectSubpriorityByElevation(0, &gSprites[0], 1);
            CtrFollower_Update();
            /* The original game's 256-pixel depth wrap needs a local tie fix. */
            assert(DrawOrder(Follower()) < DrawOrder(&gSprites[0]));
        }
    }
    gSpriteCoordOffsetY = 0;
    CheckNpcReservation(4, 2);
    /* Between tiles, keep both occupied ends reserved until the follower leaves. */
    assert(CtrFollower_GetPosition(Follower(), &fx, &fy, &pad));
    assert(fy % 16 != 0);
    CheckNpcReservation(4, fy / 16 + 1);
    gObjectEvents[1].active = TRUE;
    gObjectEvents[1].currentCoords = (struct Coords16){5, 2};
    gObjectEvents[1].previousCoords = gObjectEvents[1].currentCoords;
    Step(64, 0);
    assert(!CtrFollower_BlocksObjectAt(&gObjectEvents[1], 4, 2));
    assert(GetCollisionAtCoords(&gObjectEvents[1], 4, 2, DIR_WEST) == COLLISION_NONE);
    /* A same-row turn and a southward walk must reverse the layering correctly. */
    Step(66, 0); Step(80, 0);
    assert(!Follower()->invisible);
    Step(80, 2); Step(80, 16); Step(80, 32);
    assert(DrawOrder(Follower()) > DrawOrder(&gSprites[0]));
    assert(CtrFollower_GetPosition(Follower(), &fx, &fy, &pad));
    gObjectEvents[1].active = FALSE;
    struct ObjectEvent *npc = &gObjectEvents[1];
    sElevation = 3; CtrFollower_Update();
    npc->active = TRUE; npc->currentElevation = 4;
    assert(!CtrFollower_BlocksObjectAt(npc, fx / 16, fy / 16));
    npc->currentElevation = 0;
    assert(CtrFollower_BlocksObjectAt(npc, fx / 16, fy / 16));
    ++sSave.location.mapNum;
    assert(!CtrFollower_BlocksObjectAt(npc, fx / 16, fy / 16));
    --sSave.location.mapNum;
    sFlags = PLAYER_AVATAR_FLAG_MACH_BIKE;
    assert(!CtrFollower_BlocksObjectAt(npc, fx / 16, fy / 16));
    sFlags = PLAYER_AVATAR_FLAG_ON_FOOT;
    sElevation = 0;
    sEnabled = FALSE;
    assert(!CtrFollower_BlocksObjectAt(npc, fx / 16, fy / 16));
    CtrFollower_Update(); npc->active = FALSE;
    /* Spawning must avoid both the departure and arrival tiles of a moving NPC. */
    npc->active = TRUE;
    npc->previousCoords = (struct Coords16){3, 0};
    npc->currentCoords = (struct Coords16){4, 0};
    sEnabled = TRUE; gObjectEvents[0].facingDirection = DIR_SOUTH; Step(48, 16);
    assert(!Follower()->invisible && Follower()->y == 16);
    sEnabled = FALSE; CtrFollower_Update(); npc->active = FALSE;
    /* Immediate placement follows facing; a blocked rear tile falls back safely. */
    sEnabled = TRUE; gObjectEvents[0].facingDirection = DIR_EAST; Step(48, 16);
    assert(Follower()->x == 40 && Follower()->y == 16 && Follower()->hFlip);
    sEnabled = FALSE; CtrFollower_Update();
    sEnabled = TRUE; gObjectEvents[0].facingDirection = DIR_WEST; Step(48, 16);
    assert(Follower()->x == 72 && !Follower()->hFlip);
    sEnabled = FALSE; CtrFollower_Update();
    sEnabled = TRUE; gObjectEvents[0].facingDirection = DIR_SOUTH; sBlocked = 4; Step(48, 16);
    assert(!Follower()->invisible && Follower()->x == 40 && Follower()->y == 16);
    sEnabled = FALSE; CtrFollower_Update();
    sEnabled = TRUE; sBlocked = 15; Step(48, 16); assert(Follower()->invisible);
    sBlocked = 0; CtrFollower_Update(); assert(!Follower()->invisible);
    sEnabled = FALSE; CtrFollower_Update();
    memset(gSprites, 0, sizeof(gSprites)); memset(sPalette, 0xFF, sizeof(sPalette));
    /* Wild/follower icons sharing a palette can be removed in either order. */
    u8 wild = CtrFieldIcon_Create(1, 0, 0x5657, false);
    u8 follower = CtrFieldIcon_Create(2, 0, 0x464C, false);
    /* Facing lives in OAM too, so the voxel decoder and 2D renderer agree. */
    gSprites[follower].oam.matrixNum = 0x10;
    CtrFieldIcon_SetMotion(follower, 1, 0);
    assert(gSprites[follower].hFlip && gSprites[follower].oam.matrixNum == 0x18);
    assert(gSprites[follower].data[4] == 14 && gSprites[wild].data[4] == 14);
    gSprites[follower].callback(&gSprites[follower]);
    s16 frame = gSprites[follower].data[5];
    for (unsigned i = 0; i < 14; ++i) gSprites[follower].callback(&gSprites[follower]);
    assert(gSprites[follower].data[5] == frame);
    gSprites[follower].callback(&gSprites[follower]);
    assert(gSprites[follower].data[5] != frame);
    CtrFieldIcon_SetMotion(follower, -2, 0);
    assert(!gSprites[follower].hFlip && gSprites[follower].oam.matrixNum == 0x10);
    assert(gSprites[follower].data[4] == 7 && gSprites[follower].data[6] == 7);
    frame = gSprites[follower].data[5];
    for (unsigned i = 0; i < 7; ++i) gSprites[follower].callback(&gSprites[follower]);
    assert(gSprites[follower].data[5] == frame);
    gSprites[follower].callback(&gSprites[follower]);
    assert(gSprites[follower].data[5] != frame);
    CtrFieldIcon_SetMotion(follower, 0, 2);
    assert(!gSprites[follower].hFlip && gSprites[follower].data[4] == 7);
    CtrFieldIcon_SetMotion(follower, 0, 0);
    assert(!gSprites[follower].hFlip && gSprites[follower].data[4] == 14);
    assert(gSprites[wild].data[4] == 14);
    assert(CtrFieldIcon_FootPad(follower) == 8);
    gSprites[follower].data[5] = 1;
    assert(CtrFieldIcon_FootPad(follower) == 8); /* Stable standing frame, not the current bob. */
    unsigned freed = sFreed;
    CtrFieldIcon_Destroy(wild, 1); assert(sFreed == freed && sPalette[0] == 12);
    CtrFieldIcon_Destroy(follower, 2); assert(sFreed == freed + 1 && sPalette[0] == 0xFF);
    wild = CtrFieldIcon_Create(1, 0, 0x5657, false);
    follower = CtrFieldIcon_Create(2, 0, 0x464C, false);
    freed = sFreed;
    CtrFieldIcon_Destroy(follower, 2); assert(sFreed == freed);
    CtrFieldIcon_Destroy(wild, 1); assert(sFreed == freed + 1);
    sPaletteFull = TRUE; assert(CtrFieldIcon_Create(1, 0, 0x464C, false) == MAX_SPRITES);
    sPaletteFull = FALSE;
    for (unsigned i = 0; i < MAX_SPRITES; ++i) gSprites[i].inUse = TRUE;
    created = sCreated;
    assert(CtrFieldIcon_Create(1, 0, 0x464C, false) == MAX_SPRITES && sCreated == created);
    puts("PASS follower runtime: real OAM depth/collision hooks, persistent dialogues, lifecycle, palettes, animation and facing");
    return 0;
}
