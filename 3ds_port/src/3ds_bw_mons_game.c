/*
 * The battlers' Black 2 / White 2 pictures, game side (3ds_bw_mons.h): which
 * file each battler is drawn from, where its feet are, and what its sprite
 * is doing this frame.
 *
 * Files are looked up as the battler's graphics are loaded, never during a
 * frame: CTR_BW_DIR front|back / NNN[form][_f][_s].bin, the national number
 * on three digits, the form as Bulbagarden names it (Unown B-Z, EX, QU;
 * Castform S, R, H; Deoxys S), _f a female's own picture, _s a shiny one. A
 * female without her own picture takes the common one; a form without its
 * own, the plain species'. With none at all, the game's sprite is shown.
 */
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "main.h"
#include "battle.h"
#include "battle_anim.h"
#include "pokemon.h"
#include "pokemon_icon.h"
#include "sprite.h"
#include "contest.h"
#include "constants/battle.h"
#include "constants/species.h"

#include "3ds_bw_mons.h"
#include "3ds_extras.h"

static CtrBwBattler sBattlers[CTR_BW_BATTLERS];
/* The sprite id each battler's Pokémon was last seen as (CtrBwMons_Battlers),
 * or MAX_SPRITES. */
static u8 sConfirmed[CTR_BW_BATTLERS] = {MAX_SPRITES, MAX_SPRITES, MAX_SPRITES, MAX_SPRITES};
static uint32_t sVersion;
/* Evolution scene slots (CtrBwMons_Scene): their sprite and its position's
 * template, in place of a battler's. */
static bool sScene[CTR_BW_BATTLERS];
static u8 sSceneSprite[CTR_BW_BATTLERS];
static u8 sScenePosition[CTR_BW_BATTLERS];

/* The form's part of a file name. */
static const char *FormSuffix(u16 species, u8 form)
{
    static const char *const unown[28] = {
        "", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N",
        "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "EX", "QU",
    };
    /* CASTFORM_NORMAL, _FIRE (sunny), _WATER (rainy), _ICE (snowy). */
    static const char *const castform[4] = {"", "S", "R", "H"};

    if (species == SPECIES_UNOWN) return form < 28 ? unown[form] : "";
    if (species == SPECIES_CASTFORM) return form < 4 ? castform[form] : "";
    if (species == SPECIES_DEOXYS) return form ? "S" : "";
    return "";
}

static bool Find(char *path, size_t size, const char *side, unsigned dex, const char *form, bool female, bool shiny)
{
    const char *shinyPart = shiny ? "_s" : "";

    if (female)
    {
        snprintf(path, size, CTR_BW_DIR "%s/%03u%s_f%s.bin", side, dex, form, shinyPart);
        if (CtrBwPic_Exists(path)) return true;
    }
    snprintf(path, size, CTR_BW_DIR "%s/%03u%s%s.bin", side, dex, form, shinyPart);
    if (CtrBwPic_Exists(path)) return true;
    if (form[0])
        return Find(path, size, side, dex, "", female, shiny);
    return false;
}

/* The game's own picture: its lowest opaque row and the middle of its
 * opaque columns, from the 4bpp tiles (8x8 of them, a row of tiles at a
 * time) gMonSpritesGfxPtr holds. */
static void MeasurePicture(const u8 *pic, int8_t *feetY, int8_t *centreX2, int8_t *topY)
{
    int bottom = -1, left = 64, right = -1, top = 64;

    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
        {
            const u8 *tile = pic + ((y / 8) * 8 + x / 8) * 32;
            u8 byte = tile[(y % 8) * 4 + (x % 8) / 2];
            u8 index = (x & 1) ? byte >> 4 : byte & 15;

            if (!index) continue;
            if (y > bottom) bottom = y;
            if (y < top) top = y;
            if (x < left) left = x;
            if (x > right) right = x;
        }
    if (bottom < 0)
    {
        *feetY = 56;
        *centreX2 = 64;
        *topY = 8;
        return;
    }
    *topY = (int8_t)top;
    *feetY = (int8_t)bottom;
    *centreX2 = (int8_t)(left + right + 1);
}

void CtrBwMons_Loaded(unsigned char battler, unsigned short species, unsigned char form,
                      bool female, bool shiny, bool back, bool transformed)
{
    CtrBwBattler *b;
    unsigned dex;
    u8 position;
    const u8 *pic;

    if (battler >= CTR_BW_BATTLERS) return;
    b = &sBattlers[battler];
    b->active = false;
    b->clipRows = 64;
    b->copyBg = 0;
    b->transformed = transformed;
    ++sVersion;
    b->version = sVersion;
    dex = species && species < NUM_SPECIES ? SpeciesToNationalPokedexNum(species) : 0;
    if (!dex || dex > 386) return;
    if (!Find(b->path, sizeof(b->path), back ? "back" : "front", dex, FormSuffix(species, form), female, shiny))
        return;
    sScene[battler] = false;
    position = GetBattlerPosition(battler);
    pic = gMonSpritesGfxPtr ? gMonSpritesGfxPtr->sprites.byte[position] : NULL;
    if (pic && species == SPECIES_CASTFORM) pic += (form & 3) * MON_PIC_SIZE;
    if (pic)
        MeasurePicture(pic, &b->feetY, &b->centreX2, &b->topY);
    else
    {
        b->feetY = 56;
        b->centreX2 = 64;
        b->topY = 8;
    }
    b->back = back;
    b->active = true;
    /* Read now, on the reader thread: the send-out's ball takes longer than
     * the card, so the picture is there when the Pokémon comes out. */
    CtrBwPic_Request(battler, b->path);
}

void CtrBwMons_Scene(unsigned char slot, unsigned short species, unsigned int personality,
                     unsigned int otId, unsigned char spriteId, unsigned char position)
{
    CtrBwBattler *b;
    unsigned dex;
    const char *form = "";

    if (slot >= CTR_BW_BATTLERS || position >= MAX_BATTLERS_COUNT) return;
    /* The battle's own are over: their sprites are gone with it. */
    for (unsigned i = 0; i < CTR_BW_BATTLERS; ++i)
        if (!sScene[i]) CtrBwMons_Clear((unsigned char)i);
    b = &sBattlers[slot];
    b->active = false;
    b->clipRows = 64;
    b->copyBg = 0;
    b->transformed = false;
    b->paused = false;
    b->version = ++sVersion;
    sScene[slot] = true;
    sSceneSprite[slot] = spriteId;
    sScenePosition[slot] = position;
    dex = species && species < NUM_SPECIES ? SpeciesToNationalPokedexNum(species) : 0;
    if (!dex || dex > 386) return;
    if (species == SPECIES_UNOWN) form = FormSuffix(species, GetUnownLetterByPersonality(personality));
    if (!Find(b->path, sizeof(b->path), "front", dex, form,
              GetGenderFromSpeciesAndPersonality(species, personality) == MON_FEMALE,
              IsShinyOtIdPersonality(otId, personality)))
        return;
    if (gMonSpritesGfxPtr)
        MeasurePicture(gMonSpritesGfxPtr->sprites.byte[position], &b->feetY, &b->centreX2, &b->topY);
    else
    {
        b->feetY = 56;
        b->centreX2 = 64;
        b->topY = 8;
    }
    b->back = false;
    b->active = true;
    CtrBwPic_Request(slot, b->path);
}

/* Whether the sprite is still the one made from the battler template of
 * `position`: a sprite id another screen reused (the nickname screen, the
 * evolution scene after a battle) is not. */
static bool FromTemplate(const struct Sprite *sprite, u8 position)
{
    return sprite->inUse && gMonSpritesGfxPtr && position < MAX_BATTLERS_COUNT
        && sprite->images == gMonSpritesGfxPtr->templates[position].images;
}

void CtrBwMons_Clear(unsigned char battler)
{
    if (battler >= CTR_BW_BATTLERS) return;
    sScene[battler] = false;
    sBattlers[battler].active = false;
    sBattlers[battler].version = ++sVersion;
}

void CtrBwMons_Faint(unsigned char battler, int rows)
{
    if (battler >= CTR_BW_BATTLERS) return;
    sBattlers[battler].clipRows = (uint8_t)(rows < 0 ? 0 : rows > 8 ? 64 : rows * 8);
}

/* The option (OPTIONS, second screen): on unless the player turned it off. */
static bool Enabled(void)
{
    return CtrExtras_Get("bw_sprites") != 0;
}

bool CtrBwMons_Shown(unsigned char battler)
{
    return battler < CTR_BW_BATTLERS && sBattlers[battler].active && gMain.inBattle && !IsContest() && Enabled();
}

void CtrBwMons_OnBg(unsigned char battler, unsigned char bg, unsigned char palette)
{
    if (battler >= CTR_BW_BATTLERS) return;
    for (unsigned i = 0; i < CTR_BW_BATTLERS; ++i)
        if (sBattlers[i].copyBg == bg) sBattlers[i].copyBg = 0;
    sBattlers[battler].copyBg = bg;
    sBattlers[battler].copyPalette = palette;
}

void CtrBwMons_OffBg(unsigned char bg)
{
    for (unsigned i = 0; i < CTR_BW_BATTLERS; ++i)
        if (sBattlers[i].copyBg == bg) sBattlers[i].copyBg = 0;
}

void CtrBwMons_Reset(void)
{
    for (unsigned i = 0; i < CTR_BW_BATTLERS; ++i)
        CtrBwMons_Clear((unsigned char)i);
}

const CtrBwBattler *CtrBwMons_Battlers(void)
{
    if (!Enabled()) return NULL;
    for (unsigned i = 0; i < CTR_BW_BATTLERS; ++i)
    {
        CtrBwBattler *b = &sBattlers[i];
        const struct Sprite *sprite;

        b->tile = 0xFFFF;
        b->spriteHidden = true;
        if (sScene[i])
        {
            if (!b->active || sSceneSprite[i] >= MAX_SPRITES) continue;
            sprite = &gSprites[sSceneSprite[i]];
            if (!FromTemplate(sprite, sScenePosition[i])) continue;
            b->tile = sprite->oam.tileNum;
            b->spriteHidden = sprite->invisible;
            continue;
        }
        if (!gMain.inBattle)
        {
            b->copyBg = 0;
            sConfirmed[i] = MAX_SPRITES;
        }
        if (!b->active || !gMain.inBattle || i >= gBattlersCount || gBattlerSpriteIds[i] >= MAX_SPRITES)
            continue;
        sprite = &gSprites[gBattlerSpriteIds[i]];
        /* The battler's Pokémon, not its trainer (the intro, a trainer
         * sliding back in), which takes the same sprite id: the controllers
         * give a Pokémon the battler's palette and number (data[0]). Its
         * front animation then borrows data[0] (pokemon_animation.c), so a
         * sprite once seen so stays the Pokémon until it is gone. */
        if (!FromTemplate(sprite, GetBattlerPosition(i)))
        {
            sConfirmed[i] = MAX_SPRITES;
            continue;
        }
        if (sConfirmed[i] != gBattlerSpriteIds[i])
        {
            if (sprite->oam.paletteNum != i || sprite->data[0] != (s16)i) continue;
            sConfirmed[i] = gBattlerSpriteIds[i];
        }
        b->tile = sprite->oam.tileNum;
        b->spriteHidden = sprite->invisible;
        b->paused = (gBattleMons[i].status1 & (STATUS1_SLEEP | STATUS1_FREEZE)) != 0;
    }
    return sBattlers;
}
