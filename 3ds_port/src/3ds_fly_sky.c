/*
 * The Fly sky scene's supply from the game (3ds_fly_sky.h): its option, the
 * Pokemon's overworld sprites from the follower data (follower_mon.c), the
 * Pokemon doing the Fly (field_effect.c), the player seated as when surfing,
 * and the flying Pokemon of the wild tables of the maps flown from and to.
 * A game translation unit (PORT_BRIDGE).
 */
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "event_object_movement.h"
#include "field_effect.h"
#include "field_player_avatar.h"
#include "follower_mon.h"
#include "overworld.h"
#include "pokemon.h"
#include "sprite.h"
#include "wild_encounter.h"
#include "constants/event_object_movement.h"
#include "constants/map_groups.h"
#include "constants/maps.h"
#include "constants/pokemon.h"
#include "constants/species.h"
#include "constants/wild_encounter.h"

#include "port_platform.h"
#include "3ds_extras.h"
#include "3ds_log.h"
#include "3ds_fly_sky.h"

#define FMN_HEADER   8
#define FMN_PALETTES (2 * 16 * 2)
#define RIDER_SIZE   32
#define RIDER_BYTES  (RIDER_SIZE * RIDER_SIZE / 2)

bool CtrFlySky_Enabled(void)
{
    return CtrExtras_Get("skyflight") != 0;
}

bool CtrFlySky_MonSprite(uint16_t species, uint32_t personality, bool shiny, CtrSkySprite *out)
{
    const u8 *file;
    u32 frameBytes = FollowerMon_SkyFile(species, personality, &file);

    if (frameBytes == 0 || file == NULL)
        return false;
    out->tiles = file + FMN_HEADER + FMN_PALETTES;
    out->palette = (const u16 *)(file + FMN_HEADER) + (shiny ? 16 : 0);
    out->width = file[4];
    out->frames = CTR_SKY_FRAMES;
    out->frameBytes = frameBytes;
    return true;
}

bool CtrFlySky_FlyerSprite(CtrSkySprite *out, int8_t *seatX, int8_t *seatY)
{
    u16 species;
    u32 personality;
    bool8 shiny;
    s8 x, y;

    if (!FieldEffect_FlyMon(&species, &personality, &shiny))
        return false;
    if (!CtrFlySky_MonSprite(species, personality, shiny, out))
        return false;
    FollowerMon_SkySeat(out->tiles + CTR_SKY_FRAME_LEFT * out->frameBytes, out->width, &x, &y);
    *seatX = x;
    *seatY = y;
    return true;
}

/* The picture an anim of the graphics shows last (its seated pose). */
static bool LastFrame(const struct ObjectEventGraphicsInfo *info, u8 anim, u16 *image, bool *hFlip)
{
    const union AnimCmd *cmd;
    bool found = false;

    if (info->anims == NULL || (cmd = info->anims[anim]) == NULL)
        return false;
    for (; cmd->type >= 0; cmd++)
    {
        *image = cmd->frame.imageValue;
        *hFlip = cmd->frame.hFlip;
        found = true;
    }
    return found;
}

static u8 Pixel(const u8 *tiles, u32 x, u32 y)
{
    const u8 *tile = tiles + ((y / 8) * (RIDER_SIZE / 8) + x / 8) * 32;

    return (tile[(y & 7) * 4 + (x & 7) / 2] >> ((x & 1) * 4)) & 15;
}

static void PutPixel(u8 *tiles, u32 x, u32 y, u8 color)
{
    u8 *byte = tiles + ((y / 8) * (RIDER_SIZE / 8) + x / 8) * 32 + (y & 7) * 4 + (x & 7) / 2;

    *byte = (x & 1) ? (*byte & 0x0F) | (color << 4) : (*byte & 0xF0) | color;
}

/* The anim's last picture, copied (mirrored where it is), into `dest`. */
static bool CopyRiderFrame(const struct ObjectEventGraphicsInfo *info, u8 anim, bool mirror, u8 *dest)
{
    const struct SpriteFrameImage *frame;
    const u8 *src;
    u16 image;
    bool hFlip;
    u32 size, x, y;

    if (!LastFrame(info, anim, &image, &hFlip) || info->images == NULL)
        return false;
    frame = &info->images[image];
    size = Port_GetSpriteFrameSize(frame->data, frame->size);
    if (size < RIDER_BYTES)
        return false;
    src = Port_ResolveSpriteFramePointer(frame->data, RIDER_BYTES, frame->offset);
    if (src == NULL)
        return false;
    if (hFlip != mirror)
    {
        for (y = 0; y < RIDER_SIZE; y++)
            for (x = 0; x < RIDER_SIZE; x++)
                PutPixel(dest, x, y, Pixel(src, RIDER_SIZE - 1 - x, y));
    }
    else
    {
        memcpy(dest, src, RIDER_BYTES);
    }
    return true;
}

bool CtrFlySky_RiderSprite(CtrSkySprite *out)
{
    static u8 sTiles[2 * RIDER_BYTES];
    static u16 sPalette[16];
    const struct ObjectEventGraphicsInfo *info;
    const u16 *palette;

    info = GetObjectEventGraphicsInfo(GetPlayerAvatarGraphicsIdByStateId(PLAYER_AVATAR_STATE_SURFING));
    if (info == NULL || info->width != RIDER_SIZE || info->height != RIDER_SIZE)
        return false;
    palette = ObjectEventPaletteByTag(info->paletteTag);
    if (palette == NULL)
        return false;
    palette = Port_ResolveAssetPointerSized(palette, sizeof(sPalette));
    if (palette == NULL)
        return false;
    /* The surfing graphics seat the player facing west on its picture; the
     * east anim is the same picture flipped. */
    if (!CopyRiderFrame(info, ANIM_GET_ON_OFF_POKEMON_WEST, false, sTiles)
     || !CopyRiderFrame(info, ANIM_GET_ON_OFF_POKEMON_EAST, true, sTiles + RIDER_BYTES))
        return false;
    memcpy(sPalette, palette, sizeof(sPalette));
    out->tiles = sTiles;
    out->palette = sPalette;
    out->width = RIDER_SIZE;
    out->frames = 2;
    out->frameBytes = RIDER_BYTES;
    return true;
}

static bool IsFlying(u16 species)
{
    return species != SPECIES_NONE && species < NUM_SPECIES
        && (gSpeciesInfo[species].types[0] == TYPE_FLYING || gSpeciesInfo[species].types[1] == TYPE_FLYING);
}

static unsigned AddNatives(const struct WildPokemonInfo *info, u32 count, uint16_t *species, unsigned n, unsigned max)
{
    u32 i;
    unsigned j;

    if (info == NULL || info->wildPokemon == NULL)
        return n;
    for (i = 0; i < count && n < max; i++)
    {
        u16 s = info->wildPokemon[i].species;

        if (!IsFlying(s))
            continue;
        for (j = 0; j < n && species[j] != s; j++)
            ;
        if (j == n)
            species[n++] = s;
    }
    return n;
}

static unsigned AddMapNatives(u8 group, u8 num, uint16_t *species, unsigned n, unsigned max)
{
    u32 i;

    for (i = 0; gWildMonHeaders[i].mapGroup != MAP_GROUP(MAP_UNDEFINED); i++)
    {
        const struct WildPokemonHeader *h = &gWildMonHeaders[i];

        if (h->mapGroup != group || h->mapNum != num)
            continue;
        n = AddNatives(h->landMonsInfo, LAND_WILD_COUNT, species, n, max);
        n = AddNatives(h->waterMonsInfo, WATER_WILD_COUNT, species, n, max);
        n = AddNatives(h->rockSmashMonsInfo, ROCK_WILD_COUNT, species, n, max);
        n = AddNatives(h->fishingMonsInfo, FISH_WILD_COUNT, species, n, max);
    }
    return n;
}

/* The Fly's destination as group and number: the wild tables' maps matched
 * to the warp destination's header (overworld.c keeps the warp static). */
static bool DestinationMap(u8 *group, u8 *num)
{
    const struct MapHeader *dest = GetDestinationWarpMapHeader();
    u32 i;

    if (dest == NULL)
        return false;
    for (i = 0; gWildMonHeaders[i].mapGroup != MAP_GROUP(MAP_UNDEFINED); i++)
        if (Overworld_GetMapHeaderByGroupAndId(gWildMonHeaders[i].mapGroup, gWildMonHeaders[i].mapNum) == dest)
        {
            *group = gWildMonHeaders[i].mapGroup;
            *num = gWildMonHeaders[i].mapNum;
            return true;
        }
    return false;
}

unsigned CtrFlySky_Natives(uint16_t *species, unsigned max)
{
    static const u16 sFallback[] = {SPECIES_WINGULL, SPECIES_TAILLOW, SPECIES_SWABLU, SPECIES_PELIPPER};
    u8 fromGroup = gSaveBlock1Ptr->location.mapGroup, fromNum = gSaveBlock1Ptr->location.mapNum;
    u8 toGroup = 0xFF, toNum = 0xFF;
    unsigned n, i;
    char list[96];
    size_t used = 0;

    n = AddMapNatives(fromGroup, fromNum, species, 0, max);
    if (DestinationMap(&toGroup, &toNum))
        n = AddMapNatives(toGroup, toNum, species, n, max);
    if (n == 0)
        for (; n < max && n < ARRAY_COUNT(sFallback); n++)
            species[n] = sFallback[n];
    list[0] = '\0';
    for (i = 0; i < n && used + 8 < sizeof(list); i++)
        used += snprintf(list + used, sizeof(list) - used, " %u", (unsigned)species[i]);
    if (toGroup == 0xFF)
        CtrLog_Write(CTR_LOG_GAME, "fly sky: natives%s (from %u:%u, destination without wild table)", list,
                     fromGroup, fromNum);
    else
        CtrLog_Write(CTR_LOG_GAME, "fly sky: natives%s (from %u:%u to %u:%u)", list, fromGroup, fromNum,
                     toGroup, toNum);
    return n;
}

void CtrFlySky_Release(void)
{
    FollowerMon_SkyRelease();
}
