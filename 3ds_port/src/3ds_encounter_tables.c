#include "global.h"
#include "wild_encounter.h"
#include "constants/maps.h"
#include "constants/species.h"
#include "3ds_platform.h"
#include "3ds_daynight.h"
#include "3ds_encounters.h"
#include "3ds_extras.h"

#define RULE(route, slotIndex, expected, morning, day, night) \
    {MAP_GROUP(MAP_##route), MAP_NUM(MAP_##route), slotIndex, SPECIES_##expected, \
     {SPECIES_##morning, SPECIES_##day, SPECIES_##night}}

static const CtrTimedEncounterRule sRules[] = {
    RULE(ROUTE101, 2, WURMPLE, TAILLOW, WURMPLE, POOCHYENA),
    RULE(ROUTE101, 6, WURMPLE, WURMPLE, ZIGZAGOON, POOCHYENA),
    RULE(ROUTE102, 3, WURMPLE, TAILLOW, WURMPLE, POOCHYENA),
    RULE(ROUTE102, 5, LOTAD, LOTAD, LOTAD, ODDISH),
    RULE(ROUTE103, 2, POOCHYENA, WINGULL, ZIGZAGOON, POOCHYENA),
    RULE(ROUTE104, 5, POOCHYENA, TAILLOW, WINGULL, POOCHYENA),
    RULE(ROUTE104, 4, MARILL, WURMPLE, MARILL, ZUBAT),
    RULE(ROUTE110, 3, ELECTRIKE, ELECTRIKE, MINUN, ODDISH),
    RULE(ROUTE110, 7, GULPIN, GULPIN, GULPIN, ZUBAT),
    RULE(ROUTE116, 4, NINCADA, TAILLOW, NINCADA, POOCHYENA),
    RULE(ROUTE116, 7, TAILLOW, TAILLOW, TAILLOW, ZUBAT),
    RULE(ROUTE117, 3, ODDISH, MARILL, ODDISH, VOLBEAT),
    RULE(ROUTE117, 7, ILLUMISE, ILLUMISE, ROSELIA, ILLUMISE),
    RULE(ROUTE119, 2, ZIGZAGOON, ZIGZAGOON, LINOONE, ODDISH),
    RULE(ROUTE119, 7, ODDISH, TROPIUS, ODDISH, GLOOM),
    RULE(ROUTE120, 6, ODDISH, MARILL, ODDISH, MIGHTYENA),
    RULE(ROUTE121, 3, SHUPPET, ODDISH, GLOOM, SHUPPET),
    RULE(ROUTE121, 9, WINGULL, WINGULL, WINGULL, DUSKULL),
    RULE(ROUTE123, 3, SHUPPET, ODDISH, GLOOM, SHUPPET),
    RULE(ROUTE123, 9, WINGULL, WINGULL, WINGULL, DUSKULL),
};

/*
 * RUBY/SAPPHIRE POKEMON (OPTIONS > POKEDEX, on by default): the seven Hoenn
 * Pokemon Ruby and Sapphire have in the wild and Emerald does not (pret's
 * pokeruby, src/data/wild_encounters.json), on the maps and the slots where
 * those games have them, at that slot's level. A slot is taken only where the
 * species it held stays on that table (another of its slots). Surskit, whose
 * Ruby/Sapphire slots are the only Seedot and surfing Goldeen ones, is on the
 * fourth surfing slot (4%) of the routes it surfs on there, taken from Marill.
 */
#define RS(map, water, slotIndex, from, to) \
    {MAP_GROUP(MAP_##map), MAP_NUM(MAP_##map), water, slotIndex, SPECIES_##from, SPECIES_##to}

typedef struct
{
    uint8_t mapGroup, mapNum, water, slot;
    uint16_t expectedSpecies, species;
} CtrRsEncounterRule;

static const CtrRsEncounterRule sRsRules[] = {
    RS(METEOR_FALLS_1F_1R, 0, 5, SOLROCK, LUNATONE),
    RS(METEOR_FALLS_1F_1R, 0, 7, SOLROCK, LUNATONE),
    RS(METEOR_FALLS_1F_1R, 1, 2, SOLROCK, LUNATONE),
    RS(METEOR_FALLS_1F_1R, 1, 4, SOLROCK, LUNATONE),
    RS(METEOR_FALLS_1F_2R, 0, 3, SOLROCK, LUNATONE),
    RS(METEOR_FALLS_1F_2R, 0, 5, SOLROCK, LUNATONE),
    RS(METEOR_FALLS_1F_2R, 1, 2, SOLROCK, LUNATONE),
    RS(METEOR_FALLS_1F_2R, 1, 4, SOLROCK, LUNATONE),
    RS(METEOR_FALLS_B1F_1R, 0, 3, SOLROCK, LUNATONE),
    RS(METEOR_FALLS_B1F_1R, 0, 5, SOLROCK, LUNATONE),
    RS(METEOR_FALLS_B1F_1R, 1, 2, SOLROCK, LUNATONE),
    RS(METEOR_FALLS_B1F_1R, 1, 4, SOLROCK, LUNATONE),
    RS(METEOR_FALLS_B1F_2R, 0, 3, SOLROCK, LUNATONE),
    RS(METEOR_FALLS_B1F_2R, 0, 7, SOLROCK, LUNATONE),
    RS(METEOR_FALLS_B1F_2R, 1, 2, SOLROCK, LUNATONE),
    RS(METEOR_FALLS_B1F_2R, 1, 4, SOLROCK, LUNATONE),
    RS(MT_PYRE_EXTERIOR, 0, 1, SHUPPET, MEDITITE),
    RS(ROUTE114, 0, 5, LOMBRE, ZANGOOSE),
    RS(ROUTE114, 0, 9, SEVIPER, ZANGOOSE),
    RS(ROUTE117, 0, 1, ODDISH, ROSELIA),
    RS(SKY_PILLAR_1F, 0, 5, BANETTE, DUSCLOPS),
    RS(SKY_PILLAR_3F, 0, 5, BANETTE, DUSCLOPS),
    RS(SKY_PILLAR_5F, 0, 5, BANETTE, DUSCLOPS),
    RS(VICTORY_ROAD_B1F, 0, 3, LAIRON, MEDICHAM),
    RS(VICTORY_ROAD_B1F, 0, 9, MAWILE, MEDITITE),
    RS(VICTORY_ROAD_B2F, 0, 3, LAIRON, MEDICHAM),
    RS(VICTORY_ROAD_B2F, 0, 11, MAWILE, MEDICHAM),
    RS(ROUTE102, 1, 3, MARILL, SURSKIT),
    RS(ROUTE111, 1, 3, MARILL, SURSKIT),
    RS(ROUTE114, 1, 3, MARILL, SURSKIT),
    RS(ROUTE117, 1, 3, MARILL, SURSKIT),
    RS(ROUTE120, 1, 3, MARILL, SURSKIT),
};

static bool RsEnabled(void)
{
    return CtrExtras_Get("rs_pokemon") != 0;
}

/* The Ruby/Sapphire slots of a table copied into `slots`; whether any. A
 * slot is changed only while it still holds the species the rule was made
 * for (a pack of other tables leaves it alone). */
static bool ApplyRs(struct WildPokemon *slots, unsigned count, unsigned mapGroup, unsigned mapNum, bool water)
{
    bool changed = false;

    if (!RsEnabled())
        return false;
    for (unsigned i = 0; i < ARRAY_COUNT(sRsRules); ++i)
    {
        const CtrRsEncounterRule *rule = &sRsRules[i];

        if (rule->mapGroup != mapGroup || rule->mapNum != mapNum || rule->water != water || rule->slot >= count
         || slots[rule->slot].species != rule->expectedSpecies)
            continue;
        slots[rule->slot].species = rule->species;
        changed = true;
    }
    return changed;
}

bool CtrEncounters_MapHasExtra(unsigned mapGroup, unsigned mapNum, uint16_t species)
{
    if (!RsEnabled())
        return false;
    for (unsigned i = 0; i < ARRAY_COUNT(sRsRules); ++i)
        if (sRsRules[i].mapGroup == mapGroup && sRsRules[i].mapNum == mapNum && sRsRules[i].species == species)
            return true;
    return false;
}

const struct WildPokemonInfo *CtrEncounters_Water(const struct WildPokemonInfo *original,
                                                 unsigned mapGroup, unsigned mapNum)
{
    static struct WildPokemon slots[5];
    static struct WildPokemonInfo copy;

    if (!original || !original->wildPokemon)
        return original;
    for (unsigned index = 0; index < ARRAY_COUNT(slots); ++index)
        slots[index] = original->wildPokemon[index];
    if (!ApplyRs(slots, ARRAY_COUNT(slots), mapGroup, mapNum, true))
        return original;
    copy = *original;
    copy.wildPokemon = slots;
    return &copy;
}

const struct WildPokemonInfo *CtrEncounters_Land(const struct WildPokemonInfo *original,
                                                unsigned mapGroup, unsigned mapNum)
{
    static struct WildPokemon slots[12];
    static struct WildPokemonInfo timed;
    unsigned seconds, rate;
    bool changed = false;

    if (!original || !original->wildPokemon)
        return original;
    for (unsigned index = 0; index < ARRAY_COUNT(slots); ++index)
        slots[index] = original->wildPokemon[index];
    /* Ruby/Sapphire's first, on the game's species; then the hour's. */
    changed = ApplyRs(slots, ARRAY_COUNT(slots), mapGroup, mapNum, false);
    if (CtrSettings_GetInt("timed_encounters", 0) == 1 && CtrGame_IsOutdoor() && CtrDayNight_Time(&seconds, &rate))
    {
        unsigned phase = CtrEncounters_Phase(seconds);

        for (unsigned index = 0; index < ARRAY_COUNT(slots); ++index)
        {
            uint16_t species = CtrEncounters_Resolve(sRules, ARRAY_COUNT(sRules), mapGroup, mapNum,
                                                    index, slots[index].species, phase);

            changed |= species != slots[index].species;
            slots[index].species = species;
        }
    }
    if (!changed)
        return original;
    timed = *original;
    timed.wildPokemon = slots;
    return &timed;
}

/* A separately enabled wild randomizer overrides this identity projection.
 * Keep it side-effect-free: the bottom-screen snapshot reads it every frame. */
__attribute__((weak)) uint16_t CtrEncounters_DisplaySpecies(uint16_t species)
{
    return species;
}
