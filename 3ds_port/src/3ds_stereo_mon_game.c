/*
 * The stereoscopic Pokémon planes, game side (3ds_stereo_mon.h): the
 * battle's positions named for the compositor each frame, with the
 * pictures the game itself decoded, and CHEATS' 3D WILD ONLY.
 */
#include <string.h>

#include "global.h"
#include "main.h"
#include "data.h"
#include "pokemon.h"
#include "battle.h"
#include "battle_anim.h"
#include "battle_setup.h"
#include "graphics.h"
#include "random.h"
#include "wild_encounter.h"
#include "constants/map_groups.h"
#include "constants/wild_encounter.h"
#include "constants/map_types.h"
#include "constants/battle.h"
#include "constants/trainers.h"
#include "constants/opponents.h"
#include "constants/characters.h"
#include "constants/species.h"

#include "port_platform.h"
#include "3ds_platform.h"
#include "3ds_extras.h"
#include "3ds_log.h"
#include "3ds_video.h"
#include "3ds_stereo_mon.h"

static void BattleFrame(void);

#define FRAME_BYTES (CTR_STEREO_MON_DIM * CTR_STEREO_MON_DIM / 2)


/* LZ77 data of a game symbol (an asset stub, resolved from the game data)
 * into a buffer of `capacity` bytes; the size comes from the stream's own
 * header. As 3ds_bottom_ui.c's Unlz, without the heap: the game's is nearly
 * full by the time the field is up. */
static u32 Unlz(const void *src, void *dst, u32 capacity)
{
    const u8 *res = src ? Port_ResolveAssetPointer(src) : NULL;
    u32 header, size;

    if (!res)
        return 0;
    header = res[0] | (res[1] << 8) | (res[2] << 16) | ((u32)res[3] << 24);
    size = header >> 8;
    if ((header & 0xFF) != 0x10 || size == 0 || size > capacity)
        return 0;
    LZ77UnCompWram((const u32 *)res, dst);
    return size;
}

/* The picture an id names (CTR_STEREO_ID_*): a species' front (Castform's
 * has four frames, every other one or two; the battle takes two) or back,
 * a trainer's front, or a player's back (an uncompressed sheet of up to
 * five frames), as the game's 4bpp tiles (`*tilesOut`, valid until the
 * next call) and its palette. Returns its frames, 0 without a picture. */
static unsigned LoadPicture(u16 id, const u8 **tilesOut, u16 *pal)
{
    static u8 tiles[CTR_STEREO_MON_MAX_FRAMES * FRAME_BYTES];
    unsigned kind = id & 0xC000, number = id & 0x3FFF, frames = 0;
    const void *tilesLz = NULL, *palLz = NULL;
    u32 size;

    if (kind == 0 || kind == CTR_STEREO_ID_BACK)
    {
        u16 species = NationalPokedexNumToSpecies(number);

        if (species == SPECIES_NONE) return 0;
        tilesLz = kind == 0 ? gMonFrontPicTable[species].data : gMonBackPicTable[species].data;
        palLz = gMonPaletteTable[species].data;
    }
    else if (kind == CTR_STEREO_ID_TRAINER)
    {
        tilesLz = gTrainerFrontPicTable[number].data;
        palLz = gTrainerFrontPicPaletteTable[number].data;
    }
    else
    {
        /* The players' backs are raw sheets. */
        const u8 *sheet = Port_ResolveAssetPointer(gTrainerBackPicTable[number].data);

        size = gTrainerBackPicTable[number].size;
        if (sheet == NULL || size < FRAME_BYTES || Unlz(gTrainerBackPicPaletteTable[number].data, pal, 32) != 32)
            return 0;
        frames = size / FRAME_BYTES;
        if (frames > CTR_STEREO_MON_MAX_FRAMES) frames = CTR_STEREO_MON_MAX_FRAMES;
        *tilesOut = sheet;
        return frames;
    }
    size = Unlz(tilesLz, tiles, sizeof(tiles));
    if (size >= FRAME_BYTES && Unlz(palLz, pal, 32) == 32)
    {
        frames = size >= 2 * FRAME_BYTES ? 2 : 1;
        *tilesOut = tiles;
    }
    else
        CtrLog_Write(CTR_LOG_ERROR, "stereo: id %04x has no picture (%lu bytes)", id, (unsigned long)size);
    return frames;
}

void CtrStereoMonGame_Frame(void)
{
    BattleFrame();
}

/*
 * The battle's four positions for the compositor (CtrStereoBattle_SetSlot):
 * each one's sprite tile, its picture buffer (gMonSpritesGfxPtr: the
 * Pokémon's front or back as the game decoded it, or the trainer's front
 * during the intro), the ids of the planes the buffer could be showing -
 * the Pokémon's (Transformed, what it turned into; the party's otherwise)
 * and, on the opponent's side in a trainer battle, the trainer's - and on
 * the player's side the raw sheet of the player's back with its id. A
 * position without a sprite is cleared.
 */
static u16 MonIdOf(u8 battler, bool8 player)
{
    u16 species = gBattleSpritesDataPtr->battlerData[battler].transformSpecies;
    u16 national;

    if (species == SPECIES_NONE && gBattlerPartyIndexes[battler] < PARTY_SIZE)
        species = GetMonData(player ? &gPlayerParty[gBattlerPartyIndexes[battler]]
                                    : &gEnemyParty[gBattlerPartyIndexes[battler]], MON_DATA_SPECIES);
    if (species == SPECIES_NONE || species >= NUM_SPECIES) return 0;
    national = SpeciesToNationalPokedexNum(species);
    if (player) return CtrStereoMon_HasBack(CTR_STEREO_ID_BACK | national) ? (CTR_STEREO_ID_BACK | national) : 0;
    return CtrStereoMon_Has(national) ? national : 0;
}

static u16 TrainerIdOf(unsigned position)
{
    u16 trainer, pic;

    if (!(gBattleTypeFlags & BATTLE_TYPE_TRAINER) || (gBattleTypeFlags & (BATTLE_TYPE_LINK | BATTLE_TYPE_FRONTIER)))
        return 0;
    trainer = position == B_POSITION_OPPONENT_RIGHT && (gBattleTypeFlags & BATTLE_TYPE_TWO_OPPONENTS)
            ? gTrainerBattleOpponent_B : gTrainerBattleOpponent_A;
    if (trainer == 0 || trainer >= TRAINERS_COUNT) return 0;
    pic = gTrainers[trainer].trainerPic;
    return CtrStereoMon_HasBack(CTR_STEREO_ID_TRAINER | pic) ? (CTR_STEREO_ID_TRAINER | pic) : 0;
}

/* The first frame of an id's own picture for a position's candidate
 * (0 the Pokémon, 1 the trainer): the compositor tells by it which of the
 * two the position's buffer holds, the planes having no outline. Decoded
 * once per id; NULL without one. */
static const u8 *ReferenceOf(unsigned position, unsigned which, u16 id)
{
    static u8 sReference[CTR_STEREO_SLOTS][2][FRAME_BYTES];
    static u16 sReferenceId[CTR_STEREO_SLOTS][2];
    static bool8 sReferenceOk[CTR_STEREO_SLOTS][2];

    if (id == 0) return NULL;
    if (sReferenceId[position][which] != id)
    {
        const u8 *tiles = NULL;
        u16 pal[16];

        sReferenceId[position][which] = id;
        sReferenceOk[position][which] = LoadPicture(id, &tiles, pal) != 0;
        if (sReferenceOk[position][which]) memcpy(sReference[position][which], tiles, FRAME_BYTES);
    }
    return sReferenceOk[position][which] ? sReference[position][which] : NULL;
}

static void BattleFrame(void)
{
    if (!gMain.inBattle || gMonSpritesGfxPtr == NULL || gBattleSpritesDataPtr == NULL
        || gBattleSpritesDataPtr->battlerData == NULL)
    {
        CtrStereoBattle_ClearBattlers();
        return;
    }
    for (unsigned position = 0; position < CTR_STEREO_SLOTS; ++position)
    {
        u8 battler = GetBattlerAtPosition(position);
        bool8 player = position == B_POSITION_PLAYER_LEFT || position == B_POSITION_PLAYER_RIGHT;
        bool8 right = position == B_POSITION_PLAYER_RIGHT || position == B_POSITION_OPPONENT_RIGHT;
        u8 spriteId;
        const u8 *backSheet = NULL;
        unsigned backFrames = 0;
        u16 playerId = 0, monId, trainerId;

        if (battler >= gBattlersCount || (right && !(gBattleTypeFlags & BATTLE_TYPE_DOUBLE)))
        {
            CtrStereoBattle_SetSlot(position, 0, NULL, 0, 0, 0, NULL, NULL, 0, NULL, 0);
            continue;
        }
        spriteId = gBattlerSpriteIds[battler];
        if (spriteId >= MAX_SPRITES || !gSprites[spriteId].inUse)
        {
            CtrStereoBattle_SetSlot(position, 0, NULL, 0, 0, 0, NULL, NULL, 0, NULL, 0);
            continue;
        }
        if (player)
        {
            u16 backPic = position == B_POSITION_PLAYER_RIGHT ? TRAINER_BACK_PIC_STEVEN
                        : gSaveBlock2Ptr->playerGender + TRAINER_BACK_PIC_BRENDAN;

            if (CtrStereoMon_HasBack(CTR_STEREO_ID_PLAYER | backPic))
            {
                backSheet = Port_ResolveAssetPointer(gTrainerBackPicTable[backPic].data);
                backFrames = gTrainerBackPicTable[backPic].size / CTR_STEREO_BATTLE_FRAME_BYTES;
                playerId = CTR_STEREO_ID_PLAYER | backPic;
            }
        }
        monId = MonIdOf(battler, player);
        trainerId = player ? 0 : TrainerIdOf(position);
        CtrStereoBattle_SetSlot(position, gSprites[spriteId].oam.tileNum, gMonSpritesGfxPtr->sprites.ptr[position],
                                CTR_STEREO_BUFFER_FRAMES, monId, trainerId, ReferenceOf(position, 0, monId),
                                ReferenceOf(position, 1, trainerId), playerId, backSheet, backFrames);
    }
}

uint16_t CtrStereoMonGame_WildSpecies(uint16_t species)
{
    unsigned count;
    u16 pick;

    if (!CtrExtras_Get("wild_3d") || !CtrStereoMon_Load()) return species;
    count = CtrStereoMon_Count();
    if (count == 0 || CtrStereoMon_Has(SpeciesToNationalPokedexNum(species))) return species;
    pick = NationalPokedexNumToSpecies(CtrStereoMon_NationalAt(Random() % count));
    return pick != SPECIES_NONE ? pick : species;
}
