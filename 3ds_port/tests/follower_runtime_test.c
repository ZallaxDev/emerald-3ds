/* Exercise the game-facing follower and palette ownership against the real headers. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "global.h"
#include "main.h"
#include "pokemon.h"
#include "pokemon_icon.h"
#include "sprite.h"
#include "3ds_field_icon.h"
#include "3ds_follower.h"
#include "constants/pokemon_icon.h"

void CtrFollower_Update(void);

struct Sprite gSprites[MAX_SPRITES + 1];
u8 VRAM_[0x18000];
struct ObjectEvent gObjectEvents[OBJECT_EVENTS_COUNT];
struct PlayerAvatar gPlayerAvatar;
struct Main gMain;
struct Pokemon gPlayerParty[PARTY_SIZE];
u8 gPlayerPartyCount;
static struct SaveBlock1 sSave;
struct SaveBlock1 *gSaveBlock1Ptr = &sSave;
const u8 gMonIconPaletteIndices[NUM_SPECIES] = {[1] = 0, [2] = 0, [3] = 1};

static u8 sPalette[3], sOccupant = OBJECT_EVENTS_COUNT;
static u8 sFlags = PLAYER_AVATAR_FLAG_ON_FOOT;
static bool8 sEnabled = TRUE, sLocked, sScript, sPaletteFull;
static unsigned sCreated, sDestroyed, sFreed;
static s16 sCamera;
static struct { u16 species, hp; bool8 egg; u32 personality; } sParty[PARTY_SIZE];

void CB2_Overworld(void) {}
static void Menu(void) {}
int CtrExtras_Get(const char *key) { assert(strcmp(key, "follower") == 0); return sEnabled; }
u8 GetPlayerAvatarFlags(void) { return sFlags; }
bool8 ArePlayerFieldControlsLocked(void) { return sLocked; }
bool8 ScriptContext_IsEnabled(void) { return sScript; }
u8 GetObjectEventIdByXY(s16 x, s16 y) { (void)x; (void)y; return sOccupant; }
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
            gSprites[i].x = x;
            gSprites[i].y = y;
            gSprites[i].subpriority = subpriority;
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
    gSprites[0].x = x + 8 - sCamera;
    gSprites[0].y = y;
}
static struct Sprite *Follower(void)
{
    for (unsigned i = 1; i < MAX_SPRITES; ++i)
        if (gSprites[i].inUse && gSprites[i].data[7] == 0x464C)
            return &gSprites[i];
    return NULL;
}
static void Step(s16 x, s16 y) { PlacePlayer(x, y); CtrFollower_Update(); }

int main(void)
{
    memset(sPalette, 0xFF, sizeof(sPalette));
    gMain.callback2 = CB2_Overworld;
    gObjectEvents[0].active = TRUE;
    gSprites[0].inUse = TRUE;
    gSprites[0].centerToCornerVecY = -16;
    gPlayerPartyCount = 2;
    sParty[0].species = 1; sParty[0].hp = 10;
    sParty[1].species = 2; sParty[1].hp = 10;
    Step(0, 0);
    assert(Follower() && Follower()->invisible && Follower()->coordOffsetEnabled);
    assert(CtrFollower_IsSprite(Follower()) && !CtrFollower_IsSprite(&gSprites[0]));
    Step(16, 0);
    assert(!Follower()->invisible && Follower()->x == 8 && Follower()->y == 0);
    assert(Follower()->hFlip && (Follower()->oam.matrixNum & 0x08));
    /* Camera scrolling cannot manufacture a step in world coordinates. */
    sCamera = 40; Step(16, 0);
    assert(Follower()->x == -32 && !Follower()->invisible);
    sCamera = 0; Step(16, 16);
    assert(Follower()->x == 24 && Follower()->y == 0);
    sOccupant = 3; Step(16, 16); assert(Follower()->invisible);
    sOccupant = OBJECT_EVENTS_COUNT; Step(16, 16); assert(!Follower()->invisible);
    sFlags = PLAYER_AVATAR_FLAG_SURFING; CtrFollower_Update(); assert(!Follower());
    sFlags = PLAYER_AVATAR_FLAG_MACH_BIKE; CtrFollower_Update(); assert(!Follower());
    sFlags = PLAYER_AVATAR_FLAG_ON_FOOT; Step(16, 16); assert(Follower()->invisible);
    Step(32, 16); assert(!Follower()->invisible);
    ++sSave.location.mapNum; CtrFollower_Update(); assert(Follower()->invisible);
    Step(48, 16); assert(!Follower()->invisible);
    unsigned created = sCreated;
    sParty[0].hp = 0; CtrFollower_Update(); assert(sCreated == created + 1 && Follower()->invisible);
    sParty[1].egg = TRUE; CtrFollower_Update(); assert(!Follower());
    sParty[1].egg = FALSE; Step(48, 16);
    sScript = TRUE; CtrFollower_Update(); assert(!Follower());
    sScript = FALSE; Step(48, 16);
    sLocked = TRUE; CtrFollower_Update(); assert(!Follower());
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
    memset(gSprites, 0, sizeof(gSprites)); memset(sPalette, 0xFF, sizeof(sPalette));
    /* Wild/follower icons sharing a palette can be removed in either order. */
    u8 wild = CtrFieldIcon_Create(1, 0, 0x5657);
    u8 follower = CtrFieldIcon_Create(2, 0, 0x464C);
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
    unsigned freed = sFreed;
    CtrFieldIcon_Destroy(wild, 1); assert(sFreed == freed && sPalette[0] == 12);
    CtrFieldIcon_Destroy(follower, 2); assert(sFreed == freed + 1 && sPalette[0] == 0xFF);
    wild = CtrFieldIcon_Create(1, 0, 0x5657);
    follower = CtrFieldIcon_Create(2, 0, 0x464C);
    freed = sFreed;
    CtrFieldIcon_Destroy(follower, 2); assert(sFreed == freed);
    CtrFieldIcon_Destroy(wild, 1); assert(sFreed == freed + 1);
    sPaletteFull = TRUE; assert(CtrFieldIcon_Create(1, 0, 0x464C) == MAX_SPRITES);
    sPaletteFull = FALSE;
    for (unsigned i = 0; i < MAX_SPRITES; ++i) gSprites[i].inUse = TRUE;
    created = sCreated;
    assert(CtrFieldIcon_Create(1, 0, 0x464C) == MAX_SPRITES && sCreated == created);
    puts("PASS follower runtime: lifecycle, shared palettes, motion-paced animation, facing in 2D/voxel OAM");
    return 0;
}
