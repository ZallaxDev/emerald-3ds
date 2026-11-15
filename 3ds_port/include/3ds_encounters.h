#ifndef CTR_ENCOUNTERS_H
#define CTR_ENCOUNTERS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { CTR_TIME_MORNING, CTR_TIME_DAY, CTR_TIME_NIGHT, CTR_TIME_COUNT };

typedef struct
{
    uint8_t mapGroup, mapNum, slot;
    uint16_t expectedSpecies, species[CTR_TIME_COUNT];
} CtrTimedEncounterRule;

unsigned CtrEncounters_Phase(unsigned seconds);
uint16_t CtrEncounters_Resolve(const CtrTimedEncounterRule *rules, size_t count,
                             unsigned mapGroup, unsigned mapNum, unsigned slot,
                             uint16_t original, unsigned phase);
/* Species shown by encounter previews; overridden by optional randomizers. */
uint16_t CtrEncounters_DisplaySpecies(uint16_t species);
struct WildPokemonInfo;
const struct WildPokemonInfo *CtrEncounters_Land(const struct WildPokemonInfo *original,
                                                unsigned mapGroup, unsigned mapNum);
/* The surfing table as met: Ruby/Sapphire's Pokemon in their slots
 * (RUBY/SAPPHIRE POKEMON), or the game's own. The land one above has them
 * too, before the hour's. Each returns a table of its own, kept until its
 * next call. */
const struct WildPokemonInfo *CtrEncounters_Water(const struct WildPokemonInfo *original,
                                                 unsigned mapGroup, unsigned mapNum);
/* Whether RUBY/SAPPHIRE POKEMON puts `species` on that map (the Pokedex's
 * AREA, pokedex_area_screen.c). */
bool CtrEncounters_MapHasExtra(unsigned mapGroup, unsigned mapNum, uint16_t species);

/*
 * A visible wild Pokemon (wild_encounter.c) lives on land or in the water,
 * and meets the player only where it lives: in the water the player is
 * surfing on it or diving under it (an underwater map's seaweed is water,
 * MetatileBehavior_IsWaterWildEncounter, and there the player is
 * PLAYER_AVATAR_FLAG_UNDERWATER, not surfing).
 */
bool CtrEncounters_PlayerInWater(bool surfing, bool underwater);

#endif
