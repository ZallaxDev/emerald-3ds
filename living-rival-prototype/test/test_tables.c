#include "lr_test.h"
#include "lr_enums.h"
#include "lr_facet_tables.h"

int main(void) {
    /* Universal offsets: the design doc's 8 x 8 table. */
    const int8_t *hot = LrUniversalTemperamentOffsets(LR_TEMP_HOTHEAD);
    LR_CHECK_EQ_INT(hot[LR_FACET_ANGER], 30);
    LR_CHECK_EQ_INT(hot[LR_FACET_BRAVERY], 10);
    LR_CHECK_EQ_INT(hot[LR_FACET_SOCIAL], -5);
    LR_CHECK_EQ_INT(hot[LR_FACET_DISCIPLINE], -10);
    const int8_t *cow = LrUniversalTemperamentOffsets(LR_TEMP_COWARD);
    LR_CHECK_EQ_INT(cow[LR_FACET_BRAVERY], -35);
    LR_CHECK_EQ_INT(cow[LR_FACET_HONESTY], -5);
    const int8_t *dri = LrUniversalTemperamentOffsets(LR_TEMP_DRIFTER);
    LR_CHECK_EQ_INT(dri[LR_FACET_AMBITION], -25);
    LR_CHECK_EQ_INT(dri[LR_FACET_LOYALTY], -15);
    const int8_t *loy = LrUniversalTemperamentOffsets(LR_TEMP_LOYAL);
    LR_CHECK_EQ_INT(loy[LR_FACET_LOYALTY], 35);
    const int8_t *st = LrUniversalTemperamentOffsets(LR_TEMP_STEADY);
    for (int i = 0; i < LR_UNIVERSAL_FACET_COUNT; i++) LR_CHECK_EQ_INT(st[i], 0);

    /* Out-of-range temperament -> zero row, never NULL. */
    const int8_t *bad = LrUniversalTemperamentOffsets(200);
    for (int i = 0; i < LR_UNIVERSAL_FACET_COUNT; i++) LR_CHECK_EQ_INT(bad[i], 0);

    /* Region counts. */
    LR_CHECK_EQ_INT(LrRegionFacetCount(LR_REGION_KANTO), 3);
    LR_CHECK_EQ_INT(LrRegionFacetCount(LR_REGION_JOHTO), 4);
    LR_CHECK_EQ_INT(LrRegionFacetCount(LR_REGION_HOENN), 3);
    LR_CHECK_EQ_INT(LrRegionFacetCount(LR_REGION_SINNOH), 3);
    LR_CHECK_EQ_INT(LrRegionFacetCount(LR_REGION_UNOVA), 3);

    /* Spot-check region offsets. */
    const LrRegionProfile *kanto = LrGetRegionProfile(LR_REGION_KANTO);
    LR_CHECK_EQ_INT(kanto->temperamentOffsets[LR_TEMP_PROUD][0], 10);   /* Honor */
    LR_CHECK_EQ_INT(kanto->temperamentOffsets[LR_TEMP_DRIFTER][0], -10);
    LR_CHECK_EQ_INT(kanto->temperamentOffsets[LR_TEMP_COLD][2], 5);     /* Underworld */
    LR_CHECK_EQ_INT(kanto->lawFacet, 0);
    LR_CHECK_EQ_INT(kanto->moneyFacet, 2);
    LR_CHECK_EQ_INT(kanto->familyFacet, -1);
    LR_CHECK_EQ_INT(kanto->spiritFacet, -1);

    const LrRegionProfile *johto = LrGetRegionProfile(LR_REGION_JOHTO);
    LR_CHECK_EQ_INT(johto->familyFacet, 1);   /* Harmony */
    LR_CHECK_EQ_INT(johto->spiritFacet, 3);   /* Mysticism */
    LR_CHECK_EQ_INT(johto->temperamentOffsets[LR_TEMP_LOYAL][0], 10);

    const LrRegionProfile *hoenn = LrGetRegionProfile(LR_REGION_HOENN);
    LR_CHECK_EQ_INT(hoenn->familyFacet, 0);   /* Nature */
    LR_CHECK_EQ_INT(hoenn->spiritFacet, 2);   /* Ancient */

    const LrRegionProfile *sinnoh = LrGetRegionProfile(LR_REGION_SINNOH);
    LR_CHECK_EQ_INT(sinnoh->spiritFacet, 0);  /* Myth */
    LR_CHECK_EQ_INT(sinnoh->temperamentOffsets[LR_TEMP_COWARD][2], -10);

    const LrRegionProfile *unova = LrGetRegionProfile(LR_REGION_UNOVA);
    LR_CHECK_EQ_INT(unova->moneyFacet, 1);    /* Industry */
    LR_CHECK_EQ_INT(unova->temperamentOffsets[LR_TEMP_GREEDY][1], 10);

    /* Faction counts and spot checks. */
    LR_CHECK_EQ_INT(LrFactionFacetCount(LR_FACTION_GYM_CIRCUIT), 1);
    LR_CHECK_EQ_INT(LrFactionFacetCount(LR_FACTION_ELITE_FOUR_PATH), 1);
    LR_CHECK_EQ_INT(LrFactionFacetCount(LR_FACTION_TEAM_MAGMA), 1);
    LR_CHECK_EQ_INT(LrFactionFacetCount(LR_FACTION_TEAM_AQUA), 1);
    LR_CHECK_EQ_INT(LrFactionFacetCount(LR_FACTION_CONTEST_STAR), 1);
    LR_CHECK_EQ_INT(LrFactionFacetCount(LR_FACTION_BATTLE_FRONTIER), 1);
    LR_CHECK_EQ_INT(LrFactionFacetCount(LR_FACTION_INDEPENDENT), 0);
    LR_CHECK_EQ_INT(LrFactionFacetCount(LR_FACTION_ROOKIE_LEAGUE), 0);

    const LrFactionProfile *gym = LrGetFactionProfile(LR_FACTION_GYM_CIRCUIT);
    LR_CHECK_EQ_INT(gym->archetypeOffsets[LR_ARCH_PRODIGY][0], 10);
    LR_CHECK_EQ_INT(gym->archetypeOffsets[LR_ARCH_EXPERIMENTER][0], -5);
    const LrFactionProfile *elite = LrGetFactionProfile(LR_FACTION_ELITE_FOUR_PATH);
    LR_CHECK_EQ_INT(elite->archetypeOffsets[LR_ARCH_LEGACY][0], 10);
    const LrFactionProfile *frontier = LrGetFactionProfile(LR_FACTION_BATTLE_FRONTIER);
    LR_CHECK_EQ_INT(frontier->archetypeOffsets[LR_ARCH_EXPERIMENTER][0], 10);

    /* Out-of-range lookups return a neutral, non-NULL profile. */
    const LrRegionProfile *nr = LrGetRegionProfile(255);
    LR_CHECK(nr != 0);
    LR_CHECK_EQ_INT(nr->facetCount, 0);
    const LrFactionProfile *nf = LrGetFactionProfile(255);
    LR_CHECK(nf != 0);
    LR_CHECK_EQ_INT(nf->facetCount, 0);

    /* Every offset stays inside a sane authored range, and every selector is valid. */
    for (int r = 0; r < LR_REGION_COUNT; r++) {
        const LrRegionProfile *p = LrGetRegionProfile((uint8_t)r);
        LR_CHECK(p->facetCount <= LR_MAX_REGION_FACETS);
        for (int t = 0; t < LR_TEMPERAMENT_COUNT; t++)
            for (int f = 0; f < LR_MAX_REGION_FACETS; f++)
                LR_CHECK(p->temperamentOffsets[t][f] >= -40 && p->temperamentOffsets[t][f] <= 40);
        int8_t sel[5] = { p->familyFacet, p->lawFacet, p->spiritFacet, p->moneyFacet, p->prideFacet };
        for (int i = 0; i < 5; i++)
            LR_CHECK(sel[i] >= -1 && sel[i] < p->facetCount);
    }
    for (int f = 0; f < LR_FACTION_COUNT; f++) {
        const LrFactionProfile *p = LrGetFactionProfile((uint8_t)f);
        LR_CHECK(p->facetCount <= LR_MAX_FACTION_FACETS);
        for (int a = 0; a < LR_ARCHETYPE_COUNT; a++)
            for (int i = 0; i < LR_MAX_FACTION_FACETS; i++)
                LR_CHECK(p->archetypeOffsets[a][i] >= -40 && p->archetypeOffsets[a][i] <= 40);
    }

    /* Memory/intent enums are save-format values. */
    LR_CHECK_EQ_INT(LR_MEMORY_ANGER, 0);
    LR_CHECK_EQ_INT(LR_MEMORY_FEAR, 1);
    LR_CHECK_EQ_INT(LR_MEMORY_SHAME, 2);
    LR_CHECK_EQ_INT(LR_MEMORY_GRIEF, 3);
    LR_CHECK_EQ_INT(LR_MEMORY_JOY, 4);
    LR_CHECK_EQ_INT(LR_MEMORY_PRIDE, 5);
    LR_CHECK_EQ_INT(LR_CAUSE_WORLD, 0);
    LR_CHECK_EQ_INT(LR_CAUSE_PLAYER, 1);
    LR_CHECK_EQ_INT(LR_CAUSE_OTHER_TRAINER, 2);
    LR_CHECK_EQ_INT(LR_INTENT_NONE, 0);
    LR_CHECK_EQ_INT(LR_INTENT_HUNTS, 1);
    LR_CHECK_EQ_INT(LR_INTENT_WONT_HUNT, 2);
    LR_CHECK_EQ_INT(LR_TRAINER_PLAYER, 0xFFFE);

    /* Pride selectors. */
    LR_CHECK_EQ_INT(LrGetRegionProfile(LR_REGION_KANTO)->prideFacet, 0);
    LR_CHECK_EQ_INT(LrGetRegionProfile(LR_REGION_JOHTO)->prideFacet, 0);
    LR_CHECK_EQ_INT(LrGetRegionProfile(LR_REGION_HOENN)->prideFacet, 1);
    LR_CHECK_EQ_INT(LrGetRegionProfile(LR_REGION_SINNOH)->prideFacet, 0);
    LR_CHECK_EQ_INT(LrGetRegionProfile(LR_REGION_UNOVA)->prideFacet, 0);

    return lr_test_summary("test_tables");
}
