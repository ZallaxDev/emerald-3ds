/*
 * Host test of the seeded species randomizer's core (src/3ds_randomizer_core.c):
 * each seed a true permutation of the 386 National Pokedex numbers, the same
 * every time, different between seeds; internal ids that are not real species
 * (NONE, the old Unown, the Egg, Unown's forms) never mapped and never produced.
 *
 * The species converters here stand in for the game's: ids 1-251 are their
 * own numbers, the old Unown convert beyond 386 (as the game's table has
 * them), and Hoenn's ids 277-411 convert to 252-386 in a scrambled order, so
 * the test sees the core rely on the converters and not on an order.
 */
#include <stdio.h>
#include <string.h>
#include "3ds_randomizer.h"

#define SPECIES_NONE 0
#define SPECIES_OLD_UNOWN_B 252
#define SPECIES_OLD_UNOWN_Z 276
#define SPECIES_TREECKO 277
#define SPECIES_CHIMECHO 411
#define SPECIES_EGG 412
#define SPECIES_UNOWN_QMARK 439
#define HOENN_COUNT (SPECIES_CHIMECHO - SPECIES_TREECKO + 1)

static int sFailures;

#define CHECK(cond, ...) do { if (!(cond)) { sFailures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* 97 is prime to 135 (HOENN_COUNT): a bijection of 0..134. */
static uint16_t FakeToNational(uint16_t species)
{
    if (species >= 1 && species <= 251)
        return species;
    if (species >= SPECIES_OLD_UNOWN_B && species <= SPECIES_OLD_UNOWN_Z)
        return (uint16_t)(387 + species - SPECIES_OLD_UNOWN_B);
    if (species >= SPECIES_TREECKO && species <= SPECIES_CHIMECHO)
        return (uint16_t)(252 + ((species - SPECIES_TREECKO) * 97) % HOENN_COUNT);
    return 0;
}

static uint16_t FakeToSpecies(uint16_t national)
{
    unsigned k;

    if (national >= 1 && national <= 251)
        return national;
    if (national >= 252 && national <= 386)
        for (k = 0; k < HOENN_COUNT; k++)
            if (252 + (k * 97) % HOENN_COUNT == (unsigned)national)
                return (uint16_t)(SPECIES_TREECKO + k);
    return 0;
}

/* Broken converters: the core must fall back to the original species. */
static uint16_t ZeroConvert(uint16_t value)
{
    (void)value;
    return 0;
}

static uint16_t ToOldUnown(uint16_t value)
{
    (void)value;
    return SPECIES_OLD_UNOWN_B;
}

static void TestPermutation(uint32_t seed)
{
    CtrRandomizerTable table;
    unsigned seen[CTR_RANDOMIZER_NATIONAL_COUNT + 1] = {0};
    unsigned n;

    CtrRandomizer_Build(&table, seed);
    CHECK(table.seed == seed, "seed kept");
    CHECK(table.map[0] == 0, "map[0] stays 0 (seed %u)", (unsigned)seed);
    for (n = 1; n <= CTR_RANDOMIZER_NATIONAL_COUNT; n++)
    {
        uint16_t m = CtrRandomizer_MapNational(&table, (uint16_t)n);

        CHECK(m >= 1 && m <= CTR_RANDOMIZER_NATIONAL_COUNT, "seed %u: %u maps out of range to %u", (unsigned)seed, n, m);
        if (m >= 1 && m <= CTR_RANDOMIZER_NATIONAL_COUNT)
            seen[m]++;
    }
    for (n = 1; n <= CTR_RANDOMIZER_NATIONAL_COUNT; n++)
        CHECK(seen[n] == 1, "seed %u: %u produced %u times", (unsigned)seed, n, seen[n]);
    CHECK(CtrRandomizer_MapNational(&table, 0) == 0, "national 0");
    CHECK(CtrRandomizer_MapNational(&table, 387) == 0, "national 387");
    CHECK(CtrRandomizer_MapNational(&table, 0xFFFF) == 0, "national 0xFFFF");
    CHECK(CtrRandomizer_MapNational(NULL, 1) == 0, "no table");
}

static void TestRepeatable(void)
{
    CtrRandomizerTable a, b, other;

    CtrRandomizer_Build(&a, 12345);
    CtrRandomizer_Build(&other, 999);   /* no state carried between builds */
    CtrRandomizer_Build(&b, 12345);
    CHECK(memcmp(a.map, b.map, sizeof(a.map)) == 0, "same seed, same permutation");
}

static unsigned Differences(const CtrRandomizerTable *a, const CtrRandomizerTable *b)
{
    unsigned n, count = 0;

    for (n = 1; n <= CTR_RANDOMIZER_NATIONAL_COUNT; n++)
        count += a->map[n] != b->map[n];
    return count;
}

static void TestDistinctSeeds(void)
{
    static CtrRandomizerTable tables[64];
    unsigned i, j, fixed;
    CtrRandomizerTable max;

    for (i = 0; i < 64; i++)
        CtrRandomizer_Build(&tables[i], i);
    for (i = 0; i < 64; i++)
        for (j = i + 1; j < 64; j++)
            CHECK(Differences(&tables[i], &tables[j]) >= 300, "seeds %u and %u share too much (%u differ)",
                  i, j, Differences(&tables[i], &tables[j]));
    CtrRandomizer_Build(&max, CTR_RANDOMIZER_SEED_MAX);
    CHECK(Differences(&max, &tables[0]) >= 300, "seed max vs 0");
    /* Seed 0 shuffles too: a random permutation has about one fixed point. */
    for (i = 0; i < 64; i++)
    {
        fixed = 0;
        for (j = 1; j <= CTR_RANDOMIZER_NATIONAL_COUNT; j++)
            fixed += (unsigned)tables[i].map[j] == j;
        CHECK(fixed < 16, "seed %u: %u fixed points", i, fixed);
    }
}

static void TestSpecies(uint32_t seed)
{
    CtrRandomizerTable table;
    static unsigned produced[SPECIES_UNOWN_QMARK + 1];
    unsigned s, count = 0;

    memset(produced, 0, sizeof(produced));
    CtrRandomizer_Build(&table, seed);
    for (s = 0; s <= SPECIES_UNOWN_QMARK; s++)
    {
        uint16_t out = CtrRandomizer_MapSpecies(&table, (uint16_t)s, FakeToNational, FakeToSpecies);

        if (CtrRandomizer_SpeciesEligible((uint16_t)s))
        {
            count++;
            CHECK(CtrRandomizer_SpeciesEligible(out), "seed %u: species %u became invalid %u", (unsigned)seed, s, out);
            if (out <= SPECIES_UNOWN_QMARK)
                produced[out]++;
            /* The species follows the permutation of its number. */
            CHECK(FakeToNational(out) == CtrRandomizer_MapNational(&table, FakeToNational((uint16_t)s)),
                  "seed %u: species %u not mapped by its number", (unsigned)seed, s);
        }
        else
        {
            CHECK((unsigned)out == s, "seed %u: ineligible id %u changed to %u", (unsigned)seed, s, (unsigned)out);
        }
    }
    CHECK(count == CTR_RANDOMIZER_NATIONAL_COUNT, "%u eligible species", count);
    for (s = 0; s <= SPECIES_UNOWN_QMARK; s++)
        if (CtrRandomizer_SpeciesEligible((uint16_t)s))
            CHECK(produced[s] == 1, "seed %u: species %u produced %u times", (unsigned)seed, s, produced[s]);
}

static void TestInvalidIds(void)
{
    CtrRandomizerTable table;
    uint16_t s;

    CtrRandomizer_Build(&table, 7);
    CHECK(!CtrRandomizer_SpeciesEligible(SPECIES_NONE), "NONE");
    CHECK(CtrRandomizer_SpeciesEligible(1) && CtrRandomizer_SpeciesEligible(251), "Bulbasaur, Celebi");
    for (s = SPECIES_OLD_UNOWN_B; s <= SPECIES_OLD_UNOWN_Z; s++)
        CHECK(!CtrRandomizer_SpeciesEligible(s), "old Unown %u", s);
    CHECK(CtrRandomizer_SpeciesEligible(SPECIES_TREECKO) && CtrRandomizer_SpeciesEligible(SPECIES_CHIMECHO), "Treecko, Chimecho");
    CHECK(!CtrRandomizer_SpeciesEligible(SPECIES_EGG), "Egg");
    CHECK(!CtrRandomizer_SpeciesEligible(SPECIES_EGG + 1) && !CtrRandomizer_SpeciesEligible(SPECIES_UNOWN_QMARK), "Unown forms");
    CHECK(!CtrRandomizer_SpeciesEligible(0xFFFF), "0xFFFF");
    CHECK(CtrRandomizer_MapSpecies(&table, SPECIES_EGG, FakeToNational, FakeToSpecies) == SPECIES_EGG, "Egg unchanged");
    CHECK(CtrRandomizer_MapSpecies(&table, 0xFFFF, FakeToNational, FakeToSpecies) == 0xFFFF, "0xFFFF unchanged");
    /* Converters that disagree, or give an invalid species: unchanged. */
    CHECK(CtrRandomizer_MapSpecies(&table, 25, FakeToNational, ZeroConvert) == 25, "toSpecies 0");
    CHECK(CtrRandomizer_MapSpecies(&table, 25, ZeroConvert, FakeToSpecies) == 25, "toNational 0");
    CHECK(CtrRandomizer_MapSpecies(&table, 25, FakeToNational, ToOldUnown) == 25, "old Unown result refused");
    CHECK(CtrRandomizer_MapSpecies(&table, 25, NULL, FakeToSpecies) == 25, "no converter");
    CHECK(CtrRandomizer_MapSpecies(NULL, 25, FakeToNational, FakeToSpecies) == 25, "no table");
}

static void TestSeeds(void)
{
    /* FNV-1a 32 vectors, masked to 31 bits. */
    CHECK(CtrRandomizer_SeedFromText("") == (0x811C9DC5u & 0x7FFFFFFFu), "FNV of \"\"");
    CHECK(CtrRandomizer_SeedFromText("a") == (0xE40C292Cu & 0x7FFFFFFFu), "FNV of \"a\"");
    CHECK(CtrRandomizer_SeedFromText(NULL) == (0x811C9DC5u & 0x7FFFFFFFu), "FNV of NULL");
    CHECK(CtrRandomizer_SeedFromText("emerald") != CtrRandomizer_SeedFromText("Emerald"), "case matters");
    CHECK(CtrRandomizer_MixSeed(1, 2) <= CTR_RANDOMIZER_SEED_MAX, "mixed seed in range");
    CHECK(CtrRandomizer_MixSeed(1, 2) == CtrRandomizer_MixSeed(1, 2), "mix repeatable");
    CHECK(CtrRandomizer_MixSeed(1, 2) != CtrRandomizer_MixSeed(1, 3), "mix differs");
}

int main(void)
{
    static const uint32_t seeds[] = {0, 1, 2, 42, 12345, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu};
    unsigned i;

    for (i = 0; i < sizeof(seeds) / sizeof(seeds[0]); i++)
    {
        TestPermutation(seeds[i]);
        TestSpecies(seeds[i]);
    }
    TestRepeatable();
    TestDistinctSeeds();
    TestInvalidIds();
    TestSeeds();
    if (sFailures)
    {
        printf("randomizer_test: %d failure(s)\n", sFailures);
        return 1;
    }
    printf("randomizer_test: ok\n");
    return 0;
}
