#include "global.h"
#include "wild_encounter.h"
#include "constants/maps.h"
#include "constants/species.h"
#include "3ds_platform.h"
#include "3ds_daynight.h"
#include "3ds_encounters.h"

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

const struct WildPokemonInfo *CtrEncounters_Land(const struct WildPokemonInfo *original,
                                                unsigned mapGroup, unsigned mapNum)
{
    static struct WildPokemon slots[12];
    static struct WildPokemonInfo timed;
    unsigned seconds, rate;
    bool changed = false;

    if (!original || !original->wildPokemon || CtrSettings_GetInt("timed_encounters", 0) != 1
     || !CtrGame_IsOutdoor() || !CtrDayNight_Time(&seconds, &rate))
        return original;
    unsigned phase = CtrEncounters_Phase(seconds);

    for (unsigned index = 0; index < ARRAY_COUNT(slots); ++index)
    {
        slots[index] = original->wildPokemon[index];
        slots[index].species = CtrEncounters_Resolve(sRules, ARRAY_COUNT(sRules), mapGroup, mapNum,
                                                   index, slots[index].species, phase);
        changed |= slots[index].species != original->wildPokemon[index].species;
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
