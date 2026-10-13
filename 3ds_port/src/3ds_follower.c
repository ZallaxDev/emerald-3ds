#include "global.h"
#include "event_object_movement.h"
#include "field_player_avatar.h"
#include "fieldmap.h"
#include "main.h"
#include "metatile_behavior.h"
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
    u8 elevation;
    u16 species;
    u32 personality;
    CtrFollowerPoint position;
    int footPad;
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

bool CtrFollower_GetPosition(const struct Sprite *sprite, int32_t *x, int32_t *y, int *footPad)
{
    if (!CtrFollower_IsSprite(sprite) || sprite->invisible)
        return false;
    *x = sFollower.position.x;
    *y = sFollower.position.y;
    *footPad = sFollower.footPad;
    return true;
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

static bool OnFoot(void)
{
    u8 flags = GetPlayerAvatarFlags();
    return (flags & PLAYER_AVATAR_FLAG_ON_FOOT)
        && !(flags & (PLAYER_AVATAR_FLAG_MACH_BIKE | PLAYER_AVATAR_FLAG_ACRO_BIKE
                    | PLAYER_AVATAR_FLAG_SURFING | PLAYER_AVATAR_FLAG_UNDERWATER
                    | PLAYER_AVATAR_FLAG_FORCED_MOVE));
}

bool CtrFollower_BlocksObjectAt(const struct ObjectEvent *object, int16_t x, int16_t y)
{
    if (!object || !object->active || object->isPlayer
     || !SpriteValid() || gSprites[sFollower.spriteId].invisible
     || !CtrExtras_Get("follower") || gMain.callback2 != CB2_Overworld
     || gPlayerAvatar.objectEventId >= OBJECT_EVENTS_COUNT || !OnFoot()
     || object == &gObjectEvents[gPlayerAvatar.objectEventId]
     || sFollower.mapGroup != gSaveBlock1Ptr->location.mapGroup
     || sFollower.mapNum != gSaveBlock1Ptr->location.mapNum
     || ScriptContext_IsEnabled() || ArePlayerFieldControlsLocked())
        return false;
    const struct ObjectEvent *player = &gObjectEvents[gPlayerAvatar.objectEventId];
    if (!player->active || player->spriteId >= MAX_SPRITES
     || gSprites[player->spriteId].invisible || gSprites[player->spriteId].y2 != 0)
        return false;
    if (object->currentElevation && sFollower.elevation
     && object->currentElevation != sFollower.elevation)
        return false;
    /* While between tiles, reserve both ends, just like a moving object event. */
    return x >= TileCoordinate(sFollower.position.x)
        && x <= TileCoordinate(sFollower.position.x + 15)
        && y >= TileCoordinate(sFollower.position.y)
        && y <= TileCoordinate(sFollower.position.y + 15);
}

static bool TileOccupied(s16 x, s16 y)
{
    for (unsigned i = 0; i < OBJECT_EVENTS_COUNT; ++i)
    {
        const struct ObjectEvent *object = &gObjectEvents[i];
        if (i != gPlayerAvatar.objectEventId && object->active
         && ((object->currentCoords.x == x && object->currentCoords.y == y)
          || (object->previousCoords.x == x && object->previousCoords.y == y)))
            return true;
    }
    return false;
}

static bool SeedBehindPlayer(const struct ObjectEvent *player, CtrFollowerPoint position)
{
    int facingX = 0, facingY = 1;
    switch (player->facingDirection)
    {
    case DIR_NORTH: facingY = -1; break;
    case DIR_WEST: facingX = -1; facingY = 0; break;
    case DIR_EAST: facingX = 1; facingY = 0; break;
    }
    const int offsets[4][2] = {
        {-facingX, -facingY}, {-facingY, facingX},
        {facingY, -facingX}, {facingX, facingY}
    };
    for (unsigned i = 0; i < ARRAY_COUNT(offsets); ++i)
    {
        s16 x = TileCoordinate(position.x) + offsets[i][0];
        s16 y = TileCoordinate(position.y) + offsets[i][1];
        u8 elevation = MapGridGetElevationAt(x, y);
        if (MapGridGetCollisionAt(x, y)
         || TileOccupied(x, y)
         || MetatileBehavior_IsSurfableWaterOrUnderwater(MapGridGetMetatileBehaviorAt(x, y))
         || (elevation && player->currentElevation && elevation != player->currentElevation))
            continue;
        CtrFollowerTrail_Seed(&sTrail, position, offsets[i][0], offsets[i][1]);
        /* Face the same way as the player before the first actual movement. */
        CtrFieldIcon_SetMotion(sFollower.spriteId, facingX, 0);
        return true;
    }
    return false;
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
    if (!OnFoot() || playerSprite->invisible || playerSprite->y2 != 0)
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
        sFollower.footPad = CtrFieldIcon_FootPad(spriteId);
        gSprites[spriteId].invisible = TRUE;
        /* Update runs after AnimateSprites; upload the initial frame before this OAM. */
        gSprites[spriteId].callback(&gSprites[spriteId]);
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
    /* World coordinates exclude camera scrolling and track walking/running speed. */
    s16 stepX = sTrail.count ? position.x - sTrail.last.x : 0;
    s16 stepY = sTrail.count ? position.y - sTrail.last.y : 0;
    bool hadFollowerPosition = sTrail.count == CTR_FOLLOWER_POINTS;
    CtrFollowerPoint previousFollower = sTrail.points[sTrail.next];
    if (!CtrFollowerTrail_Update(&sTrail, position, &follower))
    {
        if (!SeedBehindPlayer(player, position)
         || !CtrFollowerTrail_Update(&sTrail, position, &follower))
        {
            sprite->invisible = TRUE;
            return;
        }
        hadFollowerPosition = false;
        stepX = stepY = 0;
    }
    if (hadFollowerPosition)
    {
        stepX = follower.x - previousFollower.x;
        stepY = follower.y - previousFollower.y;
    }
    CtrFieldIcon_SetMotion(sFollower.spriteId, stepX, stepY);
    sFollower.position = follower;
    s16 x = TileCoordinate(follower.x), y = TileCoordinate(follower.y);
    s32 dx = follower.x - position.x, dy = follower.y - position.y;
    /* Let the player turn back freely; NPCs use the collision hook instead. */
    sprite->invisible = (dx > -8 && dx < 8 && dy > -8 && dy < 8);
    SetSpritePosToMapCoords(x, y, &tileX, &tileY);
    sprite->x = tileX + follower.x - x * 16 + 8;
    sprite->y = tileY + follower.y - y * 16;
    u8 elevation = MapGridGetElevationAt(x, y);
    if (!elevation || elevation == 15)
        elevation = player->previousElevation;
    sFollower.elevation = elevation;
    sprite->oam.priority = ElevationToPriority(elevation);
    /* The same feet/elevation/camera calculation as NPCs; lower on screen draws in front. */
    SetObjectSubpriorityByElevation(elevation, sprite, 1);
    /* Tile quantisation and the 256-pixel camera wrap must not invert two close feet. */
    if (sprite->oam.priority == playerSprite->oam.priority
     && dx > -32 && dx < 32 && dy > -32 && dy < 32)
    {
        if (dy > 0 && sprite->subpriority >= playerSprite->subpriority)
            sprite->subpriority = playerSprite->subpriority ? playerSprite->subpriority - 1 : 0;
        else if (dy < 0 && sprite->subpriority <= playerSprite->subpriority)
            sprite->subpriority = playerSprite->subpriority < 255 ? playerSprite->subpriority + 1 : 255;
    }
}
