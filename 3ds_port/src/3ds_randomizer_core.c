/*
 * The seeded species randomizer's permutation (include/3ds_randomizer.h).
 * SDK-free and game-free: the host test builds it as it is.
 */
#include <stddef.h>
#include "3ds_randomizer.h"

/* Seed 0 shuffles too: the seed is mixed with a constant of the randomizer's
 * own, so it shares no stream with anything else seeded by the same number. */
#define RANDOMIZER_DOMAIN 0x53504543494553ull /* "SPECIES" */

static uint64_t SplitMix64(uint64_t *state)
{
    uint64_t z = (*state += 0x9E3779B97F4A7C15ull);

    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* Uniform in 0..bound-1 (bound >= 1), without modulo bias. */
static uint32_t Below(uint64_t *state, uint32_t bound)
{
    uint32_t limit = UINT32_MAX - UINT32_MAX % bound;
    uint32_t value;

    do
        value = (uint32_t)(SplitMix64(state) >> 32);
    while (value >= limit);
    return value % bound;
}

void CtrRandomizer_Build(CtrRandomizerTable *table, uint32_t seed)
{
    uint64_t state = RANDOMIZER_DOMAIN ^ ((uint64_t)seed << 16);
    uint32_t i;

    table->seed = seed;
    table->map[0] = 0;
    for (i = 1; i <= CTR_RANDOMIZER_NATIONAL_COUNT; i++)
        table->map[i] = (uint16_t)i;
    /* Fisher-Yates over map[1..386]. */
    for (i = CTR_RANDOMIZER_NATIONAL_COUNT; i > 1; i--)
    {
        uint32_t j = 1 + Below(&state, i);
        uint16_t swap = table->map[i];

        table->map[i] = table->map[j];
        table->map[j] = swap;
    }
}

uint16_t CtrRandomizer_MapNational(const CtrRandomizerTable *table, uint16_t national)
{
    if (table == NULL || national == 0 || national > CTR_RANDOMIZER_NATIONAL_COUNT)
        return 0;
    return table->map[national];
}

bool CtrRandomizer_SpeciesEligible(uint16_t species)
{
    return (species >= 1 && species <= CTR_RANDOMIZER_SPECIES_LAST_JOHTO)
        || (species >= CTR_RANDOMIZER_SPECIES_FIRST_HOENN && species <= CTR_RANDOMIZER_SPECIES_LAST_HOENN);
}

uint16_t CtrRandomizer_MapSpecies(const CtrRandomizerTable *table, uint16_t species,
                                  CtrRandomizerConvert toNational, CtrRandomizerConvert toSpecies)
{
    uint16_t national, mapped, result;

    if (table == NULL || toNational == NULL || toSpecies == NULL || !CtrRandomizer_SpeciesEligible(species))
        return species;
    national = toNational(species);
    mapped = CtrRandomizer_MapNational(table, national);
    if (mapped == 0)
        return species;
    result = toSpecies(mapped);
    /* Only a real species that converts back to the same number. */
    if (!CtrRandomizer_SpeciesEligible(result) || toNational(result) != mapped)
        return species;
    return result;
}

uint32_t CtrRandomizer_SeedFromText(const char *text)
{
    uint32_t hash = 0x811C9DC5u;

    if (text != NULL)
        for (; *text != '\0'; text++)
        {
            hash ^= (uint8_t)*text;
            hash *= 0x01000193u;
        }
    return hash & CTR_RANDOMIZER_SEED_MAX;
}

uint32_t CtrRandomizer_MixSeed(uint64_t a, uint64_t b)
{
    uint64_t state = a ^ (b * 0x9E3779B97F4A7C15ull);

    return (uint32_t)(SplitMix64(&state) >> 32) & CTR_RANDOMIZER_SEED_MAX;
}
