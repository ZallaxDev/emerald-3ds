#include "global.h"
#include "data.h"
#include "pokemon_icon.h"
#include "pokemon.h"
#include "palette.h"
#include "port_platform.h"
#include "sprite.h"
#include "constants/pokemon_icon.h"
#include "3ds_field_icon.h"
#include "3ds_field_icon_palette.h"

#define ICON_BYTES 0x200
#define ICON_FRAME 14
/* The game uses base + 0..5 for shared icon palettes. Each shiny species owns a tag. */
#define SHINY_ICON_BASE_TAG (POKE_ICON_BASE_PAL_TAG + 6)
#define BATTLE_PALETTE_COLORS 64 /* Castform stores all four forms in one compressed asset. */

static void AnimateFieldIcon(struct Sprite *sprite)
{
    if (sprite->data[6] > 0)
    {
        --sprite->data[6];
        return;
    }
    /* Copy now: the streamed asset cache may change before a queued VBlank copy. */
    const u8 *icon = Port_ResolveAssetPointer(sprite->images);
    if (icon != NULL)
        CpuCopy16(icon + ICON_BYTES * sprite->data[5],
                  VRAM_ + 0x10000 + sprite->oam.tileNum * TILE_SIZE_4BPP, ICON_BYTES);
    sprite->data[5] ^= 1;
    sprite->data[6] = sprite->data[4];
}

static void ReleaseUnusedPalette(u16 species, u16 tag)
{
    u8 slot = IndexOfSpritePaletteTag(tag);
    if (slot == 0xFF)
        return;
    /* Followers, wild icons and field effects must not free each other's colour slot. */
    for (unsigned i = 0; i < MAX_SPRITES; ++i)
        if (gSprites[i].inUse && gSprites[i].oam.paletteNum == slot)
            return;
    if (tag >= SHINY_ICON_BASE_TAG)
        FreeSpritePaletteByTag(tag);
    else
        FreeMonIconPalette(species);
}

static bool LoadBattlePalette(const u32 *data, u16 palette[BATTLE_PALETTE_COLORS])
{
    const u8 *header = Port_ResolveAssetPointerSized(data, 4);
    if (header == NULL || header[0] != 0x10)
        return false;
    unsigned bytes = header[1] | (header[2] << 8) | (header[3] << 16);
    if (bytes < 32 || bytes > BATTLE_PALETTE_COLORS * sizeof(u16) || (bytes & 1))
        return false;
    LZ77UnCompWram(data, palette);
    return true;
}

static bool LoadShinyPalette(u16 species, u16 tag)
{
    if (IndexOfSpritePaletteTag(tag) != 0xFF)
        return true;
    u16 icon[16], result[16];
    u16 normal[BATTLE_PALETTE_COLORS], shiny[BATTLE_PALETTE_COLORS];
    const void *data = Port_ResolveAssetPointerSized(gMonIconPaletteTable[gMonIconPaletteIndices[species]].data, sizeof(icon));
    if (data == NULL)
        return false;
    /* Copy each streamed palette before resolving another asset may evict it. */
    CpuCopy16(data, icon, sizeof(icon));
    if (!LoadBattlePalette(gMonPaletteTable[species].data, normal)
     || !LoadBattlePalette(gMonShinyPaletteTable[species].data, shiny))
        return false;
    CtrFieldIcon_RecolorPalette(icon, normal, shiny, result);
    const struct SpritePalette palette = {result, tag};
    LoadSpritePalette(&palette);
    return IndexOfSpritePaletteTag(tag) != 0xFF;
}

u8 CtrFieldIcon_Create(u16 species, u32 personality, s16 marker, bool shiny)
{
    unsigned freeSlot;
    for (freeSlot = 0; freeSlot < MAX_SPRITES && gSprites[freeSlot].inUse; ++freeSlot)
        ;
    /* CreateMonIcon updates the returned sprite without checking allocation failure. */
    if (freeSlot == MAX_SPRITES || species == SPECIES_NONE || species >= NUM_SPECIES)
        return MAX_SPRITES;
    u16 tag = shiny ? SHINY_ICON_BASE_TAG + species
             : POKE_ICON_BASE_PAL_TAG + gMonIconPaletteIndices[species];
    if (tag >= SHINY_ICON_BASE_TAG)
    {
        if (!LoadShinyPalette(species, tag))
            return MAX_SPRITES;
    }
    else if (IndexOfSpritePaletteTag(tag) == 0xFF)
        LoadMonIconPalette(species);
    if (IndexOfSpritePaletteTag(tag) == 0xFF)
        return MAX_SPRITES;
    u8 id = CreateMonIcon(species, AnimateFieldIcon, 0, 0, 1, personality, TRUE);
    if (id == MAX_SPRITES)
    {
        ReleaseUnusedPalette(species, tag);
        return id;
    }
    gSprites[id].coordOffsetEnabled = TRUE;
    /* CreateMonIcon selects the normal tag; bind this instance to its own colours. */
    gSprites[id].oam.paletteNum = IndexOfSpritePaletteTag(tag);
    gSprites[id].data[3] = tag;
    gSprites[id].data[7] = marker;
    gSprites[id].data[4] = ICON_FRAME;
    gSprites[id].data[5] = 0;
    gSprites[id].data[6] = 0;
    return id;
}

void CtrFieldIcon_SetMotion(u8 spriteId, s16 dx, s16 dy)
{
    if (spriteId >= MAX_SPRITES || !gSprites[spriteId].inUse
     || gSprites[spriteId].callback != AnimateFieldIcon)
        return;
    struct Sprite *sprite = &gSprites[spriteId];
    unsigned speed = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
    if (speed < 1) speed = 1;
    if (speed > 4) speed = 4;
    /* The countdown includes its zero frame: 15 walking frames, 8 running frames. */
    s16 delay = (ICON_FRAME + speed) / speed - 1;
    sprite->data[4] = delay;
    if (sprite->data[6] > delay)
        sprite->data[6] = delay;
    if (dx != 0)
    {
        sprite->hFlip = dx > 0;
        /* Both the 2D OAM and voxel atlas read this bit; preserve vertical flip. */
        sprite->oam.matrixNum = (sprite->oam.matrixNum & ~0x08) | (sprite->hFlip ? 0x08 : 0);
    }
}

void CtrFieldIcon_Destroy(u8 spriteId, u16 species)
{
    if (spriteId >= MAX_SPRITES || !gSprites[spriteId].inUse
     || gSprites[spriteId].callback != AnimateFieldIcon)
        return;
    u16 tag = (u16)gSprites[spriteId].data[3];
    FreeAndDestroyMonIconSprite(&gSprites[spriteId]);
    ReleaseUnusedPalette(species, tag);
}

int CtrFieldIcon_FootPad(u8 spriteId)
{
    if (spriteId >= MAX_SPRITES || !gSprites[spriteId].inUse
     || gSprites[spriteId].callback != AnimateFieldIcon)
        return 0;
    const u8 *tiles = Port_ResolveAssetPointer(gSprites[spriteId].images);
    if (tiles == NULL)
        return 0;
    /* Frame zero is the standing pose; measuring every bob would cancel the animation. */
    for (int y = 31; y >= 0; --y)
        for (int tx = 0; tx < 4; ++tx)
        {
            const u8 *row = tiles + ((y / 8) * 4 + tx) * TILE_SIZE_4BPP + (y % 8) * 4;
            if (row[0] | row[1] | row[2] | row[3])
                return 31 - y;
        }
    return 0;
}
