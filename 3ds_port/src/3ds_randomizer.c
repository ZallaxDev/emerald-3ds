/*
 * The seeded species randomizer, the game's side (include/3ds_randomizer.h):
 * its settings, the permutation of the seed kept until the seed changes, and
 * the OPTIONS cells' functions (include/3ds_randomizer_extras.inc).
 *
 * Its settings are read straight from settings.txt (CtrSettings_GetInt), so
 * it works with or without its OPTIONS cells: all off unless set.
 */
#include "global.h"
#include "pokemon.h"
#include "characters.h"
#include "sound.h"
#include "constants/pokedex.h"
#include "constants/species.h"
#include "constants/songs.h"
#include "3ds_locale.h"
#include "3ds_extras.h"
#include "3ds_encounters.h"
#include "3ds_platform.h"
#include "3ds_randomizer.h"

/* The core's species ranges are Emerald's. */
_Static_assert(SPECIES_CELEBI == CTR_RANDOMIZER_SPECIES_LAST_JOHTO, "species layout");
_Static_assert(SPECIES_OLD_UNOWN_B == CTR_RANDOMIZER_SPECIES_LAST_JOHTO + 1, "species layout");
_Static_assert(SPECIES_OLD_UNOWN_Z + 1 == CTR_RANDOMIZER_SPECIES_FIRST_HOENN, "species layout");
_Static_assert(SPECIES_TREECKO == CTR_RANDOMIZER_SPECIES_FIRST_HOENN, "species layout");
_Static_assert(SPECIES_CHIMECHO == CTR_RANDOMIZER_SPECIES_LAST_HOENN, "species layout");
_Static_assert(SPECIES_EGG == CTR_RANDOMIZER_SPECIES_LAST_HOENN + 1, "species layout");
_Static_assert(NATIONAL_DEX_COUNT == CTR_RANDOMIZER_NATIONAL_COUNT, "national dex size");

static const char *const sKindKeys[CTR_RANDOMIZE_KINDS] = {"rand_wild", "rand_starters", "rand_trainers"};

const char *const gCtrRandomizerOpenText[1] = {CTR_TEXT("TAP TO OPEN", "TOCA PARA ABRIR", "OUVRIR", "APRI", "TOQUE P/ ABRIR", "タッチで ひらく")};
const char *const gCtrRandomizerBackText[1] = {CTR_TEXT("BACK TO CHEATS", "VOLVER A TRUCOS", "RETOUR", "INDIETRO", "VOLTAR", "チートに もどる")};
const char *const gCtrRandomizerNewSeedText[1] = {CTR_TEXT("TAP FOR NEW", "TOCA PARA NUEVA", "NOUVELLE", "NUOVO", "TOQUE P/ NOVA", "タッチで あたらしく")};

static CtrRandomizerTable sTable;
static bool sTableBuilt;

static u16 ToNational(u16 species)
{
    return SpeciesToNationalPokedexNum(species);
}

static u16 ToSpecies(u16 national)
{
    return NationalPokedexNumToSpecies(national);
}

bool CtrRandomizer_Enabled(unsigned kind)
{
    return kind < CTR_RANDOMIZE_KINDS && CtrSettings_GetInt(sKindKeys[kind], 0) == 1;
}

uint32_t CtrRandomizer_Seed(void)
{
    return (uint32_t)CtrSettings_GetInt("rand_seed", 0) & CTR_RANDOMIZER_SEED_MAX;
}

uint16_t CtrRandomizer_Species(unsigned kind, uint16_t species)
{
    uint32_t seed;

    if (!CtrRandomizer_Enabled(kind))
        return species;
    seed = CtrRandomizer_Seed();
    if (!sTableBuilt || sTable.seed != seed)
    {
        CtrRandomizer_Build(&sTable, seed);
        sTableBuilt = true;
        CtrLog_Write(CTR_LOG_GAME, "randomizer: seed %lu", (unsigned long)seed);
    }
    return CtrRandomizer_MapSpecies(&sTable, species, ToNational, ToSpecies);
}

/* Keep encounter previews consistent with the species met in the wild. */
u16 CtrEncounters_DisplaySpecies(u16 species)
{
    return CtrRandomizer_Species(CTR_RANDOMIZE_WILD, species);
}

/* The cells have a screen of their own (CHEATS 3), opened from the first. */
void CtrRandomizer_Open(void)
{
    CtrExtras_ShowScreen(CTR_EXTRAS_CHEATS, 2);
}

void CtrRandomizer_Back(void)
{
    CtrExtras_ShowScreen(CTR_EXTRAS_CHEATS, 0);
}

static void SetSeed(uint32_t seed)
{
    CtrSettings_SetInt("rand_seed", (int)(seed & CTR_RANDOMIZER_SEED_MAX));
}

void CtrRandomizer_StepSeed(int direction)
{
    uint32_t seed = CtrRandomizer_Seed();

    if (direction < 0)
        seed = seed == 0 ? CTR_RANDOMIZER_SEED_MAX : seed - 1;
    else
        seed = seed == CTR_RANDOMIZER_SEED_MAX ? 0 : seed + 1;
    SetSeed(seed);
}

/* The seed in decimal, in the game's characters. */
const uint8_t *CtrRandomizer_SeedText(void)
{
    static u8 text[11];
    u8 digits[10];
    uint32_t seed = CtrRandomizer_Seed();
    unsigned count = 0, i;

    do
    {
        digits[count++] = seed % 10;
        seed /= 10;
    } while (seed != 0 && count < ARRAY_COUNT(digits));
    for (i = 0; i < count; i++)
        text[i] = CHAR_0 + digits[count - 1 - i];
    text[count] = EOS;
    return text;
}

/* A new seed from the clock and the tick counter, not from the game's RNG. */
void CtrRandomizer_NewSeed(void)
{
    SetSeed(CtrRandomizer_MixSeed((uint64_t)CtrPlatform_WallSeconds(), CtrPlatform_Ticks()));
    PlaySE(SE_SUCCESS);
}
