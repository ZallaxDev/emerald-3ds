#include "global.h"
#include "event_object_movement.h"
#include "field_player_avatar.h"
#include "fieldmap.h"
#include "main.h"
#include "overworld.h"
#include "pokemon.h"
#include "script.h"
#include "sprite.h"
#include "3ds_extras.h"
#include "3ds_follower.h"
#include "3ds_field_icon.h"
#include "3ds_follower_trail.h"

#define FOLLOWER_MARKER 0x464C

static CtrFollowerTrail sTrail;
static struct
{
    bool8 active;
    u8 spriteId, mapGroup, mapNum;
    u16 species;
    u32 personality;
} sFollower;

static bool8 SpriteValid(void)
{
    return sFollower.active && sFollower.spriteId < MAX_SPRITES
        && gSprites[sFollower.spriteId].inUse
        && gSprites[sFollower.spriteId].data[7] == FOLLOWER_MARKER;
}

bool CtrFollower_IsSprite(const struct Sprite *sprite)
{
    return SpriteValid() && sprite == &gSprites[sFollower.spriteId];
}

static void RemoveFollower(void)
{
    if (SpriteValid())
        CtrFieldIcon_Destroy(sFollower.spriteId, sFollower.species);
    sFollower.active = FALSE;
    sTrail.count = 0;
}

static struct Pokemon *LeadPokemon(void)
{
    for (unsigned i = 0; i < gPlayerPartyCount; ++i)
        if (GetMonData(&gPlayerParty[i], MON_DATA_SPECIES) != SPECIES_NONE
         && !GetMonData(&gPlayerParty[i], MON_DATA_IS_EGG)
         && GetMonData(&gPlayerParty[i], MON_DATA_HP))
            return &gPlayerParty[i];
    return NULL;
}

/* Floor division keeps connected-map border coordinates correct west/north of zero. */
static s16 TileCoordinate(s32 pixel)
{
    return pixel >= 0 ? pixel / 16 : -((-pixel + 15) / 16);
}

void CtrFollower_Update(void)
{
    if (!CtrExtras_Get("follower") || gMain.callback2 != CB2_Overworld
     || gPlayerAvatar.objectEventId >= OBJECT_EVENTS_COUNT)
    {
        RemoveFollower();
        return;
    }
    const struct ObjectEvent *player = &gObjectEvents[gPlayerAvatar.objectEventId];
    if (!player->active || player->spriteId >= MAX_SPRITES)
    {
        RemoveFollower();
        return;
    }
    const struct Sprite *playerSprite = &gSprites[player->spriteId];
    u8 flags = GetPlayerAvatarFlags();
    /* Cosmetic only: never participates in object collisions or scripted movement. */
    if (!(flags & PLAYER_AVATAR_FLAG_ON_FOOT)
     || (flags & (PLAYER_AVATAR_FLAG_MACH_BIKE | PLAYER_AVATAR_FLAG_ACRO_BIKE
                | PLAYER_AVATAR_FLAG_SURFING | PLAYER_AVATAR_FLAG_UNDERWATER
                | PLAYER_AVATAR_FLAG_FORCED_MOVE))
     || playerSprite->invisible || playerSprite->y2 != 0
     || ArePlayerFieldControlsLocked() || ScriptContext_IsEnabled())
    {
        RemoveFollower();
        return;
    }
    struct Pokemon *mon = LeadPokemon();
    if (mon == NULL)
    {
        RemoveFollower();
        return;
    }
    u16 species = GetMonData(mon, MON_DATA_SPECIES);
    u32 personality = GetMonData(mon, MON_DATA_PERSONALITY);
    if (!SpriteValid() || sFollower.species != species || sFollower.personality != personality
     || sFollower.mapGroup != gSaveBlock1Ptr->location.mapGroup
     || sFollower.mapNum != gSaveBlock1Ptr->location.mapNum)
    {
        RemoveFollower();
        u8 spriteId = CtrFieldIcon_Create(species, personality, FOLLOWER_MARKER);
        if (spriteId == MAX_SPRITES)
            return;
        sFollower.spriteId = spriteId;
        sFollower.species = species;
        sFollower.personality = personality;
        sFollower.mapGroup = gSaveBlock1Ptr->location.mapGroup;
        sFollower.mapNum = gSaveBlock1Ptr->location.mapNum;
        sFollower.active = TRUE;
        gSprites[spriteId].invisible = TRUE;
    }
    s16 tileX, tileY;
    SetSpritePosToMapCoords(player->currentCoords.x, player->currentCoords.y, &tileX, &tileY);
    /* The coordinates already name the destination while a step is animating. */
    CtrFollowerPoint position = {
        player->currentCoords.x * 16 + playerSprite->x - (tileX + 8),
        player->currentCoords.y * 16 + playerSprite->y
            - (tileY + 16 + playerSprite->centerToCornerVecY)
    }, follower;
    struct Sprite *sprite = &gSprites[sFollower.spriteId];
    if (!CtrFollowerTrail_Update(&sTrail, position, &follower))
    {
        sprite->invisible = TRUE;
        return;
    }
    s16 x = TileCoordinate(follower.x), y = TileCoordinate(follower.y);
    u8 occupant = GetObjectEventIdByXY(x, y);
    s32 dx = follower.x - position.x, dy = follower.y - position.y;
    /* Turning back or an NPC entering the trail must not put two sprites on top of each other. */
    sprite->invisible = (dx > -8 && dx < 8 && dy > -8 && dy < 8)
        || (occupant != OBJECT_EVENTS_COUNT && occupant != gPlayerAvatar.objectEventId);
    SetSpritePosToMapCoords(x, y, &tileX, &tileY);
    sprite->x = tileX + follower.x - x * 16 + 8;
    sprite->y = tileY + follower.y - y * 16;
    sprite->oam.priority = playerSprite->oam.priority;
    sprite->subpriority = 200 - ((sprite->y + 64) >> 1 & 0x7F);
}
